use std::time::Duration;

use tierlink_ratectl::SteadyNs;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Arrival {
    /// Items published but never received before this one.
    pub lost: u64,
    pub request_keyframe: bool,
}

/// Per-stream receive state. A gap means the transport dropped items; for an inter-frame stream
/// the picture stays broken until a keyframe, so one is requested and requested again on later
/// arrivals until it comes, in case the request itself is lost.
pub struct GapTracker {
    repeat: Duration,
    next: Option<u64>,
    awaiting_since: Option<SteadyNs>,
    last_request: Option<SteadyNs>,
}

impl GapTracker {
    pub fn new(repeat: Duration) -> Self {
        Self {
            repeat,
            next: None,
            awaiting_since: None,
            last_request: None,
        }
    }

    pub fn on_arrival(&mut self, stream_seq: u64, keyframe: bool, now: SteadyNs) -> Arrival {
        let lost = self.next.map_or(0, |n| stream_seq.saturating_sub(n));
        self.next = Some(stream_seq + 1);
        if keyframe {
            self.awaiting_since = None;
            return Arrival {
                lost,
                request_keyframe: false,
            };
        }
        if lost > 0 && self.awaiting_since.is_none() {
            self.awaiting_since = Some(now);
            self.last_request = None;
        }
        let request_keyframe = self.awaiting_since.is_some()
            && self
                .last_request
                .is_none_or(|t| now.since(t) >= self.repeat);
        if request_keyframe {
            self.last_request = Some(now);
        }
        Arrival {
            lost,
            request_keyframe,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: i64 = 1_000_000;

    fn at(ms: i64) -> SteadyNs {
        SteadyNs(ms * MS)
    }

    #[test]
    fn in_order_arrivals_request_nothing() {
        let mut g = GapTracker::new(Duration::from_millis(250));
        assert_eq!(g.on_arrival(0, true, at(0)).lost, 0);
        let a = g.on_arrival(1, false, at(33));
        assert!(!a.request_keyframe && a.lost == 0);
    }

    #[test]
    fn a_gap_requests_until_a_keyframe_arrives() {
        let mut g = GapTracker::new(Duration::from_millis(250));
        g.on_arrival(0, true, at(0));
        let a = g.on_arrival(3, false, at(100));
        assert_eq!(a.lost, 2);
        assert!(a.request_keyframe);
        assert!(!g.on_arrival(4, false, at(200)).request_keyframe);
        assert!(g.on_arrival(5, false, at(350)).request_keyframe);
        assert!(!g.on_arrival(6, true, at(400)).request_keyframe);
        assert!(!g.on_arrival(7, false, at(800)).request_keyframe);
    }
}
