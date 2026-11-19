#!/usr/bin/env bash
# run.sh PROFILE ESTIMATOR MONITOR SECONDS OUT
#   PROFILE    a file in profiles/ (netem on the vehicle's uplink)
#   ESTIMATOR  aimd | gcc  (tierlink_edge --estimator)
#   MONITOR    aimd | ledbat  (the monitor tier's own controller)
# The server and the vehicle run in two containers of the tierlink-bench image on one docker
# network; the vehicle's eth0 is shaped. TIERLINK_BIN is the directory with tierlink_bench and
# tierlink_edge (install/tierlink/lib/tierlink); $HOME and /opt are mounted read-only so their
# libraries resolve. Writes OUT/recv.txt, OUT/edge.txt and OUT/shape.txt, and with CAPTURE=1 the
# vehicle's packets (first 200 bytes) to OUT/cap.pcap for zcap.py.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
profile=$here/profiles/$1
estimator=$2
monitor=$3
seconds=$4
out=$(mkdir -p "$5" && cd "$5" && pwd)
bin=${TIERLINK_BIN:?set TIERLINK_BIN to the directory with tierlink_bench and tierlink_edge}
params=${PARAMS:-$here/../../config/params.yaml}
net=tierlink-bench
docker network inspect $net >/dev/null 2>&1 || docker network create $net >/dev/null
mounts=(-v "$HOME:$HOME:ro" -v /opt:/opt:ro -v "$here:$here:ro" -v "$out:$out")
params=$(readlink -f "$params")
mounts+=(-v "$params:$params:ro")

server=$(docker run -d --rm --network $net "${mounts[@]}" tierlink-bench bash -c \
  "$bin/tierlink_bench recv --params $params --listen tcp/0.0.0.0:7448 --listen udp/0.0.0.0:7449 \
   --seconds $((seconds + 8)) > $out/recv.txt 2>&1")
trap 'docker rm -f "$server" >/dev/null 2>&1 || true' EXIT
ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$server")
sleep 1
docker run --rm --network $net --cap-add NET_ADMIN "${mounts[@]}" tierlink-bench bash -c "
  ${CAPTURE:+tcpdump -i eth0 -s 200 -w $out/cap.pcap 2>/dev/null &}
  $here/shape.sh $profile $((seconds + 4)) > $out/shape.txt 2>&1 &
  $bin/tierlink_edge --params $params --listen tcp/127.0.0.1:7447 \
    --connect 'tcp/$ip:7448?rel=1' --connect 'udp/$ip:7449?rel=0' --estimator $estimator \
    > $out/edge.txt 2>&1 &
  sleep 2
  $bin/tierlink_bench send --params $params --connect tcp/127.0.0.1:7447 --seconds $seconds \
    --monitor $monitor
  kill $(jobs -p) 2>/dev/null; sleep 1; true"
docker wait "$server" >/dev/null 2>&1 || true
