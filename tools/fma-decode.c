#include "fma/client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct access_unit {
    size_t offset;
    size_t size;
};

static uint64_t monotonic_ns(void) {
    struct timespec time;
    return clock_gettime(CLOCK_MONOTONIC, &time) == 0 ?
        (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec : 0;
}

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
        fclose(file);
        errno = EIO;
        return -1;
    }
    fclose(file);
    *size = (size_t)length;
    return 0;
}

static size_t find_access_units(const uint8_t *data, size_t size,
                                uint32_t codec,
                                struct access_unit **units_out) {
    size_t capacity = 64;
    size_t count = 0;
    struct access_unit *units = calloc(capacity, sizeof(*units));
    if (!units)
        return 0;
    size_t current = 0;
    bool found_aud = false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t start_code = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            start_code = 3;
        else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1)
            start_code = 4;
        if (!start_code)
            continue;
        uint32_t nal_type = codec == FMA_CODEC_HEVC ?
            (data[i + start_code] >> 1) & 0x3f : data[i + start_code] & 0x1f;
        uint32_t aud_type = codec == FMA_CODEC_HEVC ? 35u : 9u;
        if (nal_type != aud_type)
            continue;
        if (found_aud) {
            if (count == capacity) {
                capacity *= 2;
                void *grown = realloc(units, capacity * sizeof(*units));
                if (!grown) {
                    free(units);
                    return 0;
                }
                units = grown;
            }
            units[count++] = (struct access_unit){current, i - current};
        } else {
            found_aud = true;
        }
        current = i;
        i += start_code - 1;
    }
    if (found_aud && current < size)
        units[count++] = (struct access_unit){current, size - current};
    if (!found_aud) {
        units[0] = (struct access_unit){0, size};
        count = 1;
    }
    *units_out = units;
    return count;
}

static uint32_t parse_codec(const char *name) {
    if (strcmp(name, "h264") == 0 || strcmp(name, "avc") == 0)
        return FMA_CODEC_H264;
    if (strcmp(name, "hevc") == 0 || strcmp(name, "h265") == 0)
        return FMA_CODEC_HEVC;
    return 0;
}

static int process_message(struct fma_client *client,
                           const struct fma_frame_pool *pool, uint8_t *pool_map,
                           FILE *output, uint64_t *frames, bool *packet_ack,
                           bool *eos) {
    struct fma_message message;
    if (fma_client_receive(client, &message) < 0)
        return -1;
    int result = 0;
    if (message.type == FMA_MSG_FRAME_READY) {
        struct fma_frame frame;
        if (fma_decode_frame(message.payload, message.payload_size, &frame) < 0 ||
            frame.slot >= pool->slot_count || frame.bytes_used > pool->slot_size) {
            errno = EPROTO;
            result = -1;
        } else {
            if (output && fwrite(pool_map + (size_t)frame.slot * pool->slot_size,
                                 1, frame.bytes_used, output) != frame.bytes_used) {
                errno = EIO;
                result = -1;
            }
            ++*frames;
            if (fma_client_release_frame(client, frame.slot) < 0)
                result = -1;
        }
    } else if (message.type == FMA_MSG_PACKET_ACK) {
        *packet_ack = true;
    } else if (message.type == FMA_MSG_OUTPUT_EOS) {
        *eos = true;
    } else if (message.type == FMA_MSG_ERROR) {
        fprintf(stderr, "daemon: %.*s\n", (int)message.payload_size,
                message.payload ? (char *)message.payload : "error");
        errno = EPROTO;
        result = -1;
    }
    fma_message_release(&message);
    return result;
}

int main(int argc, char **argv) {
    uint32_t codec = FMA_CODEC_H264;
    uint32_t repeats = 1;
    int argument = 1;
    while (argument < argc && strncmp(argv[argument], "--", 2) == 0) {
        if (strcmp(argv[argument], "--codec") == 0 && argument + 1 < argc) {
            codec = parse_codec(argv[argument + 1]);
            argument += 2;
        } else if (strcmp(argv[argument], "--repeat") == 0 && argument + 1 < argc) {
            repeats = (uint32_t)strtoul(argv[argument + 1], NULL, 10);
            argument += 2;
        } else {
            codec = 0;
            break;
        }
    }
    int remaining = argc - argument;
    if (!codec || !repeats || repeats > 1000 || remaining < 5 || remaining > 6) {
        fprintf(stderr, "usage: %s [--codec h264|hevc] [--repeat N] "
                "SOCKET INPUT WIDTH HEIGHT FPS [OUTPUT.nv12]\n", argv[0]);
        return 2;
    }
    const char *socket_path = argv[argument + 0];
    const char *input_path = argv[argument + 1];
    uint32_t width = (uint32_t)strtoul(argv[argument + 2], NULL, 10);
    uint32_t height = (uint32_t)strtoul(argv[argument + 3], NULL, 10);
    uint32_t fps = (uint32_t)strtoul(argv[argument + 4], NULL, 10);
    const char *output_path = remaining == 6 ? argv[argument + 5] : NULL;
    if (!width || !height || !fps)
        return 2;

    uint8_t *input = NULL;
    size_t input_size = 0;
    if (read_file(input_path, &input, &input_size) < 0) {
        perror("input");
        return 1;
    }
    struct access_unit *units = NULL;
    size_t unit_count = find_access_units(input, input_size, codec, &units);
    if (!unit_count) {
        fprintf(stderr, "could not split input\n");
        free(input);
        return 1;
    }

    struct fma_client client;
    if (fma_client_connect(&client, socket_path) < 0) {
        perror("connect");
        free(units);
        free(input);
        return 1;
    }
    struct fma_decoder_config config = {
        .codec = codec, .width = width, .height = height,
        .slot_count = FMA_DEFAULT_SLOTS,
    };
    struct fma_frame_pool pool;
    int pool_fd = -1;
    if (fma_client_create_decoder(&client, &config, &pool, &pool_fd) < 0) {
        perror("create decoder");
        fma_client_close(&client);
        free(units);
        free(input);
        return 1;
    }
    size_t pool_bytes = (size_t)pool.slot_count * pool.slot_size;
    uint8_t *pool_map = mmap(NULL, pool_bytes, PROT_READ, MAP_SHARED, pool_fd, 0);
    if (pool_map == MAP_FAILED) {
        perror("mmap");
        close(pool_fd);
        fma_client_close(&client);
        free(units);
        free(input);
        return 1;
    }
    FILE *output = output_path ? fopen(output_path, "wb") : NULL;
    if (output_path && !output) {
        perror("output");
        return 1;
    }

    uint64_t frames = 0;
    uint64_t frame_bytes = 0;
    uint64_t input_bytes = 0;
    uint64_t started_ns = monotonic_ns();
    bool eos = false;
    bool failed = false;
    for (uint32_t repeat = 0; repeat < repeats && !failed; ++repeat) {
        eos = false;
        for (size_t i = 0; i < unit_count; ++i) {
            int64_t pts_us = (int64_t)(i * UINT64_C(1000000) / fps);
            bool packet_ack = false;
            if (fma_client_queue_packet(&client, input + units[i].offset,
                                        units[i].size, pts_us, 0) < 0) {
                perror("decode");
                failed = true;
                break;
            }
            input_bytes += units[i].size;
            while (!packet_ack &&
                   process_message(&client, &pool, pool_map, output, &frames,
                                   &packet_ack, &eos) == 0) {}
            if (!packet_ack) {
                perror("decode");
                failed = true;
                break;
            }
        }
        if (!failed && fma_client_drain(&client) == 0) {
            bool packet_ack = false;
            while (!eos && process_message(&client, &pool, pool_map, output,
                                           &frames, &packet_ack, &eos) == 0) {}
        }
        if (!eos)
            failed = true;
        if (!failed && repeat + 1 < repeats && fma_client_flush(&client) < 0) {
            perror("flush");
            failed = true;
        }
    }
    uint64_t elapsed_ns = monotonic_ns() - started_ns;
    frame_bytes = frames * pool.slot_size;
    double seconds = elapsed_ns ? (double)elapsed_ns / 1000000000.0 : 0.0;
    printf("codec=%s repeats=%u packets=%zu frames=%llu elapsed_ms=%.3f "
           "decode_fps=%.2f\n",
           fma_codec_name(codec), repeats, unit_count * (size_t)repeats,
           (unsigned long long)frames, seconds * 1000.0,
           seconds > 0.0 ? (double)frames / seconds : 0.0);
    printf("compressed_bytes=%llu frame_bytes=%llu shared_pool_bytes=%zu "
           "socket_frame_bytes=0\n",
           (unsigned long long)input_bytes, (unsigned long long)frame_bytes,
           pool_bytes);
    if (output)
        fclose(output);
    munmap(pool_map, pool_bytes);
    close(pool_fd);
    fma_client_close(&client);
    free(units);
    free(input);
    return !failed && eos ? 0 : 1;
}
