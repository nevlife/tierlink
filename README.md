# tierlink

Vehicle-to-server link with two tiers. Remote control (motion commands, the operator's video) is
sent first and dropped when late. Monitoring (telemetry, events, snapshots) uses what the link has
left and is thinned or delayed first when the queue grows.

| Crate | Role |
| --- | --- |
| `crates/ratectl` | How many bits per second the link can take now: delay tracking from acknowledgements, `RateController` with `Fixed` and `Aimd` |

## Build and test

```bash
cargo test
cargo clippy --all-targets -- -D warnings
cargo fmt --check
```
