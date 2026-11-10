//! Each tier has its own session, so its own TCP connection: a retransmission on the monitoring
//! connection never holds a teleop frame. Keys are `<prefix>/<tier>/data/<stream>` and
//! `<prefix>/<tier>/ack`.

mod receiver;
mod uplink;
mod wire;

use std::sync::OnceLock;
use std::time::Instant;

use tierlink_link::Tier;
use tierlink_ratectl::SteadyNs;

pub use receiver::{Received, Receiver};
pub use uplink::{Uplink, UplinkConfig};

/// A Zenoh configuration that talks only to the given endpoints: no scouting, so a local ROS
/// Zenoh router on the vehicle is never joined and ROS traffic cannot leak onto the link.
pub fn config(mode: &str, connect: &[&str], listen: &[&str]) -> zenoh::Result<zenoh::Config> {
    let quote = |eps: &[&str]| {
        let list: Vec<String> = eps.iter().map(|e| format!("\"{e}\"")).collect();
        format!("[{}]", list.join(","))
    };
    let mut c = zenoh::Config::default();
    c.insert_json5("mode", &format!("\"{mode}\""))?;
    c.insert_json5("connect/endpoints", &quote(connect))?;
    c.insert_json5("listen/endpoints", &quote(listen))?;
    c.insert_json5("scouting/multicast/enabled", "false")?;
    c.insert_json5("scouting/gossip/enabled", "false")?;
    Ok(c)
}

fn tier_name(tier: Tier) -> &'static str {
    match tier {
        Tier::Teleop => "teleop",
        Tier::Monitor => "monitor",
    }
}

/// Monotonic nanoseconds since this process first asked; only differences are used.
pub fn steady_now() -> SteadyNs {
    static START: OnceLock<Instant> = OnceLock::new();
    let start = START.get_or_init(Instant::now);
    SteadyNs(i64::try_from(start.elapsed().as_nanos()).unwrap_or(i64::MAX))
}
