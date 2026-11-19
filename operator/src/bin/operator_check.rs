//! Headless functional check of one vehicle through the operator's own code (link.rs, video.rs):
//! each step drives the vehicle or the server the way the window does and checks what comes back.
//!
//! operator_check --connect <endpoints> [--prefix scv/r01] [--robot r01] <step>...
//!
//! Steps: fleet video drive limit watchdog takeover stop estop history lidar degrade loss.
//! Prints one PASS or FAIL line per step and exits 1 if any failed.

#![allow(dead_code)]

#[path = "../link.rs"]
mod link;
#[path = "../video.rs"]
mod video;
#[path = "../wire.rs"]
mod wire;

use std::sync::Arc;
use std::time::{Duration, Instant};

use link::{Link, Status};

const MODE_REMOTE: i8 = 2;
const SOURCE_TELEOP: i8 = 1;
const MOTION_PERIOD: Duration = Duration::from_millis(20);
// teleop_control.param.yaml: vehicle.max_linear_mps and degrade.linear_mps.
const MAX_LINEAR: f64 = 1.5;
const DEGRADED_LINEAR: f64 = 0.5;
// teleop_video.param.yaml: encoder.fps.
const FPS: f64 = 15.0;
const VEHICLE_PROCESSES: [&str; 3] = ["fleet_telemetry", "teleop_control", "teleop_video"];

struct Check {
    link: Arc<Link>,
    decoder: Arc<video::Decoder>,
    connect: Vec<String>,
    prefix: String,
    robot: String,
}

type Outcome = Result<String, String>;

fn wait_for(timeout: Duration, mut ok: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + timeout;
    while Instant::now() < end {
        if ok() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    ok()
}

impl Check {
    fn status<T>(&self, f: impl FnOnce(&Status) -> T) -> T {
        f(&self.link.status.lock().unwrap())
    }

    fn armed(&self) -> bool {
        self.status(|s| s.arm.as_ref().is_some_and(|a| a.armed && s.session.as_ref().is_some_and(|x| x.1 == a.epoch)))
    }

    fn reason(&self) -> String {
        self.status(|s| s.arm.as_ref().map(|a| a.reason.clone()).unwrap_or_default())
    }

    fn speed(&self) -> f64 {
        self.status(|s| s.speed_mps.unwrap_or(0.0))
    }

    /// Asks hunter_mux for the remote source through fleet_telemetry, then arms.
    fn arm(&self) -> Result<(), String> {
        self.link.command("cmd_mode", &wire::cmd_mode_cdr(MODE_REMOTE));
        if !wait_for(Duration::from_secs(5), || self.status(|s| s.mux.is_some_and(|m| m.mode == MODE_REMOTE && m.remote_allowed))) {
            return Err(format!("mux not in remote mode: {}", self.status(|s| s.last_result.clone().unwrap_or_default())));
        }
        self.link.arm();
        if !wait_for(Duration::from_secs(2), || self.armed()) {
            return Err(format!("not armed: {} {}", self.reason(), self.status(|s| s.note.clone())));
        }
        Ok(())
    }

    /// Sends one motion command per period for `d`, the dead-man held.
    fn drive(&self, linear: f64, d: Duration) {
        let end = Instant::now() + d;
        while Instant::now() < end {
            self.link.motion(linear, 0.0, true);
            std::thread::sleep(MOTION_PERIOD);
        }
    }

    /// The highest odometry speed while driving at `linear` for `d`.
    fn drive_peak(&self, linear: f64, d: Duration) -> f64 {
        let end = Instant::now() + d;
        let mut peak: f64 = 0.0;
        while Instant::now() < end {
            self.link.motion(linear, 0.0, true);
            peak = peak.max(self.speed());
            std::thread::sleep(MOTION_PERIOD);
        }
        peak
    }

    fn settle(&self) -> bool {
        self.link.stop();
        wait_for(Duration::from_secs(5), || self.speed().abs() < 0.05)
    }

    fn fleet(&self) -> Outcome {
        if !wait_for(Duration::from_secs(10), || self.link.server_connected()) {
            return Err("no server".into());
        }
        let mut found = None;
        let complete = |v: &link::Vehicle| VEHICLE_PROCESSES.iter().all(|p| v.alive.iter().any(|a| a == p));
        wait_for(Duration::from_secs(20), || {
            found = self.link.vehicles().ok().and_then(|v| v.into_iter().find(|v| v.prefix == self.prefix));
            found.as_ref().is_some_and(complete)
        });
        let v = found.ok_or("vehicle not in fleet/vehicles")?;
        if !complete(&v) {
            return Err(format!("live processes {:?}", v.alive));
        }
        Ok(format!("{} alive {:?}", v.id, v.alive))
    }

    fn video(&self) -> Outcome {
        wait_for(Duration::from_secs(10), || self.status(|s| s.video_frames) > 0);
        let start = (self.status(|s| (s.video_frames, s.video_seq)), *self.decoder.decoded.lock().unwrap());
        std::thread::sleep(Duration::from_secs(5));
        let (frames, seq, keys, lost) = self.status(|s| (s.video_frames, s.video_seq, s.keyframes, s.video_lost));
        let decoded = *self.decoder.decoded.lock().unwrap();
        let (f, sent, d) = ((frames - start.0.0) as f64 / 5.0, (seq - start.0.1) as f64 / 5.0, (decoded - start.1) as f64 / 5.0);
        let size = self.decoder.latest.lock().unwrap().as_ref().map(|f| (f.width, f.height));
        let detail = format!("{sent:.1} fps sent, {f:.1} received, {d:.1} decoded, {keys} keyframes, {lost} lost, frame {size:?}");
        if (sent - FPS).abs() > 1.5 || f < sent - 1.0 || d < f - 1.0 || keys == 0 {
            return Err(detail);
        }
        Ok(detail)
    }

    fn drive_step(&self) -> Outcome {
        self.arm()?;
        let peak = self.drive_peak(0.8, Duration::from_secs(4));
        let source = self.status(|s| s.mux.map(|m| m.source));
        if source != Some(SOURCE_TELEOP) {
            return Err(format!("mux source {source:?}"));
        }
        if !(0.6..=1.0).contains(&peak) {
            return Err(format!("odometry peaked at {peak:.2} m/s for 0.8"));
        }
        if !self.settle() {
            return Err(format!("still {:.2} m/s after stop", self.speed()));
        }
        Ok(format!("0.8 m/s commanded, odometry {peak:.2} m/s, back to rest after stop"))
    }

    fn limit(&self) -> Outcome {
        self.arm()?;
        let peak = self.drive_peak(3.0, Duration::from_secs(6));
        self.settle();
        if peak > MAX_LINEAR + 0.1 || peak < MAX_LINEAR - 0.3 {
            return Err(format!("3.0 m/s commanded, odometry {peak:.2}"));
        }
        Ok(format!("3.0 m/s commanded, odometry {peak:.2} (limit {MAX_LINEAR})"))
    }

    fn watchdog(&self) -> Outcome {
        self.arm()?;
        self.drive(0.8, Duration::from_secs(2));
        let silent = Instant::now();
        if !wait_for(Duration::from_secs(1), || !self.armed()) {
            return Err("still armed 1 s after the commands stopped".into());
        }
        let after = silent.elapsed();
        let reason = self.reason();
        if reason != "watchdog" {
            return Err(format!("disarmed with {reason:?}"));
        }
        if !wait_for(Duration::from_secs(5), || self.speed().abs() < 0.05) {
            return Err(format!("still {:.2} m/s", self.speed()));
        }
        self.drive(0.8, Duration::from_millis(500));
        if self.speed() > 0.05 {
            return Err("moved on the old session after the watchdog".into());
        }
        Ok(format!("disarmed {} ms after the last command, old session refused", after.as_millis()))
    }

    fn takeover(&self) -> Outcome {
        // Opened first: a session takes longer to open than the watchdog.
        let other = Link::open(&self.connect, &self.prefix, &self.robot, self.decoder.clone(), false).map_err(|e| e.to_string())?;
        self.arm()?;
        let link = self.link.clone();
        let driving = std::thread::spawn(move || {
            let end = Instant::now() + Duration::from_secs(3);
            while Instant::now() < end {
                link.motion(0.0, 0.0, true);
                std::thread::sleep(MOTION_PERIOD);
            }
        });
        std::thread::sleep(Duration::from_millis(500));
        other.arm();
        let refused = wait_for(Duration::from_secs(2), || {
            other.status.lock().unwrap().arm.as_ref().is_some_and(|a| !a.armed && a.reason == "another session is armed")
        });
        driving.join().unwrap();
        let still = self.armed();
        self.link.stop();
        if !refused || !still {
            return Err(format!("second session refused {refused}, first still armed {still}"));
        }
        Ok("a second session was refused while the first drove".into())
    }

    fn stop(&self) -> Outcome {
        self.arm()?;
        self.drive(0.8, Duration::from_secs(2));
        self.link.stop();
        if !wait_for(Duration::from_secs(1), || !self.armed()) {
            return Err("still armed".into());
        }
        let reason = self.reason();
        if !wait_for(Duration::from_secs(5), || self.speed().abs() < 0.05) {
            return Err(format!("still {:.2} m/s", self.speed()));
        }
        Ok(format!("disarmed with {reason:?} and stopped"))
    }

    fn estop(&self) -> Outcome {
        self.arm()?;
        self.drive(0.8, Duration::from_secs(1));
        self.link.estop();
        if !wait_for(Duration::from_secs(3), || self.status(|s| s.estop_latched == Some(true)) && !self.armed()) {
            return Err(format!("latched {:?}, armed {}", self.status(|s| s.estop_latched), self.armed()));
        }
        if self.arm().is_ok() {
            self.drive(0.8, Duration::from_secs(1));
            if self.speed() > 0.05 {
                return Err(format!("moved at {:.2} m/s while the e-stop was latched", self.speed()));
            }
        }
        self.link.stop();
        self.link.estop_reset();
        if !wait_for(Duration::from_secs(3), || self.status(|s| s.estop_latched == Some(false))) {
            return Err("e-stop not released by the reset".into());
        }
        self.arm()?;
        let peak = self.drive_peak(0.8, Duration::from_secs(3));
        self.settle();
        if peak < 0.5 {
            return Err(format!("did not drive after the reset ({peak:.2} m/s)"));
        }
        Ok("latched, held the vehicle, released by the reset".into())
    }

    fn history(&self) -> Outcome {
        let h = self.link.history("odom", 60).map_err(|e| e.to_string())?;
        if h.len() < 5 {
            return Err(format!("{} odometry messages in 60 s", h.len()));
        }
        let speeds: Vec<f64> = h.iter().filter_map(|(_, m)| wire::odometry_speed(m)).collect();
        if speeds.len() != h.len() {
            return Err("a stored message is not odometry".into());
        }
        let peak = speeds.iter().cloned().fold(0.0, f64::max);
        let span = (h.last().unwrap().0 - h[0].0) as f64 / 1e9;
        Ok(format!("{} messages over {span:.0} s, peak {peak:.2} m/s", h.len()))
    }

    fn lidar(&self) -> Outcome {
        let count = || self.status(|s| s.streams.get("lidar").map(|x| (x.count, x.bytes)).unwrap_or((0, 0)));
        let start = count();
        std::thread::sleep(Duration::from_secs(5));
        let (n, b) = count();
        let (n, b) = (n - start.0, b - start.1);
        if n < 20 {
            return Err(format!("{n} lidar sectors in 5 s"));
        }
        Ok(format!("{n} sectors in 5 s, {:.0} kbit/s", b as f64 * 8.0 / 5.0 / 1000.0))
    }

    /// Run while the vehicle's uplink is slow: the vehicle caps the speed and says why.
    fn degrade(&self) -> Outcome {
        self.arm()?;
        let peak = self.drive_peak(1.2, Duration::from_secs(6));
        let reason = self.reason();
        self.settle();
        if !reason.starts_with("speed limited") {
            return Err(format!("arm state reason {reason:?}, odometry {peak:.2}"));
        }
        if peak > DEGRADED_LINEAR + 0.1 {
            return Err(format!("{reason}, but odometry reached {peak:.2}"));
        }
        Ok(format!("{reason}, odometry {peak:.2} m/s for 1.2"))
    }

    /// Run while the vehicle's uplink loses packets: FEC rebuilds pieces, the picture keeps going.
    fn loss(&self) -> Outcome {
        let start = self.status(|s| (s.video_frames, s.video_lost, s.keyframes));
        let decoded = *self.decoder.decoded.lock().unwrap();
        std::thread::sleep(Duration::from_secs(10));
        let (f, l, k) = self.status(|s| (s.video_frames - start.0, s.video_lost - start.1, s.keyframes - start.2));
        let d = *self.decoder.decoded.lock().unwrap() - decoded;
        if f < 100 || d < 100 {
            return Err(format!("{f} frames, {d} decoded in 10 s"));
        }
        Ok(format!("{f} frames, {d} decoded, {l} lost, {k} keyframes in 10 s"))
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let (mut connect, mut prefix, mut robot, mut steps) = (Vec::new(), "scv/r01".to_string(), "r01".to_string(), Vec::new());
    while let Some(a) = args.next() {
        match a.as_str() {
            "--connect" => connect = args.next().unwrap_or_default().split(',').map(String::from).collect(),
            "--prefix" => prefix = args.next().unwrap_or_default(),
            "--robot" => robot = args.next().unwrap_or_default(),
            step => steps.push(step.to_string()),
        }
    }
    let decoder = Arc::new(video::Decoder::new().expect("decoder"));
    let link = Arc::new(Link::open(&connect, &prefix, &robot, decoder.clone(), false).expect("link"));
    let c = Check { link, decoder, connect, prefix, robot };
    let mut failed = false;
    for step in &steps {
        let out = match step.as_str() {
            "fleet" => c.fleet(),
            "video" => c.video(),
            "drive" => c.drive_step(),
            "limit" => c.limit(),
            "watchdog" => c.watchdog(),
            "takeover" => c.takeover(),
            "stop" => c.stop(),
            "estop" => c.estop(),
            "history" => c.history(),
            "lidar" => c.lidar(),
            "degrade" => c.degrade(),
            "loss" => c.loss(),
            other => Err(format!("unknown step {other}")),
        };
        match out {
            Ok(detail) => println!("PASS {step}: {detail}"),
            Err(detail) => {
                failed = true;
                println!("FAIL {step}: {detail}");
            }
        }
    }
    std::process::exit(failed as i32);
}
