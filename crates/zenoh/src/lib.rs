//! Each tier has its own session, so its own TCP connection: a retransmission on the monitoring
//! connection never holds a teleop frame. Keys are `<prefix>/<tier>/data/<stream>`,
//! `<prefix>/<tier>/ack` and `<prefix>/<tier>/keyframe`.

mod gap;
mod receiver;
mod uplink;
mod wire;

use std::sync::OnceLock;
use std::time::Instant;

use tierlink_link::Tier;
use tierlink_ratectl::SteadyNs;

pub use receiver::{Received, Receiver};
pub use uplink::{Uplink, UplinkConfig};

/// Zenoh's own queue holds data the scheduler can no longer drop; by default up to two 64 KiB
/// batches per priority, about a second at 1 Mbit/s. A teleop session keeps it short.
#[derive(Clone, Copy, Debug)]
pub struct Tuning {
    pub batch_size: u16,
    pub queue_batches: u8,
}

impl Tuning {
    /// Starting values until measured on a narrow link.
    pub const SHORT_QUEUE: Self = Self {
        batch_size: 16_384,
        queue_batches: 1,
    };
}

/// A Zenoh configuration that talks only to the given endpoints: no scouting, so a local ROS
/// Zenoh router on the vehicle is never joined and ROS traffic cannot leak onto the link.
pub fn config(
    mode: &str,
    connect: &[&str],
    listen: &[&str],
    tuning: Option<Tuning>,
) -> zenoh::Result<zenoh::Config> {
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
    if let Some(t) = tuning {
        c.insert_json5("transport/link/tx/batch_size", &t.batch_size.to_string())?;
        for p in [
            "control",
            "real_time",
            "interactive_high",
            "interactive_low",
            "data_high",
            "data",
            "data_low",
            "background",
        ] {
            let key = format!("transport/link/tx/queue/size/{p}");
            c.insert_json5(&key, &t.queue_batches.to_string())?;
        }
    }
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
