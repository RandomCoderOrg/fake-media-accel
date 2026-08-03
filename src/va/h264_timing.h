#ifndef FMA_H264_TIMING_H
#define FMA_H264_TIMING_H

#include <stdbool.h>
#include <stdint.h>

struct fma_h264_timeline {
    uint64_t submitted;
    bool have_epoch;
    int32_t poc_base;
    int32_t max_poc;
    int64_t epoch;
};

int64_t fma_h264_picture_pts(struct fma_h264_timeline *timeline, int32_t poc,
                             bool idr);

#endif
