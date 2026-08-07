#!/bin/sh
set -eu

inspector=${1:-./build/fma-ivf-inspect}
corpus=${2:-./build/vp9-fate-smoke}
base=https://fate-suite.ffmpeg.org/vp9-test-vectors

for command in curl ffmpeg ffprobe sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
test -x "$inspector" || {
    echo "inspector is not executable: $inspector" >&2
    exit 2
}
mkdir -p "$corpus" "$corpus/ivf"

failed=0
check_sample() {
    label=$1
    file=$2
    expected_hash=$3
    expected_packets=$4
    expected_frames=$5
    source=$corpus/$file
    if [ ! -f "$source" ]; then
        curl -fL --connect-timeout 10 --max-time 180 --retry 2 \
            --max-filesize 5242880 "$base/$file" -o "$source"
    fi
    actual_hash=$(sha256sum "$source" | awk '{print $1}')
    if [ "$actual_hash" != "$expected_hash" ]; then
        echo "checksum mismatch: $file" >&2
        failed=1
        return
    fi

    case $file in
        *.ivf) ivf=$source ;;
        *)
            ivf=$corpus/ivf/${file%.webm}.ivf
            ffmpeg -v error -i "$source" -map 0:v:0 -c copy -copyinkf \
                -f ivf -y "$ivf"
            ;;
    esac
    stats=$($inspector "$ivf")
    ivf_packets=$(printf '%s\n' "$stats" |
        sed -n 's/.* packets=\([0-9][0-9]*\).*/\1/p')
    source_packets=$(ffprobe -v error -count_packets -select_streams v:0 \
        -show_entries stream=nb_read_packets -of default=nw=1:nk=1 "$source")
    decoded_frames=$(ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$source")
    matches=false
    if [ "$ivf_packets" = "$source_packets" ] &&
       [ "$source_packets" = "$expected_packets" ] &&
       [ "$decoded_frames" = "$expected_frames" ]; then
        matches=true
    else
        failed=1
    fi
    printf '%s,%s,%s,%s,%s,%s\n' "$label" "$ivf_packets" \
        "$source_packets" "$decoded_frames" "$expected_frames" "$matches"
}

echo 'sample,ivf_packets,source_packets,decoded_frames,expected_frames,matches'
check_sample quantizer vp90-2-00-quantizer-00.webm \
    92f26f690d76b46bab8b0807e95b593b7cb159472cfecc35ddf545c889825c6d 2 2
check_sample resize vp90-2-05-resize.ivf \
    84a345546e47e37de87d2959347bf731f9f440f9f2ab32bede3f0e416100c585 10 10
check_sample show_existing vp90-2-10-show-existing-frame.webm \
    21d60d305ed05f06d838f90f0e62bbd2a37c4b994304ad7f8556430cc551aa1b 13 13
check_sample intra_only vp90-2-16-intra-only.webm \
    328abd0bbb630f76ed5e6a5ed94e8065f8dda53c8f5ad120fba515a03438512b 7 7
check_sample superframe vp90-2-segmentation-sf-akiyo.webm \
    b594f9e68b6233d801338664e0084beffc102daf66205e4edf0e5b95d66af311 25 25
check_sample tiling_1080p vp90-2-tiling-pedestrian.webm \
    e5adfb9edb7c6e7a9274e65a4564392c784bc033b783203b46f84a21d58c6b88 2 2

exit "$failed"
