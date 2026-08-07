#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
    echo "usage: $0 INPUT.h264 FMA.nv12 WIDTH HEIGHT STRIDE" >&2
    exit 2
fi

input=$1
decoded=$2
width=$3
height=$4
stride=$5

fma_md5=$(
    ffmpeg -v error -f rawvideo -pix_fmt nv12 \
        -video_size "${stride}x${height}" -i "$decoded" \
        -vf "crop=${width}:${height}:0:0" -f md5 -
)
software_md5=$(
    ffmpeg -v error -i "$input" -pix_fmt nv12 -f md5 -
)

printf 'fma      %s\n' "$fma_md5"
printf 'software %s\n' "$software_md5"
test "$fma_md5" = "$software_md5" || {
    echo "decoded output mismatch" >&2
    exit 1
}
echo "decoded output matches"
