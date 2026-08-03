#include "h264_timing.h"

#include <limits.h>
#include <stdio.h>

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #expression);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void) {
    struct fma_h264_timeline timeline = {0};
    CHECK(fma_h264_picture_pts(&timeline, 65536, true) == 0);
    CHECK(fma_h264_picture_pts(&timeline, 65544, false) == 8000);
    CHECK(fma_h264_picture_pts(&timeline, 65540, false) == 4000);
    CHECK(fma_h264_picture_pts(&timeline, 65538, false) == 2000);
    CHECK(fma_h264_picture_pts(&timeline, 65542, false) == 6000);
    CHECK(fma_h264_picture_pts(&timeline, 65554, false) == 18000);
    CHECK(fma_h264_picture_pts(&timeline, 65536, true) == 20000);

    struct fma_h264_timeline missing_poc = {0};
    CHECK(fma_h264_picture_pts(&missing_poc, INT32_MIN, false) == 1000);
    puts("H.264 presentation timeline passed");
    return 0;
}
