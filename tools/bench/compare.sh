#!/usr/bin/env bash
# compare.sh PROFILE SECONDS REPEATS OUT VARIANT...: runs every VARIANT (ESTIMATOR:MONITOR, such as
# gcc:ledbat) REPEATS times, alternating so that a change of the host or network over time hits
# every variant alike, then prints summary.py's table.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
profile=$1
seconds=$2
repeats=$3
out=$4
shift 4
runs=()
for i in $(seq "$repeats"); do
  for v in "$@"; do
    dir=$out/${v/:/-}-$i
    "$here/run.sh" "$profile" "${v%%:*}" "${v##*:}" "$seconds" "$dir" >/dev/null 2>&1
    runs+=("$dir")
  done
done
python3 "$here/summary.py" "$here/profiles/$profile" "${runs[@]}"
