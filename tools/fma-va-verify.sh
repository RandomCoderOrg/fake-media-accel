#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 VIDEO" >&2
    exit 2
fi

: "${DISPLAY:=:0}"
: "${FMA_SOCKET:=/tmp/fake-media-accel.sock}"
: "${LIBVA_DRIVER_NAME:=fma}"
: "${LIBVA_DRIVERS_PATH:?set LIBVA_DRIVERS_PATH to the fma driver directory}"

work=$(mktemp -d "${TMPDIR:-/tmp}/fma-va-verify.XXXXXX")
trap 'rm -r "$work"' EXIT INT TERM

hardware_md5=$(
    DISPLAY="$DISPLAY" FMA_SOCKET="$FMA_SOCKET" \
    LIBVA_DRIVER_NAME="$LIBVA_DRIVER_NAME" \
    LIBVA_DRIVERS_PATH="$LIBVA_DRIVERS_PATH" \
    ffmpeg -hide_banner -loglevel info -benchmark \
        -hwaccel vaapi -hwaccel_output_format vaapi \
        -hwaccel_device "$DISPLAY" -i "$1" -map 0:v:0 -an \
        -vf hwdownload,format=nv12 -f md5 - 2>"$work/hardware.log"
)

software_md5=$(
    ffmpeg -hide_banner -loglevel info -benchmark -i "$1" \
        -map 0:v:0 -an -pix_fmt nv12 -f md5 - 2>"$work/software.log"
)

echo "hardware $hardware_md5"
sed -n '/^bench:/p' "$work/hardware.log"
echo "software $software_md5"
sed -n '/^bench:/p' "$work/software.log"

if [ "$hardware_md5" != "$software_md5" ]; then
    echo "decoded output mismatch" >&2
    exit 1
fi

echo "decoded output matches"
