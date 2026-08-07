# FFmpeg AV1 adapter

`0001-av1-preserve-fma-packets.patch` keeps the original low-overhead AV1 OBUs
available when FFmpeg uses the FMA VA-API driver. The adapter activates only
when libva's vendor string contains `fake-media-accel`; behavior with other VA
drivers is unchanged.

The patch was built and tested at FFmpeg commit
`5c395992f99feb47860e4cc99a0cea2009457870`:

```sh
git -C /path/to/ffmpeg checkout 5c395992f99feb47860e4cc99a0cea2009457870
git -C /path/to/ffmpeg apply \
  /path/to/fake-media-accel/patches/ffmpeg/0001-av1-preserve-fma-packets.patch
```

Build FFmpeg with VA-API and the decoders, filters and applications required by
the intended workload. The patch does not alter FFmpeg's configure options.
