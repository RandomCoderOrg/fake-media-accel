#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <va/va_backend.h>
#include <va/va_drmcommon.h>

extern VAStatus __vaDriverInit_1_14(VADriverContextP context);

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #expression);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static VASurfaceAttrib integer_attribute(VASurfaceAttribType type, int value) {
    VASurfaceAttrib attribute;
    memset(&attribute, 0, sizeof(attribute));
    attribute.type = type;
    attribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribute.value.type = VAGenericValueTypeInteger;
    attribute.value.value.i = value;
    return attribute;
}

static const VASurfaceAttrib *find_attribute(const VASurfaceAttrib *attributes,
                                             unsigned count,
                                             VASurfaceAttribType type) {
    for (unsigned i = 0; i < count; ++i)
        if (attributes[i].type == type)
            return &attributes[i];
    return NULL;
}

static uint8_t luma_value(unsigned x, unsigned y) {
    return (uint8_t)(17u + x * 3u + y * 5u);
}

static uint8_t chroma_value(unsigned x, unsigned y) {
    return (uint8_t)(91u + x * 7u + y * 11u);
}

int main(void) {
    struct VADriverVTable table = {0};
    struct VADriverContext context = {.vtable = &table};
    CHECK(__vaDriverInit_1_14(&context) == VA_STATUS_SUCCESS);

    VAImageFormat formats[2];
    int format_count = 0;
    CHECK(table.vaQueryImageFormats(&context, formats, &format_count) ==
          VA_STATUS_SUCCESS);
    CHECK(format_count == 2);
    CHECK(formats[0].fourcc == VA_FOURCC_NV12);
    CHECK(formats[0].byte_order == VA_LSB_FIRST);
    CHECK(formats[0].bits_per_pixel == 12);
    CHECK(formats[1].fourcc == VA_FOURCC_I420);
    CHECK(formats[1].byte_order == VA_LSB_FIRST);
    CHECK(formats[1].bits_per_pixel == 12);

    VAConfigID config;
    CHECK(table.vaCreateConfig(&context, VAProfileH264High, VAEntrypointVLD,
                               NULL, 0, &config) == VA_STATUS_SUCCESS);

    unsigned attribute_count = 0;
    CHECK(table.vaQuerySurfaceAttributes(&context, config, NULL,
                                         &attribute_count) ==
          VA_STATUS_SUCCESS);
    CHECK(attribute_count == 7);
    VASurfaceAttrib attributes[7];
    CHECK(table.vaQuerySurfaceAttributes(&context, config, attributes,
                                         &attribute_count) ==
          VA_STATUS_SUCCESS);
    const VASurfaceAttrib *memory = find_attribute(
        attributes, attribute_count, VASurfaceAttribMemoryType);
    const VASurfaceAttrib *usage = find_attribute(
        attributes, attribute_count, VASurfaceAttribUsageHint);
    CHECK(memory != NULL);
    CHECK(memory->value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_VA);
    CHECK(usage != NULL);
    CHECK(usage->value.value.i & VA_SURFACE_ATTRIB_USAGE_HINT_DECODER);
    bool exportable =
        (usage->value.value.i & VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT) != 0;

    VASurfaceAttrib requested[3] = {
        integer_attribute(VASurfaceAttribPixelFormat, VA_FOURCC_NV12),
        integer_attribute(VASurfaceAttribMemoryType,
                          VA_SURFACE_ATTRIB_MEM_TYPE_VA),
        integer_attribute(VASurfaceAttribUsageHint,
                          VA_SURFACE_ATTRIB_USAGE_HINT_DECODER),
    };
    VASurfaceID surface;
    CHECK(table.vaCreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 64,
                                  &surface, 1, requested, 3) ==
          VA_STATUS_SUCCESS);

    VASurfaceAttrib drm_memory = integer_attribute(
        VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);
    VASurfaceID rejected_surface = VA_INVALID_ID;
    CHECK(table.vaCreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 64,
                                  &rejected_surface, 1, &drm_memory, 1) ==
          VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);

    VAImage upload;
    CHECK(table.vaCreateImage(&context, &formats[0], 64, 64, &upload) ==
          VA_STATUS_SUCCESS);
    uint8_t *upload_pixels = NULL;
    CHECK(table.vaMapBuffer(&context, upload.buf,
                            (void **)&upload_pixels) == VA_STATUS_SUCCESS);
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x)
            upload_pixels[upload.offsets[0] + y * upload.pitches[0] + x] =
                luma_value(x, y);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 64; ++x)
            upload_pixels[upload.offsets[1] + y * upload.pitches[1] + x] =
                chroma_value(x, y);
    CHECK(table.vaUnmapBuffer(&context, upload.buf) == VA_STATUS_SUCCESS);
    CHECK(table.vaPutImage(&context, surface, upload.image_id, 0, 0, 64, 64,
                           0, 0, 64, 64) == VA_STATUS_SUCCESS);

    VAImage derived;
    CHECK(table.vaDeriveImage(&context, surface, &derived) ==
          VA_STATUS_SUCCESS);
    CHECK(derived.format.fourcc == VA_FOURCC_NV12);
    CHECK(derived.format.bits_per_pixel == 12);
    CHECK(derived.width == 64 && derived.height == 64);
    CHECK(derived.pitches[0] >= 64 && derived.pitches[1] >= 64);
    uint8_t *derived_pixels = NULL;
    CHECK(table.vaMapBuffer(&context, derived.buf,
                            (void **)&derived_pixels) == VA_STATUS_SUCCESS);
    CHECK(derived_pixels[derived.offsets[0] + 23 * derived.pitches[0] + 19] ==
          luma_value(19, 23));
    CHECK(derived_pixels[derived.offsets[1] + 11 * derived.pitches[1] + 18] ==
          chroma_value(18, 11));
    CHECK(table.vaUnmapBuffer(&context, derived.buf) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroySurfaces(&context, &surface, 1) ==
          VA_STATUS_ERROR_SURFACE_BUSY);

    CHECK(table.vaAcquireBufferHandle != NULL);
    CHECK(table.vaReleaseBufferHandle != NULL);
    VABufferInfo buffer_info;
    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    CHECK(table.vaAcquireBufferHandle(&context, derived.buf, &buffer_info) ==
          VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    memset(&buffer_info, 0, sizeof(buffer_info));
    VAStatus acquire_status = table.vaAcquireBufferHandle(
        &context, derived.buf, &buffer_info);
    if (exportable) {
        CHECK(acquire_status == VA_STATUS_SUCCESS);
        CHECK(buffer_info.type == VAImageBufferType);
        CHECK(buffer_info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME);
        CHECK(buffer_info.mem_size >= derived.data_size);
        int acquired_fd = (int)buffer_info.handle;
        CHECK(acquired_fd >= 0);
        CHECK(fcntl(acquired_fd, F_GETFD) >= 0);
        const uint8_t *acquired_pixels = mmap(
            NULL, buffer_info.mem_size, PROT_READ, MAP_SHARED, acquired_fd, 0);
        CHECK(acquired_pixels != MAP_FAILED);
        CHECK(acquired_pixels[derived.offsets[0] +
                              23 * derived.pitches[0] + 19] ==
              luma_value(19, 23));
        CHECK(acquired_pixels[derived.offsets[1] +
                              11 * derived.pitches[1] + 18] ==
              chroma_value(18, 11));
        CHECK(munmap((void *)acquired_pixels, buffer_info.mem_size) == 0);
        CHECK(table.vaMapBuffer(&context, derived.buf,
                                (void **)&derived_pixels) ==
              VA_STATUS_ERROR_SURFACE_BUSY);
        CHECK(table.vaDestroyImage(&context, derived.image_id) ==
              VA_STATUS_ERROR_SURFACE_BUSY);
        CHECK(table.vaReleaseBufferHandle(&context, derived.buf) ==
              VA_STATUS_SUCCESS);
        errno = 0;
        CHECK(fcntl(acquired_fd, F_GETFD) == -1 && errno == EBADF);
    } else {
        CHECK(acquire_status == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    }

    VAImage download;
    CHECK(table.vaCreateImage(&context, &formats[0], 32, 32, &download) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaGetImage(&context, surface, 16, 16, 32, 32,
                           download.image_id) == VA_STATUS_SUCCESS);
    uint8_t *download_pixels = NULL;
    CHECK(table.vaMapBuffer(&context, download.buf,
                            (void **)&download_pixels) == VA_STATUS_SUCCESS);
    CHECK(download_pixels[download.offsets[0] + 7 * download.pitches[0] + 3] ==
          luma_value(19, 23));
    CHECK(download_pixels[download.offsets[1] + 5 * download.pitches[1] + 2] ==
          chroma_value(18, 13));
    CHECK(table.vaUnmapBuffer(&context, download.buf) == VA_STATUS_SUCCESS);

    VAImage planar_download;
    CHECK(table.vaCreateImage(&context, &formats[1], 32, 32,
                              &planar_download) == VA_STATUS_SUCCESS);
    CHECK(planar_download.num_planes == 3);
    CHECK(planar_download.pitches[0] == 32);
    CHECK(planar_download.pitches[1] == 16);
    CHECK(planar_download.pitches[2] == 16);
    CHECK(table.vaGetImage(&context, surface, 16, 16, 32, 32,
                           planar_download.image_id) == VA_STATUS_SUCCESS);
    uint8_t *planar_pixels = NULL;
    CHECK(table.vaMapBuffer(&context, planar_download.buf,
                            (void **)&planar_pixels) == VA_STATUS_SUCCESS);
    CHECK(planar_pixels[planar_download.offsets[0] +
                        7 * planar_download.pitches[0] + 3] ==
          luma_value(19, 23));
    CHECK(planar_pixels[planar_download.offsets[1] +
                        5 * planar_download.pitches[1] + 1] ==
          chroma_value(18, 13));
    CHECK(planar_pixels[planar_download.offsets[2] +
                        5 * planar_download.pitches[2] + 1] ==
          chroma_value(19, 13));
    CHECK(table.vaUnmapBuffer(&context, planar_download.buf) ==
          VA_STATUS_SUCCESS);

    VAImage odd_planar_download;
    CHECK(table.vaCreateImage(&context, &formats[1], 31, 33,
                              &odd_planar_download) == VA_STATUS_SUCCESS);
    CHECK(table.vaGetImage(&context, surface, 0, 0, 31, 33,
                           odd_planar_download.image_id) == VA_STATUS_SUCCESS);
    uint8_t *odd_planar_pixels = NULL;
    CHECK(table.vaMapBuffer(&context, odd_planar_download.buf,
                            (void **)&odd_planar_pixels) == VA_STATUS_SUCCESS);
    CHECK(odd_planar_pixels[odd_planar_download.offsets[0] +
                            32 * odd_planar_download.pitches[0] + 30] ==
          luma_value(30, 32));
    CHECK(odd_planar_pixels[odd_planar_download.offsets[1] +
                            16 * odd_planar_download.pitches[1] + 15] ==
          chroma_value(30, 16));
    CHECK(odd_planar_pixels[odd_planar_download.offsets[2] +
                            16 * odd_planar_download.pitches[2] + 15] ==
          chroma_value(31, 16));
    CHECK(table.vaUnmapBuffer(&context, odd_planar_download.buf) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyImage(&context, odd_planar_download.image_id) ==
          VA_STATUS_SUCCESS);

    VAImage planar_upload;
    CHECK(table.vaCreateImage(&context, &formats[1], 32, 32,
                              &planar_upload) == VA_STATUS_SUCCESS);
    CHECK(table.vaMapBuffer(&context, planar_upload.buf,
                            (void **)&planar_pixels) == VA_STATUS_SUCCESS);
    for (unsigned row = 0; row < 16; ++row)
        for (unsigned column = 0; column < 16; ++column) {
            planar_pixels[planar_upload.offsets[1] +
                          row * planar_upload.pitches[1] + column] =
                (uint8_t)(41u + row + column);
            planar_pixels[planar_upload.offsets[2] +
                          row * planar_upload.pitches[2] + column] =
                (uint8_t)(131u + row + column);
        }
    CHECK(table.vaUnmapBuffer(&context, planar_upload.buf) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaPutImage(&context, surface, planar_upload.image_id,
                           0, 0, 32, 32, 0, 0, 32, 32) == VA_STATUS_SUCCESS);
    VAImage planar_check;
    CHECK(table.vaCreateImage(&context, &formats[0], 32, 32, &planar_check) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaGetImage(&context, surface, 0, 0, 32, 32,
                           planar_check.image_id) == VA_STATUS_SUCCESS);
    uint8_t *planar_check_pixels = NULL;
    CHECK(table.vaMapBuffer(&context, planar_check.buf,
                            (void **)&planar_check_pixels) == VA_STATUS_SUCCESS);
    CHECK(planar_check_pixels[planar_check.offsets[1] +
                              5 * planar_check.pitches[1] + 6] == 49);
    CHECK(planar_check_pixels[planar_check.offsets[1] +
                              5 * planar_check.pitches[1] + 7] == 139);
    CHECK(table.vaUnmapBuffer(&context, planar_check.buf) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyImage(&context, planar_check.image_id) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyImage(&context, planar_upload.image_id) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaPutImage(&context, surface, upload.image_id, 0, 0, 64, 64,
                           0, 0, 64, 64) == VA_STATUS_SUCCESS);

    VADRMPRIMESurfaceDescriptor descriptor;
    CHECK(table.vaExportSurfaceHandle != NULL);
    VAStatus export_status = table.vaExportSurfaceHandle(
        &context, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &descriptor);
    if (exportable) {
        CHECK(export_status == VA_STATUS_SUCCESS);
        CHECK(descriptor.fourcc == VA_FOURCC_NV12);
        CHECK(descriptor.width == 64 && descriptor.height == 64);
        CHECK(descriptor.num_objects == 1 && descriptor.num_layers == 1);
        CHECK(descriptor.layers[0].num_planes == 2);
        CHECK(descriptor.layers[0].object_index[0] == 0);
        CHECK(descriptor.layers[0].object_index[1] == 0);
        CHECK(descriptor.layers[0].offset[0] == 0);
        CHECK(descriptor.layers[0].offset[1] ==
              descriptor.layers[0].pitch[0] * descriptor.height);
        CHECK(descriptor.objects[0].fd >= 0);
        const uint8_t *exported_pixels = mmap(
            NULL, descriptor.objects[0].size, PROT_READ, MAP_SHARED,
            descriptor.objects[0].fd, 0);
        CHECK(exported_pixels != MAP_FAILED);
        CHECK(exported_pixels[descriptor.layers[0].offset[0] +
                              23 * descriptor.layers[0].pitch[0] + 19] ==
              luma_value(19, 23));
        CHECK(exported_pixels[descriptor.layers[0].offset[1] +
                              11 * descriptor.layers[0].pitch[1] + 18] ==
              chroma_value(18, 11));
        CHECK(munmap((void *)exported_pixels, descriptor.objects[0].size) == 0);
        close(descriptor.objects[0].fd);

        memset(&descriptor, 0, sizeof(descriptor));
        CHECK(table.vaExportSurfaceHandle(
                  &context, surface,
                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                  VA_EXPORT_SURFACE_READ_ONLY |
                      VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                  &descriptor) == VA_STATUS_SUCCESS);
        CHECK(descriptor.num_objects == 1 && descriptor.num_layers == 2);
        CHECK(descriptor.layers[0].num_planes == 1);
        CHECK(descriptor.layers[1].num_planes == 1);
        CHECK(descriptor.layers[1].offset[0] > 0);
        close(descriptor.objects[0].fd);
    } else {
        CHECK(export_status == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    }

    VASurfaceID aligned_surface;
    CHECK(table.vaCreateSurfaces2(&context, VA_RT_FORMAT_YUV420, 64, 62,
                                  &aligned_surface, 1, requested, 3) ==
          VA_STATUS_SUCCESS);
    VAImage aligned_image;
    CHECK(table.vaDeriveImage(&context, aligned_surface, &aligned_image) ==
          VA_STATUS_SUCCESS);
    CHECK(aligned_image.width == 64 && aligned_image.height == 62);
    CHECK(aligned_image.offsets[1] == 64 * 64);
    CHECK(aligned_image.data_size == 64 * 64 * 3 / 2);
    if (exportable) {
        memset(&descriptor, 0, sizeof(descriptor));
        CHECK(table.vaExportSurfaceHandle(
                  &context, aligned_surface,
                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                  VA_EXPORT_SURFACE_READ_ONLY |
                      VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                  &descriptor) == VA_STATUS_SUCCESS);
        CHECK(descriptor.width == 64 && descriptor.height == 62);
        CHECK(descriptor.layers[0].offset[1] == 64 * 64);
        CHECK(descriptor.objects[0].size == 64 * 64 * 3 / 2);
        close(descriptor.objects[0].fd);
    }
    CHECK(table.vaDestroyImage(&context, aligned_image.image_id) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroySurfaces(&context, &aligned_surface, 1) ==
          VA_STATUS_SUCCESS);

    CHECK(table.vaDestroyImage(&context, download.image_id) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyImage(&context, planar_download.image_id) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyImage(&context, derived.image_id) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyImage(&context, upload.image_id) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroySurfaces(&context, &surface, 1) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyConfig(&context, config) == VA_STATUS_SUCCESS);
    CHECK(table.vaTerminate(&context) == VA_STATUS_SUCCESS);
    printf("VA surface contract passed (CPU NV12; DRM PRIME %s)\n",
           exportable ? "available" : "unavailable");
    return 0;
}
