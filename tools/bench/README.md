# bench

One vehicle's traffic through `tierlink_edge` to a local server, with netem on the vehicle's
uplink, for comparing link estimators and parameters on the same profile.

| File             | Role                                                                                                                                                  |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `bench_main.cpp` | `tierlink_bench send` (video sized to the teleop budget, point cloud, telemetry, arm state) and `recv` (the server: acks, one line per arrival)       |
| `run.sh`         | One run: the server and the vehicle in two containers of the `tierlink-bench` image on one docker network                                             |
| `compare.sh`     | Several variants (`estimator:monitor`), alternating, then `summary.py`                                                                                |
| `summary.py`     | Video delay, lost frames, rebuilt pieces, keyframes, state and telemetry delay; per profile segment the video's share of the netem rate and its delay |
| `shape.sh`       | Replays the `up` lines of a profile with `tc netem` on the vehicle's `eth0`                                                                           |
| `zcap.py`        | Reads a capture (`CAPTURE=1`): Zenoh batches per direction, link, reliability and priority                                                            |
| `profiles/`      | `steps` (3, 1, 0.5, 2 Mbit/s), `loss2` (2 % random loss), `lte_handover`, `tether`, `tether_bad`                                                      |

```bash
docker build -t tierlink-bench tools/bench
export TIERLINK_BIN=<install>/tierlink/lib/tierlink
tools/bench/compare.sh steps 84 3 /tmp/steps aimd:aimd gcc:aimd gcc:ledbat
PARAMS=my_params.yaml tools/bench/run.sh loss2 aimd aimd 30 /tmp/loss   # another parameter file
```

Both containers share the host's clock, so the one-way delays in `recv.txt` are absolute. Downlink
(acks, budgets) is not shaped.
