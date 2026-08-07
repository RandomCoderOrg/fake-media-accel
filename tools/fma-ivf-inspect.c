#include "fma/av1_obu.h"
#include "fma/ivf.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_file(const char *path, uint8_t **data, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    if (fseek(file, 0, SEEK_END) < 0) {
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) < 0) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    *data = malloc((size_t)length);
    if (!*data) {
        fclose(file);
        return -1;
    }
    if (fread(*data, 1, (size_t)length, file) != (size_t)length) {
        free(*data);
        *data = NULL;
        fclose(file);
        errno = EIO;
        return -1;
    }
    fclose(file);
    *size = (size_t)length;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s INPUT.ivf\n", argv[0]);
        return 2;
    }
    uint8_t *data = NULL;
    size_t size = 0;
    if (read_file(argv[1], &data, &size) < 0) {
        perror("input");
        return 1;
    }
    struct fma_ivf_stream stream;
    if (fma_ivf_parse(data, size, &stream) < 0) {
        perror("IVF parse");
        free(data);
        return 1;
    }
    size_t payload_bytes = 0;
    size_t min_size = SIZE_MAX;
    size_t max_size = 0;
    size_t av1_obus = 0;
    uint8_t max_temporal_id = 0;
    uint8_t max_spatial_id = 0;
    for (size_t i = 0; i < stream.frame_count; ++i) {
        payload_bytes += stream.frames[i].size;
        if (stream.frames[i].size < min_size)
            min_size = stream.frames[i].size;
        if (stream.frames[i].size > max_size)
            max_size = stream.frames[i].size;
        if (stream.fourcc == FMA_IVF_AV1) {
            struct fma_av1_obu_info info;
            if (fma_av1_scan_obus(data + stream.frames[i].offset,
                                  stream.frames[i].size, &info) < 0) {
                perror("AV1 OBU parse");
                fma_ivf_release(&stream);
                free(data);
                return 1;
            }
            av1_obus += info.obu_count;
            if (info.max_temporal_id > max_temporal_id)
                max_temporal_id = info.max_temporal_id;
            if (info.max_spatial_id > max_spatial_id)
                max_spatial_id = info.max_spatial_id;
        }
    }
    char fourcc[5] = {
        (char)(stream.fourcc & 0xff),
        (char)((stream.fourcc >> 8) & 0xff),
        (char)((stream.fourcc >> 16) & 0xff),
        (char)((stream.fourcc >> 24) & 0xff),
        '\0',
    };
    printf("fourcc=%s width=%u height=%u rate=%u scale=%u "
           "packets=%zu declared_packets=%u payload_bytes=%zu "
           "min_packet_bytes=%zu max_packet_bytes=%zu",
           fourcc, stream.width, stream.height, stream.rate, stream.scale,
           stream.frame_count, stream.declared_frames, payload_bytes,
           min_size, max_size);
    if (stream.fourcc == FMA_IVF_AV1)
        printf(" av1_obus=%zu max_temporal_id=%u max_spatial_id=%u",
               av1_obus, max_temporal_id, max_spatial_id);
    putchar('\n');
    fma_ivf_release(&stream);
    free(data);
    return 0;
}
