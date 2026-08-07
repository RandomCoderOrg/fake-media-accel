#include "fma/av1_obu.h"
#include "fma/protocol.h"
#include "fma/transport.h"

#include <android/hardware_buffer.h>
#include <android/sharedmem.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef bool (*binder_set_max_threads_fn)(uint32_t);
typedef void (*binder_start_thread_pool_fn)(void);
typedef bool (*media_format_get_rect_fn)(AMediaFormat *, const char *,
                                         int32_t *, int32_t *, int32_t *,
                                         int32_t *);

#ifndef DMA_BUF_IOCTL_SYNC
struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_WRITE (2u << 0)
#define DMA_BUF_SYNC_START (0u << 2)
#define DMA_BUF_SYNC_END (1u << 2)
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif

#define FMA_MAX_OUTPUT_TARGETS 64u

struct output_target {
    bool used;
    int64_t pts_us;
    int fd;
    uint8_t *map;
    size_t bytes;
};

struct decoder_session {
    uint64_t id;
    AMediaCodec *codec;
    AImageReader *reader;
    AMediaFormat *format;
    struct fma_frame_pool pool;
    int pool_fd;
    uint8_t *pool_map;
    size_t pool_bytes;
    bool slots[FMA_MAX_SLOTS];
    uint32_t next_slot;
    bool started;
    uint32_t codec_id;
    uint64_t packets;
    uint64_t input_buffers;
    uint64_t input_bytes;
    uint32_t input_chunk_limit;
    uint64_t frames;
    uint64_t frame_bytes;
    uint64_t acquire_ns;
    uint64_t copy_ns;
    bool logged_image_layout;
    uint32_t visible_width;
    uint32_t visible_height;
    uint64_t direct_frames;
    struct output_target targets[FMA_MAX_OUTPUT_TARGETS];
};

static volatile sig_atomic_t running = 1;

static uint64_t monotonic_ns(void) {
    struct timespec time;
    return clock_gettime(CLOCK_MONOTONIC, &time) == 0 ?
        (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec : 0;
}

static void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static bool sync_dma_buf(int fd, uint64_t flags) {
    struct dma_buf_sync sync = {.flags = flags};
    int result;
    do {
        result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

static void release_target(struct output_target *target) {
    if (!target->used)
        return;
    if (target->map && target->map != MAP_FAILED)
        munmap(target->map, target->bytes);
    if (target->fd >= 0)
        close(target->fd);
    memset(target, 0, sizeof(*target));
    target->fd = -1;
}

static void clear_targets(struct decoder_session *session) {
    for (unsigned i = 0; i < FMA_MAX_OUTPUT_TARGETS; ++i)
        release_target(&session->targets[i]);
}

static struct output_target *find_target(struct decoder_session *session,
                                         int64_t pts_us) {
    for (unsigned i = 0; i < FMA_MAX_OUTPUT_TARGETS; ++i)
        if (session->targets[i].used &&
            session->targets[i].pts_us == pts_us)
            return &session->targets[i];
    return NULL;
}

static int register_target(struct decoder_session *session,
                           struct fma_message *request) {
    if (request->fd_count == 0)
        return 0;
    if (request->fd_count != 1 || request->fds[0] < 0) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned i = 0; i < FMA_MAX_OUTPUT_TARGETS; ++i) {
        struct output_target *target = &session->targets[i];
        if (target->used)
            continue;
        int fd = request->fds[0];
        uint8_t *map = mmap(NULL, session->pool.slot_size,
                            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED)
            return -1;
        *target = (struct output_target) {
            .used = true,
            .pts_us = request->pts_us,
            .fd = fd,
            .map = map,
            .bytes = session->pool.slot_size,
        };
        request->fds[0] = -1;
        request->fd_count = 0;
        return 0;
    }
    errno = EBUSY;
    return -1;
}

static int start_binder_thread_pool(void) {
    void *library = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return -1;
    binder_set_max_threads_fn set_max_threads =
        (binder_set_max_threads_fn)dlsym(
            library, "ABinderProcess_setThreadPoolMaxThreadCount");
    binder_start_thread_pool_fn start_thread_pool =
        (binder_start_thread_pool_fn)dlsym(library, "ABinderProcess_startThreadPool");
    if (!set_max_threads || !start_thread_pool || !set_max_threads(4))
        return -1;
    start_thread_pool();
    return 0;
}

static int send_reply(int fd, const struct fma_message *request, uint16_t type,
                      const void *payload, uint32_t payload_size,
                      uint64_t session_id, int passed_fd) {
    struct fma_message reply;
    fma_message_init(&reply, type);
    reply.request_id = request ? request->request_id : 0;
    reply.session_id = session_id;
    reply.pts_us = request ? request->pts_us : 0;
    reply.payload = (uint8_t *)(uintptr_t)payload;
    reply.payload_size = payload_size;
    if (passed_fd >= 0) {
        reply.fd_count = 1;
        reply.fds[0] = passed_fd;
    }
    return fma_send_message(fd, &reply);
}

static int send_error(int fd, const struct fma_message *request, const char *text) {
    fprintf(stderr, "%s\n", text);
    (void)send_reply(fd, request, FMA_MSG_ERROR, text, (uint32_t)strlen(text),
                     request ? request->session_id : 0, -1);
    errno = EPROTO;
    return -1;
}

static void destroy_decoder(struct decoder_session *session) {
    if (session->packets) {
        printf("metrics codec=%s packets=%llu input_buffers=%llu "
               "input_bytes=%llu frames=%llu "
               "direct_frames=%llu "
               "frame_bytes=%llu acquire_ms=%.3f copy_ms=%.3f "
               "socket_frame_bytes=0\n",
               fma_codec_name(session->codec_id),
               (unsigned long long)session->packets,
               (unsigned long long)session->input_buffers,
               (unsigned long long)session->input_bytes,
               (unsigned long long)session->frames,
               (unsigned long long)session->direct_frames,
               (unsigned long long)session->frame_bytes,
               (double)session->acquire_ns / 1000000.0,
               (double)session->copy_ns / 1000000.0);
        fflush(stdout);
    }
    if (session->codec && session->started)
        AMediaCodec_stop(session->codec);
    if (session->codec)
        AMediaCodec_delete(session->codec);
    if (session->format)
        AMediaFormat_delete(session->format);
    if (session->reader)
        AImageReader_delete(session->reader);
    clear_targets(session);
    if (session->pool_map && session->pool_map != MAP_FAILED)
        munmap(session->pool_map, session->pool_bytes);
    if (session->pool_fd >= 0)
        close(session->pool_fd);
    memset(session, 0, sizeof(*session));
    session->pool_fd = -1;
}

static uint32_t probe_decoders(void) {
    uint32_t mask = 0;
    for (uint32_t codec_id = FMA_CODEC_H264; codec_id <= FMA_CODEC_AV1; ++codec_id) {
        AMediaCodec *codec = AMediaCodec_createDecoderByType(fma_codec_mime(codec_id));
        if (codec) {
            mask |= FMA_CODEC_BIT(codec_id);
            AMediaCodec_delete(codec);
        }
    }
    return mask;
}

static int32_t max_input_size(void) {
    const char *value = getenv("FMA_ANDROID_MAX_INPUT_SIZE");
    if (!value || !*value)
        return (int32_t)FMA_MAX_PAYLOAD;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    return end && *end == '\0' && parsed > 0 &&
                   parsed <= (long)FMA_MAX_PAYLOAD ?
        (int32_t)parsed : (int32_t)FMA_MAX_PAYLOAD;
}

static int create_decoder(struct decoder_session *session,
                          const struct fma_decoder_config *config) {
    destroy_decoder(session);
    session->id = 1;
    session->pool_fd = -1;
    const char *mime = fma_codec_mime(config->codec);
    if (!mime || !config->width || !config->height || !config->slot_count ||
        config->width > INT32_MAX || config->height > INT32_MAX ||
        config->slot_count > FMA_MAX_SLOTS ||
        config->width > UINT32_MAX - 63u ||
        config->height > UINT32_MAX - 15u)
        return -1;
    session->codec_id = config->codec;
    session->visible_width = config->width;
    session->visible_height = config->height;

    uint32_t stride = (config->width + 63u) & ~63u;
    uint32_t storage_height = (config->height + 15u) & ~15u;
    uint64_t slot_size = (uint64_t)stride * storage_height * 3u / 2u;
    if (slot_size > UINT32_MAX)
        return -1;
    session->pool = (struct fma_frame_pool) {
        .pixel_format = FMA_PIXFMT_NV12,
        .width = config->width,
        .height = storage_height,
        .stride = stride,
        .slot_count = config->slot_count,
        .slot_size = (uint32_t)slot_size,
    };
    session->pool_bytes = (size_t)session->pool.slot_size * session->pool.slot_count;
    if (session->pool_bytes > FMA_MAX_POOL_BYTES)
        goto fail;
    session->pool_fd = ASharedMemory_create("fake-media-accel-frames",
                                             session->pool_bytes);
    if (session->pool_fd < 0)
        goto fail;
    session->pool_map = mmap(NULL, session->pool_bytes, PROT_READ | PROT_WRITE,
                             MAP_SHARED, session->pool_fd, 0);
    if (session->pool_map == MAP_FAILED) {
        session->pool_map = NULL;
        goto fail;
    }

    media_status_t status = AImageReader_newWithUsage(
        (int32_t)config->width, (int32_t)config->height,
        AIMAGE_FORMAT_YUV_420_888, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        (int32_t)config->slot_count, &session->reader);
    if (status != AMEDIA_OK)
        goto fail;
    ANativeWindow *window = NULL;
    if (AImageReader_getWindow(session->reader, &window) != AMEDIA_OK || !window)
        goto fail;

    session->codec = AMediaCodec_createDecoderByType(mime);
    session->format = AMediaFormat_new();
    if (!session->codec || !session->format)
        goto fail;
    AMediaFormat_setString(session->format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(session->format, AMEDIAFORMAT_KEY_WIDTH,
                          (int32_t)config->width);
    AMediaFormat_setInt32(session->format, AMEDIAFORMAT_KEY_HEIGHT,
                          (int32_t)config->height);
    AMediaFormat_setInt32(session->format, AMEDIAFORMAT_KEY_MAX_WIDTH,
                          (int32_t)config->width);
    AMediaFormat_setInt32(session->format, AMEDIAFORMAT_KEY_MAX_HEIGHT,
                          (int32_t)config->height);
    session->input_chunk_limit = (uint32_t)max_input_size();
    AMediaFormat_setInt32(session->format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                          (int32_t)session->input_chunk_limit);
    if (AMediaCodec_configure(session->codec, session->format, window, NULL, 0) !=
            AMEDIA_OK ||
        AMediaCodec_start(session->codec) != AMEDIA_OK)
        goto fail;
    session->started = true;
    return 0;

fail:
    destroy_decoder(session);
    return -1;
}

static int wait_fence(int fence_fd) {
    if (fence_fd < 0)
        return 0;
    struct pollfd poll_fd = {.fd = fence_fd, .events = POLLIN};
    int result;
    do {
        result = poll(&poll_fd, 1, 1000);
    } while (result < 0 && errno == EINTR);
    close(fence_fd);
    return result == 1 ? 0 : -1;
}

static int acquire_image(AImageReader *reader, AImage **image) {
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int fence_fd = -1;
        media_status_t status =
            AImageReader_acquireNextImageAsync(reader, image, &fence_fd);
        if (status == AMEDIA_OK)
            return wait_fence(fence_fd);
        if (status != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
            return -1;
        usleep(1000);
    }
    return -1;
}

static int find_free_slot(struct decoder_session *session, uint32_t *slot_out) {
    for (uint32_t i = 0; i < session->pool.slot_count; ++i) {
        uint32_t slot = (session->next_slot + i) % session->pool.slot_count;
        if (!session->slots[slot]) {
            session->slots[slot] = true;
            session->next_slot = (slot + 1) % session->pool.slot_count;
            *slot_out = slot;
            return 0;
        }
    }
    errno = EBUSY;
    return -1;
}

static int copy_image_to_nv12(AImage *image, struct decoder_session *session,
                              uint32_t slot, struct output_target *target,
                              struct fma_frame *frame) {
    int32_t planes = 0;
    AImageCropRect crop;
    if (AImage_getNumberOfPlanes(image, &planes) != AMEDIA_OK || planes != 3 ||
        AImage_getCropRect(image, &crop) != AMEDIA_OK)
        return -1;
    int32_t width = crop.right - crop.left;
    int32_t height = crop.bottom - crop.top;
    if (width <= 0 || height <= 0 || (uint32_t)width > session->pool.width ||
        (uint32_t)height > session->pool.height)
        return -1;

    uint8_t *plane_data[3];
    int plane_length[3];
    int32_t row_stride[3];
    int32_t pixel_stride[3];
    for (int plane = 0; plane < 3; ++plane) {
        if (AImage_getPlaneData(image, plane, &plane_data[plane],
                                &plane_length[plane]) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, plane, &row_stride[plane]) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, plane, &pixel_stride[plane]) != AMEDIA_OK)
            return -1;
    }

    uintptr_t u_address = (uintptr_t)plane_data[1];
    uintptr_t v_address = (uintptr_t)plane_data[2];
    bool uv_interleaved = v_address == u_address + 1u &&
                          pixel_stride[1] == 2 && pixel_stride[2] == 2 &&
                          row_stride[1] == row_stride[2];
    if (!session->logged_image_layout) {
        const char *chroma_layout = uv_interleaved ? "UV-interleaved" :
            u_address == v_address + 1u ? "VU-interleaved" : "separate";
        fprintf(stderr,
                "fma-android-layout crop=%dx%d+%d+%d "
                "row_stride=%d,%d,%d pixel_stride=%d,%d,%d "
                "plane_bytes=%d,%d,%d chroma=%s\n",
                width, height, crop.left, crop.top, row_stride[0],
                row_stride[1], row_stride[2], pixel_stride[0],
                pixel_stride[1], pixel_stride[2], plane_length[0],
                plane_length[1], plane_length[2], chroma_layout);
        session->logged_image_layout = true;
    }

    uint8_t *destination = target ? target->map :
        session->pool_map + (size_t)slot * session->pool.slot_size;
    if (target && !sync_dma_buf(target->fd,
                                DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
        return -1;
    size_t y_size = (size_t)session->pool.stride * session->pool.height;
    memset(destination, 16, y_size);
    memset(destination + y_size, 128, y_size / 2);
    for (int32_t y = 0; y < height; ++y) {
        size_t source_row = (size_t)(crop.top + y) * row_stride[0] +
                            (size_t)crop.left * pixel_stride[0];
        size_t destination_row = (size_t)y * session->pool.stride;
        if (pixel_stride[0] == 1 && source_row + (size_t)width <= (size_t)plane_length[0]) {
            memcpy(destination + destination_row, plane_data[0] + source_row,
                   (size_t)width);
        } else {
            for (int32_t x = 0; x < width; ++x) {
                size_t source = source_row + (size_t)x * pixel_stride[0];
                if (source >= (size_t)plane_length[0])
                    goto fail;
                destination[destination_row + (size_t)x] = plane_data[0][source];
            }
        }
    }
    for (int32_t y = 0; y < height / 2; ++y) {
        size_t u_row = (size_t)(crop.top / 2 + y) * row_stride[1] +
                       (size_t)(crop.left / 2) * pixel_stride[1];
        size_t v_row = (size_t)(crop.top / 2 + y) * row_stride[2] +
                       (size_t)(crop.left / 2) * pixel_stride[2];
        size_t destination_row = y_size + (size_t)y * session->pool.stride;
        if (uv_interleaved && width >= 2 && (width & 1) == 0) {
            /*
             * YUV_420_888 commonly exposes the U and V planes as adjacent
             * views into an already interleaved NV12 row. Plane 1's reported
             * length ends at its final U byte, so copy through that byte and
             * take only the last V byte from plane 2 without reading beyond
             * either public plane range.
             */
            size_t last_v = v_row + ((size_t)width / 2u - 1u) * 2u;
            size_t prefix = (size_t)width - 1u;
            if (u_row + prefix > (size_t)plane_length[1] ||
                last_v >= (size_t)plane_length[2])
                goto fail;
            memcpy(destination + destination_row, plane_data[1] + u_row,
                   prefix);
            destination[destination_row + prefix] = plane_data[2][last_v];
            continue;
        }
        for (int32_t x = 0; x < width / 2; ++x) {
            size_t u = u_row + (size_t)x * pixel_stride[1];
            size_t v = v_row + (size_t)x * pixel_stride[2];
            if (u >= (size_t)plane_length[1] || v >= (size_t)plane_length[2])
                goto fail;
            destination[destination_row + (size_t)x * 2] = plane_data[1][u];
            destination[destination_row + (size_t)x * 2 + 1] = plane_data[2][v];
        }
    }
    if (target && !sync_dma_buf(target->fd,
                                DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
        return -1;
    uint32_t visible_width = session->visible_width;
    uint32_t visible_height = session->visible_height;
    if (!visible_width || visible_width > (uint32_t)width)
        visible_width = (uint32_t)width;
    if (!visible_height || visible_height > (uint32_t)height)
        visible_height = (uint32_t)height;
    *frame = (struct fma_frame) {
        .slot = target ? FMA_DIRECT_OUTPUT_SLOT : slot,
        .bytes_used = session->pool.slot_size,
        .width = visible_width,
        .height = visible_height,
        .stride = session->pool.stride,
        .pixel_format = FMA_PIXFMT_NV12,
    };
    return 0;

fail:
    if (target)
        (void)sync_dma_buf(target->fd,
                           DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    return -1;
}

static void update_output_format(struct decoder_session *session) {
    AMediaFormat *format = AMediaCodec_getOutputFormat(session->codec);
    if (!format)
        return;
    int32_t width = 0;
    int32_t height = 0;
    int32_t crop_left = 0;
    int32_t crop_top = 0;
    int32_t crop_right = -1;
    int32_t crop_bottom = -1;
    (void)AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
    (void)AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
    media_format_get_rect_fn get_rect =
        (media_format_get_rect_fn)dlsym(RTLD_DEFAULT, "AMediaFormat_getRect");
    bool have_crop = get_rect &&
        get_rect(format, "crop", &crop_left, &crop_top, &crop_right,
                 &crop_bottom);
    if (!have_crop)
        have_crop =
        AMediaFormat_getInt32(format, "crop-left", &crop_left) &&
        AMediaFormat_getInt32(format, "crop-top", &crop_top) &&
        AMediaFormat_getInt32(format, "crop-right", &crop_right) &&
        AMediaFormat_getInt32(format, "crop-bottom", &crop_bottom);
    if (have_crop && crop_right >= crop_left && crop_bottom >= crop_top) {
        width = crop_right - crop_left + 1;
        height = crop_bottom - crop_top + 1;
    }
    if (width > 0 && height > 0 &&
        (uint32_t)width <= session->pool.width &&
        (uint32_t)height <= session->pool.height) {
        session->visible_width = (uint32_t)width;
        session->visible_height = (uint32_t)height;
    }
    fprintf(stderr, "fma-android-output-format visible=%ux%u %s\n",
            session->visible_width, session->visible_height,
            AMediaFormat_toString(format));
    AMediaFormat_delete(format);
}

static int emit_output(int fd, const struct fma_message *request,
                       struct decoder_session *session, int64_t timeout_us,
                       bool wait_for_eos) {
    unsigned empty_polls = 0;
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t index = AMediaCodec_dequeueOutputBuffer(
            session->codec, &info, empty_polls ? 100000 : timeout_us);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            update_output_format(session);
            continue;
        }
        if (index < 0) {
            if (!wait_for_eos)
                return 0;
            if (++empty_polls >= 100)
                return -1;
            continue;
        }
        empty_polls = 0;
        bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        bool has_frame = info.size > 0;
        bool direct_frame = false;
        if (AMediaCodec_releaseOutputBuffer(session->codec, (size_t)index,
                                            has_frame) != AMEDIA_OK)
            return -1;
        if (has_frame) {
            AImage *image = NULL;
            uint32_t slot = UINT32_MAX;
            struct output_target *target =
                find_target(session, info.presentationTimeUs);
            struct fma_frame frame;
            uint64_t started_ns = monotonic_ns();
            int slot_status = target ? 0 : find_free_slot(session, &slot);
            int acquire_status = slot_status == 0 ?
                acquire_image(session->reader, &image) : -1;
            session->acquire_ns += monotonic_ns() - started_ns;
            started_ns = monotonic_ns();
            int copy_status = acquire_status == 0 ?
                copy_image_to_nv12(image, session, slot, target, &frame) : -1;
            session->copy_ns += monotonic_ns() - started_ns;
            if (slot_status < 0 || acquire_status < 0 || copy_status < 0) {
                if (image)
                    AImage_delete(image);
                if (slot < session->pool.slot_count)
                    session->slots[slot] = false;
                if (target)
                    release_target(target);
                return -1;
            }
            AImage_delete(image);
            session->frames++;
            if (target) {
                session->direct_frames++;
                direct_frame = true;
                release_target(target);
            }
            session->frame_bytes += frame.bytes_used;
            uint8_t payload[24];
            fma_encode_frame(&frame, payload);
            struct fma_message frame_request = *request;
            frame_request.pts_us = info.presentationTimeUs;
            if (send_reply(fd, &frame_request, FMA_MSG_FRAME_READY, payload,
                           sizeof(payload), session->id, -1) < 0)
                return -1;
        }
        if (eos) {
            if (send_reply(fd, request, FMA_MSG_OUTPUT_EOS, NULL, 0,
                           session->id, -1) < 0)
                return -1;
            return 2;
        }
        if (has_frame)
            return direct_frame ? 3 : 1;
        timeout_us = 0;
    }
}

static int receive_frame_release(int fd, struct decoder_session *session) {
    struct fma_message release;
    if (fma_receive_message(fd, &release) < 0)
        return -1;
    int result = -1;
    if (release.type == FMA_MSG_RELEASE_FRAME && release.payload_size == 4) {
        uint32_t slot = fma_get_u32(release.payload);
        if (slot < session->pool.slot_count && session->slots[slot]) {
            session->slots[slot] = false;
            result = 0;
        }
    }
    fma_message_release(&release);
    return result;
}

static int queue_input(struct decoder_session *session,
                       const struct fma_message *request, uint32_t codec_flags) {
    size_t offset = 0;
    bool empty_packet = request->payload_size == 0;
    do {
        ssize_t index = AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        for (unsigned attempt = 0; attempt < 50; ++attempt) {
            index = AMediaCodec_dequeueInputBuffer(session->codec, 100000);
            if (index >= 0)
                break;
        }
        if (index < 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        size_t capacity = 0;
        uint8_t *buffer = AMediaCodec_getInputBuffer(
            session->codec, (size_t)index, &capacity);
        if (session->input_chunk_limit &&
            capacity > session->input_chunk_limit)
            capacity = session->input_chunk_limit;
        size_t remaining = request->payload_size - offset;
        size_t chunk = remaining < capacity ? remaining : capacity;
        if (!buffer || (!empty_packet && chunk == 0)) {
            errno = ENOBUFS;
            return -1;
        }
        if (remaining > capacity &&
            (session->codec_id == FMA_CODEC_VP9 ||
             session->codec_id == FMA_CODEC_AV1)) {
            errno = EMSGSIZE;
            return -1;
        }
        if (chunk)
            memcpy(buffer, request->payload + offset, chunk);
        uint32_t flags = codec_flags;
        if (chunk < remaining)
            flags |= AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME;
        if (AMediaCodec_queueInputBuffer(
                session->codec, (size_t)index, 0, chunk,
                (uint64_t)request->pts_us, flags) != AMEDIA_OK)
            return -1;
        ++session->input_buffers;
        offset += chunk;
        empty_packet = false;
    } while (offset < request->payload_size || empty_packet);
    return 0;
}

static int serve_client(int fd) {
    struct decoder_session session = {.id = 1, .pool_fd = -1};
    int result = 0;
    while (running) {
        struct fma_message request;
        if (fma_receive_message(fd, &request) < 0)
            break;
        if (request.type != FMA_MSG_HELLO && request.session_id != session.id) {
            result = send_error(fd, &request, "invalid session");
            fma_message_release(&request);
            break;
        }
        switch (request.type) {
        case FMA_MSG_HELLO:
            if (request.payload_size != 4 ||
                fma_get_u16(request.payload) != FMA_PROTOCOL_MAJOR)
                result = send_error(fd, &request, "incompatible protocol");
            else
                result = send_reply(fd, &request, FMA_MSG_HELLO_REPLY, NULL, 0,
                                    session.id, -1);
            break;
        case FMA_MSG_QUERY_CAPABILITIES: {
            struct fma_capabilities caps = {
                .decoder_mask = probe_decoders(),
                .pixel_format_mask = 1u << (FMA_PIXFMT_NV12 - 1u),
                .flags = FMA_CAP_SHARED_FRAME_POOL | FMA_CAP_CAN_FLUSH |
                         FMA_CAP_CAN_POLL | FMA_CAP_DIRECT_OUTPUT,
                .max_width = 0,
                .max_height = 0,
            };
            uint8_t payload[20];
            fma_encode_capabilities(&caps, payload);
            result = send_reply(fd, &request, FMA_MSG_CAPABILITIES, payload,
                                sizeof(payload), session.id, -1);
            break;
        }
        case FMA_MSG_CREATE_DECODER: {
            struct fma_decoder_config config;
            if (fma_decode_decoder_config(request.payload, request.payload_size,
                                          &config) < 0 ||
                create_decoder(&session, &config) < 0) {
                result = send_error(fd, &request, "MediaCodec decoder setup failed");
                break;
            }
            result = send_reply(fd, &request, FMA_MSG_DECODER_READY, NULL, 0,
                                session.id, -1);
            if (result == 0) {
                uint8_t payload[24];
                fma_encode_frame_pool(&session.pool, payload);
                result = send_reply(fd, NULL, FMA_MSG_FRAME_POOL, payload,
                                    sizeof(payload), session.id, session.pool_fd);
            }
            break;
        }
        case FMA_MSG_QUEUE_PACKET: {
            uint32_t flags = (request.flags & FMA_PACKET_CODEC_CONFIG) ?
                AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG : 0;
            struct fma_av1_obu_info av1_info;
            bool unsupported_av1_layer = session.codec_id == FMA_CODEC_AV1 &&
                !(request.flags & FMA_PACKET_CODEC_CONFIG) &&
                (fma_av1_scan_obus(request.payload, request.payload_size,
                                   &av1_info) < 0 ||
                 av1_info.max_spatial_id != 0);
            session.packets++;
            session.input_bytes += request.payload_size;
            bool wants_direct = request.fd_count != 0;
            int target_status = wants_direct ?
                register_target(&session, &request) : 0;
            if (!session.started || unsupported_av1_layer || target_status < 0 ||
                queue_input(&session, &request, flags) < 0 ||
                emit_output(fd, &request, &session, 0, false) < 0) {
                struct output_target *target =
                    find_target(&session, request.pts_us);
                if (target)
                    release_target(target);
                result = send_error(fd, &request, "MediaCodec packet failed");
            } else {
                result = send_reply(fd, &request, FMA_MSG_PACKET_ACK, NULL, 0,
                                    session.id, -1);
            }
            break;
        }
        case FMA_MSG_RELEASE_FRAME: {
            uint32_t slot = request.payload_size == 4 ? fma_get_u32(request.payload) : 16;
            if (slot >= session.pool.slot_count)
                result = send_error(fd, &request, "invalid frame slot");
            else
                session.slots[slot] = false;
            break;
        }
        case FMA_MSG_FLUSH:
            if (!session.started || AMediaCodec_flush(session.codec) != AMEDIA_OK)
                result = send_error(fd, &request, "MediaCodec flush failed");
            else {
                memset(session.slots, 0, sizeof(session.slots));
                clear_targets(&session);
                result = send_reply(fd, &request, FMA_MSG_FLUSHED, NULL, 0,
                                    session.id, -1);
            }
            break;
        case FMA_MSG_DRAIN:
            if (!session.started ||
                queue_input(&session, &request,
                            AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) < 0) {
                result = send_error(fd, &request, "MediaCodec drain failed");
                break;
            }
            for (;;) {
                int output = emit_output(fd, &request, &session, 100000, true);
                if (output == 2)
                    break;
                if (output < 0 || (output == 1 &&
                                   receive_frame_release(fd, &session) < 0)) {
                    result = send_error(fd, &request, "MediaCodec drain failed");
                    break;
                }
            }
            break;
        case FMA_MSG_POLL_OUTPUT: {
            if (!session.started || request.payload_size != 4) {
                result = send_error(fd, &request, "invalid output poll");
                break;
            }
            uint32_t timeout_ms = fma_get_u32(request.payload);
            if (timeout_ms > 1000)
                timeout_ms = 1000;
            int output = emit_output(fd, &request, &session,
                                     (int64_t)timeout_ms * 1000, false);
            if (output < 0)
                result = send_error(fd, &request, "MediaCodec output poll failed");
            else
                result = send_reply(fd, &request, FMA_MSG_POLL_DONE, NULL, 0,
                                    session.id, -1);
            break;
        }
        case FMA_MSG_CLOSE:
            fma_message_release(&request);
            destroy_decoder(&session);
            return 0;
        default:
            result = send_error(fd, &request, "unsupported message");
            break;
        }
        fma_message_release(&request);
        if (result < 0)
            break;
    }
    destroy_decoder(&session);
    return result;
}

int main(int argc, char **argv) {
    const char *socket_path = argc == 2 ? argv[1] : "@fake-media-accel";
    if (start_binder_thread_pool() < 0)
        fprintf(stderr, "NDK Binder pool unavailable; using MediaCodec defaults\n");
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    int server = fma_listen_unix(socket_path, 4);
    if (server < 0) {
        perror("listen");
        return 1;
    }
    if (socket_path[0] != '@')
        chmod(socket_path, 0600);
    printf("fake-media-acceld listening on %s\n", socket_path);
    fflush(stdout);
    while (running) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        (void)serve_client(client);
        close(client);
    }
    close(server);
    if (socket_path[0] != '@')
        unlink(socket_path);
    return 0;
}
