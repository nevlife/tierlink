# tierlink

Vehicle-to-server link with two tiers. Remote control (motion commands, the operator's video) is
sent first and dropped when late. Monitoring (telemetry, events, snapshots) uses what the link has
left and is thinned or delayed first when the queue grows.

| Crate | Role |
| --- | --- |
| `crates/ratectl` | How many bits per second the link can take now: delay tracking from acknowledgements, `RateController` with `Fixed` and `Aimd` |
| `crates/link` | Stream declarations (tier, delivery), pending-data rules per delivery, the tier scheduler, and the monitoring allowance that the teleop process publishes |
| `crates/zenoh` | Zenoh binding. `Uplink` sends one tier's streams over its own session and feeds acknowledgements to the controller; `Receiver` delivers both tiers on the server and acknowledges each sample |

Each tier has its own Zenoh session, so its own TCP connection. Keys are
`<prefix>/<tier>/data/<stream>` and `<prefix>/<tier>/ack`. The sequence number, send time and
keyframe flag travel in the Zenoh attachment, so the payload is exactly what the codec produced.
Sessions are opened without scouting and connect only to the given endpoints.

Zenoh drops the newest message when its queue is full and reports success, so a dropped
inter-frame would break the picture silently. Each item therefore carries a per-stream sequence;
the receiver requests a keyframe on `<prefix>/<tier>/keyframe` when it sees a gap, and repeats the
request on later arrivals until a keyframe comes. The encoder asks `take_keyframe_request` before
each frame. A teleop session can also shorten Zenoh's queue with `Tuning`.

Delivery is one of three:

| Delivery | Rule |
| --- | --- |
| `latest` | Only the newest value is kept |
| `reliable` | Values are kept in order up to a capacity; the oldest is dropped when full |
| `frame` | An unsent frame is replaced by a newer one. For inter-frame codecs the replaced frame breaks decoding, so frames are discarded until a keyframe and the encoder is asked for one |

## Build and test

```bash
cargo test
cargo clippy --all-targets -- -D warnings
cargo fmt --check
```
