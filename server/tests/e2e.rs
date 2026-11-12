//! A router, fleetd and a fake vehicle on localhost: the vehicle's catalog and monitor data come
//! back as acks, history replies and an MCAP file, and its token shows in fleet/vehicles.

use std::net::TcpListener;
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use zenoh::Wait;

fn open(mode: &str, endpoint: &str) -> zenoh::Session {
    let mut c = zenoh::Config::default();
    c.insert_json5("mode", &format!("\"{mode}\"")).unwrap();
    let key = if mode == "router" { "listen/endpoints" } else { "connect/endpoints" };
    c.insert_json5(key, &format!("[\"{endpoint}\"]")).unwrap();
    c.insert_json5("scouting/multicast/enabled", "false").unwrap();
    zenoh::open(c).wait().unwrap()
}

fn header(seq: u64) -> Vec<u8> {
    let mut h = seq.to_le_bytes().to_vec();
    h.extend_from_slice(&seq.to_le_bytes());
    h.extend_from_slice(&0u64.to_le_bytes());
    h.push(0);
    h
}

#[test]
fn stores_acks_and_answers() {
    let port = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
    let endpoint = format!("tcp/127.0.0.1:{port}");
    let _router = open("router", &endpoint);
    let dir = std::env::temp_dir().join(format!("fleetd-e2e-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let config = dir.join("fleetd.yaml");
    std::fs::write(
        &config,
        format!(
            "connect: [\"{endpoint}\"]\ndata_dir: {}\nhistory_seconds: 60\nvehicles:\n  - {{ id: r01, prefix: t/r01, model: hunter }}\n",
            dir.join("data").display()
        ),
    )
    .unwrap();
    let mut fleetd = Command::new(env!("CARGO_BIN_EXE_fleetd")).arg(&config).spawn().unwrap();
    struct KillOnPanic(u32);
    impl Drop for KillOnPanic {
        fn drop(&mut self) {
            if std::thread::panicking() {
                unsafe { libc::kill(self.0 as i32, libc::SIGKILL) };
            }
        }
    }
    let _guard = KillOnPanic(fleetd.id());
    std::thread::sleep(Duration::from_secs(1));

    let vehicle = open("client", &endpoint);
    let _alive = vehicle.liveliness().declare_token("t/r01/alive/fleet_telemetry").wait().unwrap();
    let acks = Arc::new(Mutex::new(Vec::new()));
    let a = acks.clone();
    let _ack_sub = vehicle
        .declare_subscriber("t/r01/monitor/ack")
        .callback(move |s| a.lock().unwrap().push(u64::from_le_bytes(s.payload().to_bytes()[..8].try_into().unwrap())))
        .wait()
        .unwrap();
    std::thread::sleep(Duration::from_millis(500));
    vehicle.put("t/r01/monitor/ctl/catalog", "odom\tnav_msgs/msg/Odometry\t/odom\tlatest\n").wait().unwrap();
    std::thread::sleep(Duration::from_millis(300));
    for seq in 1..=3u64 {
        let cdr = vec![0u8, 1, 0, 0, seq as u8];
        vehicle
            .put("t/r01/monitor/data/odom", zstd::encode_all(&cdr[..], 3).unwrap())
            .attachment(header(seq))
            .wait()
            .unwrap();
    }
    std::thread::sleep(Duration::from_millis(500));
    assert_eq!(*acks.lock().unwrap(), vec![1, 2, 3]);

    let reply = vehicle.get("t/r01/history/odom?seconds=60").wait().unwrap().recv().unwrap();
    let body = reply.result().unwrap().payload().to_bytes().to_vec();
    let mut messages = Vec::new();
    let mut at = 0;
    while at < body.len() {
        let n = u32::from_le_bytes(body[at + 8..at + 12].try_into().unwrap()) as usize;
        messages.push(body[at + 12..at + 12 + n].to_vec());
        at += 12 + n;
    }
    assert_eq!(messages, vec![vec![0, 1, 0, 0, 1], vec![0, 1, 0, 0, 2], vec![0, 1, 0, 0, 3]]);

    let list = vehicle.get("fleet/vehicles").wait().unwrap().recv().unwrap();
    let list: serde_json::Value = serde_json::from_slice(&list.result().unwrap().payload().to_bytes()).unwrap();
    assert_eq!(list[0]["id"], "r01");
    assert_eq!(list[0]["alive"][0], "fleet_telemetry");

    unsafe { libc::kill(fleetd.id() as i32, libc::SIGTERM) };
    assert!(fleetd.wait().unwrap().success());
    let files: Vec<_> = std::fs::read_dir(dir.join("data/r01")).unwrap().map(|e| e.unwrap().path()).collect();
    assert_eq!(files.len(), 1);
    let bytes = std::fs::read(&files[0]).unwrap();
    let summary = mcap::Summary::read(&bytes).unwrap().unwrap();
    assert_eq!(summary.stats.unwrap().message_count, 3);
    std::fs::remove_dir_all(&dir).unwrap();
}
