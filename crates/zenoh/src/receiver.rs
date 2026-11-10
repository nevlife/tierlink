use tierlink_link::Tier;
use tokio::sync::mpsc;
use zenoh::pubsub::Subscriber;
use zenoh::qos::{CongestionControl, Priority};
use zenoh::{Session, Wait};

use crate::wire::{Header, encode_ack};
use crate::{steady_now, tier_name};

#[derive(Debug)]
pub struct Received {
    pub tier: Tier,
    pub stream: String,
    pub seq: u64,
    pub keyframe: bool,
    pub payload: Vec<u8>,
}

/// The server side: delivers every stream of both tiers and acknowledges each sample.
pub struct Receiver {
    _subscribers: Vec<Subscriber<()>>,
}

impl Receiver {
    pub async fn open(
        session: &Session,
        prefix: &str,
    ) -> zenoh::Result<(Self, mpsc::UnboundedReceiver<Received>)> {
        let (tx, rx) = mpsc::unbounded_channel();
        let mut subscribers = Vec::new();
        for tier in [Tier::Teleop, Tier::Monitor] {
            let base = format!("{prefix}/{}", tier_name(tier));
            // A lost ack costs nothing: the next one covers it.
            let acks = session
                .declare_publisher(format!("{base}/ack"))
                .priority(Priority::InteractiveHigh)
                .express(true)
                .congestion_control(CongestionControl::Drop)
                .await?;
            let tx = tx.clone();
            let sub = session
                .declare_subscriber(format!("{base}/data/*"))
                .callback(move |sample| {
                    let received_ns = steady_now().0;
                    let Some(header) = sample
                        .attachment()
                        .and_then(|a| Header::decode(&a.to_bytes()))
                    else {
                        return;
                    };
                    let _ = acks
                        .put(encode_ack(header.seq, received_ns).to_vec())
                        .wait();
                    let stream = sample.key_expr().as_str().rsplit('/').next().unwrap_or("");
                    let _ = tx.send(Received {
                        tier,
                        stream: stream.to_owned(),
                        seq: header.seq,
                        keyframe: header.keyframe,
                        payload: sample.payload().to_bytes().into_owned(),
                    });
                })
                .await?;
            subscribers.push(sub);
        }
        Ok((
            Self {
                _subscribers: subscribers,
            },
            rx,
        ))
    }
}
