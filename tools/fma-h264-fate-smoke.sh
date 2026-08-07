#!/bin/sh
set -eu

inspector=${1:-./build/fma-h264-inspect}
corpus=${2:-./build/h264-fate-smoke}
base=https://fate-suite.ffmpeg.org/h264-conformance

for command in curl ffprobe sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
test -x "$inspector" || {
    echo "inspector is not executable: $inspector" >&2
    exit 2
}
mkdir -p "$corpus"

failed=0
check_sample() {
    label=$1
    file=$2
    expected_hash=$3
    expected_frames=$4
    path=$corpus/$file
    mkdir -p "$(dirname "$path")"
    if [ ! -f "$path" ]; then
        curl -fL --connect-timeout 10 --max-time 180 --retry 2 \
            "$base/$file" -o "$path"
    fi
    actual_hash=$(sha256sum "$path" | awk '{print $1}')
    if [ "$actual_hash" != "$expected_hash" ]; then
        echo "checksum mismatch: $file" >&2
        failed=1
        return
    fi
    stats=$("$inspector" "$path")
    units=$(printf '%s\n' "$stats" |
        sed -n 's/.*vcl_units=\([0-9][0-9]*\).*/\1/p')
    frames=$(ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$path")
    matches=false
    if [ "$units" = "$frames" ] && [ "$frames" = "$expected_frames" ]; then
        matches=true
    else
        failed=1
    fi
    printf '%s,%s,%s,%s,%s\n' "$label" "$units" "$frames" \
        "$expected_frames" "$matches"
}

echo 'sample,fma_access_units,ffmpeg_frames,expected_frames,matches'
check_sample aud AUD_MW_E.264 \
    173b75a2883f3208894877549362f118721f45633bf08007396703ef2c358987 100
check_sample baseline BA1_FT_C.264 \
    1bd3abeea6a5612602556a455cd2c59f235cba23e09a1a5556641296324c0bb7 299
check_sample cabac CABA3_TOSHIBA_E.264 \
    5051192d0fa49c938048791a31b8de7110b5f0d2b1ceb791b57bcc172572e813 300
check_sample mbaff CAMP_MOT_MBAFF_L31.26l \
    ecf283ed5c6b60a80fcabbf33f8c8b107eadd991890a3e44fd22bad3e88f5889 30
check_sample multi_ref MR1_BT_A.h264 \
    20dc67331c81adcf40048bb37357883a69b3ab002b0927e599f43d86be9c3d8b 62
check_sample high FRext/HPCA_BRCM_C.264 \
    0e0b8fedcd50c3659f35de4a462c43fe3625b2d0d4165759fd9836cbb87e914d 300

exit "$failed"
