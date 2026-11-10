use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use tierlink_link::{Delivery, Item, Pushed, Scheduler, StreamSpec, Tier};
use tierlink_ratectl::{Decision, DelayTracker, Feedback, RateController};
use tokio::task::JoinHandle;
use zenoh::Session;
use zenoh::pubsub::{Publisher, Subscriber};
use zenoh::qos::{CongestionControl, Priority};

use crate::wire::{Header, decode_ack};
use crate::{steady_now, tier_name};

pub struct UplinkConfig {
    pub prefix: String,
    pub tier: Tier,
    pub streams: Vec<StreamSpec>,
    /// How often the controller is consulted and pending data is sent.
    pub tick: Duration,
    pub burst: Duration,
    pub delay_window: Duration,
}

struct State {
    scheduler: Scheduler,
    tracker: DelayTracker,
    controller: Box<dyn RateController>,
    cap_bps: Option<u64>,
    decision: Option<Decision>,
    next_seq: u64,
    stream_seq: Vec<u64>,
    keyframe_requested: Vec<bool>,
}

/// One tier's sending side: its streams, its controller and its own session.
pub struct Uplink {
    state: Arc<Mutex<State>>,
    task: JoinHandle<()>,
    _acks: Subscriber<()>,
    _keyframe_requests: Subscriber<()>,
}

fn lock(state: &Mutex<State>) -> MutexGuard<'_, State> {
    state
        .lock()
        .expect("no code panics while holding the uplink state")
}

impl Uplink {
    pub async fn open(
        session: &Session,
        cfg: UplinkConfig,
        controller: Box<dyn RateController>,
    ) -> zenoh::Result<Self> {
        let base = format!("{}/{}", cfg.prefix, tier_name(cfg.tier));
        let mut publishers = Vec::with_capacity(cfg.streams.len());
        for s in &cfg.streams {
            publishers.push(declare(session, &base, s).await?);
        }
        let n = cfg.streams.len();
        let state = Arc::new(Mutex::new(State {
            scheduler: Scheduler::new(cfg.streams, 0, cfg.burst),
            tracker: DelayTracker::new(cfg.delay_window),
            controller,
            cap_bps: None,
            decision: None,
            next_seq: 0,
            stream_seq: vec![0; n],
            keyframe_requested: vec![false; n],
        }));
        let acked = Arc::clone(&state);
        let acks = session
            .declare_subscriber(format!("{base}/ack"))
            .callback(move |sample| {
                if let Some((seq, received)) = decode_ack(&sample.payload().to_bytes()) {
                    lock(&acked).tracker.on_ack(seq, received, steady_now());
                }
            })
            .await?;
        let requested = Arc::clone(&state);
        let keyframe_requests = session
            .declare_subscriber(format!("{base}/keyframe"))
            .callback(move |sample| {
                let name = sample.payload().to_bytes();
                let mut s = lock(&requested);
                let id = std::str::from_utf8(&name)
                    .ok()
                    .and_then(|n| s.scheduler.id(n));
                if let Some(id) = id {
                    s.keyframe_requested[id] = true;
                }
            })
            .await?;
        let task = tokio::spawn(send_loop(Arc::clone(&state), publishers, cfg.tick));
        Ok(Self {
            state,
            task,
            _acks: acks,
            _keyframe_requests: keyframe_requests,
        })
    }

    pub fn stream_id(&self, name: &str) -> Option<usize> {
        lock(&self.state).scheduler.id(name)
    }

    pub fn push(&self, id: usize, payload: Vec<u8>, keyframe: bool) -> Pushed {
        let mut s = lock(&self.state);
        let pushed = s.scheduler.push(id, Item { payload, keyframe });
        if pushed == Pushed::KeyframeNeeded {
            s.keyframe_requested[id] = true;
        }
        pushed
    }

    /// True once after this stream dropped a frame or the receiver asked for a keyframe; the
    /// encoder checks it before each frame.
    pub fn take_keyframe_request(&self, id: usize) -> bool {
        std::mem::take(&mut lock(&self.state).keyframe_requested[id])
    }

    /// An upper bound from outside, such as the allowance the teleop process publishes.
    pub fn set_cap(&self, bps: Option<u64>) {
        lock(&self.state).cap_bps = bps;
    }

    pub fn status(&self) -> (Option<Decision>, Feedback) {
        let s = lock(&self.state);
        (s.decision, s.tracker.feedback(steady_now()))
    }
}

impl Drop for Uplink {
    fn drop(&mut self) {
        self.task.abort();
    }
}

async fn declare(
    session: &Session,
    base: &str,
    spec: &StreamSpec,
) -> zenoh::Result<Publisher<'static>> {
    let teleop = spec.tier == Tier::Teleop;
    // Block keeps reliable data instead of dropping it in Zenoh's queue; the scheduler already
    // paces it below the link budget.
    let congestion = if spec.delivery == Delivery::Reliable {
        CongestionControl::Block
    } else {
        CongestionControl::Drop
    };
    session
        .declare_publisher(format!("{base}/data/{}", spec.name))
        .priority(if teleop {
            Priority::InteractiveHigh
        } else {
            Priority::Data
        })
        .express(teleop)
        .congestion_control(congestion)
        .await
}

async fn send_loop(state: Arc<Mutex<State>>, publishers: Vec<Publisher<'static>>, tick: Duration) {
    let mut interval = tokio::time::interval(tick);
    loop {
        interval.tick().await;
        let batch = {
            let mut s = lock(&state);
            let now = steady_now();
            let feedback = s.tracker.feedback(now);
            let decision = s.controller.update(now, &feedback);
            let budget = s
                .cap_bps
                .map_or(decision.target_bps, |c| c.min(decision.target_bps));
            s.scheduler.set_budget(budget);
            s.decision = Some(decision);
            let mut batch = Vec::new();
            while decision
                .max_in_flight_bytes
                .is_none_or(|max| s.tracker.feedback(now).in_flight_bytes < max)
            {
                let Some((id, item)) = s.scheduler.next(now) else {
                    break;
                };
                let seq = s.next_seq;
                s.next_seq += 1;
                let stream_seq = s.stream_seq[id];
                s.stream_seq[id] += 1;
                s.tracker.on_sent(seq, item.payload.len() as u64, now);
                let header = Header {
                    seq,
                    stream_seq,
                    sent_ns: now.0,
                    keyframe: item.keyframe,
                };
                batch.push((id, header, item.payload));
            }
            batch
        };
        for (id, header, payload) in batch {
            let put = publishers[id]
                .put(payload)
                .attachment(header.encode().to_vec());
            // Only a closed session fails a put; Drop congestion reports success.
            if put.await.is_err() {
                return;
            }
        }
    }
}
