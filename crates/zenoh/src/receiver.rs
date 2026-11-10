use std::collections::HashMap;
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Duration;

use tierlink_link::Tier;
use tokio::sync::mpsc;
use zenoh::pubsub::{Publisher, Subscriber};
use zenoh::qos::{CongestionControl, Priority};
use zenoh::{Session, Wait};

use crate::gap::GapTracker;
use crate::wire::{Header, encode_ack};
use crate::{steady_now, tier_name};

#[derive(Debug)]
pub struct Received {
    pub tier: Tier,
    pub stream: String,
    pub seq: u64,
    pub keyframe: bool,
    /// Items of this stream that the transport dropped just before this one.
    pub lost: u64,
    pub payload: Vec<u8>,
}

/// The server side: delivers every stream of both tiers, acknowledges each sample and asks for a
/// keyframe when a stream has a gap.
pub struct Receiver {
    keyframe_requests: [Arc<Publisher<'static>>; 2],
    _subscribers: Vec<Subscriber<()>>,
}

async fn control_publisher(session: &Session, key: String) -> zenoh::Result<Publisher<'static>> {
    // A lost control message costs little: acks are cumulative and keyframe requests repeat.
    session
        .declare_publisher(key)
        .priority(Priority::InteractiveHigh)
        .express(true)
        .congestion_control(CongestionControl::Drop)
        .await
}

impl Receiver {
    pub async fn open(
        session: &Session,
        prefix: &str,
        keyframe_repeat: Duration,
    ) -> zenoh::Result<(Self, mpsc::UnboundedReceiver<Received>)> {
        let (tx, rx) = mpsc::unbounded_channel();
        let mut subscribers = Vec::new();
        let mut requests = Vec::new();
        for tier in [Tier::Teleop, Tier::Monitor] {
            let base = format!("{prefix}/{}", tier_name(tier));
            let acks = control_publisher(session, format!("{base}/ack")).await?;
            let request = Arc::new(control_publisher(session, format!("{base}/keyframe")).await?);
            requests.push(Arc::clone(&request));
            let gaps: Mutex<HashMap<String, GapTracker>> = Mutex::new(HashMap::new());
            let tx = tx.clone();
            let sub = session
                .declare_subscriber(format!("{base}/data/*"))
                .callback(move |sample| {
                    let now = steady_now();
                    let Some(header) = sample
                        .attachment()
                        .and_then(|a| Header::decode(&a.to_bytes()))
                    else {
                        return;
                    };
                    let _ = acks.put(encode_ack(header.seq, now.0).to_vec()).wait();
                    let stream = sample.key_expr().as_str().rsplit('/').next().unwrap_or("");
                    let arrival = gaps
                        .lock()
                        .unwrap_or_else(PoisonError::into_inner)
                        .entry(stream.to_owned())
                        .or_insert_with(|| GapTracker::new(keyframe_repeat))
                        .on_arrival(header.stream_seq, header.keyframe, now);
                    if arrival.request_keyframe {
                        let _ = request.put(stream.as_bytes().to_vec()).wait();
                    }
                    let _ = tx.send(Received {
                        tier,
                        stream: stream.to_owned(),
                        seq: header.seq,
                        keyframe: header.keyframe,
                        lost: arrival.lost,
                        payload: sample.payload().to_bytes().into_owned(),
                    });
                })
                .await?;
            subscribers.push(sub);
        }
        let [teleop, monitor]: [Arc<Publisher<'static>>; 2] = requests
            .try_into()
            .map_err(|_| "one keyframe publisher per tier")?;
        Ok((
            Self {
                keyframe_requests: [teleop, monitor],
                _subscribers: subscribers,
            },
            rx,
        ))
    }

    /// For a decoder that fails without a visible gap.
    pub async fn request_keyframe(&self, tier: Tier, stream: &str) -> zenoh::Result<()> {
        let i = usize::from(tier == Tier::Monitor);
        self.keyframe_requests[i]
            .put(stream.as_bytes().to_vec())
            .await
    }
}
