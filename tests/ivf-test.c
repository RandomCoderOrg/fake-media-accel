#include "fma/ivf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #expression);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void) {
    static const uint8_t valid[] = {
        'D', 'K', 'I', 'F', 0, 0, 32, 0,
        'V', 'P', '9', '0', 64, 1, 144, 0,
        30, 0, 0, 0, 1, 0, 0, 0,
        2, 0, 0, 0, 0, 0, 0, 0,
        3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xaa, 0xbb, 0xcc,
        2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0xdd, 0xee,
    };
    struct fma_ivf_stream stream;
    CHECK(fma_ivf_parse(valid, sizeof(valid), &stream) == 0);
    CHECK(stream.fourcc == FMA_IVF_VP9);
    CHECK(stream.width == 320 && stream.height == 144);
    CHECK(stream.rate == 30 && stream.scale == 1);
    CHECK(stream.declared_frames == 2 && stream.frame_count == 2);
    CHECK(stream.frames[0].offset == 44 && stream.frames[0].size == 3 &&
          stream.frames[0].timestamp == 0);
    CHECK(stream.frames[1].offset == 59 && stream.frames[1].size == 2 &&
          stream.frames[1].timestamp == 3);
    int64_t pts_us = -1;
    CHECK(fma_ivf_timestamp_us(&stream, stream.frames[1].timestamp,
                               &pts_us) == 0);
    CHECK(pts_us == 100000);
    fma_ivf_release(&stream);

    uint8_t malformed[sizeof(valid)];
    memcpy(malformed, valid, sizeof(valid));
    malformed[47] = 0xff;
    errno = 0;
    CHECK(fma_ivf_parse(malformed, sizeof(malformed), &stream) < 0);
    CHECK(errno == EINVAL && stream.frames == NULL);

    puts("IVF parser tests passed");
    return 0;
}
