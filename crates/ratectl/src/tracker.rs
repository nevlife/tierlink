use std::collections::VecDeque;
use std::time::Duration;

use crate::{Feedback, SteadyNs};

struct Sent {
    id: u64,
    at: SteadyNs,
    bytes: u64,
}

/// Minimum over a sliding time window.
struct WindowMin {
    window: Duration,
    samples: VecDeque<(SteadyNs, i64)>,
}

impl WindowMin {
    fn new(window: Duration) -> Self {
        Self {
            window,
            samples: VecDeque::new(),
        }
    }

    fn push(&mut self, now: SteadyNs, value: i64) -> i64 {
        while self.samples.back().is_some_and(|&(_, v)| v >= value) {
            self.samples.pop_back();
        }
        self.samples.push_back((now, value));
        while self
            .samples
            .front()
            .is_some_and(|&(t, _)| now.since(t) > self.window)
        {
            self.samples.pop_front();
        }
        self.samples
            .front()
            .expect("the sample just pushed is in the window")
            .1
    }

    fn min(&self) -> Option<i64> {
        self.samples.front().map(|&(_, v)| v)
    }
}

pub struct DelayTracker {
    unacked: VecDeque<Sent>,
    in_flight: u64,
    // Receiver clock minus sender clock plus the path delay; the unknown offset cancels against
    // the minimum, so the clocks need no synchronization. The window bounds clock drift.
    one_way: WindowMin,
    rtt: WindowMin,
    queue_delay: Duration,
}

impl DelayTracker {
    pub fn new(window: Duration) -> Self {
        Self {
            unacked: VecDeque::new(),
            in_flight: 0,
            one_way: WindowMin::new(window),
            rtt: WindowMin::new(window),
            queue_delay: Duration::ZERO,
        }
    }

    /// Records a frame handed to the transport. Ids increase in sending order.
    pub fn on_sent(&mut self, id: u64, bytes: u64, at: SteadyNs) {
        self.unacked.push_back(Sent { id, at, bytes });
        self.in_flight += bytes;
    }

    /// The transport is ordered, so an acknowledgement also covers every earlier frame.
    pub fn on_ack(&mut self, id: u64, received_remote_ns: i64, now: SteadyNs) {
        let covered = self.unacked.iter().take_while(|s| s.id <= id).count();
        let mut acked = None;
        for sent in self.unacked.drain(..covered) {
            self.in_flight -= sent.bytes;
            if sent.id == id {
                acked = Some(sent.at);
            }
        }
        let Some(sent_at) = acked else { return };
        let one_way = received_remote_ns.saturating_sub(sent_at.0);
        let min = self.one_way.push(now, one_way);
        self.queue_delay = Duration::from_nanos(u64::try_from(one_way - min).unwrap_or(0));
        self.rtt.push(now, now.0.saturating_sub(sent_at.0));
    }

    pub fn feedback(&self, now: SteadyNs) -> Feedback {
        let stall = self.unacked.front().map_or(Duration::ZERO, |oldest| {
            let base =
                Duration::from_nanos(u64::try_from(self.rtt.min().unwrap_or(0)).unwrap_or(0));
            now.since(oldest.at).saturating_sub(base)
        });
        Feedback {
            queue_delay: self.queue_delay,
            stall,
            in_flight_bytes: self.in_flight,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: i64 = 1_000_000;
    // The receiver clock runs 5 s ahead; only differences matter.
    const OFFSET: i64 = 5_000 * MS;

    fn tracker() -> DelayTracker {
        DelayTracker::new(Duration::from_secs(10))
    }

    #[test]
    fn queue_delay_is_one_way_delay_above_its_minimum() {
        let mut t = tracker();
        t.on_sent(0, 1000, SteadyNs(0));
        t.on_ack(0, OFFSET + 20 * MS, SteadyNs(40 * MS));
        assert_eq!(t.feedback(SteadyNs(40 * MS)).queue_delay, Duration::ZERO);
        t.on_sent(1, 1000, SteadyNs(100 * MS));
        t.on_ack(1, OFFSET + 100 * MS + 70 * MS, SteadyNs(190 * MS));
        assert_eq!(
            t.feedback(SteadyNs(190 * MS)).queue_delay,
            Duration::from_millis(50)
        );
    }

    #[test]
    fn an_ack_covers_earlier_frames() {
        let mut t = tracker();
        for id in 0..3 {
            t.on_sent(id, 500, SteadyNs(i64::try_from(id).unwrap() * MS));
        }
        assert_eq!(t.feedback(SteadyNs(3 * MS)).in_flight_bytes, 1500);
        t.on_ack(1, OFFSET + 30 * MS, SteadyNs(40 * MS));
        assert_eq!(t.feedback(SteadyNs(40 * MS)).in_flight_bytes, 500);
    }

    #[test]
    fn stall_grows_while_acks_stop() {
        let mut t = tracker();
        t.on_sent(0, 1000, SteadyNs(0));
        t.on_ack(0, OFFSET + 20 * MS, SteadyNs(40 * MS));
        t.on_sent(1, 1000, SteadyNs(100 * MS));
        assert_eq!(t.feedback(SteadyNs(140 * MS)).stall, Duration::ZERO);
        assert_eq!(
            t.feedback(SteadyNs(640 * MS)).stall,
            Duration::from_millis(500)
        );
        t.on_ack(1, OFFSET + 640 * MS, SteadyNs(660 * MS));
        assert_eq!(t.feedback(SteadyNs(660 * MS)).stall, Duration::ZERO);
    }

    #[test]
    fn old_minimum_leaves_the_window() {
        let mut t = DelayTracker::new(Duration::from_millis(500));
        t.on_sent(0, 1, SteadyNs(0));
        t.on_ack(0, OFFSET + 10 * MS, SteadyNs(20 * MS));
        t.on_sent(1, 1, SteadyNs(1000 * MS));
        t.on_ack(1, OFFSET + 1000 * MS + 30 * MS, SteadyNs(1040 * MS));
        assert_eq!(t.feedback(SteadyNs(1040 * MS)).queue_delay, Duration::ZERO);
    }
}
