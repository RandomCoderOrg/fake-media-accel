#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 INPUT.ivf FMA-visible.nv12 FRAME-INFO.csv" >&2
    exit 2
fi

input=$1
decoded=$2
frame_info=$3

for command in ffmpeg dd sed cmp diff mktemp; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done

software=$(mktemp "${TMPDIR:-/tmp}/fma-software-checksums.XXXXXX")
hardware=$(mktemp "${TMPDIR:-/tmp}/fma-hardware-checksums.XXXXXX")
trap 'rm -f "$software" "$hardware"' EXIT INT TERM

ffmpeg -hide_banner -i "$input" -vf format=yuv420p,showinfo -f null - \
    2>&1 | sed -n \
    's/.*s:\([0-9][0-9]*x[0-9][0-9]*\).*plane_checksum:\[\([^]]*\)\].*/\1 \2/p' \
    > "$software"

tail -n +2 "$frame_info" | while IFS=, read -r frame offset bytes width height stride; do
    expected=$((width * height * 3 / 2))
    if [ "$bytes" -ne "$expected" ]; then
        echo "frame $frame has $bytes bytes; expected $expected" >&2
        exit 1
    fi
    checksum=$(
        dd if="$decoded" bs=1 skip="$offset" count="$bytes" 2>/dev/null |
            ffmpeg -hide_banner -f rawvideo -pixel_format nv12 \
                -video_size "${width}x${height}" -i pipe:0 \
                -vf format=yuv420p,showinfo -f null - 2>&1 |
            sed -n \
                's/.*s:\([0-9][0-9]*x[0-9][0-9]*\).*plane_checksum:\[\([^]]*\)\].*/\1 \2/p'
    )
    [ -n "$checksum" ] || {
        echo "could not checksum frame $frame" >&2
        exit 1
    }
    printf '%s\n' "$checksum"
done > "$hardware"

if ! cmp -s "$software" "$hardware"; then
    diff -u "$software" "$hardware" || true
    echo "decoded dynamic output mismatch" >&2
    exit 1
fi

frames=$(wc -l < "$hardware" | tr -d ' ')
echo "decoded dynamic output matches: $frames frames"
