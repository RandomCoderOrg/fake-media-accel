# FFmpeg AV1 adapter

The patches are deliberately split by responsibility:

- `0001-av1-preserve-fma-packets.patch` keeps the original low-overhead AV1
  OBUs available when FFmpeg uses the FMA VA-API driver. The adapter activates
  only when libva's vendor string contains `fake-media-accel`; behavior with
  other VA drivers is unchanged.
- `0002-ffplay-allow-filtered-vaapi-sdl-output.patch` lets FFplay create a
  standalone VA-API device when its Vulkan renderer is unavailable. This is
  limited to explicitly requested VA-API decoding; other hardware-device types
  retain upstream behavior.

The patches were built and tested at FFmpeg commit
`5c395992f99feb47860e4cc99a0cea2009457870`:

```sh
git -C /path/to/ffmpeg checkout 5c395992f99feb47860e4cc99a0cea2009457870
git -C /path/to/ffmpeg apply \
  /path/to/fake-media-accel/patches/ffmpeg/0001-av1-preserve-fma-packets.patch \
  /path/to/fake-media-accel/patches/ffmpeg/0002-ffplay-allow-filtered-vaapi-sdl-output.patch
```

Build FFmpeg with VA-API and the decoders, filters and applications required by
the intended workload. For FFplay this includes SDL2, `ffplay`, `hwdownload`
and the `format` filter. The patches do not alter FFmpeg's configure options.

The FFplay bridge is a compatibility path, not zero-copy presentation. VA-API
decodes into FMA surfaces, then `hwdownload,format=yuv420p` supplies SDL with a
portable software frame. The OpenGL or software renderer used by SDL is an
application-environment choice and is not an FMA dependency.
