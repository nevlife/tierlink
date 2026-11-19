//! The operator's side of tierlink: receives the teleop video and the monitor streams, acks
//! them, asks for keyframes after a gap, and sends control messages and commands.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use zenoh::qos::{CongestionControl, Priority, Reliability};
use zenoh::sample::{Sample, SampleKind};
use zenoh::{Session, Wait};

use crate::video::Decoder;
use crate::wire;

const VIDEO_STREAM: &str = "front_cam";
const KEYFRAME_REPEAT: Duration = Duration::from_millis(200);
const COMMAND_TTL_MS: u64 = 2000;
const DELAY_WINDOW: Duration = Duration::from_secs(10);

fn monotonic_raw_ns() -> i64 {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
    t.tv_sec * 1_000_000_000 + t.tv_nsec
}

pub fn unix_ns() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos() as u64
}

fn mono_ns(start: Instant) -> u64 {
    start.elapsed().as_nanos() as u64
}

#[derive(Default)]
pub struct StreamStat {
    pub msg_type: String,
    pub count: u64,
    pub bytes: u64,
    pub last: Option<Instant>,
}

#[derive(Default)]
pub struct Status {
    pub video_frames: u64,
    pub video_bytes: u64,
    pub video_lost: u64,
    pub keyframes: u64,
    pub video_last: Option<Instant>,
    pub streams: BTreeMap<String, StreamStat>,
    pub speed_mps: Option<f64>,
    pub gps: Option<(f64, f64)>,
    pub arm: Option<wire::ArmState>,
    pub session: Option<(String, u64)>,
    // Asked for but not yet confirmed: the vehicle refuses an arm while another session is live.
    pending: Option<(String, u64)>,
    pub last_result: Option<String>,
    pub note: String,
    pub sent: u64,
    // Sums over the frames since the screen last read them: one-way delay minus its windowed
    // minimum (valid across hosts), and the raw one-way delay (valid on one host only).
    pub delay_sum: (f64, f64, u64),
    /// Vehicle processes whose liveliness token the router shows: <prefix>/alive/<name>.
    pub alive: BTreeSet<String>,
    pub last_sent: (f64, f64, bool),
    /// Round trips to the server: the vehicle's (from its hop key) and this client's, with when
    /// each was measured.
    pub vehicle_rtt: Option<(Duration, Instant)>,
    pub operator_rtt: Option<(Duration, Instant)>,
    pub mux: Option<wire::MuxStatus>,
    pub estop_latched: Option<bool>,
}

// The first frame after joining needs a keyframe too: the encoder's GOP never ends.
struct Gaps {
    next: Option<u64>,
    awaiting: bool,
    last_request: Option<Instant>,
}

impl Gaps {
    fn arrival(&mut self, stream_seq: u64, keyframe: bool) -> (u64, bool) {
        let lost = match self.next {
            Some(n) if stream_seq > n => stream_seq - n,
            _ => 0,
        };
        self.next = Some(stream_seq + 1);
        if keyframe {
            self.awaiting = false;
            return (lost, false);
        }
        if lost > 0 && !self.awaiting {
            self.awaiting = true;
            self.last_request = None;
        }
        let request = self.awaiting && self.last_request.is_none_or(|t| t.elapsed() >= KEYFRAME_REPEAT);
        if request {
            self.last_request = Some(Instant::now());
        }
        (lost, request)
    }
}

/// One entry of fleetd's vehicle list.
#[derive(Clone, Debug, PartialEq)]
pub struct Vehicle {
    pub id: String,
    pub prefix: String,
    pub model: String,
    pub alive: Vec<String>,
}

impl Vehicle {
    fn from_json(v: &serde_json::Value) -> Option<Self> {
        Some(Vehicle {
            id: v.get("id")?.as_str()?.to_string(),
            prefix: v.get("prefix")?.as_str()?.to_string(),
            model: v.get("model").and_then(|m| m.as_str()).unwrap_or("").to_string(),
            alive: v
                .get("alive")
                .and_then(|a| a.as_array())
                .map(|a| a.iter().filter_map(|n| n.as_str().map(String::from)).collect())
                .unwrap_or_default(),
        })
    }
}

pub struct Link {
    session: Session,
    prefix: String,
    robot: String,
    pub status: Arc<Mutex<Status>>,
    sequence: Mutex<u64>,
    _subscribers: Vec<zenoh::pubsub::Subscriber<()>>,
    _alive: zenoh::liveliness::LivelinessToken,
}

fn control_put(session: &Session, key: String, payload: Vec<u8>) {
    let _ = session
        .put(key, payload)
        .congestion_control(CongestionControl::Drop)
        .priority(Priority::InteractiveHigh)
        .express(true)
        .wait();
}

// Motion commands repeat every 20 ms and the vehicle drops late ones by sequence, so a lost one is
// replaced by the next: best effort takes the UDP link and never waits for a retransmission.
fn motion_put(session: &Session, key: String, payload: Vec<u8>) {
    let _ = session
        .put(key, payload)
        .congestion_control(CongestionControl::Drop)
        .priority(Priority::InteractiveHigh)
        .reliability(Reliability::BestEffort)
        .express(true)
        .wait();
}

fn config(connect: &[String]) -> anyhow::Result<zenoh::Config> {
    let mut c = zenoh::Config::default();
    let set = |c: &mut zenoh::Config, k: &str, v: String| c.insert_json5(k, &v).map_err(|e| anyhow::anyhow!("{k}: {e}"));
    // A router opens every endpoint, so `tcp/...?rel=1` and `udp/...?rel=0` form one transport:
    // reliable messages take TCP and best-effort ones (video, motion) UDP. A client would use
    // only the first endpoint that connects.
    set(&mut c, "mode", "\"router\"".into())?;
    set(&mut c, "connect/endpoints", format!("{connect:?}"))?;
    set(&mut c, "listen/endpoints", "[]".into())?;
    set(&mut c, "transport/unicast/max_links", "4".into())?;
    // One UDP datagram per packet: a fragmented datagram is lost with any of its fragments.
    set(&mut c, "transport/link/tx/batch_size", "1400".into())?;
    set(&mut c, "scouting/multicast/enabled", "false".into())?;
    set(&mut c, "scouting/gossip/enabled", "false".into())?;
    // As tierlink's make_config: a silent link counts as lost after one second.
    set(&mut c, "transport/link/tx/lease", "1000".into())?;
    set(&mut c, "transport/link/tx/keep_alive", "4".into())?;
    Ok(c)
}

impl Link {
    pub fn open(connect: &[String], prefix: &str, robot: &str, decoder: Arc<Decoder>, ack_monitor: bool) -> anyhow::Result<Self> {
        let session = zenoh::open(config(connect)?).wait().map_err(|e| anyhow::anyhow!("{e}"))?;
        let status = Arc::new(Mutex::new(Status::default()));
        let start = Instant::now();
        let mut subs = Vec::new();

        let (s, st, p) = (session.clone(), status.clone(), prefix.to_string());
        let gaps = Mutex::new(Gaps { next: None, awaiting: true, last_request: None });
        let video_pieces = Mutex::new(wire::Reassembler::default());
        let window: Mutex<VecDeque<(Instant, i64)>> = Mutex::new(VecDeque::new());
        subs.push(
            session
                .declare_subscriber(format!("{prefix}/teleop/data/{VIDEO_STREAM}"))
                .callback(move |sample: Sample| {
                    let Some(h) = sample.attachment().and_then(|a| wire::decode_header(&a.to_bytes())) else { return };
                    control_put(&s, format!("{p}/teleop/ack"), wire::encode_ack(h.seq, mono_ns(start)));
                    let Some(bytes) = video_pieces.lock().unwrap().add(&h, sample.payload().to_bytes().to_vec()) else { return };
                    let (lost, request) = gaps.lock().unwrap().arrival(h.stream_seq, h.keyframe);
                    if request {
                        control_put(&s, format!("{p}/teleop/keyframe"), VIDEO_STREAM.as_bytes().to_vec());
                    }
                    let raw = monotonic_raw_ns() - h.sent_ns;
                    let queue = {
                        let mut w = window.lock().unwrap();
                        w.push_back((Instant::now(), raw));
                        while w.front().is_some_and(|(t, _)| t.elapsed() > DELAY_WINDOW) {
                            w.pop_front();
                        }
                        raw - w.iter().map(|(_, d)| *d).min().unwrap_or(raw)
                    };
                    {
                        let mut st = st.lock().unwrap();
                        st.video_frames += 1;
                        st.video_bytes += bytes.len() as u64;
                        st.video_lost += lost;
                        st.keyframes += h.keyframe as u64;
                        st.video_last = Some(Instant::now());
                        st.delay_sum.0 += queue as f64 / 1e6;
                        st.delay_sum.1 += raw as f64 / 1e6;
                        st.delay_sum.2 += 1;
                    }
                    decoder.push(&bytes);
                })
                .wait()
                .map_err(|e| anyhow::anyhow!("{e}"))?,
        );

        let (s, st, p) = (session.clone(), status.clone(), prefix.to_string());
        let monitor_pieces: Mutex<BTreeMap<String, wire::Reassembler>> = Mutex::new(BTreeMap::new());
        subs.push(
            session
                .declare_subscriber(format!("{prefix}/monitor/data/*"))
                .callback(move |sample: Sample| {
                    let Some(h) = sample.attachment().and_then(|a| wire::decode_header(&a.to_bytes())) else { return };
                    if ack_monitor {
                        control_put(&s, format!("{p}/monitor/ack"), wire::encode_ack(h.seq, mono_ns(start)));
                    }
                    let name = sample.key_expr().as_str().rsplit('/').next().unwrap_or("").to_string();
                    let piece = sample.payload().to_bytes().to_vec();
                    let Some(raw) = monitor_pieces.lock().unwrap().entry(name.clone()).or_default().add(&h, piece) else { return };
                    let cdr = zstd::decode_all(&raw[..]).ok();
                    let mut st = st.lock().unwrap();
                    let entry = st.streams.entry(name).or_default();
                    entry.count += 1;
                    entry.bytes += raw.len() as u64;
                    entry.last = Some(Instant::now());
                    let msg_type = entry.msg_type.clone();
                    if let Some(cdr) = cdr {
                        match msg_type.as_str() {
                            "nav_msgs/msg/Odometry" => st.speed_mps = wire::odometry_speed(&cdr).or(st.speed_mps),
                            "sensor_msgs/msg/NavSatFix" => st.gps = wire::navsatfix(&cdr).or(st.gps),
                            "hunter_mux_msgs/msg/MuxStatus" => st.mux = wire::mux_status(&cdr).or(st.mux),
                            "hunter_mux_msgs/msg/EStopStatus" => st.estop_latched = wire::estop_latched(&cdr).or(st.estop_latched),
                            _ => {}
                        }
                    }
                })
                .wait()
                .map_err(|e| anyhow::anyhow!("{e}"))?,
        );

        let st = status.clone();
        subs.push(
            session
                .declare_subscriber(format!("{prefix}/monitor/ctl/catalog"))
                .callback(move |sample: Sample| {
                    let mut st = st.lock().unwrap();
                    for e in wire::decode_catalog(&sample.payload().to_bytes()) {
                        st.streams.entry(e.name).or_default().msg_type = e.msg_type;
                    }
                })
                .wait()
                .map_err(|e| anyhow::anyhow!("{e}"))?,
        );

        let st = status.clone();
        subs.push(
            session
                .declare_subscriber(format!("{prefix}/monitor/ctl/result"))
                .callback(move |sample: Sample| {
                    if let Some((id, result)) = wire::decode_result(&sample.payload().to_bytes()) {
                        st.lock().unwrap().last_result = Some(format!("{id}: {result}"));
                    }
                })
                .wait()
                .map_err(|e| anyhow::anyhow!("{e}"))?,
        );

        let st = status.clone();
        subs.push(
            session
                .declare_subscriber(format!("{prefix}/teleop/ctl/state"))
                .callback(move |sample: Sample| {
                    if let Some(state) = wire::decode_state(&sample.payload().to_bytes()) {
                        let mut st = st.lock().unwrap();
                        if state.armed && st.pending.as_ref().is_some_and(|p| p.1 == state.epoch) {
                            st.session = st.pending.take();
                        }
                        st.arm = Some(state);
                    }
                })
                .wait()
                .map_err(|e| anyhow::anyhow!("{e}"))?,
        );

        let st = status.clone();
        subs.push(
            session
                .declare_subscriber(format!("{prefix}/teleop/ctl/hop"))
                .callback(move |sample: Sample| {
                    if let Some(ns) = wire::decode_hop(&sample.payload().to_bytes()) {
                        st.lock().unwrap().vehicle_rtt = Some((Duration::from_nanos(ns), Instant::now()));
                    }
                })
                .wait()
                .map_err(|e| anyhow::anyhow!("{e}"))?,
        );

        let st = status.clone();
        subs.push(
            session
                .liveliness()
                .declare_subscriber(format!("{prefix}/alive/*"))
                .history(true)
                .callback(move |sample: Sample| {
                    let name = sample.key_expr().as_str().rsplit('/').next().unwrap_or("").to_string();
                    let mut st = st.lock().unwrap();
                    if sample.kind() == SampleKind::Put {
                        st.alive.insert(name);
                    } else {
                        st.alive.remove(&name);
                    }
                })
                .wait()
                .map_err(|e| anyhow::anyhow!("{e}"))?,
        );
        let alive = session.liveliness().declare_token(format!("{prefix}/alive/operator")).wait().map_err(|e| anyhow::anyhow!("{e}"))?;

        Ok(Link {
            session,
            prefix: prefix.into(),
            robot: robot.into(),
            status,
            sequence: Mutex::new(0),
            _subscribers: subs,
            _alive: alive,
        })
    }

    /// Takes a new session from the vehicle and arms it. Blocks for up to a second.
    pub fn arm(&self) {
        {
            let st = self.status.lock().unwrap();
            let ours = st.arm.as_ref().is_some_and(|a| a.armed && st.session.as_ref().is_some_and(|s| s.1 == a.epoch));
            if ours {
                return;
            }
        }
        let replies = match self.session.get(format!("{}/teleop/session", self.prefix)).timeout(Duration::from_secs(1)).wait() {
            Ok(r) => r,
            Err(e) => return self.note(format!("session request failed: {e}")),
        };
        let grant = replies.iter().find_map(|r| {
            let text = r.result().ok()?.payload().try_to_string().ok()?.into_owned();
            let (id, epoch) = text.split_once(':')?;
            Some((id.to_string(), epoch.parse::<u64>().ok()?))
        });
        let Some((id, epoch)) = grant else { return self.note("no session from the vehicle".into()) };
        *self.sequence.lock().unwrap() = 0;
        let arm = wire::encode_arm(&self.robot, &id, epoch);
        let mut st = self.status.lock().unwrap();
        st.pending = Some((id, epoch));
        st.note.clear();
        drop(st);
        control_put(&self.session, self.key("teleop", "arm"), arm);
    }

    pub fn stop(&self) {
        let (id, epoch) = self.status.lock().unwrap().session.clone().unwrap_or_default();
        control_put(&self.session, self.key("teleop", "stop"), wire::encode_stop(&self.robot, &id, epoch));
    }

    /// Latches the vehicle's e-stop through teleop_control; honored from any session.
    pub fn estop(&self) {
        let (id, epoch) = self.status.lock().unwrap().session.clone().unwrap_or_default();
        control_put(&self.session, self.key("teleop", "estop"), wire::encode_stop(&self.robot, &id, epoch));
    }

    /// Asks hunter_mux to release the e-stop; it stays latched while a cause is still active.
    pub fn estop_reset(&self) {
        let (id, epoch) = self.status.lock().unwrap().session.clone().unwrap_or_default();
        control_put(&self.session, self.key("teleop", "estop_reset"), wire::encode_stop(&self.robot, &id, epoch));
    }

    /// Times this client's hop to the server (`<prefix>/server/ping`). Blocks for up to a second.
    pub fn ping_server(&self) {
        let start = Instant::now();
        let Ok(replies) = self.session.get(format!("{}/server/ping", self.prefix)).timeout(Duration::from_secs(1)).wait() else { return };
        if replies.recv().is_ok_and(|r| r.result().is_ok()) {
            self.status.lock().unwrap().operator_rtt = Some((start.elapsed(), Instant::now()));
        }
    }

    pub fn motion(&self, linear_mps: f64, angular_rps: f64, deadman: bool) {
        let Some((id, epoch)) = self.status.lock().unwrap().session.clone() else { return };
        let sequence = {
            let mut s = self.sequence.lock().unwrap();
            *s += 1;
            *s
        };
        let m = wire::Motion {
            robot: &self.robot,
            session: &id,
            epoch,
            sequence,
            sent_unix_ns: unix_ns(),
            valid_for_ns: 200_000_000,
            linear_mps,
            angular_rps,
            deadman,
        };
        motion_put(&self.session, self.key("teleop", "motion"), wire::encode_motion(&m));
        let mut st = self.status.lock().unwrap();
        st.sent += 1;
        st.last_sent = (linear_mps, angular_rps, deadman);
    }

    /// A command registered in the vehicle's streams.yaml, with a CDR payload.
    pub fn command(&self, interface: &str, payload: &[u8]) {
        let now_ms = unix_ns() / 1_000_000;
        let id = format!("op-{now_ms}");
        control_put(&self.session, self.key("monitor", "command"), wire::encode_command(&id, interface, now_ms, COMMAND_TTL_MS, payload));
        self.status.lock().unwrap().last_result = Some(format!("{id}: sent"));
    }

    /// fleetd's list of vehicles (`fleet/vehicles`). Blocks for up to a second.
    pub fn vehicles(&self) -> anyhow::Result<Vec<Vehicle>> {
        let replies = self.session.get("fleet/vehicles").timeout(Duration::from_secs(1)).wait().map_err(|e| anyhow::anyhow!("{e}"))?;
        let reply = replies.recv().map_err(|_| anyhow::anyhow!("no reply from fleetd"))?;
        let sample = reply.result().map_err(|_| anyhow::anyhow!("fleetd refused the query"))?;
        let body: serde_json::Value = serde_json::from_slice(&sample.payload().to_bytes())?;
        Ok(body
            .as_array()
            .map(|a| a.iter().filter_map(Vehicle::from_json).collect())
            .unwrap_or_default())
    }

    /// The messages fleetd kept of one of this vehicle's streams over the last `seconds`
    /// (`<prefix>/history/<stream>`): receive time (Unix ns) and the message. Blocks for up to
    /// two seconds.
    pub fn history(&self, stream: &str, seconds: u64) -> anyhow::Result<Vec<(u64, Vec<u8>)>> {
        let key = format!("{}/history/{stream}?seconds={seconds}", self.prefix);
        let replies = self.session.get(key).timeout(Duration::from_secs(2)).wait().map_err(|e| anyhow::anyhow!("{e}"))?;
        let reply = replies.recv().map_err(|_| anyhow::anyhow!("no reply from fleetd"))?;
        let sample = reply.result().map_err(|_| anyhow::anyhow!("fleetd refused the query"))?;
        Ok(wire::decode_history(&sample.payload().to_bytes()))
    }

    pub fn prefix(&self) -> &str {
        &self.prefix
    }

    /// Whether this client has a link to a router (the server) right now.
    pub fn server_connected(&self) -> bool {
        self.session.info().routers_zid().wait().next().is_some()
    }

    fn key(&self, tier: &str, name: &str) -> String {
        format!("{}/{tier}/ctl/{name}", self.prefix)
    }

    fn note(&self, text: String) {
        self.status.lock().unwrap().note = text;
    }
}
