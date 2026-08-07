#include "fma/av1_obu.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #expression);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int rejects(const uint8_t *data, size_t size) {
    struct fma_av1_obu_info info;
    errno = 0;
    return fma_av1_scan_obus(data, size, &info) < 0 && errno == EINVAL &&
           info.obu_count == 0;
}

int main(void) {
    static const uint8_t temporal_unit[] = {
        0x0a, 0x02, 0xaa, 0xbb,
        0x1e, 0x30, 0x03, 0xcc, 0xdd, 0xee,
    };
    struct fma_av1_obu_info info;
    CHECK(fma_av1_scan_obus(temporal_unit, sizeof(temporal_unit), &info) == 0);
    CHECK(info.obu_count == 2 && info.has_sequence_header);
    CHECK(info.max_temporal_id == 1 && info.max_spatial_id == 2);

    static const uint8_t no_size[] = {0x20, 0x01, 0x02, 0x03};
    CHECK(fma_av1_scan_obus(no_size, sizeof(no_size), &info) == 0);
    CHECK(info.obu_count == 1 && !info.has_sequence_header);

    static const uint8_t forbidden[] = {0x8a, 0x00};
    static const uint8_t reserved_header[] = {0x0b, 0x00};
    static const uint8_t reserved_extension[] = {0x1e, 0x01, 0x00};
    static const uint8_t truncated_leb[] = {0x0a, 0x80};
    static const uint8_t oversized_payload[] = {0x0a, 0x05, 0x00};
    CHECK(rejects(forbidden, sizeof(forbidden)));
    CHECK(rejects(reserved_header, sizeof(reserved_header)));
    CHECK(rejects(reserved_extension, sizeof(reserved_extension)));
    CHECK(rejects(truncated_leb, sizeof(truncated_leb)));
    CHECK(rejects(oversized_payload, sizeof(oversized_payload)));

    puts("AV1 OBU scanner tests passed");
    return 0;
}
