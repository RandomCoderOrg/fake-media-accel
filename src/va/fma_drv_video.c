#define _GNU_SOURCE

#include "fma/client.h"
#include "h264_annexb.h"
#include "h264_timing.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <va/va_backend.h>

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
    unsigned stride;
    size_t size;
    uint8_t *data;
    int64_t pts_us;
    VASurfaceStatus status;
};

struct va_context {
    bool used;
    VAConfigID config_id;
    unsigned width;
    unsigned height;
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
    bool have_picture;
    bool have_first_slice;
};

struct va_buffer {
    bool used;
    VABufferType type;
    unsigned size;
    unsigned elements;
    void *data;
    bool owns_data;
};

struct va_image {
    bool used;
    VAImage image;
};

struct va_driver {
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
    return id && id <= FMA_VA_MAX_CONTEXTS && driver->contexts[id - 1].used ?
        &driver->contexts[id - 1] : NULL;
}

static struct va_buffer *get_buffer(struct va_driver *driver, VABufferID id) {
    return id && id <= FMA_VA_MAX_BUFFERS && driver->buffers[id - 1].used ?
        &driver->buffers[id - 1] : NULL;
}

static bool profile_supported(VAProfile profile) {
    return profile == VAProfileH264ConstrainedBaseline ||
           profile == VAProfileH264Main || profile == VAProfileH264High;
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
    memset(context, 0, sizeof(*context));
    context->client.socket_fd = -1;
    context->pool_fd = -1;
}

static bool store_frame(struct va_driver *driver, struct va_context *context,
                        const struct fma_message *message) {
    struct fma_frame frame;
    if (fma_decode_frame(message->payload, message->payload_size, &frame) < 0 ||
        frame.slot >= context->pool.slot_count ||
        frame.bytes_used > context->pool.slot_size)
        return false;
    struct va_surface *surface = NULL;
    for (unsigned i = 0; i < FMA_VA_MAX_SURFACES; ++i) {
        if (driver->surfaces[i].used &&
            driver->surfaces[i].pts_us == message->pts_us) {
            surface = &driver->surfaces[i];
            break;
        }
    }
    bool valid = true;
    if (surface) {
        if (frame.width > surface->width || frame.height > surface->height ||
            frame.stride < frame.width || frame.pixel_format != FMA_PIXFMT_NV12)
            valid = false;
        else {
            const uint8_t *source = context->pool_map +
                (size_t)frame.slot * context->pool.slot_size;
            size_t source_y = (size_t)frame.stride * context->pool.height;
            size_t destination_y = (size_t)surface->stride * surface->height;
            memset(surface->data, 16, destination_y);
            memset(surface->data + destination_y, 128, destination_y / 2);
            for (uint32_t row = 0; row < frame.height; ++row)
                memcpy(surface->data + (size_t)row * surface->stride,
                       source + (size_t)row * frame.stride, frame.width);
            for (uint32_t row = 0; row < frame.height / 2; ++row)
                memcpy(surface->data + destination_y +
                           (size_t)row * surface->stride,
                       source + source_y + (size_t)row * frame.stride,
                       frame.width);
            surface->status = VASurfaceReady;
        }
    }
    if (debug_enabled())
        fprintf(stderr, "fma-va: frame pts=%lld slot=%u matched=%d\n",
                (long long)message->pts_us, frame.slot, surface != NULL);
    if (fma_client_release_frame(&context->client, frame.slot) < 0)
        return false;
    return valid;
}

static bool process_until(struct va_driver *driver, struct va_context *context,
                          uint16_t terminal) {
    for (;;) {
        struct fma_message message;
        if (fma_client_receive(&context->client, &message) < 0)
            return false;
        bool valid = true;
        if (message.type == FMA_MSG_FRAME_READY)
            valid = store_frame(driver, context, &message);
        else if (message.type == FMA_MSG_ERROR)
            valid = false;
        bool done = message.type == terminal;
        fma_message_release(&message);
        if (!valid)
            return false;
        if (done)
            return true;
    }
}

static VAStatus terminate(VADriverContextP ctx) {
    struct va_driver *driver = ctx->pDriverData;
    if (!driver)
        return VA_STATUS_SUCCESS;
    for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i)
        if (driver->contexts[i].used)
            close_context(&driver->contexts[i]);
    for (unsigned i = 0; i < FMA_VA_MAX_SURFACES; ++i)
        free(driver->surfaces[i].data);
    for (unsigned i = 0; i < FMA_VA_MAX_BUFFERS; ++i)
        if (driver->buffers[i].owns_data)
            free(driver->buffers[i].data);
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
    *count = 3;
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

static VAStatus create_surfaces(VADriverContextP ctx, int width, int height,
                                int format, int count, VASurfaceID *ids) {
    struct va_driver *driver = ctx->pDriverData;
    if (!ids || width <= 0 || height <= 0 || count <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!(format & VA_RT_FORMAT_YUV420))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    unsigned stride = ((unsigned)width + 63u) & ~63u;
    if ((size_t)stride > SIZE_MAX / (unsigned)height)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t y_size = (size_t)stride * (unsigned)height;
    size_t bytes = y_size + y_size / 2;
    int created = 0;
    for (unsigned i = 0; i < FMA_VA_MAX_SURFACES && created < count; ++i) {
        if (driver->surfaces[i].used)
            continue;
        uint8_t *data = malloc(bytes);
        if (!data)
            break;
        memset(data, 0, bytes);
        driver->surfaces[i] = (struct va_surface) {
            .used = true, .width = (unsigned)width,
            .height = (unsigned)height, .stride = stride, .size = bytes,
            .data = data, .pts_us = -1, .status = VASurfaceReady,
        };
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
        free(surface->data);
        memset(surface, 0, sizeof(*surface));
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus create_context(VADriverContextP ctx, VAConfigID config_id,
                               int width, int height, int flags,
                               VASurfaceID *targets, int target_count,
                               VAContextID *id) {
    (void)flags;
    struct va_driver *driver = ctx->pDriverData;
    if (!id || width <= 0 || height <= 0 || !get_config(driver, config_id))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (int i = 0; i < target_count; ++i)
        if (!get_surface(driver, targets[i]))
            return VA_STATUS_ERROR_INVALID_SURFACE;
    for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i) {
        if (driver->contexts[i].used)
            continue;
        struct va_context *context = &driver->contexts[i];
        memset(context, 0, sizeof(*context));
        context->client.socket_fd = -1;
        context->pool_fd = -1;
        const char *socket_path = getenv("FMA_SOCKET");
        if (!socket_path || !*socket_path)
            socket_path = "/tmp/fake-media-accel.sock";
        struct fma_decoder_config config = {
            .codec = FMA_CODEC_H264, .width = (uint32_t)width,
            .height = (uint32_t)height, .slot_count = FMA_MAX_SLOTS,
        };
        if (fma_client_connect(&context->client, socket_path) < 0 ||
            fma_client_create_decoder(&context->client, &config,
                                      &context->pool, &context->pool_fd) < 0)
            goto fail;
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
        context->target = VA_INVALID_ID;
        *id = i + 1;
        return VA_STATUS_SUCCESS;
fail:
        close_context(context);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus destroy_context(VADriverContextP ctx, VAContextID id) {
    struct va_context *context = get_context(ctx->pDriverData, id);
    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    close_context(context);
    return VA_STATUS_SUCCESS;
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
    buffer->elements = elements;
    return VA_STATUS_SUCCESS;
}

static VAStatus map_buffer(VADriverContextP ctx, VABufferID id, void **data) {
    struct va_buffer *buffer = get_buffer(ctx->pDriverData, id);
    if (!buffer || !data)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    *data = buffer->data;
    return VA_STATUS_SUCCESS;
}

static VAStatus unmap_buffer(VADriverContextP ctx, VABufferID id) {
    return get_buffer(ctx->pDriverData, id) ? VA_STATUS_SUCCESS :
                                             VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus destroy_buffer(VADriverContextP ctx, VABufferID id) {
    struct va_buffer *buffer = get_buffer(ctx->pDriverData, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->owns_data)
        free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
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
    context->target = target_id;
    context->slice_size = 0;
    context->have_picture = false;
    context->have_first_slice = false;
    surface->pts_us = -1;
    surface->status = VASurfaceRendering;
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
    static const uint8_t start_code[] = {0, 0, 0, 1};
    for (int i = 0; i < count; ++i) {
        struct va_buffer *buffer = get_buffer(driver, ids[i]);
        if (!buffer)
            return VA_STATUS_ERROR_INVALID_BUFFER;
        size_t bytes = (size_t)buffer->size * buffer->elements;
        if (buffer->type == VAPictureParameterBufferType &&
            bytes >= sizeof(context->picture)) {
            memcpy(&context->picture, buffer->data, sizeof(context->picture));
            context->have_picture = true;
        } else if (buffer->type == VASliceParameterBufferType &&
                   bytes >= sizeof(context->first_slice) &&
                   !context->have_first_slice) {
            memcpy(&context->first_slice, buffer->data,
                   sizeof(context->first_slice));
            context->have_first_slice = true;
        } else if (buffer->type == VASliceDataBufferType && bytes) {
            if (context->slice_size &&
                !append_data(&context->slices, &context->slice_size,
                             &context->slice_capacity, start_code,
                             sizeof(start_code)))
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            if (!append_data(&context->slices, &context->slice_size,
                             &context->slice_capacity, buffer->data, bytes))
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
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
    if (!surface || !config || !context->have_picture ||
        !context->have_first_slice)
        return VA_STATUS_ERROR_DECODING_ERROR;
    uint8_t *packet = NULL;
    size_t packet_size = 0;
    if (!fma_h264_build_packet(config->profile, &context->picture,
                               &context->first_slice, context->slices,
                               context->slice_size, &packet, &packet_size) ||
        packet_size > FMA_MAX_PAYLOAD) {
        free(packet);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    unsigned nal_type = context->slice_size ? context->slices[0] & 0x1fu : 0;
    int64_t pts_us = fma_h264_picture_pts(
        &context->timeline, context->picture.CurrPic.TopFieldOrderCnt,
        nal_type == 5);
    if (debug_enabled()) {
        fprintf(stderr,
                "fma-va: picture surface=%u frame_num=%u top_poc=%d "
                "bottom_poc=%d slice_type=%u nal_type=%u pts=%lld\n",
                context->target, context->picture.frame_num,
                context->picture.CurrPic.TopFieldOrderCnt,
                context->picture.CurrPic.BottomFieldOrderCnt,
                context->first_slice.slice_type, nal_type,
                (long long)pts_us);
    }
    surface->pts_us = pts_us;
    dump_packet(packet, packet_size);
    int sent = fma_client_queue_packet(&context->client, packet, packet_size,
                                       pts_us, 0);
    free(packet);
    if (sent < 0 || !process_until(driver, context, FMA_MSG_PACKET_ACK))
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (debug_enabled())
        fprintf(stderr, "fma-va: submitted surface=%u pts=%lld\n",
                context->target, (long long)pts_us);
    context->target = VA_INVALID_ID;
    return VA_STATUS_SUCCESS;
}

static VAStatus sync_surface(VADriverContextP ctx, VASurfaceID id) {
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
            if (fma_client_poll_output(&context->client, 50) < 0 ||
                !process_until(driver, context, FMA_MSG_POLL_DONE))
                return VA_STATUS_ERROR_OPERATION_FAILED;
            if (surface->status == VASurfaceReady)
                return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_TIMEDOUT;
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
    memset(&formats[0], 0, sizeof(formats[0]));
    formats[0].fourcc = VA_FOURCC_NV12;
    *count = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus allocate_image(struct va_driver *driver, VAImageFormat *format,
                               unsigned width, unsigned height, void *data,
                               bool owns, VAImage *image) {
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
    if ((size_t)width > SIZE_MAX / height)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t y_size = (size_t)width * height;
    size_t bytes = y_size + y_size / 2;
    driver->buffers[buffer_index] = (struct va_buffer) {
        .used = true, .type = VAImageBufferType, .size = (unsigned)bytes,
        .elements = 1, .data = data, .owns_data = owns,
    };
    struct va_image *record = &driver->images[image_index];
    memset(record, 0, sizeof(*record));
    record->used = true;
    record->image.image_id = image_index + 1;
    record->image.format = *format;
    record->image.buf = buffer_index + 1;
    record->image.width = width;
    record->image.height = height;
    record->image.data_size = (unsigned)bytes;
    record->image.num_planes = 2;
    record->image.pitches[0] = width;
    record->image.pitches[1] = width;
    record->image.offsets[0] = 0;
    record->image.offsets[1] = (unsigned)y_size;
    *image = record->image;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_image(VADriverContextP ctx, VAImageFormat *format,
                             int width, int height, VAImage *image) {
    if (!format || format->fourcc != VA_FOURCC_NV12 || width <= 0 ||
        height <= 0 || !image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t bytes = (size_t)width * (size_t)height * 3u / 2u;
    void *data = malloc(bytes);
    if (!data)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    VAStatus status = allocate_image(ctx->pDriverData, format, (unsigned)width,
                                     (unsigned)height, data, true, image);
    if (status != VA_STATUS_SUCCESS)
        free(data);
    return status;
}

static VAStatus derive_image(VADriverContextP ctx, VASurfaceID id,
                             VAImage *image) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_surface *surface = get_surface(driver, id);
    if (!surface || !image)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    VAStatus status = sync_surface(ctx, id);
    if (status != VA_STATUS_SUCCESS)
        return status;
    VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    status = allocate_image(driver, &format, surface->stride, surface->height,
                            surface->data, false, image);
    if (status == VA_STATUS_SUCCESS) {
        image->width = surface->width;
        image->pitches[0] = surface->stride;
        image->pitches[1] = surface->stride;
        image->offsets[1] = surface->stride * surface->height;
    }
    return status;
}

static VAStatus destroy_image(VADriverContextP ctx, VAImageID id) {
    struct va_driver *driver = ctx->pDriverData;
    if (!id || id > FMA_VA_MAX_IMAGES || !driver->images[id - 1].used)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    struct va_image *image = &driver->images[id - 1];
    struct va_buffer *buffer = get_buffer(driver, image->image.buf);
    if (buffer) {
        if (buffer->owns_data)
            free(buffer->data);
        memset(buffer, 0, sizeof(*buffer));
    }
    memset(image, 0, sizeof(*image));
    return VA_STATUS_SUCCESS;
}

static VAStatus get_image(VADriverContextP ctx, VASurfaceID surface_id,
                          int x, int y, unsigned width, unsigned height,
                          VAImageID image_id) {
    struct va_driver *driver = ctx->pDriverData;
    struct va_surface *surface = get_surface(driver, surface_id);
    if (!surface || !image_id || image_id > FMA_VA_MAX_IMAGES ||
        !driver->images[image_id - 1].used)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    struct va_image *image = &driver->images[image_id - 1];
    struct va_buffer *buffer = get_buffer(driver, image->image.buf);
    if (!buffer || !buffer->data || x < 0 || y < 0 || (x & 1) || (y & 1) ||
        (width & 1) || (height & 1) ||
        (unsigned)x + width > surface->width ||
        (unsigned)y + height > surface->height ||
        width > image->image.width || height > image->image.height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    VAStatus status = sync_surface(ctx, surface_id);
    if (status != VA_STATUS_SUCCESS)
        return status;
    uint8_t *destination = buffer->data;
    for (unsigned row = 0; row < height; ++row)
        memcpy(destination + image->image.offsets[0] +
                   row * image->image.pitches[0],
               surface->data + ((unsigned)y + row) * surface->stride +
                   (unsigned)x,
               width);
    const uint8_t *source_uv =
        surface->data + (size_t)surface->stride * surface->height;
    for (unsigned row = 0; row < height / 2; ++row)
        memcpy(destination + image->image.offsets[1] +
                   row * image->image.pitches[1],
               source_uv + ((unsigned)y / 2 + row) * surface->stride +
                   (unsigned)x,
               width);
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
    const unsigned required = 6;
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
UNIMPLEMENTED(put_image,
              (VADriverContextP ctx, VASurfaceID surface, VAImageID image,
               int src_x, int src_y, unsigned src_width, unsigned src_height,
               int dst_x, int dst_y, unsigned dst_width, unsigned dst_height))
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
    for (unsigned i = 0; i < FMA_VA_MAX_CONTEXTS; ++i) {
        driver->contexts[i].client.socket_fd = -1;
        driver->contexts[i].pool_fd = -1;
    }
    ctx->pDriverData = driver;
    ctx->version_major = 0;
    ctx->version_minor = 1;
    ctx->max_profiles = 3;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    ctx->max_image_formats = 1;
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
    return VA_STATUS_SUCCESS;
}
