//! fleetd: the server's receiver of the monitor tier. For each configured vehicle it acks the
//! monitor data (the vehicle's link controller paces on these acks), stores it per UTC hour as
//! MCAP, keeps the last minutes for history queries, and tracks the vehicle's liveliness tokens.
//! It connects as a client to the server's Zenoh router; teleop traffic only passes the router.
//!
//! fleetd <config.yaml>
//!
//! Queries:
//!   fleet/vehicles                         JSON list of vehicles with their live processes
//!   <prefix>/history/<stream>?seconds=N    one reply: per stored message its receive time (Unix
//!                                          ns, u64 LE), CDR length (u32 LE) and CDR

mod store;
mod wire;

use std::collections::{BTreeMap, BTreeSet, HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::Deserialize;
use zenoh::qos::{CongestionControl, Priority};
use zenoh::sample::{Sample, SampleKind};
use zenoh::{Session, Wait};

#[derive(Deserialize)]
struct Config {
    connect: Vec<String>,
    data_dir: PathBuf,
    history_seconds: u64,
    vehicles: Vec<VehicleConfig>,
}

#[derive(Deserialize, Clone)]
struct VehicleConfig {
    id: String,
    prefix: String,
    model: String,
}

#[derive(Default)]
struct Vehicle {
    // Stream name to (type, topic), from the vehicle's catalog.
    catalog: HashMap<String, (String, String)>,
    history: HashMap<String, VecDeque<(u64, Vec<u8>)>>,
    alive: BTreeSet<String>,
    pieces: HashMap<String, wire::Reassembler>,
    // Arrivals before the catalog named their stream: not stored, counted for the log.
    unknown: u64,
}

static STOP: AtomicBool = AtomicBool::new(false);

fn unix_ns() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos() as u64
}

fn zerr(e: impl std::fmt::Display) -> anyhow::Error {
    anyhow::anyhow!("{e}")
}

fn session(connect: &[String]) -> anyhow::Result<Session> {
    let mut c = zenoh::Config::default();
    let set = |c: &mut zenoh::Config, k: &str, v: String| c.insert_json5(k, &v).map_err(|e| anyhow::anyhow!("{k}: {e}"));
    set(&mut c, "mode", "\"client\"".into())?;
    set(&mut c, "connect/endpoints", serde_json::to_string(connect)?)?;
    set(&mut c, "scouting/multicast/enabled", "false".into())?;
    zenoh::open(c).wait().map_err(zerr)
}

struct Subscriptions {
    _subscribers: Vec<zenoh::pubsub::Subscriber<()>>,
    _queryables: Vec<zenoh::query::Queryable<()>>,
}

fn watch(
    session: &Session,
    v: &VehicleConfig,
    state: Arc<Mutex<Vehicle>>,
    store: Arc<Mutex<store::Store>>,
    history: Duration,
    start: Instant,
) -> anyhow::Result<Subscriptions> {
    let prefix = v.prefix.clone();
    let mut subs = Vec::new();

    let st = state.clone();
    subs.push(
        session
            .declare_subscriber(format!("{prefix}/monitor/ctl/catalog"))
            .callback(move |sample: Sample| {
                let mut st = st.lock().unwrap();
                for e in wire::decode_catalog(&sample.payload().to_bytes()) {
                    st.catalog.insert(e.name, (e.msg_type, e.topic));
                }
            })
            .wait()
            .map_err(zerr)?,
    );

    let (s, st, p) = (session.clone(), state.clone(), prefix.clone());
    subs.push(
        session
            .declare_subscriber(format!("{prefix}/monitor/data/*"))
            .callback(move |sample: Sample| {
                let Some(h) = sample.attachment().and_then(|a| wire::decode_header(&a.to_bytes())) else { return };
                let _ = s
                    .put(format!("{p}/monitor/ack"), wire::encode_ack(h.seq, start.elapsed().as_nanos() as u64))
                    .congestion_control(CongestionControl::Drop)
                    .priority(Priority::InteractiveHigh)
                    .express(true)
                    .wait();
                let name = sample.key_expr().as_str().rsplit('/').next().unwrap_or("").to_string();
                let mut st = st.lock().unwrap();
                let Some(item) = st.pieces.entry(name.clone()).or_default().add(&h, sample.payload().to_bytes().to_vec()) else { return };
                let Ok(cdr) = zstd::decode_all(&item[..]) else { return };
                let now = unix_ns();
                let Some((msg_type, topic)) = st.catalog.get(&name).cloned() else {
                    st.unknown += 1;
                    return;
                };
                if let Err(e) = store.lock().unwrap().write(&topic, &msg_type, now, &cdr) {
                    eprintln!("{p}: storing {name}: {e}");
                }
                let ring = st.history.entry(name).or_default();
                ring.push_back((now, cdr));
                let oldest = now.saturating_sub(history.as_nanos() as u64);
                while ring.front().is_some_and(|(t, _)| *t < oldest) {
                    ring.pop_front();
                }
            })
            .wait()
            .map_err(zerr)?,
    );

    let st = state.clone();
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
            .map_err(zerr)?,
    );

    let st = state.clone();
    let queryable = session
        .declare_queryable(format!("{prefix}/history/*"))
        .callback(move |query| {
            let stream = query.key_expr().as_str().rsplit('/').next().unwrap_or("").to_string();
            let seconds: u64 = query.parameters().get("seconds").and_then(|s| s.parse().ok()).unwrap_or(60);
            let since = unix_ns().saturating_sub(seconds * 1_000_000_000);
            let messages: Vec<(u64, Vec<u8>)> = st
                .lock()
                .unwrap()
                .history
                .get(&stream)
                .map(|r| r.iter().filter(|(t, _)| *t >= since).cloned().collect())
                .unwrap_or_default();
            let mut body = Vec::new();
            for (t, cdr) in messages {
                body.extend_from_slice(&t.to_le_bytes());
                body.extend_from_slice(&(cdr.len() as u32).to_le_bytes());
                body.extend_from_slice(&cdr);
            }
            let _ = query.reply(query.key_expr().clone(), body).wait();
        })
        .wait()
        .map_err(zerr)?;
    Ok(Subscriptions { _subscribers: subs, _queryables: vec![queryable] })
}

fn main() -> anyhow::Result<()> {
    let path = std::env::args().nth(1).ok_or_else(|| anyhow::anyhow!("usage: fleetd <config.yaml>"))?;
    let config: Config = serde_yaml::from_str(&std::fs::read_to_string(&path)?)?;
    let session = session(&config.connect)?;
    let start = Instant::now();
    let history = Duration::from_secs(config.history_seconds);

    let mut states = BTreeMap::new();
    let mut stores = Vec::new();
    let mut watches = Vec::new();
    for v in &config.vehicles {
        let state = Arc::new(Mutex::new(Vehicle::default()));
        let store = Arc::new(Mutex::new(store::Store::new(config.data_dir.join(&v.id))));
        watches.push(watch(&session, v, state.clone(), store.clone(), history, start)?);
        states.insert(v.id.clone(), (v.clone(), state));
        stores.push(store);
    }

    let list = states.clone();
    let _vehicles = session
        .declare_queryable("fleet/vehicles")
        .callback(move |query| {
            let body: Vec<serde_json::Value> = list
                .values()
                .map(|(v, st)| {
                    let st = st.lock().unwrap();
                    serde_json::json!({"id": v.id, "prefix": v.prefix, "model": v.model, "alive": st.alive})
                })
                .collect();
            let _ = query.reply("fleet/vehicles", serde_json::to_vec(&body).unwrap()).wait();
        })
        .wait()
        .map_err(zerr)?;

    println!("fleetd: {} vehicles, data in {}", config.vehicles.len(), config.data_dir.display());
    // The MCAP index is written on finish, so a stop signal ends the loop instead of the process.
    extern "C" fn on_signal(_: libc::c_int) {
        STOP.store(true, Ordering::Relaxed);
    }
    unsafe {
        libc::signal(libc::SIGINT, on_signal as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_signal as *const () as libc::sighandler_t);
    }
    while !STOP.load(Ordering::Relaxed) {
        std::thread::sleep(Duration::from_millis(200));
    }
    drop(watches);
    for s in stores {
        s.lock().unwrap().finish()?;
    }
    for (id, (_, st)) in &states {
        let st = st.lock().unwrap();
        if st.unknown > 0 {
            eprintln!("{id}: {} messages arrived before the catalog named their stream", st.unknown);
        }
    }
    Ok(())
}
