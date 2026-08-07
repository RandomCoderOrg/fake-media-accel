#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va_backend.h>
#include <va/va_dec_vp9.h>

#include "h264_annexb.h"

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
    CHECK(profile_count == 4);
    CHECK(profiles[3] == VAProfileVP9Profile0);
    VAConfigID unsupported_config;
    CHECK(table.vaCreateConfig(&context, VAProfileVP9Profile2,
                               VAEntrypointVLD, NULL, 0,
                               &unsupported_config) ==
          VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

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
    slice.slice_data_size = sizeof(slice_data);
    static const uint8_t expected_packet[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x2a,
        0xac, 0xe8, 0x42, 0x68, 0x06, 0xd0, 0x44, 0x23,
        0x50, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38,
        0x30, 0x00, 0x00, 0x00, 0x01, 0x65, 0xb8, 0x40,
    };
    uint8_t *packet = NULL;
    size_t packet_size = 0;
    CHECK(fma_h264_build_packet(VAProfileH264High, &picture, NULL, &slice,
                                slice_data, sizeof(slice_data), &packet,
                                &packet_size) == FMA_H264_BUILD_OK);
    CHECK(packet_size == sizeof(expected_packet));
    CHECK(memcmp(packet, expected_packet, sizeof(expected_packet)) == 0);
    free(packet);

    picture.seq_fields.bits.pic_order_cnt_type = 1;
    CHECK(fma_h264_build_packet(VAProfileH264High, &picture, NULL, &slice,
                                slice_data, sizeof(slice_data), &packet,
                                &packet_size) ==
          FMA_H264_BUILD_UNREPRESENTABLE);
    CHECK(packet == NULL && packet_size == 0);
    picture.seq_fields.bits.pic_order_cnt_type = 0;

    VAPictureParameterBufferH264 uhd_picture = picture;
    uhd_picture.picture_width_in_mbs_minus1 = 255;
    uhd_picture.picture_height_in_mbs_minus1 = 134;
    CHECK(fma_h264_build_packet(VAProfileH264High, &uhd_picture, NULL,
                                &slice, slice_data, sizeof(slice_data),
                                &packet, &packet_size) == FMA_H264_BUILD_OK);
    CHECK(packet_size > 8 && packet[7] == 51);
    free(packet);

    VAPictureParameterBufferH264 unsupported_picture = picture;
    unsupported_picture.bit_depth_luma_minus8 = 2;
    CHECK(fma_h264_build_packet(
              VAProfileH264High, &unsupported_picture, NULL, &slice,
              slice_data, sizeof(slice_data), &packet, &packet_size) ==
          FMA_H264_BUILD_UNREPRESENTABLE);

    VAIQMatrixBufferH264 iq_matrix = {0};
    memset(iq_matrix.ScalingList4x4, 16,
           sizeof(iq_matrix.ScalingList4x4));
    memset(iq_matrix.ScalingList8x8, 16,
           sizeof(iq_matrix.ScalingList8x8));
    CHECK(fma_h264_build_packet(VAProfileH264High, &picture, &iq_matrix,
                                &slice, slice_data, sizeof(slice_data),
                                &packet, &packet_size) == FMA_H264_BUILD_OK);
    CHECK(packet_size > sizeof(expected_packet));
    free(packet);

    VASliceParameterBufferH264 second_slice = slice;
    const uint8_t second_slice_data[] = {0xff, 0xee, 0x41, 0xe0};
    second_slice.slice_data_offset = 2;
    second_slice.slice_data_size = 2;
    static const uint8_t assembled_slices[] = {
        0x65, 0xb8, 0x40, 0x00, 0x00, 0x00, 0x01, 0x41, 0xe0,
    };
    uint8_t *expected_multi_packet = NULL;
    size_t expected_multi_packet_size = 0;
    CHECK(fma_h264_build_packet(
              VAProfileH264High, &picture, &iq_matrix, &slice,
              assembled_slices, sizeof(assembled_slices),
              &expected_multi_packet, &expected_multi_packet_size) ==
          FMA_H264_BUILD_OK);

    VABufferID buffers[6];
    CHECK(table.vaCreateBuffer(&context, decoder,
                               VAPictureParameterBufferType, sizeof(picture),
                               1, &picture, &buffers[0]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, decoder,
                               VAIQMatrixBufferType, sizeof(iq_matrix), 1,
                               &iq_matrix, &buffers[1]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, decoder,
                               VASliceParameterBufferType, sizeof(slice), 1,
                               &slice, &buffers[2]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, decoder, VASliceDataBufferType,
                               sizeof(slice_data), 1, (void *)slice_data,
                               &buffers[3]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, decoder,
                               VASliceParameterBufferType,
                               sizeof(second_slice), 1, &second_slice,
                               &buffers[4]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, decoder, VASliceDataBufferType,
                               sizeof(second_slice_data), 1,
                               (void *)second_slice_data,
                               &buffers[5]) == VA_STATUS_SUCCESS);
    const char *dump_path = "/tmp/fma-va-driver-test.h264";
    (void)remove(dump_path);
    CHECK(setenv("FMA_VA_DUMP", dump_path, 1) == 0);
    CHECK(table.vaBeginPicture(&context, decoder, surfaces[0]) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaRenderPicture(&context, decoder, buffers, 6) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaEndPicture(&context, decoder) == VA_STATUS_SUCCESS);
    FILE *dump = fopen(dump_path, "rb");
    CHECK(dump != NULL);
    uint8_t *dumped_packet = malloc(expected_multi_packet_size);
    CHECK(dumped_packet != NULL);
    CHECK(fread(dumped_packet, 1, expected_multi_packet_size, dump) ==
          expected_multi_packet_size);
    CHECK(fgetc(dump) == EOF);
    CHECK(fclose(dump) == 0);
    CHECK(memcmp(dumped_packet, expected_multi_packet,
                 expected_multi_packet_size) == 0);
    free(dumped_packet);
    free(expected_multi_packet);
    CHECK(remove(dump_path) == 0);
    CHECK(unsetenv("FMA_VA_DUMP") == 0);

    /*
     * The Android backend acknowledges input before MediaCodec necessarily
     * emits its frame. Reusing a still-rendering surface must collect that
     * pending output before the next picture replaces its routing identity.
     */
    picture.frame_num = 1;
    picture.CurrPic.TopFieldOrderCnt = 2;
    picture.CurrPic.BottomFieldOrderCnt = 2;
    VAPictureParameterBufferH264 *mapped_picture = NULL;
    CHECK(table.vaMapBuffer(&context, buffers[0],
                            (void **)&mapped_picture) == VA_STATUS_SUCCESS);
    CHECK(mapped_picture != NULL);
    *mapped_picture = picture;
    CHECK(table.vaUnmapBuffer(&context, buffers[0]) == VA_STATUS_SUCCESS);
    VASliceParameterBufferH264 *mapped_slice = NULL;
    CHECK(table.vaMapBuffer(&context, buffers[2], (void **)&mapped_slice) ==
          VA_STATUS_SUCCESS);
    mapped_slice->slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    CHECK(table.vaUnmapBuffer(&context, buffers[2]) == VA_STATUS_SUCCESS);
    CHECK(table.vaMapBuffer(&context, buffers[4], (void **)&mapped_slice) ==
          VA_STATUS_SUCCESS);
    mapped_slice->slice_data_flag = VA_SLICE_DATA_FLAG_END;
    CHECK(table.vaUnmapBuffer(&context, buffers[4]) == VA_STATUS_SUCCESS);
    CHECK(table.vaBeginPicture(&context, decoder, surfaces[0]) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaRenderPicture(&context, decoder, buffers, 6) ==
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

    for (unsigned i = 0; i < 6; ++i)
        CHECK(table.vaDestroyBuffer(&context, buffers[i]) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyContext(&context, decoder) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroySurfaces(&context, surfaces, 2) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyConfig(&context, config) == VA_STATUS_SUCCESS);

    VAConfigID vp9_config;
    CHECK(table.vaCreateConfig(&context, VAProfileVP9Profile0,
                               VAEntrypointVLD, NULL, 0,
                               &vp9_config) == VA_STATUS_SUCCESS);
    VASurfaceID vp9_surfaces[2];
    CHECK(table.vaCreateSurfaces(&context, 64, 64, VA_RT_FORMAT_YUV420, 2,
                                 vp9_surfaces) == VA_STATUS_SUCCESS);
    VAContextID vp9_decoder;
    CHECK(table.vaCreateContext(&context, vp9_config, 64, 64, 0,
                                vp9_surfaces, 2, &vp9_decoder) ==
          VA_STATUS_SUCCESS);
    VADecPictureParameterBufferVP9 vp9_picture = {0};
    vp9_picture.frame_width = 64;
    vp9_picture.frame_height = 64;
    vp9_picture.profile = 0;
    vp9_picture.bit_depth = 8;
    vp9_picture.pic_fields.bits.subsampling_x = 1;
    vp9_picture.pic_fields.bits.subsampling_y = 1;
    vp9_picture.pic_fields.bits.show_frame = 1;
    VASliceParameterBufferVP9 vp9_slice = {0};
    static const uint8_t vp9_buffer_data[] = {
        0xff, 0xee, 0x82, 0x49, 0x83, 0x42,
    };
    static const uint8_t expected_vp9_packet[] = {
        0x82, 0x49, 0x83, 0x42,
    };
    vp9_slice.slice_data_offset = 2;
    vp9_slice.slice_data_size = sizeof(expected_vp9_packet);
    vp9_slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    VABufferID vp9_buffers[3];
    CHECK(table.vaCreateBuffer(&context, vp9_decoder,
                               VAPictureParameterBufferType,
                               sizeof(vp9_picture), 1, &vp9_picture,
                               &vp9_buffers[0]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, vp9_decoder,
                               VASliceParameterBufferType,
                               sizeof(vp9_slice), 1, &vp9_slice,
                               &vp9_buffers[1]) == VA_STATUS_SUCCESS);
    CHECK(table.vaCreateBuffer(&context, vp9_decoder,
                               VASliceDataBufferType,
                               sizeof(vp9_buffer_data), 1,
                               (void *)vp9_buffer_data,
                               &vp9_buffers[2]) == VA_STATUS_SUCCESS);
    const char *vp9_dump_path = "/tmp/fma-va-driver-test.vp9";
    (void)remove(vp9_dump_path);
    CHECK(setenv("FMA_VA_DUMP", vp9_dump_path, 1) == 0);
    CHECK(table.vaBeginPicture(&context, vp9_decoder, vp9_surfaces[0]) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaRenderPicture(&context, vp9_decoder, vp9_buffers, 3) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaEndPicture(&context, vp9_decoder) == VA_STATUS_SUCCESS);
    CHECK(table.vaSyncSurface(&context, vp9_surfaces[0]) == VA_STATUS_SUCCESS);
    dump = fopen(vp9_dump_path, "rb");
    CHECK(dump != NULL);
    uint8_t vp9_dumped[sizeof(expected_vp9_packet)];
    CHECK(fread(vp9_dumped, 1, sizeof(vp9_dumped), dump) ==
          sizeof(vp9_dumped));
    CHECK(fgetc(dump) == EOF && fclose(dump) == 0);
    CHECK(memcmp(vp9_dumped, expected_vp9_packet,
                 sizeof(vp9_dumped)) == 0);
    CHECK(remove(vp9_dump_path) == 0);
    CHECK(unsetenv("FMA_VA_DUMP") == 0);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(table.vaDestroyBuffer(&context, vp9_buffers[i]) ==
              VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyContext(&context, vp9_decoder) == VA_STATUS_SUCCESS);
    CHECK(table.vaDestroySurfaces(&context, vp9_surfaces, 2) ==
          VA_STATUS_SUCCESS);
    CHECK(table.vaDestroyConfig(&context, vp9_config) == VA_STATUS_SUCCESS);
    CHECK(table.vaTerminate(&context) == VA_STATUS_SUCCESS);
    puts("VA driver roundtrip passed");
    return 0;
}
