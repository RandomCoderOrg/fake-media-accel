#include "fma/client.h"
#include "fma/av1_obu.h"
#include "fma/h264_stream.h"
#include "fma/ivf.h"

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
    uint32_t flags;
    int64_t pts_us;
    bool has_pts;
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
    if (codec == FMA_CODEC_H264) {
        struct fma_h264_access_unit *h264_units = NULL;
        size_t h264_count = 0;
        if (fma_h264_split_annexb(data, size, &h264_units, &h264_count) < 0)
            return 0;
        struct access_unit *units = calloc(h264_count, sizeof(*units));
        if (!units) {
            free(h264_units);
            return 0;
        }
        for (size_t i = 0; i < h264_count; ++i) {
            units[i].offset = h264_units[i].offset;
            units[i].size = h264_units[i].size;
            units[i].flags = h264_units[i].key_frame ?
                FMA_PACKET_KEY_FRAME : 0;
        }
        free(h264_units);
        *units_out = units;
        return h264_count;
    }
    if (codec == FMA_CODEC_VP9 || codec == FMA_CODEC_AV1) {
        struct fma_ivf_stream stream;
        if (fma_ivf_parse(data, size, &stream) < 0)
            return 0;
        uint32_t expected_fourcc = codec == FMA_CODEC_VP9 ?
            FMA_IVF_VP9 : FMA_IVF_AV1;
        /*
         * IVF dimensions describe the initial presentation size. VP9 and AV1
         * streams may change coded size later, so the caller supplies the
         * decoder/output bounds independently.
         */
        if (stream.fourcc != expected_fourcc) {
            fma_ivf_release(&stream);
            errno = EINVAL;
            return 0;
        }
        struct access_unit *units = calloc(stream.frame_count, sizeof(*units));
        if (!units) {
            fma_ivf_release(&stream);
            return 0;
        }
        for (size_t i = 0; i < stream.frame_count; ++i) {
            if (codec == FMA_CODEC_AV1) {
                struct fma_av1_obu_info info;
                if (fma_av1_scan_obus(data + stream.frames[i].offset,
                                      stream.frames[i].size, &info) < 0 ||
                    info.max_spatial_id != 0) {
                    free(units);
                    fma_ivf_release(&stream);
                    errno = info.max_spatial_id ? ENOTSUP : EINVAL;
                    return 0;
                }
            }
            units[i].offset = stream.frames[i].offset;
            units[i].size = stream.frames[i].size;
            if (fma_ivf_timestamp_us(&stream, stream.frames[i].timestamp,
                                     &units[i].pts_us) < 0) {
                free(units);
                fma_ivf_release(&stream);
                return 0;
            }
            units[i].has_pts = true;
        }
        size_t frame_count = stream.frame_count;
        fma_ivf_release(&stream);
        *units_out = units;
        return frame_count;
    }
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
            units[count++] = (struct access_unit) {
                .offset = current,
                .size = i - current,
                .flags = 0,
            };
        } else {
            found_aud = true;
        }
        current = i;
        i += start_code - 1;
    }
    if (found_aud && current < size) {
        if (count == capacity) {
            capacity *= 2;
            void *grown = realloc(units, capacity * sizeof(*units));
            if (!grown) {
                free(units);
                return 0;
            }
            units = grown;
        }
        units[count++] = (struct access_unit) {
            .offset = current,
            .size = size - current,
            .flags = 0,
        };
    }
    if (!found_aud) {
        units[0] = (struct access_unit) {
            .offset = 0,
            .size = size,
            .flags = 0,
        };
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
    if (strcmp(name, "vp9") == 0)
        return FMA_CODEC_VP9;
    if (strcmp(name, "av1") == 0)
        return FMA_CODEC_AV1;
    return 0;
}

static int write_visible_nv12(FILE *output,
                              const struct fma_frame_pool *pool,
                              const struct fma_frame *frame,
                              const uint8_t *slot, size_t *written) {
    if (!output || !pool || !frame || !slot || !written ||
        frame->pixel_format != FMA_PIXFMT_NV12 ||
        frame->stride != pool->stride || !frame->width || !frame->height ||
        (frame->width & 1u) || (frame->height & 1u) ||
        frame->width > pool->stride || frame->height > pool->height)
        return -1;
    size_t y_allocation = (size_t)pool->stride * pool->height;
    size_t uv_allocation = y_allocation / 2u;
    if (y_allocation > pool->slot_size ||
        uv_allocation > pool->slot_size - y_allocation)
        return -1;
    const uint8_t *uv = slot + y_allocation;
    for (uint32_t row = 0; row < frame->height; ++row) {
        if (fwrite(slot + (size_t)row * pool->stride, 1,
                   frame->width, output) != frame->width)
            return -1;
    }
    for (uint32_t row = 0; row < frame->height / 2u; ++row) {
        if (fwrite(uv + (size_t)row * pool->stride, 1,
                   frame->width, output) != frame->width)
            return -1;
    }
    *written = (size_t)frame->width * frame->height * 3u / 2u;
    return 0;
}

static int process_message(struct fma_client *client,
                           const struct fma_frame_pool *pool, uint8_t *pool_map,
                           FILE *output, bool visible_output, FILE *frame_info,
                           uint64_t *output_bytes, uint64_t *frames,
                           bool *packet_ack, bool *eos, bool *poll_done) {
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
            const uint8_t *slot =
                pool_map + (size_t)frame.slot * pool->slot_size;
            size_t recorded_bytes = visible_output ?
                (size_t)frame.width * frame.height * 3u / 2u :
                frame.bytes_used;
            if (output) {
                int write_result = 0;
                if (visible_output) {
                    write_result = write_visible_nv12(
                        output, pool, &frame, slot, &recorded_bytes);
                } else if (fwrite(slot, 1, frame.bytes_used, output) !=
                           frame.bytes_used) {
                    write_result = -1;
                }
                if (write_result < 0) {
                    errno = EIO;
                    result = -1;
                }
            }
            if (frame_info &&
                fprintf(frame_info, "%llu,%llu,%zu,%u,%u,%u\n",
                        (unsigned long long)*frames,
                        (unsigned long long)*output_bytes, recorded_bytes,
                        frame.width, frame.height, frame.stride) < 0)
                result = -1;
            *output_bytes += recorded_bytes;
            ++*frames;
            if (fma_client_release_frame(client, frame.slot) < 0)
                result = -1;
        }
    } else if (message.type == FMA_MSG_PACKET_ACK) {
        *packet_ack = true;
    } else if (message.type == FMA_MSG_OUTPUT_EOS) {
        *eos = true;
    } else if (message.type == FMA_MSG_POLL_DONE) {
        *poll_done = true;
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
    bool visible_output = false;
    const char *frame_info_path = NULL;
    int argument = 1;
    while (argument < argc && strncmp(argv[argument], "--", 2) == 0) {
        if (strcmp(argv[argument], "--codec") == 0 && argument + 1 < argc) {
            codec = parse_codec(argv[argument + 1]);
            argument += 2;
        } else if (strcmp(argv[argument], "--repeat") == 0 && argument + 1 < argc) {
            repeats = (uint32_t)strtoul(argv[argument + 1], NULL, 10);
            argument += 2;
        } else if (strcmp(argv[argument], "--visible-output") == 0) {
            visible_output = true;
            argument += 1;
        } else if (strcmp(argv[argument], "--frame-info") == 0 &&
                   argument + 1 < argc) {
            frame_info_path = argv[argument + 1];
            argument += 2;
        } else {
            codec = 0;
            break;
        }
    }
    int remaining = argc - argument;
    if (!codec || !repeats || repeats > 1000 || remaining < 5 || remaining > 6) {
        fprintf(stderr, "usage: %s [--codec h264|hevc|vp9|av1] [--repeat N] "
                "[--visible-output] [--frame-info FILE.csv] "
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
        perror("could not split input");
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
    FILE *frame_info = frame_info_path ? fopen(frame_info_path, "w") : NULL;
    if (frame_info_path && !frame_info) {
        perror("frame info");
        if (output)
            fclose(output);
        return 1;
    }
    if (frame_info)
        fputs("frame,offset,bytes,width,height,stride\n", frame_info);

    uint64_t frames = 0;
    uint64_t frame_bytes = 0;
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t started_ns = monotonic_ns();
    bool eos = false;
    bool failed = false;
    for (uint32_t repeat = 0; repeat < repeats && !failed; ++repeat) {
        eos = false;
        for (size_t i = 0; i < unit_count; ++i) {
            int64_t pts_us = units[i].has_pts ? units[i].pts_us :
                (int64_t)(i * UINT64_C(1000000) / fps);
            bool packet_ack = false;
            bool poll_done = false;
            if (fma_client_queue_packet(&client, input + units[i].offset,
                                        units[i].size, pts_us,
                                        units[i].flags) < 0) {
                perror("decode");
                failed = true;
                break;
            }
            input_bytes += units[i].size;
            while (!packet_ack &&
                   process_message(&client, &pool, pool_map, output,
                                   visible_output, frame_info, &output_bytes,
                                   &frames, &packet_ack, &eos,
                                   &poll_done) == 0) {}
            if (!packet_ack) {
                perror("decode");
                failed = true;
                break;
            }
        }
        if (!failed && fma_client_poll_output(&client, 0) == 0) {
            bool packet_ack = false;
            bool poll_done = false;
            while (!poll_done &&
                   process_message(&client, &pool, pool_map, output,
                                   visible_output, frame_info, &output_bytes,
                                   &frames, &packet_ack, &eos,
                                   &poll_done) == 0) {}
            if (!poll_done)
                failed = true;
        }
        if (!failed && fma_client_drain(&client) == 0) {
            bool packet_ack = false;
            bool poll_done = false;
            while (!eos && process_message(
                               &client, &pool, pool_map, output,
                               visible_output, frame_info, &output_bytes,
                               &frames, &packet_ack, &eos, &poll_done) == 0) {}
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
    printf("compressed_bytes=%llu frame_bytes=%llu output_bytes=%llu "
           "shared_pool_bytes=%zu socket_frame_bytes=0\n",
           (unsigned long long)input_bytes, (unsigned long long)frame_bytes,
           (unsigned long long)output_bytes, pool_bytes);
    if (output)
        fclose(output);
    if (frame_info)
        fclose(frame_info);
    munmap(pool_map, pool_bytes);
    close(pool_fd);
    fma_client_close(&client);
    free(units);
    free(input);
    return !failed && eos ? 0 : 1;
}
