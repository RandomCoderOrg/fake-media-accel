#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 INPUT.ivf FMA-visible.nv12 FRAME-INFO.csv" >&2
    exit 2
fi

input=$1
decoded=$2
frame_info=$3

for command in ffmpeg dd sed cmp diff mktemp tail wc; do
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

last_record=$(tail -n 1 "$frame_info")
IFS=, read -r _ last_offset last_bytes _ _ _ <<EOF
$last_record
EOF
expected_file_bytes=$((last_offset + last_bytes))
actual_file_bytes=$(wc -c < "$decoded" | tr -d ' ')
[ "$actual_file_bytes" -eq "$expected_file_bytes" ] || {
    echo "decoded output has $actual_file_bytes bytes; expected $expected_file_bytes" >&2
    exit 1
}

exec 3< "$decoded"
cursor=0
tail -n +2 "$frame_info" | while IFS=, read -r frame offset bytes width height _stride; do
    [ "$offset" -eq "$cursor" ] || {
        echo "frame $frame starts at $offset; expected $cursor" >&2
        exit 1
    }
    chroma_row=$((((width + 1) / 2) * 2))
    chroma_rows=$(((height + 1) / 2))
    expected=$((width * height + chroma_row * chroma_rows))
    if [ "$bytes" -ne "$expected" ]; then
        echo "frame $frame has $bytes bytes; expected $expected" >&2
        exit 1
    fi
    checksum=$(
        dd bs="$bytes" count=1 <&3 2>/dev/null |
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
    cursor=$((cursor + bytes))
done > "$hardware"

if ! cmp -s "$software" "$hardware"; then
    diff -u "$software" "$hardware" || true
    echo "decoded dynamic output mismatch" >&2
    exit 1
fi

frames=$(wc -l < "$hardware" | tr -d ' ')
echo "decoded dynamic output matches: $frames frames"
