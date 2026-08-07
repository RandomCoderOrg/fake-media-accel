#include "fma/h264_stream.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct rbsp_reader {
    uint8_t bytes[32];
    size_t bit_count;
    size_t bit_offset;
};

static bool find_start_code(const uint8_t *data, size_t size, size_t from,
                            size_t *offset, size_t *length) {
    for (size_t i = from; i + 3 <= size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0)
            continue;
        if (data[i + 2] == 1) {
            *offset = i;
            *length = 3;
            return true;
        }
        if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
            *offset = i;
            *length = 4;
            return true;
        }
    }
    return false;
}

static bool read_bit(struct rbsp_reader *reader, unsigned *value) {
    if (reader->bit_offset >= reader->bit_count)
        return false;
    size_t byte = reader->bit_offset / CHAR_BIT;
    unsigned shift = 7u - (unsigned)(reader->bit_offset % CHAR_BIT);
    *value = (reader->bytes[byte] >> shift) & 1u;
    ++reader->bit_offset;
    return true;
}

static bool read_ue(struct rbsp_reader *reader, unsigned *value) {
    unsigned leading_zeroes = 0;
    unsigned bit = 0;
    while (read_bit(reader, &bit) && !bit) {
        if (++leading_zeroes > 31)
            return false;
    }
    if (!bit)
        return false;
    uint32_t code = 1;
    for (unsigned i = 0; i < leading_zeroes; ++i) {
        if (!read_bit(reader, &bit))
            return false;
        code = (code << 1) | bit;
    }
    *value = code - 1;
    return true;
}

static bool first_mb_in_slice(const uint8_t *ebsp, size_t size,
                              unsigned *first_mb) {
    struct rbsp_reader reader = {0};
    unsigned zeroes = 0;
    for (size_t i = 0; i < size &&
                       reader.bit_count / CHAR_BIT < sizeof(reader.bytes);
         ++i) {
        if (zeroes >= 2 && ebsp[i] == 3) {
            zeroes = 0;
            continue;
        }
        reader.bytes[reader.bit_count / CHAR_BIT] = ebsp[i];
        reader.bit_count += CHAR_BIT;
        zeroes = ebsp[i] == 0 ? zeroes + 1 : 0;
    }
    return read_ue(&reader, first_mb);
}

static int append_unit(struct fma_h264_access_unit **units, size_t *count,
                       size_t *capacity, size_t offset, size_t end,
                       bool has_vcl, bool key_frame) {
    if (end <= offset)
        return 0;
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 64;
        if (next < *capacity || next > SIZE_MAX / sizeof(**units)) {
            errno = ENOMEM;
            return -1;
        }
        void *resized = realloc(*units, next * sizeof(**units));
        if (!resized)
            return -1;
        *units = resized;
        *capacity = next;
    }
    (*units)[(*count)++] = (struct fma_h264_access_unit) {
        .offset = offset,
        .size = end - offset,
        .has_vcl = has_vcl,
        .key_frame = key_frame,
    };
    return 0;
}

int fma_h264_split_annexb(const uint8_t *data, size_t size,
                          struct fma_h264_access_unit **units,
                          size_t *unit_count) {
    if (!data || !size || !units || !unit_count) {
        errno = EINVAL;
        return -1;
    }
    *units = NULL;
    *unit_count = 0;
    size_t first_start = 0;
    size_t first_prefix = 0;
    if (!find_start_code(data, size, 0, &first_start, &first_prefix) ||
        first_start + first_prefix >= size) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < first_start; ++i) {
        if (data[i] != 0) {
            errno = EINVAL;
            return -1;
        }
    }

    size_t capacity = 0;
    size_t current_start = 0;
    size_t next_prefix_start = SIZE_MAX;
    bool have_vcl = false;
    bool key_frame = false;
    size_t nal_start = first_start;
    size_t prefix = first_prefix;

    while (nal_start < size) {
        size_t header = nal_start + prefix;
        if (header >= size)
            goto invalid;
        size_t next_start = size;
        size_t next_prefix = 0;
        (void)find_start_code(data, size, header + 1, &next_start,
                              &next_prefix);
        /*
         * End-of-sequence and end-of-stream NAL units have no RBSP payload.
         * A header-only non-VCL NAL is therefore valid; VCL units still fail
         * below when first_mb_in_slice cannot read their slice header.
         */
        if (next_start < header + 1)
            goto invalid;
        unsigned nal_type = data[header] & 0x1fu;
        bool is_vcl = nal_type >= 1 && nal_type <= 5;

        if (nal_type == 9) {
            if (have_vcl &&
                append_unit(units, unit_count, &capacity, current_start,
                            nal_start, true, key_frame) < 0)
                goto fail;
            current_start = nal_start;
            next_prefix_start = SIZE_MAX;
            have_vcl = false;
            key_frame = false;
        } else if (is_vcl) {
            unsigned first_mb = 0;
            if (!first_mb_in_slice(data + header + 1,
                                   next_start - header - 1, &first_mb))
                goto invalid;
            if (have_vcl && first_mb == 0) {
                size_t boundary = next_prefix_start != SIZE_MAX ?
                    next_prefix_start : nal_start;
                if (append_unit(units, unit_count, &capacity, current_start,
                                boundary, true, key_frame) < 0)
                    goto fail;
                current_start = boundary;
                next_prefix_start = SIZE_MAX;
                key_frame = false;
            }
            have_vcl = true;
            key_frame |= nal_type == 5;
        } else if (have_vcl && next_prefix_start == SIZE_MAX &&
                   (nal_type == 6 || nal_type == 7 || nal_type == 8 ||
                    nal_type == 10 || nal_type == 11 || nal_type == 13 ||
                    nal_type == 14 || nal_type == 15 || nal_type == 18)) {
            next_prefix_start = nal_start;
        }

        if (next_start == size)
            break;
        nal_start = next_start;
        prefix = next_prefix;
    }

    if (append_unit(units, unit_count, &capacity, current_start, size,
                    have_vcl, key_frame) < 0)
        goto fail;
    if (*unit_count == 0)
        goto invalid;
    return 0;

invalid:
    errno = EINVAL;
fail:
    free(*units);
    *units = NULL;
    *unit_count = 0;
    return -1;
}
