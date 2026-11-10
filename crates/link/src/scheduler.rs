use std::time::Duration;

use tierlink_ratectl::SteadyNs;

use crate::{Outbox, Pushed, StreamSpec};

/// Sends pending data within a bit budget: teleop streams before monitor streams, and within a
/// tier in declaration order.
pub struct Scheduler {
    streams: Vec<(StreamSpec, Outbox)>,
    order: Vec<usize>,
    budget_bps: u64,
    burst: Duration,
    tokens_bits: f64,
    last_refill: Option<SteadyNs>,
}

impl Scheduler {
    /// `burst` bounds how much unused budget carries over, so an idle stream cannot later flood
    /// the link.
    pub fn new(specs: Vec<StreamSpec>, budget_bps: u64, burst: Duration) -> Self {
        let mut order: Vec<usize> = (0..specs.len()).collect();
        order.sort_by_key(|&i| specs[i].tier);
        let streams = specs
            .into_iter()
            .map(|s| {
                let outbox = Outbox::new(&s);
                (s, outbox)
            })
            .collect();
        Self {
            streams,
            order,
            budget_bps,
            burst,
            tokens_bits: 0.0,
            last_refill: None,
        }
    }

    pub fn id(&self, name: &str) -> Option<usize> {
        self.streams.iter().position(|(s, _)| s.name == name)
    }

    pub fn spec(&self, id: usize) -> &StreamSpec {
        &self.streams[id].0
    }

    pub fn set_budget(&mut self, bps: u64) {
        self.budget_bps = bps;
    }

    pub fn push(&mut self, id: usize, payload: Vec<u8>, keyframe: bool) -> Pushed {
        self.streams[id].1.push(payload, keyframe)
    }

    /// The next payload to send, or `None` when nothing is pending or the budget is spent. A
    /// payload larger than the tokens left is still sent and leaves the bucket in debt, so a large
    /// frame is delayed by its own size instead of waiting forever.
    #[expect(
        clippy::cast_precision_loss,
        reason = "bit counts and rates are far below 2^52"
    )]
    pub fn next(&mut self, now: SteadyNs) -> Option<(usize, Vec<u8>)> {
        let rate = self.budget_bps as f64;
        let elapsed = self.last_refill.map_or(0.0, |t| now.since(t).as_secs_f64());
        self.last_refill = Some(now);
        self.tokens_bits = (self.tokens_bits + rate * elapsed).min(rate * self.burst.as_secs_f64());
        if self.tokens_bits <= 0.0 {
            return None;
        }
        let id = *self
            .order
            .iter()
            .find(|&&i| self.streams[i].1.peek_len().is_some())?;
        let payload = self.streams[id].1.pop()?;
        self.tokens_bits -= (payload.len() * 8) as f64;
        Some((id, payload))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Delivery, Tier};

    const MS: i64 = 1_000_000;

    fn spec(name: &str, tier: Tier) -> StreamSpec {
        StreamSpec {
            name: name.into(),
            tier,
            delivery: Delivery::Reliable,
            inter_frame: false,
            capacity: 16,
        }
    }

    fn scheduler(bps: u64) -> Scheduler {
        Scheduler::new(
            vec![spec("odom", Tier::Monitor), spec("video", Tier::Teleop)],
            bps,
            Duration::from_millis(100),
        )
    }

    #[test]
    fn teleop_goes_before_monitor() {
        let mut s = scheduler(1_000_000);
        s.next(SteadyNs(0));
        s.push(0, vec![0; 10], false);
        s.push(1, vec![1; 10], false);
        let at = SteadyNs(10 * MS);
        assert_eq!(s.next(at).map(|(id, _)| id), Some(1));
        assert_eq!(s.next(at).map(|(id, _)| id), Some(0));
        assert_eq!(s.next(at), None);
    }

    #[test]
    fn spending_past_the_budget_waits_for_refill() {
        // 80 kbit/s: one 1000-byte payload costs 100 ms of budget.
        let mut s = scheduler(80_000);
        s.next(SteadyNs(0));
        for _ in 0..3 {
            s.push(0, vec![0; 1000], false);
        }
        assert!(s.next(SteadyNs(50 * MS)).is_some());
        assert!(s.next(SteadyNs(60 * MS)).is_none());
        assert!(s.next(SteadyNs(150 * MS)).is_some());
    }

    #[test]
    fn idle_time_carries_over_only_one_burst() {
        let mut s = scheduler(80_000);
        s.next(SteadyNs(0));
        for _ in 0..3 {
            s.push(0, vec![0; 1000], false);
        }
        let later = SteadyNs(10_000 * MS);
        assert!(s.next(later).is_some());
        assert!(s.next(later).is_none());
    }

    #[test]
    fn a_zero_budget_sends_nothing() {
        let mut s = scheduler(0);
        s.push(1, vec![1], false);
        assert!(s.next(SteadyNs(MS)).is_none());
    }
}
