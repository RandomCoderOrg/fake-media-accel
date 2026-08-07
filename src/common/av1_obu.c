#include "fma/av1_obu.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int fma_av1_scan_obus(const uint8_t *data, size_t size,
                      struct fma_av1_obu_info *info) {
    if (!data || !size || !info) {
        errno = EINVAL;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    size_t offset = 0;
    while (offset < size) {
        uint8_t header = data[offset++];
        if ((header & 0x81u) != 0) {
            errno = EINVAL;
            goto fail;
        }
        uint8_t type = (header >> 3) & 0x0fu;
        bool extension = (header & 0x04u) != 0;
        bool has_size = (header & 0x02u) != 0;
        if (!type)
            goto invalid;
        if (extension) {
            if (offset >= size || (data[offset] & 0x07u) != 0)
                goto invalid;
            uint8_t temporal_id = data[offset] >> 5;
            uint8_t spatial_id = (data[offset] >> 3) & 0x03u;
            if (temporal_id > info->max_temporal_id)
                info->max_temporal_id = temporal_id;
            if (spatial_id > info->max_spatial_id)
                info->max_spatial_id = spatial_id;
            ++offset;
        }

        size_t payload_size = size - offset;
        if (has_size) {
            uint64_t value = 0;
            bool complete = false;
            for (unsigned byte_index = 0; byte_index < 8; ++byte_index) {
                if (offset >= size)
                    goto invalid;
                uint8_t byte = data[offset++];
                value |= (uint64_t)(byte & 0x7fu) << (byte_index * 7u);
                if (!(byte & 0x80u)) {
                    complete = true;
                    break;
                }
            }
            if (!complete || value > SIZE_MAX)
                goto invalid;
            payload_size = (size_t)value;
            if (payload_size > size - offset)
                goto invalid;
        }
        ++info->obu_count;
        if (type == 1)
            info->has_sequence_header = true;
        offset += payload_size;
        if (!has_size)
            break;
    }
    return info->obu_count ? 0 : -1;

invalid:
    errno = EINVAL;
fail:
    memset(info, 0, sizeof(*info));
    return -1;
}
