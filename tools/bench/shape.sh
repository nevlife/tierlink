#!/usr/bin/env bash
# shape.sh PROFILE SECONDS: replays the "up" lines of PROFILE ("<second> up <netem args>") on eth0,
# looping until SECONDS. Prints the start time first, for lining up with the receiver's log.
set -eu
start=$(date +%s.%N)
echo "start $start"
end=$(awk "BEGIN {printf \"%.3f\", $start + $2}")
gw=$(ip route | awk '/default/ {print $3}')
while awk "BEGIN {exit !($(date +%s.%N) < $end)}"; do
  last=0
  while read -r at dir rest; do
    case $at in "#"* | "") continue ;; esac
    [ "$dir" = up ] || continue
    sleep "$(awk "BEGIN {print $at - $last}")"
    last=$at
    tc qdisc replace dev eth0 root netem $rest
    # netem keeps the old slot's dequeue timer; one datagram wakes the queue.
    echo >"/dev/udp/$gw/9" || true
  done <"$1"
  [ "$last" = 0 ] && exec sleep "$2"
done
