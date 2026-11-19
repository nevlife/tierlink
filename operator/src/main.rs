//! Operator prototype: one window with the front camera, the vehicle's state, the drive input
//! and the commands, fleetd's vehicle list to pick the vehicle, and its recent history.
//!
//! teleop_operator_gui [--connect tcp/<server>:<port>?rel=1,udp/<server>:<port>?rel=0] [--prefix scv/r01]
//!     [--robot r01] [--ack-monitor]
//!
//! The server's fleetd acks the monitor tier; --ack-monitor is for a link without fleetd.

mod input;
mod link;
mod video;
mod wire;

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use eframe::egui;

const MOTION_PERIOD: Duration = Duration::from_millis(20);
// The window stopped updating the input: send a released dead-man instead of the last command.
const INPUT_STALE: Duration = Duration::from_millis(100);
// std_msgs/msg/Empty in CDR, for the vehicle's registered stop command.
const EMPTY_CDR: [u8; 5] = [0, 1, 0, 0, 0];

struct Args {
    connect: Vec<String>,
    prefix: String,
    robot: String,
    ack_monitor: bool,
}

fn args() -> Args {
    let mut a = Args { connect: vec!["tcp/127.0.0.1:7447".into()], prefix: "scv/r01".into(), robot: "r01".into(), ack_monitor: false };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        match k.as_str() {
            "--connect" => a.connect = it.next().unwrap_or_default().split(',').map(String::from).collect(),
            "--prefix" => a.prefix = it.next().unwrap_or_default(),
            "--robot" => a.robot = it.next().unwrap_or_default(),
            "--ack-monitor" => a.ack_monitor = true,
            other => eprintln!("unknown argument {other}"),
        }
    }
    a
}

struct Rates {
    at: Instant,
    video: (u64, u64, u64),
    streams: Vec<(String, u64)>,
    video_fps: f64,
    video_kbps: f64,
    decoded_fps: f64,
    queue_ms: f64,
    one_way_ms: f64,
    decode_ms: f64,
    server: bool,
    stream_hz: std::collections::BTreeMap<String, f64>,
}

type Shared<T> = Arc<Mutex<T>>;

/// A history query to fleetd, answered on a thread because it blocks.
struct History {
    open: bool,
    stream: String,
    seconds: u64,
    result: Shared<Option<Result<Vec<(u64, Vec<u8>)>, String>>>,
}

struct App {
    link: Arc<link::Link>,
    // The link the motion and ping threads use; replaced when another vehicle is picked.
    current: Shared<Arc<link::Link>>,
    connect: Vec<String>,
    ack_monitor: bool,
    vehicles: Shared<Result<Vec<link::Vehicle>, String>>,
    history: History,
    decoder: Arc<video::Decoder>,
    input: input::Input,
    drive: input::Drive,
    texture: Option<egui::TextureHandle>,
    last_poll: Instant,
    shared: Arc<Mutex<(input::Drive, Instant)>>,
    rates: Rates,
}

const VEHICLE_NODES: [&str; 3] = ["teleop_control", "teleop_video", "fleet_telemetry"];
const VIDEO_STALE: Duration = Duration::from_secs(1);
// Driving blind is not allowed: with no new frame for this long the dead-man counts as released.
const VIDEO_DEADMAN: Duration = Duration::from_millis(500);
// A round trip older than this is not shown: that hop's pings stopped coming back.
const RTT_STALE: Duration = Duration::from_secs(3);
const MODE_NAV: i8 = 1;
const MODE_REMOTE: i8 = 2;

/// Which link broke, from what this client can see. The server comes first: without it the
/// vehicle's tokens can no longer be observed.
fn diagnose(server: bool, st: &link::Status) -> (&'static str, egui::Color32) {
    let (red, orange, green) =
        (egui::Color32::from_rgb(210, 60, 60), egui::Color32::from_rgb(220, 140, 40), egui::Color32::from_rgb(40, 170, 80));
    if !server {
        return ("SERVER LOST: vehicle state unknown", red);
    }
    if !VEHICLE_NODES.iter().any(|n| st.alive.contains(*n)) {
        return ("VEHICLE LOST: server is up", red);
    }
    if !st.alive.contains("teleop_video") || st.video_last.is_none_or(|t| t.elapsed() > VIDEO_STALE) {
        return ("vehicle connected, video stopped", orange);
    }
    if !st.alive.contains("teleop_control") {
        return ("vehicle connected, control node stopped", orange);
    }
    ("link OK", green)
}

fn armed(st: &link::Status) -> bool {
    st.arm.as_ref().is_some_and(|a| a.armed && st.session.as_ref().is_some_and(|s| s.1 == a.epoch))
}

fn video_fresh(st: &link::Status) -> bool {
    st.video_last.is_some_and(|t| t.elapsed() < VIDEO_DEADMAN)
}

fn hop_ms(rtt: Option<(Duration, Instant)>) -> String {
    match rtt {
        Some((d, at)) if at.elapsed() < RTT_STALE => format!("{:.1} ms", d.as_secs_f64() * 1000.0 / 2.0),
        _ => "-".into(),
    }
}

fn mux_text(st: &link::Status) -> String {
    let estop = match st.estop_latched {
        Some(true) => "E-STOP LATCHED",
        Some(false) => "e-stop clear",
        None => "e-stop unknown",
    };
    let Some(m) = st.mux else { return format!("{estop}, mux unknown") };
    let mode = match m.mode {
        MODE_NAV => "navigation",
        MODE_REMOTE => "remote",
        _ => "idle",
    };
    let source = match m.source {
        0 => "navigation",
        1 => "teleop",
        _ => "none",
    };
    format!("{estop}, mode {mode}, driving {source}{}", if m.remote_allowed { "" } else { ", remote not allowed" })
}

/// What fleetd kept: how much, over how long, and for odometry the speed over time.
fn history_view(ui: &mut egui::Ui, messages: &[(u64, Vec<u8>)], msg_type: &str) {
    let (Some(first), Some(last)) = (messages.first(), messages.last()) else {
        ui.label("nothing stored in that window");
        return;
    };
    let bytes: usize = messages.iter().map(|(_, m)| m.len()).sum();
    let span = (last.0 - first.0) as f64 / 1e9;
    ui.label(format!("{} messages over {span:.0} s, {:.1} kB, last {:.0} s ago", messages.len(), bytes as f64 / 1e3, (link::unix_ns().saturating_sub(last.0)) as f64 / 1e9));
    if msg_type != "nav_msgs/msg/Odometry" {
        return;
    }
    let speeds: Vec<(f64, f64)> =
        messages.iter().filter_map(|(t, m)| Some(((t - first.0) as f64 / 1e9, wire::odometry_speed(m)?))).collect();
    let top = speeds.iter().map(|s| s.1.abs()).fold(0.5, f64::max);
    let (rect, _) = ui.allocate_exact_size(egui::vec2(ui.available_width().max(300.0), 120.0), egui::Sense::hover());
    let painter = ui.painter_at(rect);
    painter.rect_stroke(rect, 0.0, egui::Stroke::new(1.0, egui::Color32::GRAY), egui::StrokeKind::Inside);
    let at = |(t, v): (f64, f64)| {
        egui::pos2(
            rect.left() + (t / span.max(1e-9)) as f32 * rect.width(),
            rect.center().y - (v / top) as f32 * rect.height() / 2.0,
        )
    };
    painter.add(egui::Shape::line(speeds.iter().copied().map(at).collect(), egui::Stroke::new(1.5, egui::Color32::LIGHT_BLUE)));
    ui.label(format!("speed, full scale {top:.1} m/s"));
}

fn age(t: Option<Instant>) -> String {
    match t {
        Some(t) => format!("{:.1} s", t.elapsed().as_secs_f64()),
        None => "never".into(),
    }
}

impl App {
    /// Drives another vehicle. Refused while armed: the old vehicle would keep its session until
    /// its watchdog.
    fn switch(&mut self, v: &link::Vehicle) {
        if armed(&self.link.status.lock().unwrap()) {
            self.link.status.lock().unwrap().note = "stop before picking another vehicle".into();
            return;
        }
        match link::Link::open(&self.connect, &v.prefix, &v.id, self.decoder.clone(), self.ack_monitor) {
            Ok(l) => {
                let l = Arc::new(l);
                *self.current.lock().unwrap() = l.clone();
                self.link = l;
                self.texture = None;
                self.history.result = Arc::new(Mutex::new(None));
            }
            Err(e) => self.link.status.lock().unwrap().note = format!("cannot open {}: {e}", v.id),
        }
    }

    fn fetch_history(&mut self) {
        let (l, stream, seconds) = (self.link.clone(), self.history.stream.clone(), self.history.seconds);
        let result = self.history.result.clone();
        *result.lock().unwrap() = None;
        std::thread::spawn(move || {
            *result.lock().unwrap() = Some(l.history(&stream, seconds).map_err(|e| e.to_string()));
        });
    }

    fn fleet(&mut self, ui: &mut egui::Ui) {
        ui.heading("Fleet");
        let list = self.vehicles.lock().unwrap().clone();
        let mut pick = None;
        match list {
            Err(e) => {
                ui.label(e);
            }
            Ok(list) => {
                egui::Grid::new("fleet").num_columns(3).show(ui, |ui| {
                    for v in &list {
                        let here = v.prefix == self.link.prefix();
                        ui.label(if here { egui::RichText::new(&v.id).strong() } else { egui::RichText::new(&v.id) });
                        ui.label(format!("{} {}", v.model, if v.alive.is_empty() { "offline".into() } else { v.alive.join(", ") }));
                        if !here && ui.button("drive").clicked() {
                            pick = Some(v.clone());
                        }
                        ui.end_row();
                    }
                });
            }
        }
        if let Some(v) = pick {
            self.switch(&v);
        }
    }

    fn history_window(&mut self, ctx: &egui::Context) {
        let mut open = self.history.open;
        let streams: Vec<(String, String)> =
            self.link.status.lock().unwrap().streams.iter().map(|(n, s)| (n.clone(), s.msg_type.clone())).collect();
        let mut fetch = false;
        egui::Window::new("History").open(&mut open).show(ctx, |ui| {
            ui.horizontal(|ui| {
                egui::ComboBox::from_label("stream").selected_text(self.history.stream.clone()).show_ui(ui, |ui| {
                    for (name, _) in &streams {
                        ui.selectable_value(&mut self.history.stream, name.clone(), name);
                    }
                });
                ui.add(egui::Slider::new(&mut self.history.seconds, 10..=600).text("s"));
                fetch = ui.add_enabled(!self.history.stream.is_empty(), egui::Button::new("fetch")).clicked();
            });
            let msg_type = streams.iter().find(|(n, _)| *n == self.history.stream).map(|(_, t)| t.clone()).unwrap_or_default();
            match &*self.history.result.lock().unwrap() {
                None => {
                    ui.label("-");
                }
                Some(Err(e)) => {
                    ui.label(e);
                }
                Some(Ok(m)) => history_view(ui, m, &msg_type),
            }
        });
        self.history.open = open;
        if fetch {
            self.fetch_history();
        }
    }

    fn update_rates(&mut self) {
        let dt = self.rates.at.elapsed().as_secs_f64();
        if dt < 1.0 {
            return;
        }
        self.rates.server = self.link.server_connected();
        let mut st = self.link.status.lock().unwrap();
        let (q, r, n) = std::mem::take(&mut st.delay_sum);
        if n > 0 {
            self.rates.queue_ms = q / n as f64;
            self.rates.one_way_ms = r / n as f64;
        }
        let (ds, dn) = std::mem::take(&mut *self.decoder.decode_sum.lock().unwrap());
        if dn > 0 {
            self.rates.decode_ms = ds / dn as f64;
        }
        let decoded = *self.decoder.decoded.lock().unwrap();
        let (f, b, d) = self.rates.video;
        self.rates.video_fps = (st.video_frames - f) as f64 / dt;
        self.rates.video_kbps = (st.video_bytes - b) as f64 * 8.0 / 1000.0 / dt;
        self.rates.decoded_fps = (decoded - d) as f64 / dt;
        self.rates.video = (st.video_frames, st.video_bytes, decoded);
        let before: std::collections::BTreeMap<_, _> = self.rates.streams.drain(..).collect();
        for (name, s) in &st.streams {
            let hz = (s.count - before.get(name).copied().unwrap_or(s.count)) as f64 / dt;
            self.rates.stream_hz.insert(name.clone(), hz);
            self.rates.streams.push((name.clone(), s.count));
        }
        self.rates.at = Instant::now();
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let keys = ctx.input(|i| input::Keys {
            forward: i.key_down(egui::Key::W),
            back: i.key_down(egui::Key::S),
            left: i.key_down(egui::Key::A),
            right: i.key_down(egui::Key::D),
            deadman: i.key_down(egui::Key::Space),
            focused: i.focused,
        });
        let dt = self.last_poll.elapsed().as_secs_f64();
        self.last_poll = Instant::now();
        self.drive = self.input.poll(&keys, dt);
        *self.shared.lock().unwrap() = (self.drive, Instant::now());
        self.update_rates();

        if let Some(f) = self.decoder.latest.lock().unwrap().take() {
            let image = egui::ColorImage::from_rgba_unmultiplied([f.width, f.height], &f.rgba);
            match &mut self.texture {
                Some(t) => t.set(image, egui::TextureOptions::LINEAR),
                None => self.texture = Some(ctx.load_texture("front_cam", image, egui::TextureOptions::LINEAR)),
            }
        }

        egui::TopBottomPanel::top("bar").show(ctx, |ui| {
            ui.horizontal(|ui| {
                let st = self.link.status.lock().unwrap();
                let (text, color) = match &st.arm {
                    Some(a) if a.armed && !a.reason.is_empty() => (format!("ARMED  epoch {}, {}", a.epoch, a.reason), egui::Color32::from_rgb(200, 170, 40)),
                    Some(a) if a.armed => (format!("ARMED  epoch {}", a.epoch), egui::Color32::from_rgb(40, 170, 80)),
                    Some(a) => (format!("DISARMED: {}", a.reason), egui::Color32::from_rgb(200, 120, 40)),
                    None => ("NO VEHICLE STATE".to_string(), egui::Color32::GRAY),
                };
                let note = st.note.clone();
                let (link_text, link_color) = diagnose(self.rates.server, &st);
                drop(st);
                if ui.button("Arm").clicked() {
                    let link = self.link.clone();
                    std::thread::spawn(move || link.arm());
                }
                if ui.button("Stop").clicked() {
                    self.link.stop();
                }
                ui.separator();
                if ui.button("Send command: stop").clicked() {
                    self.link.command("stop", &EMPTY_CDR);
                }
                ui.separator();
                let estop = egui::Button::new(egui::RichText::new("E-STOP").strong().color(egui::Color32::WHITE))
                    .fill(egui::Color32::from_rgb(190, 30, 30));
                if ui.add(estop).clicked() {
                    self.link.estop();
                }
                if ui.button("Reset e-stop").clicked() {
                    self.link.estop_reset();
                }
                if ui.button("Mode: remote").clicked() {
                    self.link.command("cmd_mode", &wire::cmd_mode_cdr(MODE_REMOTE));
                }
                if ui.button("Mode: navigation").clicked() {
                    self.link.command("cmd_mode", &wire::cmd_mode_cdr(MODE_NAV));
                }
                if ui.button("History").clicked() {
                    self.history.open = !self.history.open;
                }
                ui.separator();
                ui.colored_label(link_color, egui::RichText::new(link_text).strong());
                ui.separator();
                ui.colored_label(color, egui::RichText::new(text).strong());
                if !note.is_empty() {
                    ui.separator();
                    ui.label(note);
                }
            });
        });

        egui::SidePanel::right("vehicle").min_width(260.0).show(ctx, |ui| {
            let st = self.link.status.lock().unwrap();
            ui.heading("Vehicle");
            egui::Grid::new("state").num_columns(2).show(ui, |ui| {
                ui.label("speed");
                ui.label(st.speed_mps.map_or("-".into(), |v| format!("{v:.2} m/s")));
                ui.end_row();
                ui.label("position");
                ui.label(st.gps.map_or("-".into(), |(la, lo)| format!("{la:.6}, {lo:.6}")));
                ui.end_row();
                ui.label("session");
                ui.label(st.session.as_ref().map_or("-".into(), |(_, e)| format!("epoch {e}")));
                ui.end_row();
                ui.label("last command");
                ui.label(st.last_result.clone().unwrap_or("-".into()));
                ui.end_row();
                ui.label("mux");
                ui.label(mux_text(&st));
                ui.end_row();
            });
            ui.separator();
            ui.heading("Link");
            egui::Grid::new("link").num_columns(2).show(ui, |ui| {
                ui.label("server");
                ui.label(if self.rates.server { "connected" } else { "lost" });
                ui.end_row();
                ui.label("vehicle - server");
                ui.label(hop_ms(st.vehicle_rtt));
                ui.end_row();
                ui.label("server - operator");
                ui.label(hop_ms(st.operator_rtt));
                ui.end_row();
                for n in VEHICLE_NODES {
                    ui.label(n);
                    ui.label(if st.alive.contains(n) { "alive" } else { "gone" });
                    ui.end_row();
                }
            });
            ui.separator();
            ui.heading("Streams");
            egui::Grid::new("streams").num_columns(3).striped(true).show(ui, |ui| {
                for (name, s) in &st.streams {
                    ui.label(name);
                    ui.label(format!("{:.1} Hz", self.rates.stream_hz.get(name).copied().unwrap_or(0.0)));
                    ui.label(age(s.last));
                    ui.end_row();
                }
            });
            drop(st);
            ui.separator();
            self.fleet(ui);
        });
        self.history_window(ctx);

        egui::TopBottomPanel::bottom("drive").show(ctx, |ui| {
            ui.horizontal(|ui| {
                let d = self.drive;
                let source = if d.gamepad { "gamepad (hold RB)" } else { "keyboard (hold Space, W/S A/D)" };
                ui.label(format!("{source}   linear {:+.2} m/s   angular {:+.2} rad/s", d.linear_mps, d.angular_rps));
                let blind = d.deadman && !video_fresh(&self.link.status.lock().unwrap());
                ui.colored_label(
                    if blind {
                        egui::Color32::from_rgb(220, 140, 40)
                    } else if d.deadman {
                        egui::Color32::from_rgb(40, 170, 80)
                    } else {
                        egui::Color32::GRAY
                    },
                    if blind {
                        "deadman released: video stale"
                    } else if d.deadman {
                        "DEADMAN HELD"
                    } else {
                        "deadman released"
                    },
                );
                {
                    let st = self.link.status.lock().unwrap();
                    let (l, a, d) = st.last_sent;
                    ui.separator();
                    ui.label(format!("sent {} commands, last {l:+.2} m/s {a:+.2} rad/s{}", st.sent, if d { " dead-man" } else { "" }));
                }
                if let Some(name) = self.input.gamepad_name() {
                    ui.separator();
                    ui.label(name);
                }
            });
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            let st = self.link.status.lock().unwrap();
            ui.label(format!(
                "front_cam  {:.1} fps in, {:.1} fps decoded, {:.0} kbit/s, lost {}, keyframes {}, last {}",
                self.rates.video_fps, self.rates.decoded_fps, self.rates.video_kbps, st.video_lost, st.keyframes, age(st.video_last)
            ));
            let (vehicle_hop, operator_hop) = (hop_ms(st.vehicle_rtt), hop_ms(st.operator_rtt));
            drop(st);
            ui.label(format!(
                "delay  network queue +{:.1} ms (over the 10 s minimum)   one-way {:.1} ms (same host only)   decode {:.1} ms   hops (half round trip): vehicle-server {}, server-operator {}",
                self.rates.queue_ms,
                self.rates.one_way_ms,
                self.rates.decode_ms,
                vehicle_hop,
                operator_hop
            ));
            match &self.texture {
                Some(t) => {
                    let avail = ui.available_size();
                    let size = t.size_vec2();
                    let scale = (avail.x / size.x).min(avail.y / size.y);
                    ui.centered_and_justified(|ui| ui.image((t.id(), size * scale)));
                }
                None => {
                    ui.centered_and_justified(|ui| ui.label("waiting for video"));
                }
            }
        });

        ctx.request_repaint_after(Duration::from_millis(10));
    }
}

fn main() -> anyhow::Result<()> {
    let a = args();
    let decoder = Arc::new(video::Decoder::new()?);
    let link = Arc::new(link::Link::open(&a.connect, &a.prefix, &a.robot, decoder.clone(), a.ack_monitor)?);
    let current = Arc::new(Mutex::new(link.clone()));
    let shared = Arc::new(Mutex::new((input::Drive::default(), Instant::now())));
    let (c, sh) = (current.clone(), shared.clone());
    std::thread::spawn(move || loop {
        std::thread::sleep(MOTION_PERIOD);
        let l = c.lock().unwrap().clone();
        let (armed, seeing) = {
            let st = l.status.lock().unwrap();
            (armed(&st), video_fresh(&st))
        };
        if armed {
            let (d, at) = *sh.lock().unwrap();
            let fresh = at.elapsed() < INPUT_STALE;
            l.motion(d.linear_mps, d.angular_rps, d.deadman && fresh && seeing);
        }
    });
    let c = current.clone();
    std::thread::spawn(move || loop {
        let next = Instant::now() + Duration::from_secs(1);
        let l = c.lock().unwrap().clone();
        l.ping_server();
        std::thread::sleep(next.saturating_duration_since(Instant::now()));
    });
    let vehicles: Shared<Result<Vec<link::Vehicle>, String>> = Arc::new(Mutex::new(Err("asking fleetd".into())));
    let (c, v) = (current.clone(), vehicles.clone());
    std::thread::spawn(move || loop {
        let next = Instant::now() + Duration::from_secs(2);
        let l = c.lock().unwrap().clone();
        *v.lock().unwrap() = l.vehicles().map_err(|e| format!("vehicle list: {e}"));
        std::thread::sleep(next.saturating_duration_since(Instant::now()));
    });
    let app = App {
        link,
        current,
        connect: a.connect.clone(),
        ack_monitor: a.ack_monitor,
        vehicles,
        history: History { open: false, stream: String::new(), seconds: 60, result: Arc::new(Mutex::new(None)) },
        decoder,
        input: input::Input::new(),
        drive: input::Drive::default(),
        texture: None,
        last_poll: Instant::now(),
        shared: shared.clone(),
        rates: Rates {
            at: Instant::now(),
            video: (0, 0, 0),
            streams: Vec::new(),
            video_fps: 0.0,
            video_kbps: 0.0,
            decoded_fps: 0.0,
            queue_ms: 0.0,
            one_way_ms: 0.0,
            decode_ms: 0.0,
            server: false,
            stream_hz: Default::default(),
        },
    };
    eframe::run_native(
        &format!("teleop operator - {}", a.robot),
        eframe::NativeOptions { viewport: egui::ViewportBuilder::default().with_inner_size([1280.0, 760.0]), ..Default::default() },
        Box::new(|_| Ok(Box::new(app))),
    )
    .map_err(|e| anyhow::anyhow!("{e}"))
}
