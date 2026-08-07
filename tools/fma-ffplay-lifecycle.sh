#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 VIDEO [hardware|software]" >&2
    exit 2
fi

input=$1
mode=${2:-hardware}
case "$mode" in
    hardware|software) ;;
    *) echo "invalid mode: $mode" >&2; exit 2 ;;
esac
ffplay=${FMA_FFPLAY:-ffplay}
ffprobe=${FMA_FFPROBE:-ffprobe}
for command in "$ffplay" "$ffprobe" awk grep head mktemp pgrep python3 \
        sed sleep tail tr xdotool; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
[ -r "$input" ] || {
    echo "cannot read input: $input" >&2
    exit 2
}

duration=$($ffprobe -v error -show_entries format=duration \
    -of default=noprint_wrappers=1:nokey=1 "$input" | head -n 1)
if ! awk -v duration="${duration:-0}" 'BEGIN { exit !(duration >= 12) }'; then
    echo "lifecycle input must be at least 12 seconds" >&2
    exit 2
fi
timeout_seconds=$(awk -v duration="$duration" \
    'BEGIN { value = int(duration + 20.999); print value < 30 ? 30 : value }')
work=$(mktemp -d "${TMPDIR:-/tmp}/fma-ffplay-lifecycle.XXXXXX")
script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
measure=${FMA_MEASURE:-$script_dir/fma-measure.py}
[ -r "$measure" ] || {
    echo "cannot read measurement helper: $measure" >&2
    exit 2
}
log="$work/ffplay.log"
timing="$work/ffplay.time"
events="$work/events.log"
measure_pid=
ffplay_pid=

cleanup() {
    if [ -n "$ffplay_pid" ]; then
        kill -TERM -- "-$ffplay_pid" 2>/dev/null || true
    fi
    if [ -n "$measure_pid" ]; then
        kill "$measure_pid" 2>/dev/null || true
        wait "$measure_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

position() {
    tr '\r' '\n' < "$log" 2>/dev/null | awk \
        '/M-V:/ { for (i = 1; i <= NF; ++i) if ($i == "M-V:") { print $(i - 1); break } }' | \
        tail -n 1
}

event() {
    printf '%s\n' "$1" >> "$events"
}

if [ "$mode" = hardware ]; then
    python3 "$measure" "$timing" "$timeout_seconds" \
        env FMA_VA_METRICS=1 \
        "$ffplay" -hide_banner -loglevel verbose -autoexit -an \
        -hwaccel vaapi -vf hwdownload,format=yuv420p "$input" \
        >"$log" 2>&1 &
else
    python3 "$measure" "$timing" "$timeout_seconds" \
        "$ffplay" -hide_banner -loglevel verbose -autoexit -an "$input" \
        >"$log" 2>&1 &
fi
measure_pid=$!

window=
attempt=0
while [ "$attempt" -lt 100 ]; do
    ffplay_pid=$(pgrep -P "$measure_pid" | head -n 1 || true)
    if [ -n "$ffplay_pid" ]; then
        window=$(xdotool search --onlyvisible --pid "$ffplay_pid" 2>/dev/null | \
            head -n 1 || true)
    fi
    [ -n "$window" ] && break
    kill -0 "$measure_pid" 2>/dev/null || break
    sleep 0.05
    attempt=$((attempt + 1))
done

if [ -z "$window" ]; then
    event "window=missing"
    echo "FFplay window did not appear; logs=$work" >&2
    exit 1
fi
event "mode=$mode window=$window ffplay_pid=$ffplay_pid"

sleep 2
xdotool key --window "$window" p
sleep 0.25
pause_start=$(position)
pause_start=${pause_start:-0}
sleep 1
pause_end=$(position)
pause_end=${pause_end:-0}
event "pause_start_s=$pause_start pause_end_s=$pause_end"

xdotool key --window "$window" p
sleep 1
resume_end=$(position)
resume_end=${resume_end:-0}
event "resume_end_s=$resume_end"

seek_start=$resume_end
xdotool key --window "$window" Right
event "seek_start_s=$seek_start"

set +e
wait "$measure_pid"
status=$?
set -e
measure_pid=
ffplay_pid=
final=$(position)
final=${final:-0}
drops=$(tr '\r' '\n' < "$log" | \
    sed -n 's/.*fd= *\([0-9][0-9]*\).*/\1/p' | tail -n 1)
drops=${drops:-0}
event "final_s=$final drops=$drops exit=$status"

if ! awk -v start="$pause_start" -v end="$pause_end" \
    'BEGIN { delta = end - start; exit !(delta >= -0.10 && delta <= 0.20) }'; then
    echo "pause did not hold playback: start=$pause_start end=$pause_end" >&2
    status=1
fi
if ! awk -v paused="$pause_end" -v resumed="$resume_end" \
    'BEGIN { exit !(resumed >= paused + 0.50) }'; then
    echo "resume did not advance playback: paused=$pause_end resumed=$resume_end" >&2
    status=1
fi
if ! awk -v before="$seek_start" -v final="$final" \
    'BEGIN { exit !(final >= before + 5.0) }'; then
    echo "seek did not advance playback: before=$seek_start final=$final" >&2
    status=1
fi
if grep -qE 'Failed to open file|configure filtergraph|Failed to get pixel format' "$log"; then
    status=1
fi

cat "$events"
cat "$timing"
tr '\r' '\n' < "$log" | grep -E \
    'Creating a standalone|Initialized .* renderer|fma-va-metrics' | tail -n 5 || true
echo "logs=$work"
exit "$status"
