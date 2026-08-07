#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 VIDEO [hardware|software|both]" >&2
    exit 2
fi

input=$1
mode=${2:-both}
case "$mode" in
    hardware|software|both) ;;
    *) echo "invalid mode: $mode" >&2; exit 2 ;;
esac

for command in cvlc ffprobe python3 awk grep mktemp; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
[ -r "$input" ] || {
    echo "cannot read input: $input" >&2
    exit 2
}

duration=$(ffprobe -v error -show_entries format=duration \
    -of default=noprint_wrappers=1:nokey=1 "$input" | head -n 1)
timeout_seconds=$(awk -v duration="${duration:-0}" \
    'BEGIN { value = int(duration + 20.999); print value < 30 ? 30 : value }')
work=$(mktemp -d "${TMPDIR:-/tmp}/fma-vlc-benchmark.XXXXXX")
script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
measure=${FMA_MEASURE:-$script_dir/fma-measure.py}
[ -r "$measure" ] || {
    echo "cannot read measurement helper: $measure" >&2
    exit 2
}
daemon_pid=${FMA_DAEMON_PID:-}
runs=${FMA_RUNS:-1}
case "$runs" in
    ''|*[!0-9]*|0) echo "FMA_RUNS must be a positive integer" >&2; exit 2 ;;
esac
clock_ticks=$(getconf CLK_TCK 2>/dev/null || echo 100)

process_ticks() {
    if [ -n "$daemon_pid" ] && [ -r "/proc/$daemon_pid/stat" ]; then
        awk '{ print $14 + $15 }' "/proc/$daemon_pid/stat"
    else
        echo 0
    fi
}

metric() {
    key=$1
    file=$2
    awk -F= -v key="$key" '$1 == key { print $2; exit }' "$file"
}

run_case() {
    label=$1
    hardware=$2
    run=$3
    log="$work/$label-$run.log"
    timing="$work/$label-$run.time"
    ticks_before=$(process_ticks)

    set +e
    if [ -n "${FMA_APP_USER:-}" ] && [ "$(id -u)" -eq 0 ]; then
        python3 "$measure" "$timing" "$timeout_seconds" \
            runuser -u "$FMA_APP_USER" -- env \
            FMA_VA_METRICS=1 cvlc -I dummy -vv --play-and-exit \
            --no-video-title-show --no-audio --avcodec-hw="$hardware" \
            "$input" >"$log" 2>&1
    else
        python3 "$measure" "$timing" "$timeout_seconds" \
            env FMA_VA_METRICS=1 cvlc -I dummy -vv \
            --play-and-exit --no-video-title-show --no-audio \
            --avcodec-hw="$hardware" "$input" >"$log" 2>&1
    fi
    status=$?
    set -e

    ticks_after=$(process_ticks)
    daemon_ticks=$((ticks_after - ticks_before))
    wall=$(metric wall_s "$timing")
    user=$(metric user_s "$timing")
    system_cpu=$(metric system_s "$timing")
    maxrss=$(metric maxrss_kib "$timing")
    late=$(grep -c 'too late to be displayed' "$log" || true)
    drops=$(grep -c 'late frames, dropping frame' "$log" || true)
    vaapi=$(grep -c 'using hw decoder module.*vaapi' "$log" || true)
    glconv=$(grep -c 'using glconv module.*vaapi_x11' "$log" || true)
    values=$(awk -v user="$user" -v system_cpu="$system_cpu" \
        -v ticks="$daemon_ticks" -v hz="$clock_ticks" \
        'BEGIN {
            app = user + system_cpu;
            daemon = hz > 0 ? ticks / hz : 0;
            printf "app_cpu_s=%.3f daemon_cpu_s=%.3f total_cpu_s=%.3f", \
                app, daemon, app + daemon
        }')
    printf 'result mode=%s run=%s exit=%s wall_s=%s %s maxrss_kib=%s late=%s drops=%s vaapi=%s glconv_vaapi=%s\n' \
        "$label" "$run" "$status" "$wall" "$values" "$maxrss" "$late" \
        "$drops" "$vaapi" "$glconv"
    grep -E 'using (glconv|hw decoder) module|fma-va-metrics' "$log" || true
}

echo "input=$input duration_s=$duration timeout_s=$timeout_seconds runs=$runs logs=$work"
run=1
while [ "$run" -le "$runs" ]; do
    if [ "$mode" = both ] && [ $((run % 2)) -eq 0 ]; then
        run_case software none "$run"
        run_case hardware vaapi "$run"
    else
        if [ "$mode" = hardware ] || [ "$mode" = both ]; then
            run_case hardware vaapi "$run"
        fi
        if [ "$mode" = software ] || [ "$mode" = both ]; then
            run_case software none "$run"
        fi
    fi
    run=$((run + 1))
done
