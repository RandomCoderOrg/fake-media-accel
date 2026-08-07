#ifndef FMA_IVF_H
#define FMA_IVF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FMA_FOURCC(a, b, c, d)                                                \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) |                 \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

#define FMA_IVF_VP9 FMA_FOURCC('V', 'P', '9', '0')
#define FMA_IVF_AV1 FMA_FOURCC('A', 'V', '0', '1')

struct fma_ivf_frame {
    size_t offset;
    size_t size;
    uint64_t timestamp;
};

struct fma_ivf_stream {
    uint32_t fourcc;
    uint16_t width;
    uint16_t height;
    uint32_t rate;
    uint32_t scale;
    uint32_t declared_frames;
    struct fma_ivf_frame *frames;
    size_t frame_count;
};

int fma_ivf_parse(const uint8_t *data, size_t size,
                  struct fma_ivf_stream *stream);
int fma_ivf_timestamp_us(const struct fma_ivf_stream *stream,
                         uint64_t timestamp, int64_t *pts_us);
void fma_ivf_release(struct fma_ivf_stream *stream);

#ifdef __cplusplus
}
#endif

#endif
