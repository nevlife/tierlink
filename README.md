# tierlink

Vehicle-to-server link with two tiers. Remote control (motion commands, the operator's video) is
sent first and dropped when late. Monitoring (telemetry, events, snapshots) uses what the link has
left and is thinned or delayed first when the queue grows.

A C++ library in an `ament_cmake` package. It depends on Zenoh (`zenoh_cpp_vendor`) and not on
`rclcpp`, so nodes and plain programs link it alike.

| Header                         | Role                                                                                                 |
| ------------------------------ | ---------------------------------------------------------------------------------------------------- |
| `tierlink/rate_controller.hpp` | `RateController` with `Fixed` and `Aimd`: how many bits per second the link can take now             |
| `tierlink/delay_tracker.hpp`   | Queue delay, stall and bytes in flight from sent items and acknowledgements                          |
| `tierlink/stream.hpp`          | Stream declaration (tier, delivery) and the pending-data rule per delivery                           |
| `tierlink/scheduler.hpp`       | The tier scheduler, and the monitoring allowance that the teleop process publishes                   |
| `tierlink/wire.hpp`            | The data header, acknowledgements, and gap tracking on the receiver                                  |
| `tierlink/zenoh_link.hpp`      | `Uplink` sends one tier's streams over its own session; `Receiver` delivers both tiers on the server |
| `tierlink/control.hpp`         | Small control messages in either direction, and the server's session epochs                          |

Delivery is one of three:

| Delivery   | Rule                                                                                                                                                                                                                          |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Latest`   | Only the newest value is kept                                                                                                                                                                                                 |
| `Reliable` | Values are kept in order up to a capacity; the oldest is dropped when full                                                                                                                                                    |
| `Frame`    | Frames wait up to `max_delay`, then are dropped. For inter-frame codecs a drop breaks decoding, so frames are discarded until a keyframe and the encoder is asked for one; a keyframe also clears the frames queued before it |

Each tier has its own Zenoh session, so its own TCP connection. Keys are
`<prefix>/<tier>/data/<stream>`, `<prefix>/<tier>/ack` and `<prefix>/<tier>/keyframe`. The
sequence numbers, send time and keyframe flag travel in the Zenoh attachment, so the payload is
exactly what the codec produced. Sessions are opened without scouting and connect only to the
given endpoints.

Zenoh drops the newest message when its queue is full and reports success, so a dropped
inter-frame would break the picture silently. The receiver therefore requests a keyframe when a
stream has a gap, and repeats the request on later arrivals until a keyframe comes. The encoder
asks `take_keyframe_request` before each frame. A teleop session shortens Zenoh's queue with
`kShortQueue`.

Control messages (motion commands, arm requests, vehicle state) use `ControlPublisher` and
`ControlSubscriber` on `<prefix>/<tier>/ctl/<name>`. They are not paced: they are tiny and their
sender owns their rate. Session epochs come from `EpochIssuer` on `<prefix>/teleop/session`; an
operator asks with `request_epoch`. Epochs only grow, across restarts of the issuer as well.

## Server

`tierlink_server` is a Zenoh router for vehicles and operators, the receiver of both tiers and the
epoch issuer. It prints per-stream counts every second and can write a stream to a file; an H.265
stream written this way plays as is.

```bash
tierlink_server --listen tcp/0.0.0.0:7447 --prefix scv/r01 --dump front_cam=/tmp/front.h265
```

## Build and test

```bash
colcon build --symlink-install --packages-select tierlink
colcon test --packages-select tierlink
colcon test-result --test-result-base build/tierlink
```
