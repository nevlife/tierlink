use std::collections::VecDeque;

use crate::{Delivery, StreamSpec};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Item {
    pub payload: Vec<u8>,
    pub keyframe: bool,
}

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
    Latest(Option<Item>),
    Reliable {
        queue: VecDeque<Item>,
        capacity: usize,
    },
    Frame {
        pending: Option<Item>,
        inter_frame: bool,
        awaiting_keyframe: bool,
    },
}

fn replace(slot: &mut Option<Item>, item: Item) -> Pushed {
    if slot.replace(item).is_some() {
        Pushed::Replaced
    } else {
        Pushed::Queued
    }
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

    pub fn push(&mut self, item: Item) -> Pushed {
        match self {
            Self::Latest(slot)
            | Self::Frame {
                pending: slot,
                inter_frame: false,
                ..
            } => replace(slot, item),
            Self::Reliable { queue, capacity } => {
                let full = queue.len() >= *capacity;
                if full {
                    queue.pop_front();
                }
                queue.push_back(item);
                if full {
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
                if item.keyframe {
                    *awaiting_keyframe = false;
                    return replace(pending, item);
                }
                if *awaiting_keyframe {
                    return Pushed::Discarded;
                }
                if pending.take().is_some() {
                    // The dropped frame and everything after it are undecodable without it.
                    *awaiting_keyframe = true;
                    return Pushed::KeyframeNeeded;
                }
                *pending = Some(item);
                Pushed::Queued
            }
        }
    }

    pub fn pop(&mut self) -> Option<Item> {
        match self {
            Self::Latest(slot) | Self::Frame { pending: slot, .. } => slot.take(),
            Self::Reliable { queue, .. } => queue.pop_front(),
        }
    }

    pub fn is_empty(&self) -> bool {
        match self {
            Self::Latest(slot) | Self::Frame { pending: slot, .. } => slot.is_none(),
            Self::Reliable { queue, .. } => queue.is_empty(),
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

    fn push(o: &mut Outbox, byte: u8, keyframe: bool) -> Pushed {
        o.push(Item {
            payload: vec![byte],
            keyframe,
        })
    }

    fn pop(o: &mut Outbox) -> Option<u8> {
        o.pop().map(|i| i.payload[0])
    }

    #[test]
    fn latest_keeps_the_newest() {
        let mut o = outbox(Delivery::Latest, false);
        assert_eq!(push(&mut o, 1, false), Pushed::Queued);
        assert_eq!(push(&mut o, 2, false), Pushed::Replaced);
        assert_eq!(pop(&mut o), Some(2));
        assert_eq!(pop(&mut o), None);
    }

    #[test]
    fn reliable_keeps_order_and_drops_the_oldest_when_full() {
        let mut o = outbox(Delivery::Reliable, false);
        push(&mut o, 1, false);
        push(&mut o, 2, false);
        assert_eq!(push(&mut o, 3, false), Pushed::Replaced);
        assert_eq!(pop(&mut o), Some(2));
        assert_eq!(pop(&mut o), Some(3));
    }

    #[test]
    fn stateless_frames_replace_freely() {
        let mut o = outbox(Delivery::Frame, false);
        push(&mut o, 1, false);
        assert_eq!(push(&mut o, 2, false), Pushed::Replaced);
        assert_eq!(pop(&mut o), Some(2));
    }

    #[test]
    fn a_dropped_inter_frame_waits_for_a_keyframe() {
        let mut o = outbox(Delivery::Frame, true);
        assert_eq!(push(&mut o, 1, true), Pushed::Queued);
        assert_eq!(push(&mut o, 2, false), Pushed::KeyframeNeeded);
        assert_eq!(pop(&mut o), None);
        assert_eq!(push(&mut o, 3, false), Pushed::Discarded);
        assert_eq!(push(&mut o, 4, true), Pushed::Queued);
        assert_eq!(push(&mut o, 5, false), Pushed::KeyframeNeeded);
        assert_eq!(push(&mut o, 6, true), Pushed::Queued);
        assert_eq!(
            o.pop(),
            Some(Item {
                payload: vec![6],
                keyframe: true
            })
        );
        assert_eq!(push(&mut o, 7, false), Pushed::Queued);
        assert_eq!(pop(&mut o), Some(7));
    }
}
