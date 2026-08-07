#include "fma/h264_stream.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #expression);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void) {
    static const uint8_t multi_slice[] = {
        0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1e,
        0, 0, 1, 0x68, 0xce, 0x38,
        0, 0, 1, 0x65, 0x80, /* first_mb_in_slice = 0 */
        0, 0, 1, 0x41, 0x30, /* first_mb_in_slice = 5 */
        0, 0, 0, 1, 0x41, 0x80,
    };
    struct fma_h264_access_unit *units = NULL;
    size_t count = 0;
    CHECK(fma_h264_split_annexb(multi_slice, sizeof(multi_slice), &units,
                                &count) == 0);
    CHECK(count == 2);
    CHECK(units[0].offset == 0 && units[0].size == 24);
    CHECK(units[0].has_vcl && units[0].key_frame);
    CHECK(units[1].offset == 24 && units[1].size == 6);
    CHECK(units[1].has_vcl && !units[1].key_frame);
    free(units);

    static const uint8_t with_aud[] = {
        0, 0, 1, 9, 0xf0, 0, 0, 1, 0x65, 0x80,
        0, 0, 1, 9, 0xf0, 0, 0, 1, 0x41, 0x80,
    };
    CHECK(fma_h264_split_annexb(with_aud, sizeof(with_aud), &units,
                                &count) == 0);
    CHECK(count == 2);
    CHECK(units[0].size == 10 && units[0].key_frame);
    CHECK(units[1].offset == 10 && units[1].size == 10);
    free(units);

    static const uint8_t malformed[] = {0, 0, 1, 0x65, 0};
    errno = 0;
    CHECK(fma_h264_split_annexb(malformed, sizeof(malformed), &units,
                                &count) < 0);
    CHECK(errno == EINVAL && units == NULL && count == 0);

    static const uint8_t leading_zeroes[] = {
        0, 0, 0, 0, 1, 0x65, 0x80,
    };
    CHECK(fma_h264_split_annexb(leading_zeroes, sizeof(leading_zeroes),
                                &units, &count) == 0);
    CHECK(count == 1 && units[0].offset == 0 &&
          units[0].size == sizeof(leading_zeroes));
    free(units);

    static const uint8_t terminal_end_of_sequence[] = {
        0, 0, 1, 0x65, 0x80,
        0, 0, 1, 0x0a,
    };
    CHECK(fma_h264_split_annexb(terminal_end_of_sequence,
                                sizeof(terminal_end_of_sequence), &units,
                                &count) == 0);
    CHECK(count == 1 && units[0].key_frame && units[0].has_vcl);
    CHECK(units[0].size == sizeof(terminal_end_of_sequence));
    free(units);

    puts("H264 Annex-B access-unit tests passed");
    return 0;
}
