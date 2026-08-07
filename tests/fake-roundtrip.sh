#!/bin/sh
set -eu

daemon="$1"
decoder="$2"
socket="$3"
input="$4"
output="$5"

"$daemon" "$socket" --once &
daemon_pid=$!
trap 'kill "$daemon_pid" 2>/dev/null || true' EXIT INT TERM

attempt=0
while [ ! -S "$socket" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || exit 1
    sleep 0.01
done

result=$("$decoder" --codec hevc --repeat 3 "$socket" "$input" 64 64 30 "$output")
printf '%s\n' "$result"
case "$result" in
    *"repeats=3 packets=3 frames=3 "*) ;;
    *) exit 1 ;;
esac
wait "$daemon_pid"
trap - EXIT INT TERM
test -s "$output"
