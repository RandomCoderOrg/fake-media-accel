#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 INPUT.ivf [FMA_FFMPEG] [SOFTWARE_FFMPEG]" >&2
    exit 2
fi

input=$1
hardware_ffmpeg=${2:-ffmpeg-fma}
software_ffmpeg=${3:-ffmpeg}
display=${DISPLAY:-:0}

for command in "$hardware_ffmpeg" "$software_ffmpeg" cmp mktemp wc; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done

hardware=$(mktemp "${TMPDIR:-/tmp}/fma-av1-va.XXXXXX")
software=$(mktemp "${TMPDIR:-/tmp}/fma-av1-software.XXXXXX")
trap 'rm -f "$hardware" "$software"' EXIT INT TERM

FMA_VA_METRICS=1 "$hardware_ffmpeg" -hide_banner -loglevel error \
    -vaapi_device "$display" -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$input" -vf hwdownload,format=nv12 -noautoscale \
    -f rawvideo -y "$hardware"

"$software_ffmpeg" -hide_banner -loglevel error -i "$input" \
    -pix_fmt nv12 -noautoscale -f rawvideo -y "$software"

hardware_bytes=$(wc -c < "$hardware" | tr -d ' ')
software_bytes=$(wc -c < "$software" | tr -d ' ')
if [ "$hardware_bytes" -ne "$software_bytes" ]; then
    echo "decoded size mismatch: hardware=$hardware_bytes software=$software_bytes" >&2
    exit 1
fi
if ! cmp -s "$hardware" "$software"; then
    echo "decoded AV1 output mismatch" >&2
    exit 1
fi

echo "AV1 VA output matches software: $hardware_bytes bytes"
