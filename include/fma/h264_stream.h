#ifndef FMA_H264_STREAM_H
#define FMA_H264_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fma_h264_access_unit {
    size_t offset;
    size_t size;
    bool has_vcl;
    bool key_frame;
};

int fma_h264_split_annexb(const uint8_t *data, size_t size,
                          struct fma_h264_access_unit **units,
                          size_t *unit_count);

#ifdef __cplusplus
}
#endif

#endif
