# VA-API bridge status

FMA exposes H.264 and VP9 Profile 0 MediaCodec decoding through an ordinary
VA-API driver. On
Android systems where `/dev/dma_heap/system` is accessible, decoded NV12
surfaces are backed by linear DMA-BUFs and can be exported with
`vaExportSurfaceHandle()` as `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`.
The ownership and synchronization behavior follows the
[libva core API](https://intel.github.io/libva/group__api__core.html).

```mermaid
flowchart LR
    A[Linux VA-API client] --> B[FMA VA driver]
    B --> C[Android MediaCodec daemon]
    C --> D[AImage frame]
    D --> E[DMA-BUF NV12 VA surface]
    E --> F[Standard DRM PRIME export]
    F --> G[EGL / Vulkan / other Linux consumer]
```

This route does not require a Termux:X11 patch. The X server is only one
possible final consumer; FMA's contract ends at the standard DRM PRIME
descriptor.

## Current contract

- H.264 constrained-baseline, main and high profile decode.
- VP9 Profile 0, 8-bit 4:2:0 decode. The complete compressed VP9 frame supplied
  by libva is forwarded unchanged; Profile 2 is rejected because the current
  decoded-frame contract is NV12.
- Exact VA reconstruction for representable 8-bit 4:2:0 streams. POC type 1
  requires the packet-preserving path because the standard VA picture buffer
  omits its SPS offset arrays; the driver rejects it instead of guessing.
- NV12 `vaCreateImage`, `vaDeriveImage`, `vaGetImage` and unscaled
  `vaPutImage`.
- DMA-BUF cache synchronization around CPU access.
- Composed NV12 and separate R8/GR88 DRM PRIME export layouts.
- Legacy `vaAcquireBufferHandle()` / `vaReleaseBufferHandle()` PRIME export for
  consumers such as VLC's standard VA/EGL converter.
- Macroblock-aligned backing storage while preserving the requested visible
  dimensions and exporting the correct UV-plane offset.
- Per-context protocol serialization, so decode and presentation threads cannot
  consume each other's daemon replies.
- Direct-output registration passes an exportable VA surface DMA-BUF to the
  daemon with its compressed packet. MediaCodec output is copied from the
  `AImage` directly into that final surface, bypassing the intermediate shared
  frame-pool copy.
- A malloc-backed fallback when no DMA heap is available. It remains usable
  through VA image mapping but correctly reports DRM PRIME as unsupported.
- `FMA_VA_DMA_HEAP=none` forces the fallback; a different heap path may be
  supplied through the same variable.

The neutral surface test covers both allocation modes:

```sh
ctest --test-dir build --output-on-failure
```

`fma-va-egl-probe` uploads known NV12 values through public VA-API, exports the
surface, imports both planes through `EGL_EXT_image_dma_buf_import`, samples
them on the GPU and checks the resulting pixel. On the Pixel test device it
reported:

```text
VA->DRM PRIME->EGL passed: renderer=Mali-G78 (Panfrost) pixel=64,96,160,255
```

An unmodified VLC 3.0.16 run then selected `vaapi_vld`, the FMA driver and
`glconv_vaapi_x11`. Three normal six-thread runs of the 152-frame 1080p sample
completed with zero decode or VA/EGL interop failures. They reported 2, 4 and 3
late-frame warnings respectively. This validates application-level use of the
standard path; it is not a VLC-specific integration.

`FMA_VA_DEBUG=1` exposes protocol message flow plus context creation, packet
submission, surface synchronization and drain durations. It is intended for
short controlled probes, not normal playback.

Codec-level corpora, exact-output results and VA boundaries are documented in
the [H.264 checkpoint](h264.md) and [VP9 checkpoint](vp9.md).

## Cost measured on the current 1080p30 sample

Five runs decoding 152 frames through MediaCodec and downloading NV12 with
FFmpeg:

| Surface backing | Mean wall time | Max RSS |
| --- | ---: | ---: |
| DMA-BUF | 1.247 s | 95.4 MB |
| malloc | 1.185 s | 135.3 MB |

The DMA-BUF route is currently about 5% slower in this download-heavy probe
because it adds cache-sync ioctls. Its benefit is that applications can import
the decoded surface without another presentation copy. The output MD5 remains
identical to software decode.

## VLC application cost

Three local, no-audio runs used the same file and Panfrost GL output. The FMA
total includes CPU time charged to the Android decoder daemon (`CLK_TCK=100`),
so offloaded work is not hidden.

| Decode route | Mean wall | VLC user | VLC system | Daemon CPU | Counted CPU total |
| --- | ---: | ---: | ---: | ---: | ---: |
| FMA VA-API | 10.925 s | 0.896 s | 3.067 s | 1.407 s | 5.369 s |
| VLC software | 6.771 s | 5.987 s | 1.551 s | 0 s | 7.538 s |

FMA reduced counted CPU by about 29%, but wall time was about 61% worse. A
traced run spent only 1.81 seconds across all 152 packet submissions, 95 ms
across 300 surface-sync calls and 54 ms draining the context. The VA context
finished around 6.1 seconds after driver initialization while VLC remained
alive until roughly 10.3 seconds. The open performance issue is therefore
frame pacing or video-output backlog after decode, not the context drain. This
needs its own probe before changing the X server.

## Remaining copy

MediaCodec currently writes an `AImage`, and the Android daemon copies that
image once into the registered exportable DMA-BUF surface. If direct output is
unavailable, the compatible fallback still uses the shared frame pool and a
second VA-driver copy. A later AHardwareBuffer-backed path can remove the final
CPU copy. It does not require changing Termux:X11 unless a future presentation
probe finds a server-specific failure after standard EGL import succeeds.
