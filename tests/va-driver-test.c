#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va_backend.h>

extern VAStatus __vaDriverInit_1_14(VADriverContextP context);

#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #expression);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void) {
    struct VADriverVTable table = {0};
    struct VADriverContext context = {.vtable = &table};
    CHECK(__vaDriverInit_1_14(&context) == VA_STATUS_SUCCESS);

    VAProfile profiles[4];
    int profile_count = 0;
    CHECK(table.vaQueryConfigProfiles(&context, profiles, &profile_count) ==
          VA_STATUS_SUCCESS);
    CHECK(profile_count == 3);

    VAConfigID config;
    CHECK(table.vaCreateConfig(&context, VAProfileH264High, VAEntrypointVLD,
                               NULL, 0, &config) == VA_STATUS_SUCCESS);
    VASurfaceID surfaces[2];
    CHECK(table.vaCreateSurfaces(&context, 64, 64, VA_RT_FORMAT_YUV420, 2,
                                 surfaces) == VA_STATUS_SUCCESS);
    VAContextID decoder;
    CHECK(table.vaCreateContext(&context, config, 64, 64, 0, surfaces, 2,
                                &decoder) == VA_STATUS_SUCCESS);

    VAPictureParameterBufferH264 picture = {0};
    picture.picture_width_in_mbs_minus1 = 3;
    picture.picture_height_in_mbs_minus1 = 3;
    picture.num_ref_frames = 1;
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.seq_fields.bits.direct_8x8_inference_flag = 1;
    picture.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    picture.seq_fields.bits.pic_order_cnt_type = 0;
    picture.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
    VASliceParameterBufferH264 slice = {0};
    const uint8_t slice_data[] = {0x65, 0xb8, 0x40};

    VABufferID buffers[3];
    CHECK(table.vaCreateBuffer(&context, decoder,
                               VAPictureParameterBufferType, sizeof(picture),
                               1, &picture, &buffers[0]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, decoder,
                               VASliceParameterBufferType, sizeof(slice), 1,
                               &slice, &buffers[1]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, decoder, VASliceDataBufferType,
                               sizeof(slice_data), 1, (void *)slice_data,
                               &buffers[2]) == VA_STATUS_SUCCESS);
    CHECK(table.vaBeginPicture(&context, decoder, surfaces[0]) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaRenderPicture(&context, decoder, buffers, 3) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaEndPicture(&context, decoder) == VA_STATUS_SUCCESS);
    CHECK(table.vaSyncSurface(&context, surfaces[0]) == VA_STATUS_SUCCESS);

    VAImage image;
    CHECK(table.vaDeriveImage(&context, surfaces[0], &image) ==
          VA_STATUS_SUCCESS);
    void *pixels = NULL;
    CHECK(table.vaMapBuffer(&context, image.buf, &pixels) == VA_STATUS_SUCCESS);
    CHECK(pixels != NULL);
    CHECK(((const uint8_t *)pixels)[0] != 0);
    CHECK(table.vaUnmapBuffer(&context, image.buf) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyImage(&context, image.image_id) == VA_STATUS_SUCCESS);

    for (unsigned i = 0; i < 3; ++i)
        CHECK(table.vaDestroyBuffer(&context, buffers[i]) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyContext(&context, decoder) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroySurfaces(&context, surfaces, 2) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyConfig(&context, config) == VA_STATUS_SUCCESS);
    CHECK(table.vaTerminate(&context) == VA_STATUS_SUCCESS);
    puts("VA driver roundtrip passed");
    return 0;
}
