#ifndef FMA_H264_ANNEXB_H
#define FMA_H264_ANNEXB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <va/va.h>

enum fma_h264_build_status {
    FMA_H264_BUILD_OK = 0,
    FMA_H264_BUILD_INVALID,
    FMA_H264_BUILD_UNREPRESENTABLE,
    FMA_H264_BUILD_NO_MEMORY,
};

enum fma_h264_build_status fma_h264_build_packet(
    VAProfile profile, const VAPictureParameterBufferH264 *picture,
    const VAIQMatrixBufferH264 *iq_matrix,
    const VASliceParameterBufferH264 *slice,
    const uint8_t *slice_data, size_t slice_size,
    uint8_t **packet, size_t *packet_size);

#endif
