#define _GNU_SOURCE

#include "fma/client.h"
#include "fma/va_private.h"
#include "h264_annexb.h"
#include "h264_timing.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <va/va_backend.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_vp9.h>
#include <va/va_drmcommon.h>

#if defined(__has_include)
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#endif
#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#endif
#endif

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC                                                   \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif

#ifndef DMA_BUF_IOCTL_SYNC
struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_READ (1u << 0)
#define DMA_BUF_SYNC_WRITE (2u << 0)
#define DMA_BUF_SYNC_RW (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START (0u << 2)
#define DMA_BUF_SYNC_END (1u << 2)
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif

#define FMA_DRM_FOURCC(a, b, c, d)                                            \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |           \
     ((uint32_t)(d) << 24))
#define FMA_DRM_FORMAT_R8 FMA_DRM_FOURCC('R', '8', ' ', ' ')
#define FMA_DRM_FORMAT_GR88 FMA_DRM_FOURCC('G', 'R', '8', '8')
#define FMA_DRM_FORMAT_NV12 FMA_DRM_FOURCC('N', 'V', '1', '2')
#define FMA_DRM_FORMAT_MOD_LINEAR UINT64_C(0)

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#define FMA_VA_MAX_CONFIGS 16
#define FMA_VA_MAX_SURFACES 64
#define FMA_VA_MAX_CONTEXTS 8
#define FMA_VA_MAX_BUFFERS 256
#define FMA_VA_IMAGE_BUFFER_BASE 192
#define FMA_VA_MAX_IMAGES 64

struct va_config {
    bool used;
    VAProfile profile;
    VAEntrypoint entrypoint;
};

struct va_surface {
    bool used;
    unsigned width;
    unsigned height;
    unsigned allocation_height;
    unsigned stride;
    size_t uv_offset;
    size_t size;
    uint8_t *data;
    int dma_buf_fd;
    int64_t pts_us;
    VASurfaceStatus status;
    unsigned derived_images;
    unsigned external_handles;
};

struct va_context {
    bool used;
    bool retired;
    bool needs_stream_reset;
    bool io_lock_initialized;
    pthread_mutex_t io_lock;
    VAConfigID config_id;
    unsigned width;
    unsigned height;
    unsigned decoder_width;
    unsigned decoder_height;
    uint32_t codec_id;
    VAProfile profile;
    VASurfaceID target;
    struct fma_h264_timeline timeline;
    struct fma_client client;
    struct fma_frame_pool pool;
    int pool_fd;
    uint8_t *pool_map;
    size_t pool_bytes;
    uint8_t *slices;
    size_t slice_size;
    size_t slice_capacity;
    VAPictureParameterBufferH264 picture;
    VASliceParameterBufferH264 first_slice;
    VASliceParameterBufferH264 *pending_slices;
    size_t pending_slice_count;
    size_t pending_slice_capacity;
    VAIQMatrixBufferH264 iq_matrix;
    bool have_picture;
    bool have_first_slice;
    bool have_iq_matrix;
    bool slice_fragment_open;
    bool direct_output;
    uint64_t stored_frames;
    uint64_t pending_outputs;
    uint64_t next_pts_us;
    VADecPictureParameterBufferVP9 vp9_picture;
    VASliceParameterBufferVP9 vp9_slice;
    bool have_vp9_slice;
    VADecPictureParameterBufferAV1 av1_picture;
    bool have_av1_packet;
    uint32_t av1_packet_flags;
};

struct va_buffer {
    bool used;
    VABufferType type;
    unsigned size;
    unsigned elements;
    void *data;
    bool owns_data;
    VASurfaceID derived_surface;
    bool mapped;
    int exported_fd;
    bool acquired;
};

struct va_image {
    bool used;
    VAImage image;
    VASurfaceID derived_surface;
};

struct va_driver {
    uint64_t initialized_ns;
    uint64_t contexts_created;
    uint64_t context_create_ns;
    uint64_t contexts_destroyed;
    uint64_t context_destroy_ns;
    uint64_t contexts_reused;
    uint64_t submitted_frames;
    uint64_t submission_ns;
    uint64_t stored_frames;
    uint64_t direct_frames;
    uint64_t store_copy_bytes;
    uint64_t store_copy_ns;
    uint64_t sync_calls;
    uint64_t sync_ns;
    uint64_t derive_calls;
    uint64_t derive_ns;
    uint64_t surface_map_calls;
    uint64_t acquire_handle_calls;
    uint64_t get_image_calls;
    uint64_t get_image_bytes;
    uint64_t get_image_ns;
    struct va_config configs[FMA_VA_MAX_CONFIGS];
    struct va_surface surfaces[FMA_VA_MAX_SURFACES];
    struct va_context contexts[FMA_VA_MAX_CONTEXTS];
    struct va_buffer buffers[FMA_VA_MAX_BUFFERS];
    struct va_image images[FMA_VA_MAX_IMAGES];
};

static bool debug_enabled(void) {
    const char *value = getenv("FMA_VA_DEBUG");
    return value && *value && strcmp(value, "0") != 0;
}

static bool metrics_enabled(void) {
    const char *value = getenv("FMA_VA_METRICS");
    return value && *value && strcmp(value, "0") != 0;
}

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static double driver_elapsed_ms(const struct va_driver *driver) {
    return (double)(monotonic_ns() - driver->initialized_ns) / 1000000.0;
}

static void dump_packet(const uint8_t *packet, size_t packet_size) {
    const char *path = getenv("FMA_VA_DUMP");
    if (!path || !*path)
        return;
    FILE *file = fopen(path, "ab");
    if (!file) {
        if (debug_enabled())
            perror("fma-va: open packet dump");
        return;
    }
    if (fwrite(packet, 1, packet_size, file) != packet_size && debug_enabled())
        perror("fma-va: write packet dump");
    if (fclose(file) != 0 && debug_enabled())
        perror("fma-va: close packet dump");
}

static struct va_config *get_config(struct va_driver *driver, VAConfigID id) {
    return id && id <= FMA_VA_MAX_CONFIGS && driver->configs[id - 1].used ?
        &driver->configs[id - 1] : NULL;
}

static struct va_surface *get_surface(struct va_driver *driver,
                                      VASurfaceID id) {
    return id && id <= FMA_VA_MAX_SURFACES && driver->surfaces[id - 1].used ?
        &driver->surfaces[id - 1] : NULL;
}

static struct va_context *get_context(struct va_driver *driver,
                                      VAContextID id) {
    return id && id <= FMA_VA_MAX_CONTEXTS &&
        driver->contexts[id - 1].used && !driver->contexts[id - 1].retired ?
        &driver->contexts[id - 1] : NULL;
}

static struct va_buffer *get_buffer(struct va_driver *driver, VABufferID id) {
    return id && id <= FMA_VA_MAX_BUFFERS && driver->buffers[id - 1].used ?
        &driver->buffers[id - 1] : NULL;
}

static VAImageFormat nv12_image_format(void) {
    VAImageFormat format;
    memset(&format, 0, sizeof(format));
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    return format;
}

static VAImageFormat i420_image_format(void) {
    VAImageFormat format;
    memset(&format, 0, sizeof(format));
    format.fourcc = VA_FOURCC_I420;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    return format;
}

static bool nv12_layout(unsigned width, unsigned height, unsigned pitch,
                        size_t *y_size, size_t *bytes) {
    if (!width || !height || pitch < width || (pitch & 1u) ||
        (size_t)pitch > SIZE_MAX / height)
        return false;
    size_t y = (size_t)pitch * height;
    unsigned chroma_rows = (height + 1u) / 2u;
    if ((size_t)pitch > SIZE_MAX / chroma_rows)
        return false;
    size_t uv = (size_t)pitch * chroma_rows;
    if (uv > SIZE_MAX - y || y + uv > UINT_MAX)
        return false;
    if (y_size)
        *y_size = y;
    if (bytes)
        *bytes = y + uv;
    return true;
}

static const char *dma_heap_path(void) {
    const char *path = getenv("FMA_VA_DMA_HEAP");
    if (path && (!*path || strcmp(path, "0") == 0 ||
                 strcmp(path, "none") == 0))
        return NULL;
    return path ? path : "/dev/dma_heap/system";
}

static bool dma_heap_available(void) {
    const char *path = dma_heap_path();
    if (!path)
        return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static int allocate_dma_buf(size_t bytes) {
    const char *path = dma_heap_path();
    if (!path)
        return -1;
    int heap_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (heap_fd < 0)
        return -1;
    struct dma_heap_allocation_data allocation;
    memset(&allocation, 0, sizeof(allocation));
    allocation.len = bytes;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    int result;
    do {
        result = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation);
    } while (result < 0 && errno == EINTR);
    int saved = errno;
    close(heap_fd);
    if (result < 0) {
        errno = saved;
        return -1;
    }
    return (int)allocation.fd;
}

static bool sync_dma_buf(int fd, uint64_t flags) {
    if (fd < 0)
        return true;
    struct dma_buf_sync sync = {.flags = flags};
    int result;
    do {
        result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && debug_enabled())
        perror("fma-va: DMA_BUF_IOCTL_SYNC");
    return result == 0;
}

static bool begin_surface_cpu_access(struct va_surface *surface,
                                     uint64_t access) {
    return sync_dma_buf(surface->dma_buf_fd, DMA_BUF_SYNC_START | access);
}

static bool end_surface_cpu_access(struct va_surface *surface,
                                   uint64_t access) {
    return sync_dma_buf(surface->dma_buf_fd, DMA_BUF_SYNC_END | access);
}

static void release_surface(struct va_surface *surface) {
    if (surface->dma_buf_fd >= 0) {
        if (surface->data && surface->data != MAP_FAILED)
            munmap(surface->data, surface->size);
        close(surface->dma_buf_fd);
    } else {
        free(surface->data);
    }
    memset(surface, 0, sizeof(*surface));
}

static uint32_t codec_for_profile(VAProfile profile) {
    if (profile == VAProfileH264ConstrainedBaseline ||
        profile == VAProfileH264Main || profile == VAProfileH264High)
        return FMA_CODEC_H264;
    if (profile == VAProfileVP9Profile0)
        return FMA_CODEC_VP9;
    if (profile == VAProfileAV1Profile0)
        return FMA_CODEC_AV1;
    return 0;
}

static bool profile_supported(VAProfile profile) {
    return codec_for_profile(profile) != 0;
}

static bool append_data(uint8_t **data, size_t *size, size_t *capacity,
                        const void *source, size_t source_size) {
    if (source_size > SIZE_MAX - *size)
        return false;
    size_t required = *size + source_size;
    if (required > *capacity) {
        size_t next = *capacity ? *capacity : 4096;
        while (next < required) {
            if (next > SIZE_MAX / 2) {
                next = required;
                break;
            }
            next *= 2;
        }
        void *resized = realloc(*data, next);
        if (!resized)
            return false;
        *data = resized;
        *capacity = next;
    }
    memcpy(*data + *size, source, source_size);
    *size = required;
    return true;
}

static void close_context(struct va_context *context) {
    if (context->client.socket_fd >= 0)
        fma_client_close(&context->client);
    if (context->pool_map && context->pool_map != MAP_FAILED)
        munmap(context->pool_map, context->pool_bytes);
    if (context->pool_fd >= 0)
        close(context->pool_fd);
    free(context->slices);
    free(context->pending_slices);
    if (context->io_lock_initialized)
        pthread_mutex_destroy(&context->io_lock);
    memset(context, 0, sizeof(*context));
    context->client.socket_fd = -1;
    context->pool_fd = -1;
}

static bool store_frame(struct va_driver *driver, struct va_context *context,
                        const struct fma_message *message) {
    struct fma_frame frame;
    if (fma_decode_frame(message->payload, message->payload_size, &frame) < 0)
        return false;
    bool direct_output = frame.slot == FMA_DIRECT_OUTPUT_SLOT;
    if (!direct_output &&
        (frame.slot >= context->pool.slot_count ||
         frame.bytes_used > context->pool.slot_size))
        return false;
    struct va_surface *surface = NULL;
    for (unsigned i = 0; i < FMA_VA_MAX_SURFACES; ++i) {
        if (driver->surfaces[i].used &&
            driver->surfaces[i].pts_us == message->pts_us) {
            surface = &driver->surfaces[i];
            break;
        }
    }
    bool valid = surface != NULL;
    uint8_t sample_y0 = 0;
    uint8_t sample_yc = 0;
    uint8_t sample_u0 = 0;
    uint8_t sample_v0 = 0;
    uint8_t sample_uc = 0;
    uint8_t sample_vc = 0;
    uint64_t copy_started_ns = 0;
    if (surface) {
        if (frame.width > surface->stride ||
            frame.height > surface->allocation_height ||
            frame.stride < frame.width || frame.pixel_format != FMA_PIXFMT_NV12)
            valid = false;
        else if (direct_output) {
            if (surface->dma_buf_fd < 0 || frame.bytes_used > surface->size)
                valid = false;
            else {
                surface->status = VASurfaceReady;
                driver->stored_frames++;
                driver->direct_frames++;
            }
        } else if ((copy_started_ns = monotonic_ns(),
                  !begin_surface_cpu_access(surface, DMA_BUF_SYNC_WRITE))) {
            valid = false;
        } else {
            const uint8_t *source = context->pool_map +
                (size_t)frame.slot * context->pool.slot_size;
            size_t source_y = (size_t)frame.stride * context->pool.height;
            size_t destination_y = surface->uv_offset;
            bool identical_layout = frame.width == frame.stride &&
                frame.stride == surface->stride &&
                frame.height == context->pool.height &&
                frame.height == surface->allocation_height &&
                source_y == destination_y && frame.bytes_used >= surface->size;
            if (identical_layout) {
                memcpy(surface->data, source, surface->size);
            } else {
                memset(surface->data, 16, destination_y);
                memset(surface->data + destination_y, 128,
                       surface->size - destination_y);
                for (uint32_t row = 0; row < frame.height; ++row)
                    memcpy(surface->data + (size_t)row * surface->stride,
                           source + (size_t)row * frame.stride, frame.width);
                for (uint32_t row = 0;
                     row < (frame.height + 1u) / 2u; ++row)
                    memcpy(surface->data + destination_y +
                               (size_t)row * surface->stride,
                           source + source_y + (size_t)row * frame.stride,
                           frame.width);
            }
            unsigned center_x = (frame.width / 2u) & ~1u;
            unsigned center_y = frame.height / 2u;
            unsigned center_uv_y = frame.height / 4u;
            sample_y0 = surface->data[0];
            sample_yc = surface->data[(size_t)center_y * surface->stride +
                                      center_x];
            sample_u0 = surface->data[destination_y];
            sample_v0 = surface->data[destination_y + 1];
            sample_uc = surface->data[destination_y +
                                      (size_t)center_uv_y * surface->stride +
                                      center_x];
            sample_vc = surface->data[destination_y +
                                      (size_t)center_uv_y * surface->stride +
                                      center_x + 1];
            if (!end_surface_cpu_access(surface, DMA_BUF_SYNC_WRITE))
                valid = false;
            else
                surface->status = VASurfaceReady;
        }
    }
    if (copy_started_ns) {
        driver->store_copy_ns += monotonic_ns() - copy_started_ns;
        if (valid) {
            driver->stored_frames++;
            driver->store_copy_bytes +=
                (uint64_t)frame.width * frame.height * 3u / 2u;
        }
    }
    ++context->stored_frames;
    if (context->pending_outputs)
        --context->pending_outputs;
    if (debug_enabled()) {
        fprintf(stderr, "fma-va: frame pts=%lld slot=%u matched=%d\n",
                (long long)message->pts_us, frame.slot, surface != NULL);
        if (surface && (context->stored_frames <= 8 ||
                        context->stored_frames % 30 == 0))
            fprintf(stderr,
                    "fma-va: sample frame=%llu visible=%ux%u stride=%u "
                    "storage_h=%u y0=%u yc=%u uv0=%u,%u uvc=%u,%u\n",
                    (unsigned long long)context->stored_frames, frame.width,
                    frame.height, frame.stride, surface->allocation_height,
                    sample_y0, sample_yc, sample_u0, sample_v0, sample_uc,
                    sample_vc);
    }
    if (!direct_output &&
        fma_client_release_frame(&context->client, frame.slot) < 0) {
        if (debug_enabled())
            perror("fma-va: release frame");
        return false;
    }
    return valid;
}

static bool process_until(struct va_driver *driver, struct va_context *context,
                          uint16_t terminal) {
    for (;;) {
        struct fma_message message;
        if (fma_client_receive(&context->client, &message) < 0) {
            if (debug_enabled())
                perror("fma-va: receive");
            return false;
        }
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: io type=%u terminal=%u request=%llu pts=%lld\n",
                    message.type, terminal,
                    (unsigned long long)message.request_id,
                    (long long)message.pts_us);
        bool valid = true;
        if (message.type == FMA_MSG_FRAME_READY)
            valid = store_frame(driver, context, &message);
        else if (message.type == FMA_MSG_ERROR) {
            if (debug_enabled())
                fprintf(stderr, "fma-va: daemon error: %.*s\n",
                        (int)message.payload_size,
                        message.payload ? (const char *)message.payload : "");
            valid = false;
        }
        bool done = message.type == terminal;
        fma_message_release(&message);
        if (!valid) {
            if (debug_enabled())
                fprintf(stderr, "fma-va: message processing failed\n");
            return false;
        }
        if (done)
            return true;
    }
}

static bool collect_pending_outputs(struct va_driver *driver,
                                    struct va_context *context) {
    for (unsigned attempt = 0;
         context->pending_outputs && attempt < 20; ++attempt) {
        if (fma_client_poll_output(&context->client, 50) < 0 ||
            !process_until(driver, context, FMA_MSG_POLL_DONE))
            return false;
    }
    return context->pending_outputs == 0;
}

static VAStatus terminate(VADriverContextP ctx) {
    struct va_driver *driver = ctx->pDriverData;
    if (!driver)
        return VA_STATUS_SUCCESS;
    if (metrics_enabled())
        fprintf(stderr,
                "fma-va-metrics contexts_created=%llu create_ms=%.3f "
                "contexts_destroyed=%llu destroy_ms=%.3f reused=%llu "
                "submitted=%llu submission_ms=%.3f "
                "stored=%llu direct=%llu store_copy_mib=%.3f "
                "store_copy_ms=%.3f "
                "sync_calls=%llu sync_ms=%.3f derive_calls=%llu "
                "derive_ms=%.3f surface_maps=%llu acquire_handles=%llu "
                "get_image_calls=%llu get_image_mib=%.3f "
                "get_image_ms=%.3f\n",
                (unsigned long long)driver->contexts_created,
                (double)driver->context_create_ns / 1000000.0,
                (unsigned long long)driver->contexts_destroyed,
                (double)driver->context_destroy_ns / 1000000.0,
                (unsigned long long)driver->contexts_reused,
                (unsigned long long)driver->submitted_frames,
                (double)driver->submission_ns / 1000000.0,
                (unsigned long long)driver->stored_frames,
                (unsigned long long)driver->direct_frames,
                (double)driver->store_copy_bytes / (1024.0 * 1024.0),
                (double)driver->store_copy_ns / 1000000.0,
                (unsigned long long)driver->sync_calls,
                (double)driver->sync_ns / 1000000.0,
                (unsigned long long)driver->derive_calls,
                (double)driver->derive_ns / 1000000.0,
                (unsigned long long)driver->surface_map_calls,
                (unsigned long long)driver->acquire_handle_calls,
                (unsigned long long)driver->get_image_calls,
                (double)driver->get_image_bytes / (1024.0 * 1024.0),
                (double)driver->get_image_ns / 1000000.0);
    for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i)
        if (driver->contexts[i].used)
            close_context(&driver->contexts[i]);
    for (unsigned i = 0; i < FMA_VA_MAX_SURFACES; ++i)
        if (driver->surfaces[i].used)
            release_surface(&driver->surfaces[i]);
    for (unsigned i = 0; i < FMA_VA_MAX_BUFFERS; ++i)
        if (driver->buffers[i].used) {
            if (driver->buffers[i].acquired &&
                driver->buffers[i].exported_fd >= 0)
                close(driver->buffers[i].exported_fd);
            if (driver->buffers[i].owns_data)
                free(driver->buffers[i].data);
        }
    free(driver);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus query_profiles(VADriverContextP ctx, VAProfile *profiles,
                               int *count) {
    (void)ctx;
    if (!profiles || !count)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    profiles[0] = VAProfileH264ConstrainedBaseline;
    profiles[1] = VAProfileH264Main;
    profiles[2] = VAProfileH264High;
    profiles[3] = VAProfileVP9Profile0;
    profiles[4] = VAProfileAV1Profile0;
    *count = 5;
    return VA_STATUS_SUCCESS;
}

static VAStatus query_entrypoints(VADriverContextP ctx, VAProfile profile,
                                  VAEntrypoint *entrypoints, int *count) {
    (void)ctx;
    if (!entrypoints || !count)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    entrypoints[0] = VAEntrypointVLD;
    *count = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus get_config_attributes(VADriverContextP ctx, VAProfile profile,
                                      VAEntrypoint entrypoint,
                                      VAConfigAttrib *attributes, int count) {
    (void)ctx;
    if (!profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int i = 0; i < count; ++i)
        attributes[i].value = attributes[i].type == VAConfigAttribRTFormat ?
            VA_RT_FORMAT_YUV420 : VA_ATTRIB_NOT_SUPPORTED;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_config(VADriverContextP ctx, VAProfile profile,
                              VAEntrypoint entrypoint,
                              VAConfigAttrib *attributes, int count,
                              VAConfigID *id) {
    struct va_driver *driver = ctx->pDriverData;
    if (!id)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int i = 0; i < count; ++i)
        if (attributes[i].type == VAConfigAttribRTFormat &&
            !(attributes[i].value & VA_RT_FORMAT_YUV420))
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    for (unsigned i = 0; i < FMA_VA_MAX_CONFIGS; ++i) {
        if (driver->configs[i].used)
            continue;
        driver->configs[i] = (struct va_config) {
            .used = true, .profile = profile, .entrypoint = entrypoint,
        };
        *id = i + 1;
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus destroy_config(VADriverContextP ctx, VAConfigID id) {
    struct va_config *config = get_config(ctx->pDriverData, id);
    if (!config)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    memset(config, 0, sizeof(*config));
    return VA_STATUS_SUCCESS;
}

static VAStatus query_config_attributes(VADriverContextP ctx, VAConfigID id,
                                        VAProfile *profile,
                                        VAEntrypoint *entrypoint,
                                        VAConfigAttrib *attributes,
                                        int *count) {
    struct va_config *config = get_config(ctx->pDriverData, id);
    if (!config)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    if (profile)
        *profile = config->profile;
    if (entrypoint)
        *entrypoint = config->entrypoint;
    if (count) {
        if (attributes && *count > 0) {
            attributes[0].type = VAConfigAttribRTFormat;
            attributes[0].value = VA_RT_FORMAT_YUV420;
            *count = 1;
        } else
            *count = 0;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus destroy_surfaces(VADriverContextP ctx, VASurfaceID *ids,
                                 int count);
static VAStatus sync_surface(VADriverContextP ctx, VASurfaceID id);

static VAStatus create_surfaces(VADriverContextP ctx, int width, int height,
                                int format, int count, VASurfaceID *ids) {
    struct va_driver *driver = ctx->pDriverData;
    if (!ids || width <= 0 || height <= 0 || count <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!(format & VA_RT_FORMAT_YUV420))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    unsigned stride = ((unsigned)width + 63u) & ~63u;
    unsigned allocation_height = ((unsigned)height + 15u) & ~15u;
    size_t uv_offset;
    size_t bytes;
    if (!nv12_layout((unsigned)width, allocation_height, stride, &uv_offset,
                     &bytes))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    int created = 0;
    for (unsigned i = 0; i < FMA_VA_MAX_SURFACES && created < count; ++i) {
        if (driver->surfaces[i].used)
            continue;
        struct va_surface surface = {
            .used = true, .width = (unsigned)width,
            .height = (unsigned)height,
            .allocation_height = allocation_height,
            .stride = stride, .uv_offset = uv_offset, .size = bytes,
            .dma_buf_fd = -1, .pts_us = -1, .status = VASurfaceReady,
        };
        surface.dma_buf_fd = allocate_dma_buf(bytes);
        if (surface.dma_buf_fd >= 0) {
            surface.data = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                                surface.dma_buf_fd, 0);
            if (surface.data == MAP_FAILED ||
                !begin_surface_cpu_access(&surface, DMA_BUF_SYNC_WRITE)) {
                if (surface.data == MAP_FAILED)
                    surface.data = NULL;
                release_surface(&surface);
                surface.dma_buf_fd = -1;
            } else {
                memset(surface.data, 0, bytes);
                if (!end_surface_cpu_access(&surface, DMA_BUF_SYNC_WRITE)) {
                    release_surface(&surface);
                    surface.dma_buf_fd = -1;
                }
            }
        }
        if (surface.dma_buf_fd < 0) {
            surface.data = calloc(1, bytes);
            if (!surface.data)
                break;
        }
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: surface %ux%u storage=%ux%u backing=%s "
                    "bytes=%zu\n",
                    surface.width, surface.height, surface.stride,
                    surface.allocation_height,
                    surface.dma_buf_fd >= 0 ? "dma-buf" : "malloc", bytes);
        driver->surfaces[i] = surface;
        ids[created++] = i + 1;
    }
    if (created == count)
        return VA_STATUS_SUCCESS;
    destroy_surfaces(ctx, ids, created);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus destroy_surfaces(VADriverContextP ctx, VASurfaceID *ids,
                                 int count) {
    struct va_driver *driver = ctx->pDriverData;
    if (!ids || count < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (int i = 0; i < count; ++i) {
        struct va_surface *surface = get_surface(driver, ids[i]);
        if (!surface)
            return VA_STATUS_ERROR_INVALID_SURFACE;
        if (surface->derived_images || surface->external_handles)
            return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    for (int i = 0; i < count; ++i) {
        struct va_surface *surface = get_surface(driver, ids[i]);
        release_surface(surface);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus create_context(VADriverContextP ctx, VAConfigID config_id,
                               int width, int height, int flags,
                               VASurfaceID *targets, int target_count,
                               VAContextID *id) {
    (void)flags;
    struct va_driver *driver = ctx->pDriverData;
    uint64_t started_ns = monotonic_ns();
    struct va_config *va_config = get_config(driver, config_id);
    if (!id || width <= 0 || height <= 0 || !va_config)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    uint32_t codec = codec_for_profile(va_config->profile);
    if (!codec)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    for (int i = 0; i < target_count; ++i) {
        struct va_surface *surface = get_surface(driver, targets[i]);
        if (!surface)
            return VA_STATUS_ERROR_INVALID_SURFACE;
        if ((unsigned)width > surface->stride ||
            (unsigned)height > surface->allocation_height)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i) {
        struct va_context *context = &driver->contexts[i];
        if (!context->used || !context->retired ||
            context->codec_id != codec ||
            context->profile != va_config->profile ||
            (unsigned)width > context->decoder_width ||
            (unsigned)height > context->decoder_height)
            continue;
        context->retired = false;
        context->needs_stream_reset = true;
        context->config_id = config_id;
        context->width = (unsigned)width;
        context->height = (unsigned)height;
        context->target = VA_INVALID_ID;
        context->slice_size = 0;
        context->pending_slice_count = 0;
        context->have_picture = false;
        context->have_first_slice = false;
        context->have_iq_matrix = false;
        context->have_vp9_slice = false;
        context->have_av1_packet = false;
        context->av1_packet_flags = 0;
        context->slice_fragment_open = false;
        *id = i + 1;
        driver->contexts_created++;
        driver->contexts_reused++;
        driver->context_create_ns += monotonic_ns() - started_ns;
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: reused context=%u decoder=%ux%u visible=%dx%d "
                    "duration_ms=%.3f elapsed_ms=%.3f\n",
                    *id, context->decoder_width, context->decoder_height,
                    width, height,
                    (double)(monotonic_ns() - started_ns) / 1000000.0,
                    driver_elapsed_ms(driver));
        return VA_STATUS_SUCCESS;
    }
    for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i) {
        if (driver->contexts[i].used)
            continue;
        struct va_context *context = &driver->contexts[i];
        memset(context, 0, sizeof(*context));
        context->client.socket_fd = -1;
        context->pool_fd = -1;
        if (pthread_mutex_init(&context->io_lock, NULL) != 0)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        context->io_lock_initialized = true;
        const char *socket_path = getenv("FMA_SOCKET");
        if (!socket_path || !*socket_path)
            socket_path = "/tmp/fake-media-accel.sock";
        struct fma_decoder_config config = {
            .codec = codec, .width = (uint32_t)width,
            .height = (uint32_t)height, .slot_count = FMA_MAX_SLOTS,
        };
        struct fma_capabilities caps;
        if (fma_client_connect(&context->client, socket_path) < 0 ||
            fma_client_query_capabilities(&context->client, &caps) < 0 ||
            !(caps.decoder_mask & FMA_CODEC_BIT(codec)) ||
            fma_client_create_decoder(&context->client, &config,
                                      &context->pool, &context->pool_fd) < 0)
            goto fail;
        context->direct_output =
            (caps.flags & FMA_CAP_DIRECT_OUTPUT) != 0;
        context->pool_bytes =
            (size_t)context->pool.slot_size * context->pool.slot_count;
        context->pool_map = mmap(NULL, context->pool_bytes, PROT_READ,
                                 MAP_SHARED, context->pool_fd, 0);
        if (context->pool_map == MAP_FAILED) {
            context->pool_map = NULL;
            goto fail;
        }
        context->used = true;
        context->config_id = config_id;
        context->width = (unsigned)width;
        context->height = (unsigned)height;
        context->decoder_width = (unsigned)width;
        context->decoder_height = (unsigned)height;
        context->codec_id = codec;
        context->profile = va_config->profile;
        context->target = VA_INVALID_ID;
        *id = i + 1;
        driver->contexts_created++;
        driver->context_create_ns += monotonic_ns() - started_ns;
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: created context=%u duration_ms=%.3f "
                    "elapsed_ms=%.3f\n",
                    *id, (double)(monotonic_ns() - started_ns) / 1000000.0,
                    driver_elapsed_ms(driver));
        return VA_STATUS_SUCCESS;
fail:
        close_context(context);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus destroy_context(VADriverContextP ctx, VAContextID id) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_context *context = get_context(driver, id);
    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    uint64_t started_ns = monotonic_ns();
    pthread_mutex_lock(&context->io_lock);
    bool collected = collect_pending_outputs(driver, context);
    bool drained = false;
    if (!collected)
        drained = fma_client_drain(&context->client) >= 0 &&
            process_until(driver, context, FMA_MSG_OUTPUT_EOS);
    pthread_mutex_unlock(&context->io_lock);
    driver->contexts_destroyed++;
    driver->context_destroy_ns += monotonic_ns() - started_ns;
    if (debug_enabled())
        fprintf(stderr,
                "fma-va: destroy context retire=%d drain=%d pending=%llu "
                "duration_ms=%.3f "
                "elapsed_ms=%.3f\n",
                collected, drained,
                (unsigned long long)context->pending_outputs,
                (double)(monotonic_ns() - started_ns) / 1000000.0,
                driver_elapsed_ms(driver));
    if (collected) {
        context->retired = true;
        context->target = VA_INVALID_ID;
        return VA_STATUS_SUCCESS;
    }
    close_context(context);
    return drained ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus create_buffer(VADriverContextP ctx, VAContextID context_id,
                              VABufferType type, unsigned size,
                              unsigned elements, void *source,
                              VABufferID *id) {
    struct va_driver *driver = ctx->pDriverData;
    if (!id || !size || !elements || !get_context(driver, context_id) ||
        size > SIZE_MAX / elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < FMA_VA_IMAGE_BUFFER_BASE; ++i) {
        if (driver->buffers[i].used)
            continue;
        size_t bytes = (size_t)size * elements;
        void *data = calloc(1, bytes);
        if (!data)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        if (source)
            memcpy(data, source, bytes);
        driver->buffers[i] = (struct va_buffer) {
            .used = true, .type = type, .size = size, .elements = elements,
            .data = data, .owns_data = true,
            .derived_surface = VA_INVALID_ID,
            .exported_fd = -1,
        };
        *id = i + 1;
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus buffer_set_elements(VADriverContextP ctx, VABufferID id,
                                    unsigned elements) {
    struct va_buffer *buffer = get_buffer(ctx->pDriverData, id);
    if (!buffer || !elements || elements > buffer->elements)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->acquired)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    buffer->elements = elements;
    return VA_STATUS_SUCCESS;
}

static VAStatus map_buffer(VADriverContextP ctx, VABufferID id, void **data) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_buffer *buffer = get_buffer(driver, id);
    if (!buffer || !data)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->acquired)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (buffer->derived_surface != VA_INVALID_ID && !buffer->mapped) {
        struct va_surface *surface =
            get_surface(driver, buffer->derived_surface);
        if (!surface || !begin_surface_cpu_access(surface, DMA_BUF_SYNC_RW))
            return VA_STATUS_ERROR_OPERATION_FAILED;
        buffer->mapped = true;
        driver->surface_map_calls++;
    }
    *data = buffer->data;
    return VA_STATUS_SUCCESS;
}

static VAStatus unmap_buffer(VADriverContextP ctx, VABufferID id) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_buffer *buffer = get_buffer(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->acquired)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (buffer->derived_surface != VA_INVALID_ID && buffer->mapped) {
        struct va_surface *surface =
            get_surface(driver, buffer->derived_surface);
        if (!surface || !end_surface_cpu_access(surface, DMA_BUF_SYNC_RW))
            return VA_STATUS_ERROR_OPERATION_FAILED;
        buffer->mapped = false;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus destroy_buffer(VADriverContextP ctx, VABufferID id) {
    struct va_buffer *buffer = get_buffer(ctx->pDriverData, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->acquired)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (buffer->mapped && buffer->derived_surface != VA_INVALID_ID) {
        struct va_surface *surface =
            get_surface(ctx->pDriverData, buffer->derived_surface);
        if (surface)
            (void)end_surface_cpu_access(surface, DMA_BUF_SYNC_RW);
    }
    if (buffer->owns_data)
        free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
    return VA_STATUS_SUCCESS;
}

static VAStatus acquire_buffer_handle(VADriverContextP ctx, VABufferID id,
                                      VABufferInfo *info) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_buffer *buffer = get_buffer(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!info)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (buffer->type != VAImageBufferType ||
        buffer->derived_surface == VA_INVALID_ID)
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    if (buffer->acquired || buffer->mapped)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (info->mem_type &&
        !(info->mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    struct va_surface *surface =
        get_surface(driver, buffer->derived_surface);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (surface->dma_buf_fd < 0)
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    VAStatus status = sync_surface(ctx, buffer->derived_surface);
    if (status != VA_STATUS_SUCCESS)
        return status;
    if (debug_enabled() &&
        begin_surface_cpu_access(surface, DMA_BUF_SYNC_READ)) {
        unsigned center_x = (surface->width / 2u) & ~1u;
        unsigned center_y = surface->height / 2u;
        unsigned center_uv_y = surface->height / 4u;
        fprintf(stderr,
                "fma-va: acquire surface=%u size=%ux%u stride=%u "
                "uv_offset=%zu y0=%u yc=%u uv0=%u,%u uvc=%u,%u\n",
                buffer->derived_surface, surface->width, surface->height,
                surface->stride, surface->uv_offset, surface->data[0],
                surface->data[(size_t)center_y * surface->stride + center_x],
                surface->data[surface->uv_offset],
                surface->data[surface->uv_offset + 1],
                surface->data[surface->uv_offset +
                              (size_t)center_uv_y * surface->stride +
                              center_x],
                surface->data[surface->uv_offset +
                              (size_t)center_uv_y * surface->stride +
                              center_x + 1]);
        (void)end_surface_cpu_access(surface, DMA_BUF_SYNC_READ);
    }
    int exported_fd = fcntl(surface->dma_buf_fd, F_DUPFD_CLOEXEC, 0);
    if (exported_fd < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    memset(info, 0, sizeof(*info));
    info->handle = (uintptr_t)exported_fd;
    info->type = buffer->type;
    info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    info->mem_size = surface->size;
    buffer->exported_fd = exported_fd;
    buffer->acquired = true;
    surface->external_handles++;
    driver->acquire_handle_calls++;
    return VA_STATUS_SUCCESS;
}

static VAStatus release_buffer_handle(VADriverContextP ctx, VABufferID id) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_buffer *buffer = get_buffer(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!buffer->acquired || buffer->exported_fd < 0)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    close(buffer->exported_fd);
    buffer->exported_fd = -1;
    buffer->acquired = false;
    struct va_surface *surface =
        get_surface(driver, buffer->derived_surface);
    if (surface && surface->external_handles)
        surface->external_handles--;
    return VA_STATUS_SUCCESS;
}

static VAStatus begin_picture(VADriverContextP ctx, VAContextID context_id,
                              VASurfaceID target_id) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_context *context = get_context(driver, context_id);
    struct va_surface *surface = get_surface(driver, target_id);
    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (surface->external_handles)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    /*
     * VA clients may release and recycle a decode surface before an
     * asynchronous backend has emitted its previous frame. Preserve that
     * submission's surface identity until its output has been collected;
     * otherwise the new picture overwrites pts_us and the delayed frame can
     * no longer be routed to its target.
     */
    if (surface->status != VASurfaceReady) {
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: waiting before surface reuse surface=%u "
                    "pending_pts=%lld\n",
                    target_id, (long long)surface->pts_us);
        VAStatus status = sync_surface(ctx, target_id);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }
    context->target = target_id;
    context->slice_size = 0;
    context->pending_slice_count = 0;
    context->have_picture = false;
    context->have_first_slice = false;
    context->have_iq_matrix = false;
    context->have_vp9_slice = false;
    context->have_av1_packet = false;
    context->av1_packet_flags = 0;
    context->slice_fragment_open = false;
    surface->pts_us = -1;
    surface->status = VASurfaceRendering;
    return VA_STATUS_SUCCESS;
}

static VAStatus render_vp9_buffer(struct va_context *context,
                                  const struct va_buffer *buffer) {
    size_t bytes = (size_t)buffer->size * buffer->elements;
    if (buffer->type == VAPictureParameterBufferType) {
        if (buffer->elements != 1 ||
            bytes < sizeof(context->vp9_picture))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        memcpy(&context->vp9_picture, buffer->data,
               sizeof(context->vp9_picture));
        context->have_picture = true;
        return VA_STATUS_SUCCESS;
    }
    if (buffer->type == VASliceParameterBufferType) {
        if (buffer->elements != 1 ||
            buffer->size < sizeof(context->vp9_slice))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        memcpy(&context->vp9_slice, buffer->data, sizeof(context->vp9_slice));
        context->have_vp9_slice = true;
        return VA_STATUS_SUCCESS;
    }
    if (buffer->type != VASliceDataBufferType)
        return VA_STATUS_SUCCESS;
    if (!context->have_vp9_slice || !bytes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t offset = context->vp9_slice.slice_data_offset;
    size_t size = context->vp9_slice.slice_data_size;
    if (offset > bytes || !size || size > bytes - offset)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    uint32_t flag = context->vp9_slice.slice_data_flag;
    bool begins = flag == VA_SLICE_DATA_FLAG_ALL ||
                  flag == VA_SLICE_DATA_FLAG_BEGIN;
    bool continues = flag == VA_SLICE_DATA_FLAG_MIDDLE ||
                     flag == VA_SLICE_DATA_FLAG_END;
    if ((!begins && !continues) ||
        (begins && (context->slice_fragment_open || context->slice_size)) ||
        (continues && !context->slice_fragment_open))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!append_data(&context->slices, &context->slice_size,
                     &context->slice_capacity,
                     (const uint8_t *)buffer->data + offset, size))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    context->have_first_slice = true;
    context->slice_fragment_open =
        flag == VA_SLICE_DATA_FLAG_BEGIN || flag == VA_SLICE_DATA_FLAG_MIDDLE;
    return VA_STATUS_SUCCESS;
}

static VAStatus render_av1_buffer(struct va_context *context,
                                  const struct va_buffer *buffer) {
    size_t bytes = (size_t)buffer->size * buffer->elements;
    if (buffer->type == VAPictureParameterBufferType) {
        if (buffer->elements != 1 || bytes < sizeof(context->av1_picture))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        memcpy(&context->av1_picture, buffer->data,
               sizeof(context->av1_picture));
        context->have_picture = true;
        return VA_STATUS_SUCCESS;
    }
    if ((unsigned)buffer->type != FMA_VA_PACKET_BUFFER_TYPE)
        return VA_STATUS_SUCCESS;
    if (buffer->elements != 1 || bytes < sizeof(struct fma_va_packet_header) ||
        context->have_av1_packet)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    const struct fma_va_packet_header *header = buffer->data;
    if (header->magic != FMA_VA_PACKET_MAGIC ||
        header->version != FMA_VA_PACKET_VERSION ||
        header->codec != FMA_CODEC_AV1 ||
        (header->flags & ~FMA_VA_PACKET_FLAG_AV1_SHOW_EXISTING) ||
        header->payload_size != bytes - sizeof(*header) ||
        !header->payload_size || header->payload_size > FMA_MAX_PAYLOAD)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!append_data(&context->slices, &context->slice_size,
                     &context->slice_capacity, header + 1,
                     header->payload_size))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    context->have_av1_packet = true;
    context->av1_packet_flags = header->flags;
    context->have_first_slice = true;
    return VA_STATUS_SUCCESS;
}

static VAStatus render_picture(VADriverContextP ctx, VAContextID context_id,
                               VABufferID *ids, int count) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_context *context = get_context(driver, context_id);
    if (!context || context->target == VA_INVALID_ID)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!ids || count < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct va_config *config = get_config(driver, context->config_id);
    if (!config)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    uint32_t codec = codec_for_profile(config->profile);
    static const uint8_t start_code[] = {0, 0, 0, 1};
    for (int i = 0; i < count; ++i) {
        struct va_buffer *buffer = get_buffer(driver, ids[i]);
        if (!buffer)
            return VA_STATUS_ERROR_INVALID_BUFFER;
        if (codec == FMA_CODEC_VP9) {
            VAStatus status = render_vp9_buffer(context, buffer);
            if (status != VA_STATUS_SUCCESS)
                return status;
            continue;
        }
        if (codec == FMA_CODEC_AV1) {
            VAStatus status = render_av1_buffer(context, buffer);
            if (status != VA_STATUS_SUCCESS)
                return status;
            continue;
        }
        size_t bytes = (size_t)buffer->size * buffer->elements;
        if (buffer->type == VAPictureParameterBufferType &&
            bytes >= sizeof(context->picture)) {
            memcpy(&context->picture, buffer->data, sizeof(context->picture));
            context->have_picture = true;
        } else if (buffer->type == VAIQMatrixBufferType &&
                   bytes >= sizeof(context->iq_matrix)) {
            memcpy(&context->iq_matrix, buffer->data,
                   sizeof(context->iq_matrix));
            context->have_iq_matrix = true;
        } else if (buffer->type == VASliceParameterBufferType &&
                   buffer->size >= sizeof(VASliceParameterBufferH264)) {
            if (context->pending_slice_count != 0)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            size_t count = buffer->elements;
            if (count > SIZE_MAX / sizeof(*context->pending_slices))
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            if (count > context->pending_slice_capacity) {
                void *resized = realloc(context->pending_slices,
                                        count * sizeof(*context->pending_slices));
                if (!resized)
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                context->pending_slices = resized;
                context->pending_slice_capacity = count;
            }
            for (size_t j = 0; j < count; ++j)
                memcpy(&context->pending_slices[j],
                       (const uint8_t *)buffer->data + j * buffer->size,
                       sizeof(*context->pending_slices));
            context->pending_slice_count = count;
        } else if (buffer->type == VASliceDataBufferType && bytes) {
            if (!context->pending_slice_count)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            for (size_t j = 0; j < context->pending_slice_count; ++j) {
                const VASliceParameterBufferH264 *parameter =
                    &context->pending_slices[j];
                size_t offset = parameter->slice_data_offset;
                size_t size = parameter->slice_data_size;
                if (offset > bytes || size > bytes - offset || size == 0)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                bool begins =
                    parameter->slice_data_flag == VA_SLICE_DATA_FLAG_ALL ||
                    parameter->slice_data_flag == VA_SLICE_DATA_FLAG_BEGIN;
                bool continues =
                    parameter->slice_data_flag == VA_SLICE_DATA_FLAG_MIDDLE ||
                    parameter->slice_data_flag == VA_SLICE_DATA_FLAG_END;
                if ((!begins && !continues) ||
                    (begins && context->slice_fragment_open) ||
                    (continues && !context->slice_fragment_open))
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                if (begins && context->slice_size &&
                    !append_data(&context->slices, &context->slice_size,
                                 &context->slice_capacity, start_code,
                                 sizeof(start_code)))
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                if (!append_data(&context->slices, &context->slice_size,
                                 &context->slice_capacity,
                                 (const uint8_t *)buffer->data + offset, size))
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                if (!context->have_first_slice) {
                    context->first_slice = *parameter;
                    context->have_first_slice = true;
                }
                context->slice_fragment_open =
                    parameter->slice_data_flag == VA_SLICE_DATA_FLAG_BEGIN ||
                    parameter->slice_data_flag == VA_SLICE_DATA_FLAG_MIDDLE;
            }
            context->pending_slice_count = 0;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus end_picture(VADriverContextP ctx, VAContextID context_id) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_context *context = get_context(driver, context_id);
    if (!context || context->target == VA_INVALID_ID)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    struct va_surface *surface = get_surface(driver, context->target);
    struct va_config *config = get_config(driver, context->config_id);
    if (!surface || !config || !context->have_first_slice ||
        context->pending_slice_count || context->slice_fragment_open)
        return VA_STATUS_ERROR_DECODING_ERROR;
    uint32_t codec = codec_for_profile(config->profile);
    bool av1_show_existing = codec == FMA_CODEC_AV1 &&
        (context->av1_packet_flags &
         FMA_VA_PACKET_FLAG_AV1_SHOW_EXISTING);
    if (!context->have_picture && !av1_show_existing)
        return VA_STATUS_ERROR_DECODING_ERROR;
    uint8_t *owned_packet = NULL;
    const uint8_t *packet = NULL;
    size_t packet_size = 0;
    int64_t pts_us = 0;
    uint32_t packet_flags = 0;
    bool expects_output = true;
    if (codec == FMA_CODEC_H264) {
        enum fma_h264_build_status build_status = fma_h264_build_packet(
            config->profile, &context->picture,
            context->have_iq_matrix ? &context->iq_matrix : NULL,
            &context->first_slice, context->slices, context->slice_size,
            &owned_packet, &packet_size);
        if (build_status != FMA_H264_BUILD_OK) {
            if (debug_enabled())
                fprintf(stderr,
                        "fma-va: H264 packet build failed status=%d\n",
                        build_status);
            free(owned_packet);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        packet = owned_packet;
        unsigned nal_type =
            context->slice_size ? context->slices[0] & 0x1fu : 0;
        pts_us = fma_h264_picture_pts(
            &context->timeline, context->picture.CurrPic.TopFieldOrderCnt,
            nal_type == 5);
        if (nal_type == 5)
            packet_flags |= FMA_PACKET_KEY_FRAME;
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: H264 picture surface=%u frame_num=%u "
                    "top_poc=%d bottom_poc=%d slice_type=%u nal_type=%u "
                    "pts=%lld\n",
                    context->target, context->picture.frame_num,
                    context->picture.CurrPic.TopFieldOrderCnt,
                    context->picture.CurrPic.BottomFieldOrderCnt,
                    context->first_slice.slice_type, nal_type,
                    (long long)pts_us);
    } else if (codec == FMA_CODEC_VP9) {
        if (!context->have_vp9_slice || context->vp9_picture.profile != 0 ||
            context->vp9_picture.bit_depth != 8 ||
            !context->vp9_picture.pic_fields.bits.subsampling_x ||
            !context->vp9_picture.pic_fields.bits.subsampling_y ||
            !context->vp9_picture.frame_width ||
            !context->vp9_picture.frame_height ||
            context->vp9_picture.frame_width > context->width ||
            context->vp9_picture.frame_height > context->height)
            return VA_STATUS_ERROR_DECODING_ERROR;
        packet = context->slices;
        packet_size = context->slice_size;
        pts_us = (int64_t)context->next_pts_us++;
        if (!context->vp9_picture.pic_fields.bits.frame_type)
            packet_flags |= FMA_PACKET_KEY_FRAME;
        expects_output = context->vp9_picture.pic_fields.bits.show_frame;
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: VP9 picture surface=%u visible=%ux%u "
                    "profile=%u bit_depth=%u show=%u pts=%lld\n",
                    context->target, context->vp9_picture.frame_width,
                    context->vp9_picture.frame_height,
                    context->vp9_picture.profile,
                    context->vp9_picture.bit_depth, expects_output,
                    (long long)pts_us);
    } else if (codec == FMA_CODEC_AV1) {
        if (!context->have_av1_packet ||
            (!av1_show_existing && (context->av1_picture.profile != 0 ||
            context->av1_picture.bit_depth_idx != 0 ||
            !context->av1_picture.seq_info_fields.fields.subsampling_x ||
            !context->av1_picture.seq_info_fields.fields.subsampling_y ||
            context->av1_picture.seq_info_fields.fields.mono_chrome ||
            !context->av1_picture.frame_width_minus1 ||
            !context->av1_picture.frame_height_minus1 ||
            (unsigned)context->av1_picture.frame_width_minus1 + 1u >
                context->width ||
            (unsigned)context->av1_picture.frame_height_minus1 + 1u >
                context->height)))
            return VA_STATUS_ERROR_DECODING_ERROR;
        packet = context->slices;
        packet_size = context->slice_size;
        pts_us = (int64_t)context->next_pts_us++;
        if (!av1_show_existing &&
            context->av1_picture.pic_info_fields.bits.frame_type == 0)
            packet_flags |= FMA_PACKET_KEY_FRAME;
        expects_output = av1_show_existing ||
            context->av1_picture.pic_info_fields.bits.show_frame;
        if (debug_enabled())
            fprintf(stderr,
                    "fma-va: AV1 picture surface=%u visible=%ux%u "
                    "profile=%u bit_depth=%u show=%u show_existing=%u "
                    "pts=%lld\n",
                    context->target,
                    av1_show_existing ? context->width :
                        (unsigned)context->av1_picture.frame_width_minus1 + 1u,
                    av1_show_existing ? context->height :
                        (unsigned)context->av1_picture.frame_height_minus1 + 1u,
                    context->av1_picture.profile,
                    context->av1_picture.bit_depth_idx, expects_output,
                    av1_show_existing,
                    (long long)pts_us);
    } else {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (!packet || !packet_size || packet_size > FMA_MAX_PAYLOAD) {
        free(owned_packet);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    surface->pts_us = pts_us;
    dump_packet(packet, packet_size);
    uint64_t started_ns = monotonic_ns();
    pthread_mutex_lock(&context->io_lock);
    bool reset = context->needs_stream_reset &&
        (packet_flags & FMA_PACKET_KEY_FRAME);
    bool reset_ok = !reset || fma_client_flush(&context->client) >= 0;
    context->needs_stream_reset = false;
    bool direct_output = expects_output && context->direct_output &&
        surface->dma_buf_fd >= 0 &&
        surface->stride == context->pool.stride &&
        surface->allocation_height == context->pool.height &&
        surface->size >= context->pool.slot_size;
    if (expects_output)
        ++context->pending_outputs;
    int sent = reset_ok ? fma_client_queue_packet_to(
        &context->client, packet, packet_size, pts_us, packet_flags,
        direct_output ? surface->dma_buf_fd : -1) : -1;
    if (sent < 0 && expects_output && context->pending_outputs)
        --context->pending_outputs;
    free(owned_packet);
    bool processed = sent >= 0 &&
        process_until(driver, context, FMA_MSG_PACKET_ACK);
    pthread_mutex_unlock(&context->io_lock);
    driver->submitted_frames++;
    driver->submission_ns += monotonic_ns() - started_ns;
    if (!processed) {
        if (debug_enabled())
            fprintf(stderr, "fma-va: submission failed surface=%u pts=%lld "
                            "sent=%d errno=%d\n",
                    context->target, (long long)pts_us, sent, errno);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!expects_output)
        surface->status = VASurfaceReady;
    if (debug_enabled())
        fprintf(stderr,
                "fma-va: submitted surface=%u pts=%lld duration_ms=%.3f "
                "elapsed_ms=%.3f\n",
                context->target, (long long)pts_us,
                (double)(monotonic_ns() - started_ns) / 1000000.0,
                driver_elapsed_ms(driver));
    context->target = VA_INVALID_ID;
    return VA_STATUS_SUCCESS;
}

static VAStatus sync_surface_internal(VADriverContextP ctx, VASurfaceID id) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_surface *surface = get_surface(driver, id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (surface->status == VASurfaceReady)
        return VA_STATUS_SUCCESS;
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i) {
            struct va_context *context = &driver->contexts[i];
            if (!context->used)
                continue;
            pthread_mutex_lock(&context->io_lock);
            bool processed = surface->status == VASurfaceReady ||
                (fma_client_poll_output(&context->client, 50) >= 0 &&
                 process_until(driver, context, FMA_MSG_POLL_DONE));
            pthread_mutex_unlock(&context->io_lock);
            if (!processed)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            if (surface->status == VASurfaceReady)
                return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_TIMEDOUT;
}

static VAStatus sync_surface(VADriverContextP ctx, VASurfaceID id) {
    uint64_t started_ns = monotonic_ns();
    VAStatus status = sync_surface_internal(ctx, id);
    struct va_driver *driver = ctx->pDriverData;
    driver->sync_calls++;
    driver->sync_ns += monotonic_ns() - started_ns;
    if (debug_enabled())
        fprintf(stderr,
                "fma-va: sync surface=%u status=%d duration_ms=%.3f "
                "elapsed_ms=%.3f\n",
                id, status,
                (double)(monotonic_ns() - started_ns) / 1000000.0,
                driver_elapsed_ms(ctx->pDriverData));
    return status;
}

static VAStatus query_surface_status(VADriverContextP ctx, VASurfaceID id,
                                     VASurfaceStatus *status) {
    struct va_surface *surface = get_surface(ctx->pDriverData, id);
    if (!surface || !status)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    *status = surface->status;
    return VA_STATUS_SUCCESS;
}

static VAStatus query_image_formats(VADriverContextP ctx,
                                    VAImageFormat *formats, int *count) {
    (void)ctx;
    if (!formats || !count)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    formats[0] = nv12_image_format();
    formats[1] = i420_image_format();
    *count = 2;
    return VA_STATUS_SUCCESS;
}

static VAStatus allocate_image(struct va_driver *driver, VAImageFormat *format,
                               unsigned width, unsigned height, unsigned pitch,
                               unsigned storage_height, void *data, bool owns,
                               VASurfaceID derived_surface, VAImage *image) {
    unsigned image_index = FMA_VA_MAX_IMAGES;
    unsigned buffer_index = FMA_VA_MAX_BUFFERS;
    for (unsigned i = 0; i < FMA_VA_MAX_IMAGES; ++i)
        if (!driver->images[i].used) {
            image_index = i;
            break;
        }
    for (unsigned i = FMA_VA_IMAGE_BUFFER_BASE; i < FMA_VA_MAX_BUFFERS; ++i)
        if (!driver->buffers[i].used) {
            buffer_index = i;
            break;
        }
    if (image_index == FMA_VA_MAX_IMAGES || buffer_index == FMA_VA_MAX_BUFFERS)
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    size_t y_size;
    size_t chroma_size = 0;
    size_t bytes;
    unsigned chroma_pitch = pitch;
    if (format->fourcc == VA_FOURCC_NV12) {
        if (!nv12_layout(width, storage_height, pitch, &y_size, &bytes))
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
    } else if (format->fourcc == VA_FOURCC_I420) {
        if (!width || !storage_height || pitch < width || (pitch & 1u) ||
            (size_t)pitch > SIZE_MAX / storage_height)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        y_size = (size_t)pitch * storage_height;
        chroma_pitch = pitch / 2u;
        unsigned chroma_rows = (storage_height + 1u) / 2u;
        if ((size_t)chroma_pitch > SIZE_MAX / chroma_rows)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        chroma_size = (size_t)chroma_pitch * chroma_rows;
        if (chroma_size > (SIZE_MAX - y_size) / 2u ||
            y_size + chroma_size * 2u > UINT_MAX)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        bytes = y_size + chroma_size * 2u;
    } else {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    driver->buffers[buffer_index] = (struct va_buffer) {
        .used = true, .type = VAImageBufferType, .size = (unsigned)bytes,
        .elements = 1, .data = data, .owns_data = owns,
        .derived_surface = derived_surface,
        .exported_fd = -1,
    };
    struct va_image *record = &driver->images[image_index];
    memset(record, 0, sizeof(*record));
    record->used = true;
    record->derived_surface = derived_surface;
    record->image.image_id = image_index + 1;
    record->image.format = *format;
    record->image.buf = buffer_index + 1;
    record->image.width = width;
    record->image.height = height;
    record->image.data_size = (unsigned)bytes;
    record->image.num_planes = format->fourcc == VA_FOURCC_I420 ? 3 : 2;
    record->image.pitches[0] = pitch;
    record->image.pitches[1] = chroma_pitch;
    record->image.pitches[2] = chroma_pitch;
    record->image.offsets[0] = 0;
    record->image.offsets[1] = (unsigned)y_size;
    record->image.offsets[2] = (unsigned)(y_size + chroma_size);
    *image = record->image;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_image(VADriverContextP ctx, VAImageFormat *format,
                             int width, int height, VAImage *image) {
    if (!format ||
        (format->fourcc != VA_FOURCC_NV12 &&
         format->fourcc != VA_FOURCC_I420) ||
        width <= 0 || height <= 0 || !image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned pitch = ((unsigned)width + 1u) & ~1u;
    size_t bytes;
    if (!nv12_layout((unsigned)width, (unsigned)height, pitch, NULL,
                     &bytes))
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    void *data = calloc(1, bytes);
    if (!data)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    VAStatus status = allocate_image(ctx->pDriverData, format, (unsigned)width,
                                     (unsigned)height, pitch,
                                     (unsigned)height, data, true,
                                     VA_INVALID_ID, image);
    if (status != VA_STATUS_SUCCESS)
        free(data);
    return status;
}

static VAStatus derive_image(VADriverContextP ctx, VASurfaceID id,
                             VAImage *image) {
    uint64_t started_ns = monotonic_ns();
    struct va_driver *driver = ctx->pDriverData;
    struct va_surface *surface = get_surface(driver, id);
    if (!surface || !image)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    VAStatus status = sync_surface(ctx, id);
    if (status != VA_STATUS_SUCCESS)
        return status;
    VAImageFormat format = nv12_image_format();
    status = allocate_image(driver, &format, surface->width, surface->height,
                            surface->stride, surface->allocation_height,
                            surface->data, false, id, image);
    if (status == VA_STATUS_SUCCESS)
        surface->derived_images++;
    driver->derive_calls++;
    driver->derive_ns += monotonic_ns() - started_ns;
    return status;
}

static VAStatus destroy_image(VADriverContextP ctx, VAImageID id) {
    struct va_driver *driver = ctx->pDriverData;
    if (!id || id > FMA_VA_MAX_IMAGES || !driver->images[id - 1].used)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    struct va_image *image = &driver->images[id - 1];
    struct va_buffer *buffer = get_buffer(driver, image->image.buf);
    if (buffer) {
        if (buffer->acquired)
            return VA_STATUS_ERROR_SURFACE_BUSY;
        if (buffer->mapped && image->derived_surface != VA_INVALID_ID) {
            struct va_surface *surface =
                get_surface(driver, image->derived_surface);
            if (surface)
                (void)end_surface_cpu_access(surface, DMA_BUF_SYNC_RW);
        }
        if (buffer->owns_data)
            free(buffer->data);
        memset(buffer, 0, sizeof(*buffer));
    }
    if (image->derived_surface != VA_INVALID_ID) {
        struct va_surface *surface =
            get_surface(driver, image->derived_surface);
        if (surface && surface->derived_images)
            surface->derived_images--;
    }
    memset(image, 0, sizeof(*image));
    return VA_STATUS_SUCCESS;
}

static VAStatus get_image(VADriverContextP ctx, VASurfaceID surface_id,
                          int x, int y, unsigned width, unsigned height,
                          VAImageID image_id) {
    uint64_t started_ns = monotonic_ns();
    struct va_driver *driver = ctx->pDriverData;
    struct va_surface *surface = get_surface(driver, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (!image_id || image_id > FMA_VA_MAX_IMAGES ||
        !driver->images[image_id - 1].used)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    struct va_image *image = &driver->images[image_id - 1];
    struct va_buffer *buffer = get_buffer(driver, image->image.buf);
    bool planar = image->image.format.fourcc == VA_FOURCC_I420;
    if (!buffer || !buffer->data ||
        (!planar && image->image.format.fourcc != VA_FOURCC_NV12) ||
        x < 0 || y < 0 || (x & 1) || (y & 1) ||
        (unsigned)x + width > surface->width ||
        (unsigned)y + height > surface->height ||
        width > image->image.width || height > image->image.height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (buffer->acquired)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    VAStatus status = sync_surface(ctx, surface_id);
    if (status != VA_STATUS_SUCCESS)
        return status;
    struct va_surface *destination_surface =
        image->derived_surface != VA_INVALID_ID ?
            get_surface(driver, image->derived_surface) : NULL;
    bool same_surface = destination_surface == surface;
    if (!begin_surface_cpu_access(
            surface, same_surface ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ))
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (destination_surface && !same_surface &&
        !begin_surface_cpu_access(destination_surface, DMA_BUF_SYNC_WRITE)) {
        (void)end_surface_cpu_access(surface, DMA_BUF_SYNC_READ);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    uint8_t *destination = buffer->data;
    bool identical_layout = !planar &&
        x == 0 && y == 0 && width == surface->width &&
        height == surface->height && image->image.width == width &&
        image->image.height == height && image->image.offsets[0] == 0 &&
        image->image.offsets[1] == surface->uv_offset &&
        image->image.pitches[0] == surface->stride &&
        image->image.pitches[1] == surface->stride &&
        image->image.data_size >= surface->size;
    if (identical_layout) {
        memmove(destination, surface->data, surface->size);
    } else {
        for (unsigned row = 0; row < height; ++row)
            memmove(destination + image->image.offsets[0] +
                        row * image->image.pitches[0],
                    surface->data + ((unsigned)y + row) * surface->stride +
                        (unsigned)x,
                    width);
        const uint8_t *source_uv = surface->data + surface->uv_offset;
        unsigned chroma_rows = (height + 1u) / 2u;
        unsigned chroma_columns = (width + 1u) / 2u;
        unsigned chroma_bytes = chroma_columns * 2u;
        for (unsigned row = 0; row < chroma_rows; ++row) {
            const uint8_t *source_row = source_uv +
                ((unsigned)y / 2 + row) * surface->stride + (unsigned)x;
            if (!planar) {
                memmove(destination + image->image.offsets[1] +
                            row * image->image.pitches[1],
                        source_row, chroma_bytes);
                continue;
            }
            uint8_t *destination_u = destination + image->image.offsets[1] +
                row * image->image.pitches[1];
            uint8_t *destination_v = destination + image->image.offsets[2] +
                row * image->image.pitches[2];
            for (unsigned column = 0; column < chroma_columns; ++column) {
                destination_u[column] = source_row[column * 2u];
                destination_v[column] = source_row[column * 2u + 1u];
            }
        }
    }
    bool synced = true;
    if (destination_surface && !same_surface)
        synced = end_surface_cpu_access(destination_surface,
                                        DMA_BUF_SYNC_WRITE);
    if (!end_surface_cpu_access(
            surface, same_surface ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ))
        synced = false;
    driver->get_image_calls++;
    driver->get_image_bytes += (uint64_t)width * height +
        (uint64_t)((width + 1u) / 2u) * ((height + 1u) / 2u) * 2u;
    driver->get_image_ns += monotonic_ns() - started_ns;
    return synced ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus put_image(VADriverContextP ctx, VASurfaceID surface_id,
                          VAImageID image_id, int src_x, int src_y,
                          unsigned src_width, unsigned src_height, int dst_x,
                          int dst_y, unsigned dst_width, unsigned dst_height) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_surface *surface = get_surface(driver, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (!image_id || image_id > FMA_VA_MAX_IMAGES ||
        !driver->images[image_id - 1].used)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    struct va_image *image = &driver->images[image_id - 1];
    struct va_buffer *buffer = get_buffer(driver, image->image.buf);
    bool planar = image->image.format.fourcc == VA_FOURCC_I420;
    if (!buffer || !buffer->data ||
        (!planar && image->image.format.fourcc != VA_FOURCC_NV12))
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if (src_width != dst_width || src_height != dst_height)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (src_x < 0 || src_y < 0 || dst_x < 0 || dst_y < 0 ||
        (src_x & 1) || (src_y & 1) || (dst_x & 1) || (dst_y & 1) ||
        (unsigned)src_x + src_width > image->image.width ||
        (unsigned)src_y + src_height > image->image.height ||
        (unsigned)dst_x + dst_width > surface->width ||
        (unsigned)dst_y + dst_height > surface->height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (buffer->mapped || buffer->acquired)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    struct va_surface *source_surface =
        image->derived_surface != VA_INVALID_ID ?
            get_surface(driver, image->derived_surface) : NULL;
    bool same_surface = source_surface == surface;
    if (!begin_surface_cpu_access(
            surface, same_surface ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_WRITE))
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (source_surface && !same_surface &&
        !begin_surface_cpu_access(source_surface, DMA_BUF_SYNC_READ)) {
        (void)end_surface_cpu_access(surface, DMA_BUF_SYNC_WRITE);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const uint8_t *source = buffer->data;
    for (unsigned row = 0; row < src_height; ++row)
        memmove(surface->data + ((unsigned)dst_y + row) * surface->stride +
                    (unsigned)dst_x,
                source + image->image.offsets[0] +
                    ((unsigned)src_y + row) * image->image.pitches[0] +
                    (unsigned)src_x,
                src_width);
    uint8_t *destination_uv =
        surface->data + surface->uv_offset;
    unsigned chroma_rows = (src_height + 1u) / 2u;
    unsigned chroma_columns = (src_width + 1u) / 2u;
    unsigned chroma_bytes = chroma_columns * 2u;
    for (unsigned row = 0; row < chroma_rows; ++row) {
        uint8_t *destination_row = destination_uv +
            ((unsigned)dst_y / 2 + row) * surface->stride +
            (unsigned)dst_x;
        const uint8_t *source_u = source + image->image.offsets[1] +
            ((unsigned)src_y / 2 + row) * image->image.pitches[1] +
            (planar ? (unsigned)src_x / 2u : (unsigned)src_x);
        if (!planar) {
            memmove(destination_row, source_u, chroma_bytes);
            continue;
        }
        const uint8_t *source_v = source + image->image.offsets[2] +
            ((unsigned)src_y / 2 + row) * image->image.pitches[2] +
            (unsigned)src_x / 2u;
        for (unsigned column = 0; column < chroma_columns; ++column) {
            destination_row[column * 2u] = source_u[column];
            destination_row[column * 2u + 1u] = source_v[column];
        }
    }
    bool synced = true;
    if (source_surface && !same_surface)
        synced = end_surface_cpu_access(source_surface, DMA_BUF_SYNC_READ);
    if (!end_surface_cpu_access(
            surface, same_surface ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_WRITE))
        synced = false;
    if (!synced)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    surface->pts_us = -1;
    surface->status = VASurfaceReady;
    return VA_STATUS_SUCCESS;
}

static VAStatus export_surface_handle(VADriverContextP ctx,
                                      VASurfaceID surface_id,
                                      uint32_t memory_type, uint32_t flags,
                                      void *descriptor) {
    struct va_surface *surface = get_surface(ctx->pDriverData, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (!descriptor)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 ||
        surface->dma_buf_fd < 0)
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    uint32_t layer_flags =
        flags & (VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                 VA_EXPORT_SURFACE_COMPOSED_LAYERS);
    if (layer_flags == (VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                        VA_EXPORT_SURFACE_COMPOSED_LAYERS))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    int exported_fd = fcntl(surface->dma_buf_fd, F_DUPFD_CLOEXEC, 0);
    if (exported_fd < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    VADRMPRIMESurfaceDescriptor *output = descriptor;
    memset(output, 0, sizeof(*output));
    output->fourcc = VA_FOURCC_NV12;
    output->width = surface->width;
    output->height = surface->height;
    output->num_objects = 1;
    output->objects[0].fd = exported_fd;
    output->objects[0].size = (uint32_t)surface->size;
    output->objects[0].drm_format_modifier = FMA_DRM_FORMAT_MOD_LINEAR;
    uint32_t uv_offset = (uint32_t)surface->uv_offset;
    if (layer_flags == VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        output->num_layers = 2;
        output->layers[0].drm_format = FMA_DRM_FORMAT_R8;
        output->layers[0].num_planes = 1;
        output->layers[0].object_index[0] = 0;
        output->layers[0].offset[0] = 0;
        output->layers[0].pitch[0] = surface->stride;
        output->layers[1].drm_format = FMA_DRM_FORMAT_GR88;
        output->layers[1].num_planes = 1;
        output->layers[1].object_index[0] = 0;
        output->layers[1].offset[0] = uv_offset;
        output->layers[1].pitch[0] = surface->stride;
    } else {
        output->num_layers = 1;
        output->layers[0].drm_format = FMA_DRM_FORMAT_NV12;
        output->layers[0].num_planes = 2;
        output->layers[0].object_index[0] = 0;
        output->layers[0].object_index[1] = 0;
        output->layers[0].offset[0] = 0;
        output->layers[0].offset[1] = uv_offset;
        output->layers[0].pitch[0] = surface->stride;
        output->layers[0].pitch[1] = surface->stride;
    }
    return VA_STATUS_SUCCESS;
}

static void set_surface_attribute(VASurfaceAttrib *attribute,
                                  VASurfaceAttribType type, uint32_t flags,
                                  int value) {
    memset(attribute, 0, sizeof(*attribute));
    attribute->type = type;
    attribute->flags = flags;
    attribute->value.type = VAGenericValueTypeInteger;
    attribute->value.value.i = value;
}

static VAStatus query_surface_attributes(VADriverContextP ctx, VAConfigID id,
                                         VASurfaceAttrib *attributes,
                                         unsigned *count) {
    const unsigned required = 7;
    if (!count || !get_config(ctx->pDriverData, id))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!attributes) {
        *count = required;
        return VA_STATUS_SUCCESS;
    }
    if (*count < required) {
        *count = required;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    set_surface_attribute(&attributes[0], VASurfaceAttribPixelFormat,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE,
                          VA_FOURCC_NV12);
    set_surface_attribute(&attributes[1], VASurfaceAttribMinWidth,
                          VA_SURFACE_ATTRIB_GETTABLE, 16);
    set_surface_attribute(&attributes[2], VASurfaceAttribMaxWidth,
                          VA_SURFACE_ATTRIB_GETTABLE, FMA_MAX_DIMENSION);
    set_surface_attribute(&attributes[3], VASurfaceAttribMinHeight,
                          VA_SURFACE_ATTRIB_GETTABLE, 16);
    set_surface_attribute(&attributes[4], VASurfaceAttribMaxHeight,
                          VA_SURFACE_ATTRIB_GETTABLE, FMA_MAX_DIMENSION);
    set_surface_attribute(&attributes[5], VASurfaceAttribMemoryType,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE,
                          VA_SURFACE_ATTRIB_MEM_TYPE_VA);
    set_surface_attribute(&attributes[6], VASurfaceAttribUsageHint,
                          VA_SURFACE_ATTRIB_GETTABLE |
                              VA_SURFACE_ATTRIB_SETTABLE,
                          VA_SURFACE_ATTRIB_USAGE_HINT_DECODER |
                              VA_SURFACE_ATTRIB_USAGE_HINT_DISPLAY |
                              (dma_heap_available() ?
                                   VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT : 0));
    *count = required;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_surfaces2(VADriverContextP ctx, unsigned format,
                                 unsigned width, unsigned height,
                                 VASurfaceID *ids, unsigned count,
                                 VASurfaceAttrib *attributes,
                                 unsigned attribute_count) {
    for (unsigned i = 0; i < attribute_count; ++i) {
        if (attributes[i].type == VASurfaceAttribPixelFormat &&
            attributes[i].value.value.i != (int)VA_FOURCC_NV12)
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        if (attributes[i].type == VASurfaceAttribMemoryType &&
            !(attributes[i].value.value.i & VA_SURFACE_ATTRIB_MEM_TYPE_VA))
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    return create_surfaces(ctx, (int)width, (int)height, (int)format,
                           (int)count, ids);
}

#define UNIMPLEMENTED(name, arguments) \
    static VAStatus name arguments { return VA_STATUS_ERROR_UNIMPLEMENTED; }

UNIMPLEMENTED(set_image_palette,
              (VADriverContextP ctx, VAImageID image, unsigned char *palette))
UNIMPLEMENTED(query_subpicture_formats,
              (VADriverContextP ctx, VAImageFormat *formats, unsigned *flags,
               unsigned *count))
UNIMPLEMENTED(create_subpicture,
              (VADriverContextP ctx, VAImageID image, VASubpictureID *id))
UNIMPLEMENTED(destroy_subpicture,
              (VADriverContextP ctx, VASubpictureID id))
UNIMPLEMENTED(set_subpicture_image,
              (VADriverContextP ctx, VASubpictureID id, VAImageID image))
UNIMPLEMENTED(set_subpicture_chromakey,
              (VADriverContextP ctx, VASubpictureID id, unsigned minimum,
               unsigned maximum, unsigned mask))
UNIMPLEMENTED(set_subpicture_alpha,
              (VADriverContextP ctx, VASubpictureID id, float alpha))
UNIMPLEMENTED(associate_subpicture,
              (VADriverContextP ctx, VASubpictureID id, VASurfaceID *surfaces,
               int count, short src_x, short src_y, unsigned short src_width,
               unsigned short src_height, short dst_x, short dst_y,
               unsigned short dst_width, unsigned short dst_height,
               unsigned flags))
UNIMPLEMENTED(deassociate_subpicture,
              (VADriverContextP ctx, VASubpictureID id, VASurfaceID *surfaces,
               int count))
UNIMPLEMENTED(query_display_attributes,
              (VADriverContextP ctx, VADisplayAttribute *attributes,
               int *count))
UNIMPLEMENTED(get_display_attributes,
              (VADriverContextP ctx, VADisplayAttribute *attributes,
               int count))
UNIMPLEMENTED(set_display_attributes,
              (VADriverContextP ctx, VADisplayAttribute *attributes,
               int count))

__attribute__((visibility("default"))) VAStatus
__vaDriverInit_1_14(VADriverContextP ctx) {
    if (!ctx || !ctx->vtable)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    struct va_driver *driver = calloc(1, sizeof(*driver));
    if (!driver)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    driver->initialized_ns = monotonic_ns();
    for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i) {
        driver->contexts[i].client.socket_fd = -1;
        driver->contexts[i].pool_fd = -1;
    }
    ctx->pDriverData = driver;
    ctx->version_major = 0;
    ctx->version_minor = 1;
    ctx->max_profiles = 5;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    ctx->max_image_formats = 2;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;
    ctx->str_vendor = "fake-media-accel MediaCodec VA-API bridge";
    ctx->vtable->vaTerminate = terminate;
    ctx->vtable->vaQueryConfigProfiles = query_profiles;
    ctx->vtable->vaQueryConfigEntrypoints = query_entrypoints;
    ctx->vtable->vaGetConfigAttributes = get_config_attributes;
    ctx->vtable->vaCreateConfig = create_config;
    ctx->vtable->vaDestroyConfig = destroy_config;
    ctx->vtable->vaQueryConfigAttributes = query_config_attributes;
    ctx->vtable->vaCreateSurfaces = create_surfaces;
    ctx->vtable->vaDestroySurfaces = destroy_surfaces;
    ctx->vtable->vaCreateContext = create_context;
    ctx->vtable->vaDestroyContext = destroy_context;
    ctx->vtable->vaCreateBuffer = create_buffer;
    ctx->vtable->vaBufferSetNumElements = buffer_set_elements;
    ctx->vtable->vaMapBuffer = map_buffer;
    ctx->vtable->vaUnmapBuffer = unmap_buffer;
    ctx->vtable->vaDestroyBuffer = destroy_buffer;
    ctx->vtable->vaAcquireBufferHandle = acquire_buffer_handle;
    ctx->vtable->vaReleaseBufferHandle = release_buffer_handle;
    ctx->vtable->vaBeginPicture = begin_picture;
    ctx->vtable->vaRenderPicture = render_picture;
    ctx->vtable->vaEndPicture = end_picture;
    ctx->vtable->vaSyncSurface = sync_surface;
    ctx->vtable->vaQuerySurfaceStatus = query_surface_status;
    ctx->vtable->vaQueryImageFormats = query_image_formats;
    ctx->vtable->vaCreateImage = create_image;
    ctx->vtable->vaDeriveImage = derive_image;
    ctx->vtable->vaDestroyImage = destroy_image;
    ctx->vtable->vaSetImagePalette = set_image_palette;
    ctx->vtable->vaGetImage = get_image;
    ctx->vtable->vaPutImage = put_image;
    ctx->vtable->vaQuerySubpictureFormats = query_subpicture_formats;
    ctx->vtable->vaCreateSubpicture = create_subpicture;
    ctx->vtable->vaDestroySubpicture = destroy_subpicture;
    ctx->vtable->vaSetSubpictureImage = set_subpicture_image;
    ctx->vtable->vaSetSubpictureChromakey = set_subpicture_chromakey;
    ctx->vtable->vaSetSubpictureGlobalAlpha = set_subpicture_alpha;
    ctx->vtable->vaAssociateSubpicture = associate_subpicture;
    ctx->vtable->vaDeassociateSubpicture = deassociate_subpicture;
    ctx->vtable->vaQueryDisplayAttributes = query_display_attributes;
    ctx->vtable->vaGetDisplayAttributes = get_display_attributes;
    ctx->vtable->vaSetDisplayAttributes = set_display_attributes;
    ctx->vtable->vaCreateSurfaces2 = create_surfaces2;
    ctx->vtable->vaQuerySurfaceAttributes = query_surface_attributes;
    ctx->vtable->vaExportSurfaceHandle = export_surface_handle;
    return VA_STATUS_SUCCESS;
}
