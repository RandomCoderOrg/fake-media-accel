#ifndef FMA_AV1_OBU_H
#define FMA_AV1_OBU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fma_av1_obu_info {
    size_t obu_count;
    uint8_t max_temporal_id;
    uint8_t max_spatial_id;
    bool has_sequence_header;
};

int fma_av1_scan_obus(const uint8_t *data, size_t size,
                      struct fma_av1_obu_info *info);

#ifdef __cplusplus
}
#endif

#endif
