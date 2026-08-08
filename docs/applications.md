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

Set `FMA_RUNS=3` for counterbalanced repetitions. Odd runs execute hardware
first; even runs execute software first, reducing order and thermal bias.

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

The later packet-preserving FFmpeg checkpoint also decoded the complete
146.980-second source rather than the smoke segment. All 3,524 visible High
profile frames matched software plane checksums; all 3,525 decoded surfaces,
including one FFmpeg-discarded preroll surface, used direct DMA-BUF output. See
the [H.264 checkpoint](h264.md) for the full counters and the POC type 1 test.

## VP9 real-world checkpoint

A Profile 0 WebM smoke stream was generated from the same local Gravity segment
at its original 2048 x 858 resolution. It contains 358 frames, lasts 15.015
seconds and has file MD5 `0751680aa8d6a58bec880f359caf9eac`. Hardware and
software decoding produced the same visible NV12 MD5,
`6da6806f19b738f92a01ef10540012fc`.

| Measurement | FMA hardware | Software |
| --- | ---: | ---: |
| Decode-throughput CPU | 3.253 s | 11.739 s |
| Decode-throughput wall time | 3.344 s | 3.551 s |
| Median VLC wall time | 15.896 s | 16.109 s |
| Median VLC and presentation CPU | 6.436 s | 24.014 s |
| Median Android daemon CPU | 3.030 s | 0 s |
| Median total measured VLC CPU | **9.466 s** | **24.014 s** |
| Median peak application RSS | 72 MiB | 136 MiB |
| Late-display warnings | 0 | 0 |
| Decoder frame drops | 0 | 0 |

These are medians from three counterbalanced runs. Hardware wall time ranged
from 15.824 to 16.069 seconds; software ranged from 16.078 to 16.210 seconds.
All 358 VP9 surfaces in every hardware run were direct and exact. Median total
CPU fell by about 61 percent. Median VA context creation took 85 ms and drain
plus destruction took 15 ms, ruling out lifecycle setup as a pacing bottleneck.

## AV1 FFplay checkpoint

FFplay at FFmpeg commit `5c395992f99feb47860e4cc99a0cea2009457870`
was built with both patches from [`patches/ffmpeg`](../patches/ffmpeg). The
packet-preserving adapter supplies complete AV1 temporal units to FMA, while
the FFplay patch permits explicit VA-API decode with filtered SDL output when
FFplay's Vulkan renderer is unavailable.

Use the same counterbalanced process accounting as the VLC probe:

```bash
FMA_FFPLAY=/path/to/patched/ffplay \
FMA_SOFTWARE_FFPLAY=/usr/bin/ffplay \
FMA_DAEMON_PID="$(pgrep -f '/fake-media-acceld ' | head -n 1)" \
FMA_APP_USER=linux-user \
FMA_RUNS=3 \
tools/fma-ffplay-benchmark.sh video.ivf both
```

The real-world smoke is a Main-profile, 8-bit AV1 transcode of the same local
Gravity segment used by the H.264 and VP9 checkpoints. It contains 360 frames,
lasts 15.015 seconds, has file MD5
`61fb98b6a86d948e8d9b15dfc81a5192`, and preserves the 2048 x 858 visible
size. Hardware and software decoding produced exactly the same 948,879,360
visible NV12 bytes.

The following hot-device checkpoint started at Android thermal status 2 and
ended at status 3. Values are medians from three counterbalanced runs, so they
measure the current implementation under sustained pressure rather than a
cold-device peak:

| Measurement | FMA hardware | Software |
| --- | ---: | ---: |
| Wall time | 16.119 s | 16.881 s |
| FFplay and presentation CPU | 5.158 s | 15.001 s |
| Android daemon CPU | 5.330 s | 0 s |
| Total measured CPU | **10.488 s** | **15.001 s** |
| Peak application RSS | 60.8 MiB | 135.5 MiB |
| FFplay presentation drops | 6 | 1 |

```mermaid
xychart-beta
    title "AV1 FFplay median CPU seconds"
    x-axis ["FMA app", "FMA daemon", "FMA total", "Software total"]
    y-axis "CPU seconds" 0 --> 16
    bar [5.158, 5.330, 10.488, 15.001]
```

All 360 decoded outputs were stored directly in DMA-BUF-backed surfaces, with
no decoded-frame data crossing the Unix socket and no daemon-side storage
copy. FFplay currently downloads about 895 MiB of planar image data per run for
SDL presentation. That compatibility copy took under 0.52 seconds in the VA
driver, but its subsequent upload and display remain outside FMA. The selected
SDL/OpenGL renderer is therefore part of the application environment, not a
Mali or Panfork dependency of FMA.

## Playback lifecycle checkpoint

The lifecycle probe drives the actual FFplay SDL window rather than restarting
the decoder for each operation:

```bash
FMA_FFPLAY=/path/to/patched/ffplay \
tools/fma-ffplay-lifecycle.sh video.mp4 hardware
```

It waits for the X11 window, pauses playback, verifies that the media clock is
held, resumes and verifies clock advancement, then sends an in-process
ten-second seek. The bounded process-group runner prevents a failed player from
remaining in the background. `xdotool` is required only by this test probe.

| Codec | Pause | Resume | In-process seek | VA context reuse |
| --- | --- | --- | --- | --- |
| H.264 High | Passed | Passed | Passed | One context |
| VP9 Profile 0 | Passed | Passed | Passed | One context |
| AV1 Main | Passed | Passed | Passed after sequence-redelivery fix | One context |

The software AV1 control completed the same raw-IVF seek while the first FMA
attempt stopped after 107 of 360 frames. FFmpeg calls the AV1 hardware
accelerator's flush callback during a seek, but the initial FMA adapter did not
mark its cached sequence header for redelivery. Resetting that state on flush
made the next AV1 packet carry the sequence header again; the hardware run then
reached the end in the same VA context. FFplay's `fd` counter includes frames
intentionally skipped by this seek, so it is not treated as a performance-drop
measurement in this probe.

## Dynamic-resolution and soak checkpoint

FFmpeg destroys and recreates a VA context when H.264 or VP9 changes coded
dimensions. Android MediaCodec is stateful, so closing its decoder at that VA
boundary discarded inter-frame references even though the Linux application
was continuing the same stream. FMA now retires the logical VA context while
keeping the compatible MediaCodec session alive. A same-codec, same-profile
context whose requested dimensions fit the decoder allocation reuses that
session; a key frame at the start of a genuinely new stream flushes it.

The checksum probe compares each software and VA output frame's dimensions and
three plane checksums. Both application paths are exact:

| Stream | Size transition | Frames | VA contexts | Reused sessions | Result |
| --- | --- | ---: | ---: | ---: | --- |
| H.264 High | 1280x536 to 640x268 to 1280x536 | 144 | 3 | 2 | exact |
| VP9 Profile 0 | 352x288 to 282x173 to 352x288 | 10 | 3 | 2 | exact |
| AV1 Main | 1280x720 to 352x288 | 20 | 1 | 0 | exact |

The VP9 result includes the final chroma row at the odd 173-line height. The
visible NV12 contract is `width * height + 2 * ceil(width/2) *
ceil(height/2)`, rather than the even-dimension shortcut `width * height *
3/2`. Decode surfaces are realigned to the retained MediaCodec allocation when
FFmpeg recreates a smaller VA context, so all ten outputs now remain direct;
the three realignments cost 3.271 ms total in the app-domain probe.

Use the application-level verifier with a full FFmpeg build that includes the
`showinfo` filter:

```bash
tools/fma-ffmpeg-verify-dynamic.sh video.mkv EXPECTED_FRAMES ffmpeg
```

The in-process FFplay loop probe then exercised repeated context changes and
presentation without repeatedly starting the application:

| Stream | Loops | Expected/stored frames | Contexts/reused | Result |
| --- | ---: | ---: | ---: | --- |
| H.264 resize | 3 | 432/432 | 7/6 | passed |
| VP9 resize | 20 | 200/200 | 41/40 | passed |
| AV1 size-down | 10 | 200/200 | 1/0 | passed |
| AV1 switch-frame | 10 | 320/320 | 1/0 | passed |

```mermaid
xychart-beta
    title "Dynamic contexts (first bar) and reused sessions (second bar)"
    x-axis ["H264", "VP9", "AV1 down", "AV1 switch"]
    y-axis "count" 0 --> 45
    bar [7, 41, 1, 1]
    bar [6, 40, 0, 0]
```

FFplay's drop counter is presentation behavior: the tiny resize vectors are
looped faster than a normal media timeline. Decoder completeness is checked
independently by requiring the daemon's `stored` count to equal the decoded
frame count for every loop.

A separate 10-loop soak used the 15-second Gravity samples without restarting
FFplay:

| Codec | Expected/stored | Wall time | Application CPU | Peak RSS | Presentation drops |
| --- | ---: | ---: | ---: | ---: | ---: |
| H.264 | 3580/3580 | 153.740 s | 63.708 s | 89.9 MiB | 7 |
| VP9 | 3580/3580 | 150.616 s | 63.864 s | 67.5 MiB | 10 |
| AV1 | 3600/3600 | 150.479 s | 57.050 s | 62.5 MiB | 22 |

Android remained at thermal status 1 and the battery sensor rose from 38.8 to
39.2 C. These runs prove decoder and application lifecycle stability; they are
not hardware-versus-software performance comparisons.

The direct dynamic checksum helper was also changed from reopening and skipping
through the raw file once per frame to one sequential file-descriptor pass. On
the 15.3 MiB AV1 resize output, observed verification wall time fell from about
one minute to 6 seconds. This keeps resize correctness cheap enough to run
before a desktop application.

## Remaining application work

- Connect the packet-preserving AV1 adapter to additional applications that
  embed libavcodec. VLC 3.0's bundled decoder does not provide FMA's AV1 packet
  contract.
- Add browser/RDD-sandbox socket and DMA-heap access without weakening unrelated
  browser sandboxes.
- Support a non-key adaptive size increase that exceeds the MediaCodec session's
  initial output allocation. Key-frame/segment switches and changes within the
  initial allocation already work.
- Package and supervise the daemon and guest VA driver from uDroid so desktop
  applications receive one stable, vendor-neutral media contract.
