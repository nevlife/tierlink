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
| `tierlink/scheduler.hpp`       | The tier scheduler                                                                                   |
| `tierlink/link_manager.hpp`    | One link estimate per vehicle: Uplink reports, `LinkManager`, and `Budgeted` that follows a budget   |
| `tierlink/wire.hpp`            | The data header, acknowledgements, and gap tracking on the receiver                                  |
| `tierlink/zenoh_link.hpp`      | `Uplink` sends one tier's streams over its own session; `Receiver` delivers both tiers on the server |
| `tierlink/control.hpp`         | Small control messages in either direction, and the server's session epochs                          |

Delivery is one of three:

| Delivery   | Rule                                                                                                                                                                                                                          |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Latest`   | Only the newest value is kept                                                                                                                                                                                                 |
| `Reliable` | Values are kept in order up to a capacity; the oldest is dropped when full                                                                                                                                                    |
| `Frame`    | Frames wait up to `max_delay`, then are dropped. For inter-frame codecs a drop breaks decoding, so frames are discarded until a keyframe and the encoder is asked for one; a keyframe also clears the frames queued before it |

Keys are `<prefix>/<tier>/data/<stream>`, `<prefix>/<tier>/ack` and `<prefix>/<tier>/keyframe`.
`Frame` streams are published best effort and everything else reliable, so behind a router with a
TCP and a UDP link (`?rel=1`, `?rel=0`) frames take UDP and a lost packet never holds later ones. The
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
sender owns their rate. Session epochs come from `EpochIssuer` on `<prefix>/teleop/session`, which the
vehicle runs; an operator asks with `request_epoch`. Epochs only grow, across restarts of the issuer as well.

## Link manager

Every `Uplink` reports what it sent, what was acknowledged and its delay on
`<prefix>/link/report/<tier>` every `report_period`. `LinkManager` turns the reports of all
processes on the vehicle into one link rate: on congestion a fraction of what was delivered, and a
step up only while the senders use most of the rate. It then splits the rate into a budget per tier
on `<prefix>/link/budget/<tier>`: teleop between its minimum and maximum, monitoring the rest but
never under its floor, and only its floor while teleop runs and the queue is long. A tier counts
only while it was acknowledged recently, so video with no operator watching does not read as
congestion. A process wraps its own controller in `Budgeted`, which follows the budget while it is
fresh and falls back to the own controller without one.

`tierlink_edge` runs on the vehicle: the Zenoh router that the vehicle's processes connect to on
the host, holding the links to the server, and the link manager.

```bash
tierlink_edge --prefix scv/r01 --listen tcp/127.0.0.1:7447 \
  --connect "tcp/<server>:<port>?rel=1" --connect "udp/<server>:<port>?rel=0"
```

## Server

`tierlink_server` is a Zenoh router for vehicles and operators and the receiver of both tiers. It
answers `<prefix>/server/ping`, so a vehicle and an operator can each time their hop to it with
`ping_server`. It prints per-stream counts every second and can write a stream to a file; an H.265
stream written this way plays as is.

```bash
tierlink_server --listen tcp/0.0.0.0:7447 --prefix scv/r01 --dump front_cam=/tmp/front.h265
```

`server/` holds `fleetd`, the server's receiver of the monitor tier: it acks, stores each vehicle's
streams as hourly MCAP files, answers `<prefix>/history/<stream>` and `fleet/vehicles`, and runs
behind a Zenoh router. `deploy/server/` starts both with docker compose. The design of the server,
the vehicle side and the operator, with the TCP and UDP measurements behind it, is in
[docs/design.md](docs/design.md).

## Build and test

```bash
colcon build --symlink-install --packages-select tierlink
colcon test --packages-select tierlink
colcon test-result --test-result-base build/tierlink
```
