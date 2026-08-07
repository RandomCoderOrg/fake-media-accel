#include "fma/h264_stream.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, uint8_t **data, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0)
        goto fail;
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        errno = EINVAL;
        goto fail;
    }
    *data = malloc((size_t)length);
    if (!*data)
        goto fail;
    if (fread(*data, 1, (size_t)length, file) != (size_t)length) {
        free(*data);
        *data = NULL;
        errno = EIO;
        goto fail;
    }
    if (fclose(file) != 0) {
        free(*data);
        *data = NULL;
        return -1;
    }
    *size = (size_t)length;
    return 0;

fail:
    fclose(file);
    return -1;
}

int main(int argc, char **argv) {
    bool list_units = argc == 3 && strcmp(argv[1], "--units") == 0;
    if ((!list_units && argc != 2) || (list_units && argc != 3)) {
        fprintf(stderr, "usage: %s [--units] INPUT.h264\n", argv[0]);
        return 2;
    }
    const char *input_path = argv[list_units ? 2 : 1];
    uint8_t *data = NULL;
    size_t size = 0;
    if (read_file(input_path, &data, &size) < 0) {
        perror("input");
        return 1;
    }
    struct fma_h264_access_unit *units = NULL;
    size_t count = 0;
    if (fma_h264_split_annexb(data, size, &units, &count) < 0) {
        perror("H264 Annex-B parse");
        free(data);
        return 1;
    }
    size_t vcl = 0;
    size_t keys = 0;
    size_t min_bytes = SIZE_MAX;
    size_t max_bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        vcl += units[i].has_vcl;
        keys += units[i].key_frame;
        if (units[i].size < min_bytes)
            min_bytes = units[i].size;
        if (units[i].size > max_bytes)
            max_bytes = units[i].size;
        if (list_units)
            printf("unit=%zu offset=%zu bytes=%zu vcl=%u key=%u\n", i,
                   units[i].offset, units[i].size, units[i].has_vcl,
                   units[i].key_frame);
    }
    printf("bytes=%zu access_units=%zu vcl_units=%zu key_units=%zu "
           "min_unit_bytes=%zu max_unit_bytes=%zu\n",
           size, count, vcl, keys, min_bytes, max_bytes);
    free(units);
    free(data);
    return 0;
}
