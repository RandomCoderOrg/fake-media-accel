#!/bin/sh
set -eu

daemon=$1
test_binary=$2
socket=$3

rm -f "$socket"
"$daemon" "$socket" &
daemon_pid=$!
trap 'kill "$daemon_pid" 2>/dev/null || true; rm -f "$socket"' EXIT INT TERM

attempt=0
while [ ! -S "$socket" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || exit 1
    sleep 0.01
done

"$test_binary" "$socket"
