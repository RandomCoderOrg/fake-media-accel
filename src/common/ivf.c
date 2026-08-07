#include "fma/ivf.h"
#include "fma/protocol.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int fma_ivf_parse(const uint8_t *data, size_t size,
                  struct fma_ivf_stream *stream) {
    if (!stream) {
        errno = EINVAL;
        return -1;
    }
    memset(stream, 0, sizeof(*stream));
    if (!data || size < 32 || memcmp(data, "DKIF", 4) != 0) {
        errno = EINVAL;
        return -1;
    }
    uint16_t version = fma_get_u16(data + 4);
    uint16_t header_size = fma_get_u16(data + 6);
    stream->fourcc = fma_get_u32(data + 8);
    stream->width = fma_get_u16(data + 12);
    stream->height = fma_get_u16(data + 14);
    stream->rate = fma_get_u32(data + 16);
    stream->scale = fma_get_u32(data + 20);
    stream->declared_frames = fma_get_u32(data + 24);
    if (version != 0 || header_size < 32 || header_size > size ||
        !stream->width || !stream->height || !stream->rate || !stream->scale) {
        errno = EINVAL;
        return -1;
    }

    size_t offset = header_size;
    size_t capacity = 0;
    while (offset < size) {
        if (size - offset < 12)
            goto invalid;
        uint32_t frame_size = fma_get_u32(data + offset);
        uint64_t timestamp = fma_get_u64(data + offset + 4);
        offset += 12;
        if (!frame_size || frame_size > size - offset)
            goto invalid;
        if (stream->frame_count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            if (next < capacity ||
                next > SIZE_MAX / sizeof(*stream->frames)) {
                errno = ENOMEM;
                goto fail;
            }
            void *resized = realloc(stream->frames,
                                    next * sizeof(*stream->frames));
            if (!resized)
                goto fail;
            stream->frames = resized;
            capacity = next;
        }
        stream->frames[stream->frame_count++] = (struct fma_ivf_frame) {
            .offset = offset,
            .size = frame_size,
            .timestamp = timestamp,
        };
        offset += frame_size;
    }
    if (!stream->frame_count)
        goto invalid;
    return 0;

invalid:
    errno = EINVAL;
fail:
    fma_ivf_release(stream);
    return -1;
}

int fma_ivf_timestamp_us(const struct fma_ivf_stream *stream,
                         uint64_t timestamp, int64_t *pts_us) {
    if (!stream || !stream->rate || !stream->scale || !pts_us) {
        errno = EINVAL;
        return -1;
    }
    long double value = (long double)timestamp * stream->scale * 1000000.0L /
                        stream->rate;
    if (!isfinite(value) || value < 0.0L || value > (long double)INT64_MAX) {
        errno = ERANGE;
        return -1;
    }
    *pts_us = (int64_t)value;
    return 0;
}

void fma_ivf_release(struct fma_ivf_stream *stream) {
    if (!stream)
        return;
    free(stream->frames);
    memset(stream, 0, sizeof(*stream));
}
