use std::time::Duration;

use crate::{Decision, Feedback, RateController, SteadyNs};

#[derive(Clone, Debug)]
pub struct AimdConfig {
    pub start_bps: u64,
    pub min_bps: u64,
    pub max_bps: u64,
    pub step_bps: u64,
    pub factor: f64,
    /// One cut per hold: the encoder needs time before the queue reflects it.
    pub hold: Duration,
    /// How long the link must stay clear before each increase.
    pub probe: Duration,
    pub congested_above: Duration,
    pub clear_below: Duration,
}

/// Multiplicative decrease on congestion, additive increase while clear.
pub struct Aimd {
    cfg: AimdConfig,
    bps: u64,
    last_cut: Option<SteadyNs>,
    clear_since: Option<SteadyNs>,
}

impl Aimd {
    pub fn new(cfg: AimdConfig) -> Self {
        let bps = cfg.start_bps;
        Self {
            cfg,
            bps,
            last_cut: None,
            clear_since: None,
        }
    }
}

impl RateController for Aimd {
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        clippy::cast_sign_loss,
        reason = "bit rates are far below 2^52 and the product is non-negative"
    )]
    fn update(&mut self, now: SteadyNs, feedback: &Feedback) -> Decision {
        let delay = feedback.queue_delay.max(feedback.stall);
        if delay > self.cfg.congested_above {
            self.clear_since = None;
            if self.last_cut.is_none_or(|t| now.since(t) >= self.cfg.hold) {
                self.last_cut = Some(now);
                let cut = (self.bps as f64 * self.cfg.factor) as u64;
                self.bps = cut.max(self.cfg.min_bps);
            }
        } else if delay < self.cfg.clear_below {
            let since = *self.clear_since.get_or_insert(now);
            if now.since(since) >= self.cfg.probe {
                self.clear_since = Some(now);
                self.bps = (self.bps + self.cfg.step_bps).min(self.cfg.max_bps);
            }
        } else {
            self.clear_since = None;
        }
        Decision {
            target_bps: self.bps,
            max_in_flight_bytes: None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: i64 = 1_000_000;

    fn aimd() -> Aimd {
        Aimd::new(AimdConfig {
            start_bps: 1_000_000,
            min_bps: 300_000,
            max_bps: 2_000_000,
            step_bps: 100_000,
            factor: 0.8,
            hold: Duration::from_millis(300),
            probe: Duration::from_secs(1),
            congested_above: Duration::from_millis(100),
            clear_below: Duration::from_millis(30),
        })
    }

    fn delay(ms: u64) -> Feedback {
        Feedback {
            queue_delay: Duration::from_millis(ms),
            ..Feedback::default()
        }
    }

    fn rate(a: &mut Aimd, at_ms: i64, fb: &Feedback) -> u64 {
        a.update(SteadyNs(at_ms * MS), fb).target_bps
    }

    #[test]
    fn cuts_once_per_hold() {
        let mut a = aimd();
        assert_eq!(rate(&mut a, 0, &delay(150)), 800_000);
        assert_eq!(rate(&mut a, 100, &delay(150)), 800_000);
        assert_eq!(rate(&mut a, 300, &delay(150)), 640_000);
    }

    #[test]
    fn a_stall_counts_as_congestion() {
        let mut a = aimd();
        let stalled = Feedback {
            stall: Duration::from_millis(400),
            ..Feedback::default()
        };
        assert_eq!(rate(&mut a, 0, &stalled), 800_000);
    }

    #[test]
    fn increases_after_each_clear_probe() {
        let mut a = aimd();
        assert_eq!(rate(&mut a, 0, &delay(10)), 1_000_000);
        assert_eq!(rate(&mut a, 999, &delay(10)), 1_000_000);
        assert_eq!(rate(&mut a, 1000, &delay(10)), 1_100_000);
        assert_eq!(rate(&mut a, 1500, &delay(50)), 1_100_000);
        assert_eq!(rate(&mut a, 2000, &delay(10)), 1_100_000);
        assert_eq!(rate(&mut a, 3000, &delay(10)), 1_200_000);
    }

    #[test]
    fn stays_within_bounds() {
        let mut a = aimd();
        for i in 0..20 {
            rate(&mut a, i * 300, &delay(500));
        }
        assert_eq!(rate(&mut a, 6000, &delay(500)), 300_000);
        for i in 0..30 {
            rate(&mut a, 7000 + i * 1000, &delay(0));
        }
        assert_eq!(rate(&mut a, 40_000, &delay(0)), 2_000_000);
    }
}
