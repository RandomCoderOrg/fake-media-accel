#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 VIDEO EXPECTED_FRAMES [FFMPEG]" >&2
    exit 2
fi

input=$1
expected_frames=$2
ffmpeg=${3:-ffmpeg}
va_device=${FMA_VA_DEVICE:-${DISPLAY:-}}

case "$expected_frames" in
    ''|*[!0-9]*|0)
        echo "EXPECTED_FRAMES must be a positive integer" >&2
        exit 2
        ;;
esac
[ -r "$input" ] || {
    echo "cannot read input: $input" >&2
    exit 2
}
[ -n "$va_device" ] || {
    echo "set DISPLAY or FMA_VA_DEVICE to a VA display" >&2
    exit 2
}
for command in "$ffmpeg" sed cmp diff mktemp wc tail tr; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done

if [ -n "${FMA_VERIFY_WORK:-}" ]; then
    work=$FMA_VERIFY_WORK
    mkdir -p "$work"
    rm -f "$work/software.checksums" "$work/hardware.checksums" \
        "$work/software.log" "$work/hardware.log"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/fma-ffmpeg-verify.XXXXXX")
    trap 'rm -rf "$work"' EXIT INT TERM
fi
software=$work/software.checksums
hardware=$work/hardware.checksums
software_log=$work/software.log
hardware_log=$work/hardware.log

"$ffmpeg" -hide_banner -nostats -i "$input" -vf format=nv12,showinfo \
    -fps_mode passthrough -f rawvideo -y /dev/null \
    >/dev/null 2>"$software_log"
sed -n \
    's/.*s:\([0-9][0-9]*x[0-9][0-9]*\).*plane_checksum:\[\([^]]*\)\].*/\1 \2/p' \
    "$software_log" > "$software"

FMA_VA_METRICS=1 "$ffmpeg" -hide_banner -nostats \
    -vaapi_device "$va_device" \
    -hwaccel vaapi -hwaccel_output_format vaapi -i "$input" \
    -vf hwdownload,format=nv12,showinfo -fps_mode passthrough \
    -f rawvideo -y /dev/null \
    >/dev/null 2>"$hardware_log"
sed -n \
    's/.*s:\([0-9][0-9]*x[0-9][0-9]*\).*plane_checksum:\[\([^]]*\)\].*/\1 \2/p' \
    "$hardware_log" > "$hardware"

frames=$(wc -l < "$hardware" | tr -d ' ')
[ "$frames" -eq "$expected_frames" ] || {
    echo "frame mismatch: expected=$expected_frames actual=$frames" >&2
    exit 1
}
if ! cmp -s "$software" "$hardware"; then
    diff -u "$software" "$hardware" || true
    echo "VA output differs from software decoding" >&2
    exit 1
fi

echo "dynamic VA output matches: $frames frames"
sed -n '/fma-va-metrics/p' "$hardware_log" | tail -n 1
