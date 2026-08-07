#!/bin/sh
set -eu

inspector=${1:-./build/fma-ivf-inspect}
corpus=${2:-./build/av1-fate-smoke}
conformance_base=https://fate-suite.ffmpeg.org/av1-test-vectors
ffmpeg_base=https://fate-suite.ffmpeg.org/av1

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
    group=$2
    file=$3
    expected_hash=$4
    expected_packets=$5
    expected_frames=$6
    case $group in
        conformance) base=$conformance_base ;;
        ffmpeg) base=$ffmpeg_base ;;
        *) echo "unknown corpus group: $group" >&2; exit 2 ;;
    esac
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

    stats=$($inspector "$source")
    ivf_packets=$(printf '%s\n' "$stats" |
        sed -n 's/.* packets=\([0-9][0-9]*\).*/\1/p')
    codec=$(printf '%s\n' "$stats" |
        sed -n 's/.*fourcc=\([^ ]*\).*/\1/p')
    source_stats=$(ffprobe -v error -count_packets -count_frames \
        -select_streams v:0 \
        -show_entries stream=profile,pix_fmt,nb_read_packets,nb_read_frames \
        -of default=nw=1 "$source")
    profile=$(printf '%s\n' "$source_stats" | sed -n 's/^profile=//p')
    pixel_format=$(printf '%s\n' "$source_stats" | sed -n 's/^pix_fmt=//p')
    source_packets=$(printf '%s\n' "$source_stats" |
        sed -n 's/^nb_read_packets=//p')
    decoded_frames=$(printf '%s\n' "$source_stats" |
        sed -n 's/^nb_read_frames=//p')
    matches=false
    if [ "$codec" = AV01 ] && [ "$profile" = Main ] &&
       [ "$pixel_format" = yuv420p ] &&
       [ "$ivf_packets" = "$source_packets" ] &&
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
check_sample all_intra conformance av1-1-b8-02-allintra.ivf \
    5fcd265fd9f9bdd0d3179340b4c4532f1422ca5e5d97741c7481b84cb5dc122f 39 39
check_sample size_down conformance av1-1-b8-03-sizedown.ivf \
    64ede6740ca1192b2a84daf9e11e6e8f1dbc77752125cd7804f6d2fd92e9f784 20 20
check_sample size_up conformance av1-1-b8-03-sizeup.ivf \
    24b9554460aea54b24aff4adaf154c887fcc14ed114436bc87e8c29d8f4e7113 20 20
check_sample cdf_update conformance av1-1-b8-04-cdfupdate.ivf \
    14a3dbf537b6bf15efc003182d9916d61438c93624a8bd26e6e3ae7eaf33ea82 2 2
check_sample motion_vectors conformance av1-1-b8-05-mv.ivf \
    222a9050059b254dab17cfb802ff829c778e3f93af18961a622c8268576c1395 4 4
check_sample motion_field_mvs conformance av1-1-b8-06-mfmv.ivf \
    b59bf9586d8546dfda81dfec4ee4e32ceb502c9d22412ab0b63a2abb534a1f14 4 4
check_sample temporal_svc conformance av1-1-b8-22-svc-L1T2.ivf \
    eade696ab60415d476e1d2a489ec2cccd24d5eaaadfa4559c6488b7932274c5e 8 8
check_sample spatial_temporal_svc conformance av1-1-b8-22-svc-L2T2.ivf \
    14257d5fd3901581dbeb88133378cd79d50ea32506adb729909ab56b4a52af3c 8 8
check_sample film_grain conformance av1-1-b8-23-film_grain-50.ivf \
    ba3edd82a58414f009e1c821b947ec8de4e0420f33803ca7bfea39af6aeea155 10 10
check_sample short_refs ffmpeg frames_refs_short_signaling.ivf \
    d9136e7e427a1ec1423823f1d8cee37ed0aa354d16bb6607feb5413436810918 50 50
check_sample non_uniform_tiling ffmpeg non_uniform_tiling.ivf \
    c2bf1ba280ea19373a3a001b86b5ea4dbd0eacfb92011917b74866cb49ec90a7 24 24
check_sample switch_frame ffmpeg switch_frame.ivf \
    f5749d34c9076f5d123910c276b5d371ab0e3bcc70ece4cd5c229a561f05413e 32 32

exit "$failed"
