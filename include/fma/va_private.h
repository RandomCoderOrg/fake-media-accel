#ifndef FMA_VA_PRIVATE_H
#define FMA_VA_PRIVATE_H

#include <stdint.h>

#include "fma/protocol.h"

#define FMA_VA_PACKET_BUFFER_TYPE 0x10000
#define FMA_VA_PACKET_MAGIC UINT32_C(0x50414d46) /* "FMAP" */
#define FMA_VA_PACKET_VERSION 1
#define FMA_VA_PACKET_FLAG_AV1_SHOW_EXISTING (UINT32_C(1) << 0)

struct fma_va_packet_header {
    uint32_t magic;
    uint16_t version;
    uint16_t codec;
    uint32_t flags;
    uint32_t payload_size;
};

#endif
