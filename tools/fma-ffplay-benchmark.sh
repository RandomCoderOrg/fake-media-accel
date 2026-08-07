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

ffplay=${FMA_FFPLAY:-ffplay}
software_ffplay=${FMA_SOFTWARE_FFPLAY:-$ffplay}
ffprobe=${FMA_FFPROBE:-ffprobe}
for command in "$ffplay" "$software_ffplay" "$ffprobe" python3 awk grep \
        head mktemp sed tail tr; do
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
timeout_seconds=$(awk -v duration="${duration:-0}" \
    'BEGIN { value = int(duration + 20.999); print value < 30 ? 30 : value }')
work=$(mktemp -d "${TMPDIR:-/tmp}/fma-ffplay-benchmark.XXXXXX")
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
benchmark_failed=0
results=${FMA_RESULTS:-}
if [ -n "$results" ]; then
    : > "$results"
fi

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
    run=$2
    log="$work/$label-$run.log"
    timing="$work/$label-$run.time"
    ticks_before=$(process_ticks)

    set +e
    if [ "$label" = hardware ]; then
        if [ -n "${FMA_APP_USER:-}" ] && [ "$(id -u)" -eq 0 ]; then
            python3 "$measure" "$timing" "$timeout_seconds" \
                runuser -u "$FMA_APP_USER" -- env FMA_VA_METRICS=1 \
                "$ffplay" -hide_banner -loglevel verbose -autoexit -an \
                -hwaccel vaapi -vf hwdownload,format=yuv420p "$input" \
                >"$log" 2>&1
        else
            python3 "$measure" "$timing" "$timeout_seconds" \
                env FMA_VA_METRICS=1 \
                "$ffplay" -hide_banner -loglevel verbose -autoexit -an \
                -hwaccel vaapi -vf hwdownload,format=yuv420p "$input" \
                >"$log" 2>&1
        fi
    else
        if [ -n "${FMA_APP_USER:-}" ] && [ "$(id -u)" -eq 0 ]; then
            python3 "$measure" "$timing" "$timeout_seconds" \
                runuser -u "$FMA_APP_USER" -- \
                "$software_ffplay" -hide_banner -loglevel verbose -autoexit \
                -an "$input" >"$log" 2>&1
        else
            python3 "$measure" "$timing" "$timeout_seconds" \
                "$software_ffplay" -hide_banner -loglevel verbose -autoexit \
                -an "$input" >"$log" 2>&1
        fi
    fi
    status=$?
    set -e

    ticks_after=$(process_ticks)
    daemon_ticks=$((ticks_after - ticks_before))
    wall=$(metric wall_s "$timing")
    user=$(metric user_s "$timing")
    system_cpu=$(metric system_s "$timing")
    maxrss=$(metric maxrss_kib "$timing")
    drops=$(tr '\r' '\n' < "$log" | \
        sed -n 's/.*fd= *\([0-9][0-9]*\).*/\1/p' | tail -n 1)
    drops=${drops:-0}
    presented=$(tr '\r' '\n' < "$log" | awk \
        '/M-V:/ { for (i = 1; i <= NF; ++i) if ($i == "M-V:") { print $(i - 1); break } }' | \
        tail -n 1)
    presented=${presented:-0}
    vaapi=$(tr '\r' '\n' < "$log" | \
        grep -cE 'Using hardware decoding \(vaapi\)|Creating a standalone vaapi device' || true)
    renderer=$(tr '\r' '\n' < "$log" | \
        sed -n 's/.*Initialized \([^ ]*\) renderer.*/\1/p' | tail -n 1)
    renderer=${renderer:-unknown}
    values=$(awk -v user="$user" -v system_cpu="$system_cpu" \
        -v ticks="$daemon_ticks" -v hz="$clock_ticks" \
        'BEGIN {
            app = user + system_cpu;
            daemon = hz > 0 ? ticks / hz : 0;
            printf "app_cpu_s=%.3f daemon_cpu_s=%.3f total_cpu_s=%.3f", \
                app, daemon, app + daemon
        }')
    case "$presented" in
        ''|*[!0-9.]*) status=1 ;;
        *)
            if ! awk -v presented="$presented" -v duration="$duration" \
                'BEGIN { exit !(duration > 0 && presented >= duration * 0.90) }'; then
                status=1
            fi
            ;;
    esac
    if grep -qE 'Failed to open file|configure filtergraph|Failed to get pixel format' "$log"; then
        status=1
    fi
    line=$(printf 'result mode=%s run=%s exit=%s wall_s=%s %s maxrss_kib=%s presented_s=%s drops=%s vaapi=%s renderer=%s' \
        "$label" "$run" "$status" "$wall" "$values" "$maxrss" \
        "$presented" "$drops" "$vaapi" "$renderer")
    printf '%s\n' "$line"
    if [ -n "$results" ]; then
        printf '%s\n' "$line" >> "$results"
    fi
    tr '\r' '\n' < "$log" | grep -E \
        'Creating a standalone|Using hardware decoding|Initialized .* renderer|fma-va-metrics' | \
        tail -n 6 || true
    if [ "$status" -ne 0 ]; then
        benchmark_failed=1
    fi
}

echo "input=$input duration_s=$duration timeout_s=$timeout_seconds runs=$runs logs=$work"
run=1
while [ "$run" -le "$runs" ]; do
    if [ "$mode" = both ] && [ $((run % 2)) -eq 0 ]; then
        run_case software "$run"
        run_case hardware "$run"
    else
        if [ "$mode" = hardware ] || [ "$mode" = both ]; then
            run_case hardware "$run"
        fi
        if [ "$mode" = software ] || [ "$mode" = both ]; then
            run_case software "$run"
        fi
    fi
    run=$((run + 1))
done

exit "$benchmark_failed"
