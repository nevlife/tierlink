use std::time::Duration;

use tierlink_link::{Delivery, StreamSpec, Tier};
use tierlink_ratectl::Fixed;
use tierlink_zenoh::{Receiver, Uplink, UplinkConfig, config};
use tokio::time::{sleep, timeout};

fn free_port() -> u16 {
    std::net::TcpListener::bind("127.0.0.1:0")
        .expect("a free local port")
        .local_addr()
        .expect("a bound address")
        .port()
}

fn stream(name: &str, delivery: Delivery, inter_frame: bool) -> StreamSpec {
    StreamSpec {
        name: name.into(),
        tier: Tier::Teleop,
        delivery,
        inter_frame,
        capacity: 16,
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn frames_arrive_and_acks_empty_the_flight() {
    let endpoint = format!("tcp/127.0.0.1:{}", free_port());
    let server = zenoh::open(config("peer", &[], &[&endpoint]).unwrap())
        .await
        .unwrap();
    let vehicle = zenoh::open(config("client", &[&endpoint], &[]).unwrap())
        .await
        .unwrap();
    let (_receiver, mut rx) = Receiver::open(&server, "test/r01").await.unwrap();
    let uplink = Uplink::open(
        &vehicle,
        UplinkConfig {
            prefix: "test/r01".into(),
            tier: Tier::Teleop,
            streams: vec![
                stream("front_cam", Delivery::Frame, true),
                stream("cmd", Delivery::Latest, false),
            ],
            tick: Duration::from_millis(5),
            burst: Duration::from_millis(100),
            delay_window: Duration::from_secs(10),
        },
        Box::new(Fixed { bps: 10_000_000 }),
    )
    .await
    .unwrap();
    // Declarations reach the other side asynchronously.
    sleep(Duration::from_millis(500)).await;

    let cam = uplink.stream_id("front_cam").unwrap();
    uplink.push(cam, vec![7; 2000], true);
    let got = timeout(Duration::from_secs(5), rx.recv())
        .await
        .unwrap()
        .unwrap();
    assert_eq!(got.tier, Tier::Teleop);
    assert_eq!(got.stream, "front_cam");
    assert!(got.keyframe);
    assert_eq!(got.payload, vec![7; 2000]);

    sleep(Duration::from_millis(200)).await;
    let (decision, feedback) = uplink.status();
    assert_eq!(decision.unwrap().target_bps, 10_000_000);
    assert_eq!(feedback.in_flight_bytes, 0);
}
