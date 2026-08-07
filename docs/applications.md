# Application compatibility

Codec probes establish correctness before a desktop application is involved.
The application stage then measures the complete path: application decode,
Android daemon work, VA surface presentation, the Linux graphics stack and
Termux:X11.

## VLC

VLC 3.0.16 can use FMA for H.264 through its normal FFmpeg VA-API decoder. Its
OpenGL video output also needs an EGL implementation that advertises
`EGL_EXT_image_dma_buf_import`. If the desktop session uses a different EGL
stack, VLC may reject `glconv_vaapi_x11`, probe inaccessible `/dev/dri` nodes
and fall back to software decoding even though `vainfo` succeeds.

Run VLC with the same Mesa environment used by the accelerated desktop, plus
the FMA variables from [the setup guide](setup-proot.md):

```bash
export LD_LIBRARY_PATH=/path/to/mesa/lib
export LIBGL_DRIVERS_PATH=/path/to/mesa/lib/dri
export MESA_LOADER_DRIVER_OVERRIDE=panfrost
vlc --avcodec-hw=vaapi video.mp4
```

Use the bounded benchmark instead of repeatedly driving VLC by hand:

```bash
FMA_DAEMON_PID="$(pgrep -f '/fake-media-acceld ' | head -n 1)" \
FMA_APP_USER=linux-user \
tools/fma-vlc-benchmark.sh video.mp4 both
```

The report separates application CPU from daemon CPU, counts late and dropped
frames, and verifies both the VA decoder and VA/EGL conversion-module choices.
Logs remain in the printed temporary directory for diagnosis. The helper sends
SIGTERM and then SIGKILL to the whole application process group if playback
does not finish within the media duration plus a bounded grace period.

## H.264 real-world checkpoint

The local test source is a user-supplied 2K Gravity trailer; it is not included
in this repository. The repeatable smoke segment starts near 60 seconds and
retains the original H.264 bitstream.

| Property | Value |
| --- | ---: |
| Profile and level | High, level 4.0 |
| Visible size | 2048 x 858 |
| Displayed frames | 358 |
| Smoke duration | 15.099 s |
| Smoke-file MD5 | `475511c8d3ca9673c7ddb46c9f0d5a91` |
| Decoded NV12 MD5 | `b2ad26f06168a107e8d3647ec37d6cb8` |

Hardware and software decoding produced byte-identical NV12. A real-time VLC
comparison on a Pixel 6a produced:

| Measurement | FMA hardware | Software |
| --- | ---: | ---: |
| Wall time | 16.525 s | 16.586 s |
| VLC and presentation CPU | 6.822 s | 53.690 s |
| Android daemon CPU | 2.320 s | 0 s |
| Total measured CPU | **9.142 s** | **53.690 s** |
| Peak application RSS | 101 MiB | 209 MiB |
| Late-display warnings | 2 | 4 |
| Decoder frame drops | 0 | 0 |

All 358 hardware frames used direct DMA-BUF-backed VA surfaces. No decoded
frame data crossed the Unix socket, and the VA driver performed no second
presentation copy. This reduced total CPU by about 83 percent while preserving
real-time pacing.

## Remaining application work

- Run the same real-time VLC accounting on a representative VP9 source.
- Connect the packet-preserving AV1 FFmpeg adapter to applications that embed
  libavcodec; VLC 3.0's bundled decoder does not provide FMA's AV1 packet
  contract.
- Add browser/RDD-sandbox socket and DMA-heap access without weakening unrelated
  browser sandboxes.
- Validate seek, pause/resume, resolution changes and longer playback soaks.
