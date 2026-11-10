use std::time::Duration;

use tierlink_ratectl::{Decision, Feedback, SteadyNs};

/// How the teleop process limits monitoring while it drives.
#[derive(Clone, Debug)]
pub struct TierPolicy {
    /// Fraction of the link budget monitoring may use while teleop is active.
    pub monitor_share: f64,
    /// Above this queue delay monitoring stops entirely, before teleop is cut.
    pub monitor_stop_above: Duration,
}

impl TierPolicy {
    /// The budget the teleop process publishes for monitoring.
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        clippy::cast_sign_loss,
        reason = "bit rates are far below 2^52 and the product is non-negative"
    )]
    pub fn monitor_allowance(&self, link: &Decision, teleop_bps: u64, feedback: &Feedback) -> u64 {
        if feedback.queue_delay.max(feedback.stall) > self.monitor_stop_above {
            return 0;
        }
        let share = (link.target_bps as f64 * self.monitor_share) as u64;
        share.min(link.target_bps.saturating_sub(teleop_bps))
    }
}

/// The monitoring process's view of the last published allowance. Teleop that stops publishing
/// is treated as inactive, so monitoring falls back to its own controller.
pub struct Allowance {
    expires_after: Duration,
    latest: Option<(SteadyNs, u64)>,
}

impl Allowance {
    pub fn new(expires_after: Duration) -> Self {
        Self {
            expires_after,
            latest: None,
        }
    }

    pub fn update(&mut self, bps: u64, now: SteadyNs) {
        self.latest = Some((now, bps));
    }

    pub fn effective(&self, own: &Decision, now: SteadyNs) -> u64 {
        match self.latest {
            Some((at, bps)) if now.since(at) <= self.expires_after => bps.min(own.target_bps),
            _ => own.target_bps,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: i64 = 1_000_000;

    fn policy() -> TierPolicy {
        TierPolicy {
            monitor_share: 0.1,
            monitor_stop_above: Duration::from_millis(50),
        }
    }

    fn link(bps: u64) -> Decision {
        Decision {
            target_bps: bps,
            max_in_flight_bytes: None,
        }
    }

    fn delay(ms: u64) -> Feedback {
        Feedback {
            queue_delay: Duration::from_millis(ms),
            ..Feedback::default()
        }
    }

    #[test]
    fn monitoring_gets_a_share_of_what_teleop_leaves() {
        let p = policy();
        assert_eq!(
            p.monitor_allowance(&link(1_000_000), 500_000, &delay(0)),
            100_000
        );
        assert_eq!(
            p.monitor_allowance(&link(1_000_000), 950_000, &delay(0)),
            50_000
        );
    }

    #[test]
    fn monitoring_stops_when_the_queue_grows() {
        let p = policy();
        assert_eq!(p.monitor_allowance(&link(1_000_000), 0, &delay(80)), 0);
    }

    #[test]
    fn an_expired_allowance_falls_back_to_the_own_controller() {
        let mut a = Allowance::new(Duration::from_millis(500));
        let own = link(800_000);
        assert_eq!(a.effective(&own, SteadyNs(0)), 800_000);
        a.update(100_000, SteadyNs(0));
        assert_eq!(a.effective(&own, SteadyNs(400 * MS)), 100_000);
        assert_eq!(a.effective(&own, SteadyNs(600 * MS)), 800_000);
    }
}
