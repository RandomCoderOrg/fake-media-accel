#include "fma/protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void fma_message_init(struct fma_message *message, uint16_t type) {
    memset(message, 0, sizeof(*message));
    message->type = type;
    for (size_t i = 0; i < FMA_MAX_FDS; ++i)
        message->fds[i] = -1;
}

void fma_message_release(struct fma_message *message) {
    free(message->payload);
    message->payload = NULL;
    for (uint32_t i = 0; i < message->fd_count && i < FMA_MAX_FDS; ++i) {
        if (message->fds[i] >= 0)
            close(message->fds[i]);
        message->fds[i] = -1;
    }
    message->payload_size = 0;
    message->fd_count = 0;
}

void fma_put_u16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

void fma_put_u32(uint8_t *dst, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        dst[i] = (uint8_t)(value >> (i * 8));
}

void fma_put_u64(uint8_t *dst, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        dst[i] = (uint8_t)(value >> (i * 8));
}

uint16_t fma_get_u16(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

uint32_t fma_get_u32(const uint8_t *src) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= (uint32_t)src[i] << (i * 8);
    return value;
}

uint64_t fma_get_u64(const uint8_t *src) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)src[i] << (i * 8);
    return value;
}

int fma_encode_header(const struct fma_message *message,
                      uint8_t out[FMA_WIRE_HEADER_SIZE]) {
    if (!message || !out || message->payload_size > FMA_MAX_PAYLOAD ||
        message->fd_count > FMA_MAX_FDS) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, FMA_WIRE_HEADER_SIZE);
    fma_put_u32(out + 0, FMA_MAGIC);
    fma_put_u16(out + 4, FMA_PROTOCOL_MAJOR);
    fma_put_u16(out + 6, FMA_PROTOCOL_MINOR);
    fma_put_u16(out + 8, FMA_WIRE_HEADER_SIZE);
    fma_put_u16(out + 10, message->type);
    fma_put_u32(out + 12, message->flags);
    fma_put_u32(out + 16, message->payload_size);
    fma_put_u32(out + 20, message->fd_count);
    fma_put_u64(out + 24, message->request_id);
    fma_put_u64(out + 32, message->session_id);
    fma_put_u64(out + 40, (uint64_t)message->pts_us);
    return 0;
}

int fma_decode_header(const uint8_t in[FMA_WIRE_HEADER_SIZE],
                      struct fma_message *message) {
    if (!in || !message || fma_get_u32(in + 0) != FMA_MAGIC ||
        fma_get_u16(in + 4) != FMA_PROTOCOL_MAJOR ||
        fma_get_u16(in + 8) != FMA_WIRE_HEADER_SIZE) {
        errno = EPROTO;
        return -1;
    }

    fma_message_init(message, fma_get_u16(in + 10));
    message->flags = fma_get_u32(in + 12);
    message->payload_size = fma_get_u32(in + 16);
    message->fd_count = fma_get_u32(in + 20);
    message->request_id = fma_get_u64(in + 24);
    message->session_id = fma_get_u64(in + 32);
    message->pts_us = (int64_t)fma_get_u64(in + 40);
    if (message->payload_size > FMA_MAX_PAYLOAD || message->fd_count > FMA_MAX_FDS) {
        errno = EMSGSIZE;
        return -1;
    }
    return 0;
}

void fma_encode_capabilities(const struct fma_capabilities *caps, uint8_t out[20]) {
    fma_put_u32(out + 0, caps->decoder_mask);
    fma_put_u32(out + 4, caps->pixel_format_mask);
    fma_put_u32(out + 8, caps->flags);
    fma_put_u32(out + 12, caps->max_width);
    fma_put_u32(out + 16, caps->max_height);
}

int fma_decode_capabilities(const uint8_t *data, size_t size,
                            struct fma_capabilities *caps) {
    if (!data || !caps || size != 20) {
        errno = EPROTO;
        return -1;
    }
    caps->decoder_mask = fma_get_u32(data + 0);
    caps->pixel_format_mask = fma_get_u32(data + 4);
    caps->flags = fma_get_u32(data + 8);
    caps->max_width = fma_get_u32(data + 12);
    caps->max_height = fma_get_u32(data + 16);
    return 0;
}

void fma_encode_decoder_config(const struct fma_decoder_config *config,
                               uint8_t out[16]) {
    fma_put_u32(out + 0, config->codec);
    fma_put_u32(out + 4, config->width);
    fma_put_u32(out + 8, config->height);
    fma_put_u32(out + 12, config->slot_count);
}

int fma_decode_decoder_config(const uint8_t *data, size_t size,
                              struct fma_decoder_config *config) {
    if (!data || !config || size != 16) {
        errno = EPROTO;
        return -1;
    }
    config->codec = fma_get_u32(data + 0);
    config->width = fma_get_u32(data + 4);
    config->height = fma_get_u32(data + 8);
    config->slot_count = fma_get_u32(data + 12);
    if (config->codec < FMA_CODEC_H264 || config->codec > FMA_CODEC_AV1 ||
        config->width == 0 || config->height == 0 ||
        config->width > FMA_MAX_DIMENSION || config->height > FMA_MAX_DIMENSION ||
        config->slot_count == 0 || config->slot_count > FMA_MAX_SLOTS) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void fma_encode_frame_pool(const struct fma_frame_pool *pool, uint8_t out[24]) {
    fma_put_u32(out + 0, pool->pixel_format);
    fma_put_u32(out + 4, pool->width);
    fma_put_u32(out + 8, pool->height);
    fma_put_u32(out + 12, pool->stride);
    fma_put_u32(out + 16, pool->slot_count);
    fma_put_u32(out + 20, pool->slot_size);
}

int fma_decode_frame_pool(const uint8_t *data, size_t size,
                          struct fma_frame_pool *pool) {
    if (!data || !pool || size != 24) {
        errno = EPROTO;
        return -1;
    }
    pool->pixel_format = fma_get_u32(data + 0);
    pool->width = fma_get_u32(data + 4);
    pool->height = fma_get_u32(data + 8);
    pool->stride = fma_get_u32(data + 12);
    pool->slot_count = fma_get_u32(data + 16);
    pool->slot_size = fma_get_u32(data + 20);
    if (pool->pixel_format != FMA_PIXFMT_NV12 || pool->width == 0 ||
        pool->height == 0 || pool->width > FMA_MAX_DIMENSION ||
        pool->height > FMA_MAX_DIMENSION || pool->stride < pool->width ||
        pool->slot_count == 0 || pool->slot_count > FMA_MAX_SLOTS ||
        pool->slot_size == 0 ||
        (uint64_t)pool->slot_count * pool->slot_size > FMA_MAX_POOL_BYTES) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

void fma_encode_frame(const struct fma_frame *frame, uint8_t out[24]) {
    fma_put_u32(out + 0, frame->slot);
    fma_put_u32(out + 4, frame->bytes_used);
    fma_put_u32(out + 8, frame->width);
    fma_put_u32(out + 12, frame->height);
    fma_put_u32(out + 16, frame->stride);
    fma_put_u32(out + 20, frame->pixel_format);
}

int fma_decode_frame(const uint8_t *data, size_t size, struct fma_frame *frame) {
    if (!data || !frame || size != 24) {
        errno = EPROTO;
        return -1;
    }
    frame->slot = fma_get_u32(data + 0);
    frame->bytes_used = fma_get_u32(data + 4);
    frame->width = fma_get_u32(data + 8);
    frame->height = fma_get_u32(data + 12);
    frame->stride = fma_get_u32(data + 16);
    frame->pixel_format = fma_get_u32(data + 20);
    return 0;
}

const char *fma_codec_name(uint32_t codec) {
    static const char *const names[] = {"unknown", "H.264", "HEVC", "VP8", "VP9", "AV1"};
    return codec <= FMA_CODEC_AV1 ? names[codec] : names[0];
}

const char *fma_codec_mime(uint32_t codec) {
    static const char *const mimes[] = {
        NULL, "video/avc", "video/hevc", "video/x-vnd.on2.vp8",
        "video/x-vnd.on2.vp9", "video/av01"
    };
    return codec <= FMA_CODEC_AV1 ? mimes[codec] : NULL;
}
