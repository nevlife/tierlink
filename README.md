# tierlink

Vehicle-to-server link with two tiers. Remote control (motion commands, the operator's video) is
sent first and dropped when late. Monitoring (telemetry, events, snapshots) uses what the link has
left and is thinned or delayed first when the queue grows.

A C++ library in an `ament_cmake` package. It depends on Zenoh (`zenoh_cpp_vendor`) and yaml-cpp and
not on `rclcpp`, so nodes and plain programs link it alike. The programs that run off the vehicle
are Rust and need no ROS 2:

| Directory   | Runs on      | Role                                                                                   |
| ----------- | ------------ | -------------------------------------------------------------------------------------- |
| `wire/`     | -            | Crate `tierlink_wire`: the data header, reassembly with FEC, acks, catalog and history |
| `server/`   | the server   | `fleetd`, the receiver of the monitor tier                                             |
| `operator/` | the operator | The desktop operator: video, vehicle state, drive input                                |

| Header                         | Role                                                                                                                    |
| ------------------------------ | ----------------------------------------------------------------------------------------------------------------------- |
| `tierlink/rate_controller.hpp` | `RateController` with `Fixed`, `Aimd` and `Ledbat`: how many bits per second, or bytes in flight, the link can take now |
| `tierlink/delay_tracker.hpp`   | Queue delay, stall, loss and bytes in flight from sent items and acknowledgements                                       |
| `tierlink/trendline.hpp`       | GCC's overuse detector: whether the queue delay is growing, from the arrival times                                      |
| `tierlink/stream.hpp`          | Stream declaration (tier, delivery) and the pending-data rule per delivery                                              |
| `tierlink/scheduler.hpp`       | The tier scheduler                                                                                                      |
| `tierlink/link_manager.hpp`    | One link estimate per vehicle: Uplink reports, `LinkManager`, and `Budgeted` that follows a budget                      |
| `tierlink/wire.hpp`            | The data header, acknowledgements, and gap tracking on the receiver                                                     |
| `tierlink/zenoh_link.hpp`      | `Uplink` sends one tier's streams over its own session; `Receiver` delivers both tiers on the server                    |
| `tierlink/control.hpp`         | Small control messages in either direction, and the server's session epochs                                             |
| `tierlink/teleop_messages.hpp` | Teleop control messages: arm request, motion command, stop and arm state                                                |
| `tierlink/params.hpp`          | The parameter file (`config/params.yaml`) for the edge, the server and the client                                       |

Delivery is one of three:

| Delivery   | Rule                                                                                                                                                                                                                          |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Latest`   | Only the newest value is kept                                                                                                                                                                                                 |
| `Reliable` | Values are kept in order up to a capacity; the oldest is dropped when full                                                                                                                                                    |
| `Frame`    | Frames wait up to `max_delay`, then are dropped. For inter-frame codecs a drop breaks decoding, so frames are discarded until a keyframe and the encoder is asked for one; a keyframe also clears the frames queued before it |

Keys are `<prefix>/<tier>/data/<stream>`, `<prefix>/<tier>/ack` and `<prefix>/<tier>/keyframe`.
`Frame` streams and streams with `best_effort` are published best effort and everything else
reliable, so behind a router with a TCP and a UDP link (`?rel=1`, `?rel=0`) they take UDP and a
lost packet never holds later ones. The sequence numbers, send time, keyframe flag and piece
numbers travel in the 30-byte Zenoh attachment, so the payload is exactly what the codec produced.
Sessions are opened without scouting and connect only to the given endpoints.

A stream with `fec_group` sends one XOR parity piece after every `fec_group` pieces of an item,
and the receiver rebuilds one lost piece per group without a resend or a keyframe. It needs pieces
that fit one UDP datagram (`chunk_bytes` of the stream, `link.frame_chunk_bytes` of the
parameter file), because Zenoh loses a fragmented message with any of its fragments. The parity is
paced by the same budget, so an encoder aims at `budget / (1 + Uplink::parity_ratio)`.

Acknowledgements carry the receive time in Unix ns. The sender subtracts the minimum one-way delay,
so the offset to its own clock cancels; a second operator or a restarted one keeps that offset as
long as the receivers' clocks agree (NTP).

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

Every `Uplink` reports what it sent, what was acknowledged and lost, its delay and the delay
gradient on `<prefix>/link/report/<tier>` every `report_period`. `LinkManager` turns the reports of
all processes on the vehicle into one link rate with one of two estimators:

| Estimator | Rule                                                                                                                                                                                                                                                                                                         |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `aimd`    | On a queue delay above a threshold, a fraction of what was delivered; a step up after the link stayed clear for a probe period, only while the senders use most of the rate                                                                                                                                  |
| `gcc`     | GCC (WebRTC): the trendline's overuse cuts to a fraction of what was delivered, normal growth is multiplicative far from the last overuse and additive near it, loss above 10 % cuts. Teleop's gradient counts while teleop runs, because monitoring's TCP delay also grows in the vehicle's own send buffer |

It then splits the rate into a budget per tier
on `<prefix>/link/budget/<tier>`: teleop the most it sent over the last `teleop_usage_window_ms`
times `teleop_headroom` (1.25), between its minimum and maximum, so what a still scene leaves
unused goes to monitoring; monitoring the rest but never under its floor, and only its floor while
teleop runs and the queue is long. A tier counts
only while it was acknowledged recently, so video with no operator watching does not read as
congestion. A process wraps its own controller in `Budgeted`, which follows the budget while it is
fresh and falls back to the own controller without one.

Monitoring can wrap `Ledbat` (RFC 6817) as its own controller: a window of bytes in flight that
shrinks once the queue delay passes a target, so the TCP link's send buffer and the bottleneck
queue stay short. `Budgeted` keeps that window while it follows the budget's rate.

`tierlink_edge` runs on the vehicle: the Zenoh router that the vehicle's processes connect to on
the host, holding the links to the server, and the link manager. Its values are in the
`link` and `edge` sections of [config/params.yaml](config/params.yaml); the flags replace the
file's endpoints, prefix and estimator.

```bash
tierlink_edge --params config/params.yaml --prefix scv/r01 --listen tcp/127.0.0.1:7447 \
  --connect "tcp/<server>:<port>?rel=1" --connect "udp/<server>:<port>?rel=0" [--estimator aimd|gcc]
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

## Operator

`operator/` is a desktop operator in Rust (`cargo run --release -- --connect
tcp/<server>:7447 --prefix scv/r01 --robot r01`). It shows the front camera, the vehicle's state
and the link, and sends the drive input (hold Space with W/S/A/D, or RB with the sticks), arm, stop,
e-stop, e-stop reset and the mux mode (a `cmd_mode` command to the vehicle's monitor tier). The
dead-man counts as released while no frame arrived for 500 ms. The link panel shows half the round
trip from the vehicle to the server and from the server to the operator.

The fleet panel lists fleetd's vehicles (`fleet/vehicles`) with their live processes; `drive`
switches to another vehicle while this one is disarmed. The `History` window asks fleetd for one
stream's last minutes (`<prefix>/history/<stream>`) and draws the speed of an odometry stream.

`tierlink_teleop` is a debugging operator without video:

```bash
tierlink_teleop tcp/<server>:7447 scv/r01 r01 0.3 3   # arm, drive at 0.3 m/s for 3 s, stop
```

## Measurement

[tools/bench](tools/bench) runs one vehicle's traffic (video, point cloud, telemetry, arm state)
through `tierlink_edge` to a local server in two containers, with netem on the vehicle's uplink,
and compares estimators on the same profile. See its README.

## Build and test

```bash
colcon build --symlink-install --packages-select tierlink
colcon test --packages-select tierlink
colcon test-result --test-result-base build/tierlink
(cd wire && cargo test) && (cd server && cargo test) && (cd operator && cargo test)
```
