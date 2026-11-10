use std::collections::VecDeque;

use crate::{Delivery, StreamSpec};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pushed {
    Queued,
    /// An older pending value was dropped to make room.
    Replaced,
    /// The value was not kept: an inter-frame stream is waiting for a keyframe.
    Discarded,
    /// An unsent frame was dropped; the encoder must produce a keyframe next.
    KeyframeNeeded,
}

pub enum Outbox {
    Latest(Option<Vec<u8>>),
    Reliable {
        queue: VecDeque<Vec<u8>>,
        capacity: usize,
    },
    Frame {
        pending: Option<Vec<u8>>,
        inter_frame: bool,
        awaiting_keyframe: bool,
    },
}

impl Outbox {
    pub fn new(spec: &StreamSpec) -> Self {
        match spec.delivery {
            Delivery::Latest => Self::Latest(None),
            Delivery::Reliable => Self::Reliable {
                queue: VecDeque::new(),
                capacity: spec.capacity,
            },
            Delivery::Frame => Self::Frame {
                pending: None,
                inter_frame: spec.inter_frame,
                awaiting_keyframe: false,
            },
        }
    }

    /// `keyframe` is read only by inter-frame streams.
    pub fn push(&mut self, payload: Vec<u8>, keyframe: bool) -> Pushed {
        match self {
            Self::Latest(slot) => {
                if slot.replace(payload).is_some() {
                    Pushed::Replaced
                } else {
                    Pushed::Queued
                }
            }
            Self::Reliable { queue, capacity } => {
                let full = queue.len() >= *capacity;
                if full {
                    queue.pop_front();
                }
                queue.push_back(payload);
                if full {
                    Pushed::Replaced
                } else {
                    Pushed::Queued
                }
            }
            Self::Frame {
                pending,
                inter_frame: false,
                ..
            } => {
                if pending.replace(payload).is_some() {
                    Pushed::Replaced
                } else {
                    Pushed::Queued
                }
            }
            Self::Frame {
                pending,
                awaiting_keyframe,
                ..
            } => {
                if keyframe {
                    *awaiting_keyframe = false;
                    let replaced = pending.replace(payload).is_some();
                    return if replaced {
                        Pushed::Replaced
                    } else {
                        Pushed::Queued
                    };
                }
                if *awaiting_keyframe {
                    return Pushed::Discarded;
                }
                if pending.take().is_some() {
                    // The dropped frame and everything after it are undecodable without it.
                    *awaiting_keyframe = true;
                    return Pushed::KeyframeNeeded;
                }
                *pending = Some(payload);
                Pushed::Queued
            }
        }
    }

    pub fn pop(&mut self) -> Option<Vec<u8>> {
        match self {
            Self::Latest(slot) | Self::Frame { pending: slot, .. } => slot.take(),
            Self::Reliable { queue, .. } => queue.pop_front(),
        }
    }

    pub fn peek_len(&self) -> Option<usize> {
        match self {
            Self::Latest(slot) | Self::Frame { pending: slot, .. } => slot.as_ref().map(Vec::len),
            Self::Reliable { queue, .. } => queue.front().map(Vec::len),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Tier;

    fn outbox(delivery: Delivery, inter_frame: bool) -> Outbox {
        Outbox::new(&StreamSpec {
            name: "s".into(),
            tier: Tier::Monitor,
            delivery,
            inter_frame,
            capacity: 2,
        })
    }

    #[test]
    fn latest_keeps_the_newest() {
        let mut o = outbox(Delivery::Latest, false);
        assert_eq!(o.push(vec![1], false), Pushed::Queued);
        assert_eq!(o.push(vec![2], false), Pushed::Replaced);
        assert_eq!(o.pop(), Some(vec![2]));
        assert_eq!(o.pop(), None);
    }

    #[test]
    fn reliable_keeps_order_and_drops_the_oldest_when_full() {
        let mut o = outbox(Delivery::Reliable, false);
        o.push(vec![1], false);
        o.push(vec![2], false);
        assert_eq!(o.push(vec![3], false), Pushed::Replaced);
        assert_eq!(o.pop(), Some(vec![2]));
        assert_eq!(o.pop(), Some(vec![3]));
    }

    #[test]
    fn stateless_frames_replace_freely() {
        let mut o = outbox(Delivery::Frame, false);
        o.push(vec![1], false);
        assert_eq!(o.push(vec![2], false), Pushed::Replaced);
        assert_eq!(o.pop(), Some(vec![2]));
    }

    #[test]
    fn a_dropped_inter_frame_waits_for_a_keyframe() {
        let mut o = outbox(Delivery::Frame, true);
        assert_eq!(o.push(vec![1], true), Pushed::Queued);
        assert_eq!(o.push(vec![2], false), Pushed::KeyframeNeeded);
        assert_eq!(o.pop(), None);
        assert_eq!(o.push(vec![3], false), Pushed::Discarded);
        assert_eq!(o.push(vec![4], true), Pushed::Queued);
        assert_eq!(o.push(vec![5], false), Pushed::KeyframeNeeded);
        assert_eq!(o.push(vec![6], true), Pushed::Queued);
        assert_eq!(o.pop(), Some(vec![6]));
        assert_eq!(o.push(vec![7], false), Pushed::Queued);
        assert_eq!(o.pop(), Some(vec![7]));
    }
}
