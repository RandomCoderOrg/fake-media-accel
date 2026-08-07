#include "h264_annexb.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct bit_writer {
    uint8_t data[512];
    size_t bit_count;
    bool overflow;
};

struct bit_reader {
    uint8_t data[128];
    size_t bit_count;
    size_t bit_offset;
};

struct h264_level_limit {
    uint8_t level_idc;
    uint32_t max_fs;
    uint32_t max_dpb_mbs;
};

/* H.264 table A-1, beginning at the previously validated level 4.2. */
static const struct h264_level_limit h264_level_limits[] = {
    {42, 8704, 34816},
    {50, 22080, 110400},
    {51, 36864, 184320},
    {52, 36864, 184320},
    {60, 139264, 696320},
    {61, 139264, 696320},
    {62, 139264, 696320},
};

static bool append_bytes(uint8_t **data, size_t *size, size_t *capacity,
                         const void *source, size_t source_size) {
    if (source_size > SIZE_MAX - *size)
        return false;
    size_t needed = *size + source_size;
    if (needed > *capacity) {
        size_t next = *capacity ? *capacity : 4096;
        while (next < needed) {
            if (next > SIZE_MAX / 2) {
                next = needed;
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
    *size = needed;
    return true;
}

static void put_bit(struct bit_writer *writer, unsigned value) {
    if (writer->bit_count >= sizeof(writer->data) * CHAR_BIT) {
        writer->overflow = true;
        return;
    }
    size_t byte = writer->bit_count / CHAR_BIT;
    unsigned shift = 7u - (unsigned)(writer->bit_count % CHAR_BIT);
    if (value & 1u)
        writer->data[byte] |= (uint8_t)(1u << shift);
    writer->bit_count++;
}

static void put_bits(struct bit_writer *writer, uint32_t value,
                     unsigned count) {
    for (unsigned i = count; i > 0; --i)
        put_bit(writer, value >> (i - 1));
}

static void put_ue(struct bit_writer *writer, unsigned value) {
    uint64_t code = (uint64_t)value + 1u;
    unsigned bits = 0;
    for (uint64_t cursor = code; cursor; cursor >>= 1)
        ++bits;
    for (unsigned i = 1; i < bits; ++i)
        put_bit(writer, 0);
    for (unsigned i = bits; i > 0; --i)
        put_bit(writer, (unsigned)(code >> (i - 1)));
}

static void put_se(struct bit_writer *writer, int value) {
    unsigned code = value <= 0 ? (unsigned)(-(int64_t)value * 2)
                               : (unsigned)((int64_t)value * 2 - 1);
    put_ue(writer, code);
}

static const uint8_t zigzag_4x4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};

static const uint8_t zigzag_8x8[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static bool put_scaling_list(struct bit_writer *writer, const uint8_t *list,
                             const uint8_t *scan, size_t count) {
    int last_scale = 8;
    for (size_t i = 0; i < count; ++i) {
        int next_scale = list[scan[i]];
        if (next_scale <= 0)
            return false;
        int delta = next_scale - last_scale;
        while (delta < -128)
            delta += 256;
        while (delta > 127)
            delta -= 256;
        put_se(writer, delta);
        last_scale = next_scale;
    }
    return !writer->overflow;
}

static unsigned select_level_idc(
    const VAPictureParameterBufferH264 *picture) {
    uint64_t width_mbs = (uint64_t)picture->picture_width_in_mbs_minus1 + 1;
    uint64_t height_mbs =
        (uint64_t)picture->picture_height_in_mbs_minus1 + 1;
    uint64_t frame_mbs = width_mbs * height_mbs;
    uint64_t buffering = picture->num_ref_frames ? picture->num_ref_frames : 1;
    for (size_t i = 0;
         i < sizeof(h264_level_limits) / sizeof(h264_level_limits[0]); ++i) {
        const struct h264_level_limit *level = &h264_level_limits[i];
        if (frame_mbs <= level->max_fs &&
            width_mbs * width_mbs <= UINT64_C(8) * level->max_fs &&
            height_mbs * height_mbs <= UINT64_C(8) * level->max_fs &&
            frame_mbs * buffering <= level->max_dpb_mbs)
            return level->level_idc;
    }
    return 0;
}

static size_t finish_rbsp(struct bit_writer *writer) {
    put_bit(writer, 1);
    while (writer->bit_count % CHAR_BIT)
        put_bit(writer, 0);
    return writer->overflow ? 0 : writer->bit_count / CHAR_BIT;
}

static bool append_nal(uint8_t **packet, size_t *size, size_t *capacity,
                       uint8_t header, const uint8_t *rbsp,
                       size_t rbsp_size) {
    static const uint8_t start_code[] = {0, 0, 0, 1};
    if (!append_bytes(packet, size, capacity, start_code, sizeof(start_code)) ||
        !append_bytes(packet, size, capacity, &header, 1))
        return false;
    unsigned zeroes = 0;
    for (size_t i = 0; i < rbsp_size; ++i) {
        if (zeroes >= 2 && rbsp[i] <= 3) {
            const uint8_t escape = 3;
            if (!append_bytes(packet, size, capacity, &escape, 1))
                return false;
            zeroes = 0;
        }
        if (!append_bytes(packet, size, capacity, &rbsp[i], 1))
            return false;
        zeroes = rbsp[i] == 0 ? zeroes + 1 : 0;
    }
    return true;
}

static bool read_bit(struct bit_reader *reader, unsigned *value) {
    if (reader->bit_offset >= reader->bit_count)
        return false;
    size_t byte = reader->bit_offset / CHAR_BIT;
    unsigned shift = 7u - (unsigned)(reader->bit_offset % CHAR_BIT);
    *value = (reader->data[byte] >> shift) & 1u;
    ++reader->bit_offset;
    return true;
}

static bool read_ue(struct bit_reader *reader, unsigned *value) {
    unsigned leading_zeroes = 0;
    unsigned bit = 0;
    while (read_bit(reader, &bit) && !bit) {
        if (++leading_zeroes > 31)
            return false;
    }
    if (!bit)
        return false;
    uint32_t code = 1;
    for (unsigned i = 0; i < leading_zeroes; ++i) {
        if (!read_bit(reader, &bit))
            return false;
        code = (code << 1) | bit;
    }
    *value = code - 1;
    return true;
}

static unsigned slice_pps_id(const uint8_t *data, size_t size) {
    struct bit_reader reader = {0};
    if (size < 2)
        return 0;
    unsigned zeroes = 0;
    for (size_t i = 1; i < size &&
                            reader.bit_count / CHAR_BIT < sizeof(reader.data);
         ++i) {
        if (zeroes >= 2 && data[i] == 3) {
            zeroes = 0;
            continue;
        }
        reader.data[reader.bit_count / CHAR_BIT] = data[i];
        reader.bit_count += CHAR_BIT;
        zeroes = data[i] == 0 ? zeroes + 1 : 0;
    }
    unsigned ignored = 0;
    unsigned pps_id = 0;
    if (!read_ue(&reader, &ignored) || !read_ue(&reader, &ignored) ||
        !read_ue(&reader, &pps_id))
        return 0;
    return pps_id;
}

enum fma_h264_build_status fma_h264_build_packet(
    VAProfile profile, const VAPictureParameterBufferH264 *picture,
    const VAIQMatrixBufferH264 *iq_matrix,
    const VASliceParameterBufferH264 *slice,
    const uint8_t *slice_data, size_t slice_size,
    uint8_t **packet, size_t *packet_size) {
    if (!picture || !slice || !slice_data || !slice_size || !packet ||
        !packet_size)
        return FMA_H264_BUILD_INVALID;
    *packet = NULL;
    *packet_size = 0;
    if (profile != VAProfileH264ConstrainedBaseline &&
        profile != VAProfileH264Main && profile != VAProfileH264High)
        return FMA_H264_BUILD_UNREPRESENTABLE;
    if (picture->seq_fields.bits.chroma_format_idc != 1 ||
        picture->bit_depth_luma_minus8 != 0 ||
        picture->bit_depth_chroma_minus8 != 0 ||
        picture->seq_fields.bits.pic_order_cnt_type > 2)
        return FMA_H264_BUILD_UNREPRESENTABLE;
    /*
     * VA-API omits offset_for_non_ref_pic,
     * offset_for_top_to_bottom_field and offset_for_ref_frame[]. Guessing
     * those values creates a syntactically valid but semantically different
     * stream, so POC type 1 needs the packet-preserving path.
     */
    if (picture->seq_fields.bits.pic_order_cnt_type == 1)
        return FMA_H264_BUILD_UNREPRESENTABLE;
    size_t capacity = 0;
    unsigned profile_idc = profile == VAProfileH264High ? 100 :
                           profile == VAProfileH264Main ? 77 : 66;
    unsigned constraints =
        profile == VAProfileH264ConstrainedBaseline ? 0xc0 : 0;
    unsigned chroma_format = picture->seq_fields.bits.chroma_format_idc;
    unsigned level_idc = select_level_idc(picture);
    if (!level_idc)
        return FMA_H264_BUILD_UNREPRESENTABLE;

    struct bit_writer sps = {0};
    put_bits(&sps, profile_idc, 8);
    put_bits(&sps, constraints, 8);
    put_bits(&sps, level_idc, 8);
    put_ue(&sps, 0);
    if (profile_idc == 100) {
        put_ue(&sps, chroma_format);
        if (chroma_format == 3)
            put_bit(&sps,
                    picture->seq_fields.bits.residual_colour_transform_flag);
        put_ue(&sps, picture->bit_depth_luma_minus8);
        put_ue(&sps, picture->bit_depth_chroma_minus8);
        put_bit(&sps, 0);
        put_bit(&sps, 0);
    }
    put_ue(&sps, picture->seq_fields.bits.log2_max_frame_num_minus4);
    put_ue(&sps, picture->seq_fields.bits.pic_order_cnt_type);
    if (picture->seq_fields.bits.pic_order_cnt_type == 0)
        put_ue(&sps,
               picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    put_ue(&sps, picture->num_ref_frames);
    put_bit(&sps,
            picture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
    put_ue(&sps, picture->picture_width_in_mbs_minus1);
    put_ue(&sps, picture->picture_height_in_mbs_minus1);
    put_bit(&sps, picture->seq_fields.bits.frame_mbs_only_flag);
    if (!picture->seq_fields.bits.frame_mbs_only_flag)
        put_bit(&sps,
                picture->seq_fields.bits.mb_adaptive_frame_field_flag);
    put_bit(&sps, picture->seq_fields.bits.direct_8x8_inference_flag);
    put_bit(&sps, 0);
    /*
     * libva does not expose the source VUI.  Supplying no bitstream
     * restriction lets a decoder derive a reorder window from level_idc;
     * that can be larger than the VA surface pool and deadlock B-frame
     * streams.  The VA caller already owns presentation order through its
     * surfaces, so request decode-order output from MediaCodec and use POC
     * timestamps to map each returned image back to the correct surface.
     */
    unsigned buffering = picture->num_ref_frames ? picture->num_ref_frames : 1;
    put_bit(&sps, 1);  /* vui_parameters_present_flag */
    put_bit(&sps, 0);  /* aspect_ratio_info_present_flag */
    put_bit(&sps, 0);  /* overscan_info_present_flag */
    put_bit(&sps, 0);  /* video_signal_type_present_flag */
    put_bit(&sps, 0);  /* chroma_loc_info_present_flag */
    put_bit(&sps, 0);  /* timing_info_present_flag */
    put_bit(&sps, 0);  /* nal_hrd_parameters_present_flag */
    put_bit(&sps, 0);  /* vcl_hrd_parameters_present_flag */
    put_bit(&sps, 0);  /* pic_struct_present_flag */
    put_bit(&sps, 1);  /* bitstream_restriction_flag */
    put_bit(&sps, 1);  /* motion_vectors_over_pic_boundaries_flag */
    put_ue(&sps, 2);   /* max_bytes_per_pic_denom */
    put_ue(&sps, 1);   /* max_bits_per_mb_denom */
    put_ue(&sps, 16);  /* log2_max_mv_length_horizontal */
    put_ue(&sps, 16);  /* log2_max_mv_length_vertical */
    put_ue(&sps, 0);          /* max_num_reorder_frames */
    put_ue(&sps, buffering);  /* max_dec_frame_buffering */
    size_t sps_size = finish_rbsp(&sps);
    if (!sps_size || !append_nal(packet, packet_size, &capacity, 0x67,
                                 sps.data, sps_size))
        goto no_memory;

    struct bit_writer pps = {0};
    put_ue(&pps, slice_pps_id(slice_data, slice_size));
    put_ue(&pps, 0);
    put_bit(&pps, picture->pic_fields.bits.entropy_coding_mode_flag);
    put_bit(&pps, picture->pic_fields.bits.pic_order_present_flag);
    put_ue(&pps, 0);
    put_ue(&pps, slice->num_ref_idx_l0_active_minus1);
    put_ue(&pps, slice->num_ref_idx_l1_active_minus1);
    put_bit(&pps, picture->pic_fields.bits.weighted_pred_flag);
    put_bits(&pps, picture->pic_fields.bits.weighted_bipred_idc, 2);
    put_se(&pps, picture->pic_init_qp_minus26);
    put_se(&pps, picture->pic_init_qs_minus26);
    put_se(&pps, picture->chroma_qp_index_offset);
    put_bit(&pps,
            picture->pic_fields.bits.deblocking_filter_control_present_flag);
    put_bit(&pps, picture->pic_fields.bits.constrained_intra_pred_flag);
    put_bit(&pps, picture->pic_fields.bits.redundant_pic_cnt_present_flag);
    if (profile_idc == 100) {
        put_bit(&pps, picture->pic_fields.bits.transform_8x8_mode_flag);
        put_bit(&pps, iq_matrix != NULL);
        if (iq_matrix) {
            for (unsigned i = 0; i < 6; ++i) {
                put_bit(&pps, 1);
                if (!put_scaling_list(&pps, iq_matrix->ScalingList4x4[i],
                                      zigzag_4x4, 16))
                    goto invalid;
            }
            if (picture->pic_fields.bits.transform_8x8_mode_flag) {
                for (unsigned i = 0; i < 2; ++i) {
                    put_bit(&pps, 1);
                    if (!put_scaling_list(&pps,
                                          iq_matrix->ScalingList8x8[i],
                                          zigzag_8x8, 64))
                        goto invalid;
                }
            }
        }
        put_se(&pps, picture->second_chroma_qp_index_offset);
    }
    size_t pps_size = finish_rbsp(&pps);
    static const uint8_t start_code[] = {0, 0, 0, 1};
    if (!pps_size || !append_nal(packet, packet_size, &capacity, 0x68,
                                 pps.data, pps_size) ||
        !append_bytes(packet, packet_size, &capacity, start_code,
                      sizeof(start_code)) ||
        !append_bytes(packet, packet_size, &capacity, slice_data, slice_size))
        goto no_memory;
    return FMA_H264_BUILD_OK;

invalid:
    free(*packet);
    *packet = NULL;
    *packet_size = 0;
    return FMA_H264_BUILD_INVALID;

no_memory:
    free(*packet);
    *packet = NULL;
    *packet_size = 0;
    return FMA_H264_BUILD_NO_MEMORY;
}
