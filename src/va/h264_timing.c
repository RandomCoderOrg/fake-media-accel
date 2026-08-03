#include "h264_timing.h"

#include <limits.h>

int64_t fma_h264_picture_pts(struct fma_h264_timeline *timeline, int32_t poc,
                             bool idr) {
    ++timeline->submitted;
    if (poc == INT32_MIN)
        return (int64_t)timeline->submitted * 1000;
    if (!timeline->have_epoch || idr) {
        if (timeline->have_epoch) {
            int64_t span = (int64_t)timeline->max_poc - timeline->poc_base + 2;
            timeline->epoch += span > 0 ? span : 2;
        }
        timeline->have_epoch = true;
        timeline->poc_base = poc;
        timeline->max_poc = poc;
    } else if (poc > timeline->max_poc) {
        timeline->max_poc = poc;
    }
    int64_t offset = (int64_t)poc - timeline->poc_base;
    if (offset < 0)
        return (int64_t)timeline->submitted * 1000;
    return (timeline->epoch + offset) * 1000;
}
