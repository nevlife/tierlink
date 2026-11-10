//! [`DelayTracker`] turns sent frames and acknowledgements into [`Feedback`]; a
//! [`RateController`] turns feedback into a [`Decision`]. Time is passed in.

mod aimd;
mod tracker;

use std::time::Duration;

pub use aimd::{Aimd, AimdConfig};
pub use tracker::DelayTracker;

/// Monotonic time of this host in nanoseconds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct SteadyNs(pub i64);

impl SteadyNs {
    pub fn since(self, earlier: SteadyNs) -> Duration {
        Duration::from_nanos(u64::try_from(self.0.saturating_sub(earlier.0)).unwrap_or(0))
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Feedback {
    /// One-way delay above its windowed minimum, at the latest acknowledgement.
    pub queue_delay: Duration,
    /// Age of the oldest unacked frame beyond the minimum round trip; grows while acks stop.
    pub stall: Duration,
    pub in_flight_bytes: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Decision {
    pub target_bps: u64,
    /// Window-based controllers hold frames while this many bytes are unacked.
    pub max_in_flight_bytes: Option<u64>,
}

pub trait RateController: Send {
    /// Called on every acknowledgement and on a timer, so a stall is seen without one.
    fn update(&mut self, now: SteadyNs, feedback: &Feedback) -> Decision;
}

pub struct Fixed {
    pub bps: u64,
}

impl RateController for Fixed {
    fn update(&mut self, _now: SteadyNs, _feedback: &Feedback) -> Decision {
        Decision {
            target_bps: self.bps,
            max_in_flight_bytes: None,
        }
    }
}
