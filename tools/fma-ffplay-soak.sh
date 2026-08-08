#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 VIDEO [hardware|software] [LOOPS]" >&2
    exit 2
fi

input=$1
mode=${2:-hardware}
loops=${3:-10}
case "$mode" in
    hardware|software) ;;
    *) echo "invalid mode: $mode" >&2; exit 2 ;;
esac
case "$loops" in
    ''|*[!0-9]*|0) echo "LOOPS must be a positive integer" >&2; exit 2 ;;
esac

ffplay=${FMA_FFPLAY:-ffplay}
software_ffplay=${FMA_SOFTWARE_FFPLAY:-$ffplay}
ffprobe=${FMA_FFPROBE:-ffprobe}
for command in "$ffplay" "$software_ffplay" "$ffprobe" awk grep head \
        mktemp python3 sed tail tr; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
[ -r "$input" ] || {
    echo "cannot read input: $input" >&2
    exit 2
}

duration=$($ffprobe -v error -select_streams v:0 \
    -show_entries format=duration -of default=nw=1:nk=1 "$input" | head -n 1)
frames=$($ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$input" | head -n 1)
if ! awk -v duration="${duration:-0}" -v frames="${frames:-0}" \
    'BEGIN { exit !(duration > 0 && frames > 0) }'; then
    echo "input must report a positive duration and decoded frame count" >&2
    exit 2
fi

expected_frames=$((frames * loops))
timeout_seconds=$(awk -v duration="$duration" -v loops="$loops" \
    'BEGIN { value = int(duration * loops + 20.999); print value < 30 ? 30 : value }')
work=$(mktemp -d "${TMPDIR:-/tmp}/fma-ffplay-soak.XXXXXX")
script_dir=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
measure=${FMA_MEASURE:-$script_dir/fma-measure.py}
[ -r "$measure" ] || {
    echo "cannot read measurement helper: $measure" >&2
    exit 2
}
log="$work/ffplay.log"
timing="$work/ffplay.time"
daemon_pid=${FMA_DAEMON_PID:-}
clock_ticks=$(getconf CLK_TCK 2>/dev/null || echo 100)

process_ticks() {
    if [ -n "$daemon_pid" ] && [ -r "/proc/$daemon_pid/stat" ]; then
        awk '{ print $14 + $15 }' "/proc/$daemon_pid/stat"
    else
        echo 0
    fi
}

ticks_before=$(process_ticks)
set +e
if [ "$mode" = hardware ]; then
    if [ -n "${FMA_APP_USER:-}" ] && [ "$(id -u)" -eq 0 ]; then
        python3 "$measure" "$timing" "$timeout_seconds" \
            runuser -u "$FMA_APP_USER" -- env FMA_VA_METRICS=1 \
            "$ffplay" -hide_banner -loglevel verbose -autoexit -an \
            -loop "$loops" -hwaccel vaapi \
            -vf hwdownload,format=yuv420p "$input" >"$log" 2>&1
    else
        python3 "$measure" "$timing" "$timeout_seconds" \
            env FMA_VA_METRICS=1 \
            "$ffplay" -hide_banner -loglevel verbose -autoexit -an \
            -loop "$loops" -hwaccel vaapi \
            -vf hwdownload,format=yuv420p "$input" >"$log" 2>&1
    fi
else
    if [ -n "${FMA_APP_USER:-}" ] && [ "$(id -u)" -eq 0 ]; then
        python3 "$measure" "$timing" "$timeout_seconds" \
            runuser -u "$FMA_APP_USER" -- \
            "$software_ffplay" -hide_banner -loglevel verbose -autoexit -an \
            -loop "$loops" "$input" >"$log" 2>&1
    else
        python3 "$measure" "$timing" "$timeout_seconds" \
            "$software_ffplay" -hide_banner -loglevel verbose -autoexit -an \
            -loop "$loops" "$input" >"$log" 2>&1
    fi
fi
status=$?
set -e
ticks_after=$(process_ticks)

metrics=$(tr '\r' '\n' < "$log" | grep 'fma-va-metrics' | tail -n 1 || true)
metric() {
    key=$1
    printf '%s\n' "$metrics" | sed -n "s/.* $key=\\([^ ]*\\).*/\\1/p"
}
presented=$(tr '\r' '\n' < "$log" | awk \
    '/M-V:/ { for (i = 1; i <= NF; ++i) if ($i == "M-V:") { print $(i - 1); break } }' | \
    tail -n 1)
presented=${presented:-0}
drops=$(tr '\r' '\n' < "$log" | \
    sed -n 's/.*fd= *\([0-9][0-9]*\).*/\1/p' | tail -n 1)
drops=${drops:-0}
stored=$(metric stored)
contexts=$(metric contexts_created)
timed_out=$(sed -n 's/^timed_out=//p' "$timing")

if [ "$status" -eq 0 ] && [ "$timed_out" != 0 ]; then
    status=1
fi
if [ "$status" -eq 0 ] && grep -qE \
    'Failed to open file|configure filtergraph|Failed to get pixel format|Error while filtering' "$log"; then
    status=1
fi
if [ "$status" -eq 0 ] && ! awk -v presented="$presented" -v duration="$duration" \
    'BEGIN { minimum = duration - 0.10; if (minimum < 0) minimum = 0; exit !(presented >= minimum) }'; then
    status=1
fi
if [ "$status" -eq 0 ] && [ "$mode" = hardware ]; then
    if [ -z "$stored" ] || [ "$stored" -ne "$expected_frames" ] || \
       [ -z "$contexts" ] || [ "$contexts" -lt 1 ]; then
        status=1
    fi
    if [ -n "${FMA_EXPECT_CONTEXTS:-}" ] && \
       [ "$contexts" -ne "$FMA_EXPECT_CONTEXTS" ]; then
        status=1
    fi
fi

daemon_ticks=$((ticks_after - ticks_before))
daemon_cpu=$(awk -v ticks="$daemon_ticks" -v hz="$clock_ticks" \
    'BEGIN { printf "%.3f", hz > 0 ? ticks / hz : 0 }' </dev/null)
daemon_cpu=${daemon_cpu:-0.000}
printf 'result mode=%s exit=%s loops=%s frames_per_loop=%s expected_frames=%s stored=%s contexts=%s presented_s=%s drops=%s daemon_cpu_s=%s\n' \
    "$mode" "$status" "$loops" "$frames" "$expected_frames" \
    "${stored:-n/a}" "${contexts:-n/a}" "$presented" "$drops" "$daemon_cpu"
cat "$timing"
[ -n "$metrics" ] && printf '%s\n' "$metrics"
echo "logs=$work"
exit "$status"
