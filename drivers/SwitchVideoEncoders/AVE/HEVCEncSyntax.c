// SPDX-License-Identifier: BSD-2-Clause-Patent
// See HEVCEncSyntax.h. Clause numbers are ITU-T H.265 (V8); levels from Table A.8.
#include "HEVCEncSyntax.h"
#include <string.h>

enum {
    NAL_IDR_W_RADL = 19,
    NAL_TRAIL_R = 1,
    NAL_VPS = 32,
    NAL_SPS = 33,
    NAL_PPS = 34,
    SLICE_P = 1,
    SLICE_I = 2,
    PROFILE_MAIN = 1,
};

static uint32_t align(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

uint32_t hevcenc_coded_width(const HEVCEncConfig *c)
{
    return align(c->width, 1u << c->log2_ctb);
}

uint32_t hevcenc_coded_height(const HEVCEncConfig *c)
{
    return align(c->height, 1u << c->log2_ctb);
}

int hevcenc_configure(HEVCEncConfig *c)
{
    if (!c->log2_ctb)
        c->log2_ctb = 5;
    if (!c->log2_min_cb)
        c->log2_min_cb = 3;
    if (!c->log2_min_tb)
        c->log2_min_tb = 2;
    if (!c->log2_max_tb)
        c->log2_max_tb = c->log2_ctb < 5 ? c->log2_ctb : 5;
    if (c->log2_min_cb < 3 || c->log2_min_cb > c->log2_ctb || c->log2_ctb > 6 || c->log2_min_tb < 2 ||
        c->log2_min_tb >= c->log2_min_cb || c->log2_max_tb > 5 || c->log2_max_tb < c->log2_min_tb ||
        c->log2_max_tb > c->log2_ctb || c->max_tr_depth_inter > c->log2_ctb - c->log2_min_tb ||
        c->max_tr_depth_intra > c->log2_ctb - c->log2_min_tb)
        return -1;
    if (c->width < 32 || c->height < 32 || c->width > 4096 || c->height > 4096 || (c->width | c->height) & 1)
        return -1;
    if ((c->fps_num == 0) != (c->fps_den == 0))
        return -1;
    if (c->level_idc)
        return 0;

    static const struct {
        uint8_t level;
        uint32_t ps;
        uint64_t sr;
    } kLevels[] = {
        {60, 122880, 3686400},     {63, 245760, 7372800},     {90, 552960, 16588800},
        {93, 983040, 33177600},    {120, 2228224, 66846720},  {123, 2228224, 133693440},
        {150, 8912896, 267386880}, {153, 8912896, 534773760}, {156, 8912896, 1069547520},
    };

    const uint64_t ps = (uint64_t)hevcenc_coded_width(c) * hevcenc_coded_height(c);
    const uint64_t sr = c->fps_num ? (ps * c->fps_num + c->fps_den - 1) / c->fps_den : ps * 60;
    const uint64_t side = c->width > c->height ? c->width : c->height;
    for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
        if (ps <= kLevels[i].ps && sr <= kLevels[i].sr && side * side <= 8ULL * kLevels[i].ps) {
            c->level_idc = kLevels[i].level;
            return 0;
        }
    }
    return -1;
}

static size_t nal(uint8_t type, const uint8_t *rbsp, size_t size, uint8_t *out, size_t cap)
{
    if (cap < 2)
        return 0;
    size_t n = 0;
    unsigned zeros = 0;
    out[n++] = (uint8_t)(type << 1);  // forbidden_zero_bit, nal_unit_type, nuh_layer_id 0
    out[n++] = 1;                     // nuh_temporal_id_plus1
    for (size_t i = 0; i < size; i++) {
        if (zeros == 2 && rbsp[i] <= 3) {
            if (n == cap)
                return 0;
            out[n++] = 3;
            zeros = 0;
        }
        if (n == cap)
            return 0;
        out[n++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    return n;
}

static size_t finish(H264Bits *b, uint8_t type, uint8_t *out, size_t cap)
{
    h264bits_trailing(b);
    const size_t n = h264bits_size(b);
    return n ? nal(type, b->data, n, out, cap) : 0;
}

// Main, compatible with Main 10 (general_profile_compatibility_flag[1] and [2]); progressive frames only.
static void profileTierLevel(H264Bits *b, const HEVCEncConfig *c)
{
    h264bits_u(b, 0, 2);  // general_profile_space
    h264bits_u(b, 0, 1);  // general_tier_flag: Main tier
    h264bits_u(b, PROFILE_MAIN, 5);
    h264bits_u(b, 0x60000000, 32);
    h264bits_u(b, 1, 1);   // general_progressive_source_flag
    h264bits_u(b, 0, 1);   // general_interlaced_source_flag
    h264bits_u(b, 0, 1);   // general_non_packed_constraint_flag
    h264bits_u(b, 1, 1);   // general_frame_only_constraint_flag
    h264bits_u(b, 0, 32);  // general_reserved_zero_43bits, then general_inbld_flag
    h264bits_u(b, 0, 12);
    h264bits_u(b, c->level_idc, 8);
}

size_t hevcenc_write_vps(const HEVCEncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t rbsp[64];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    h264bits_u(&b, 0, 4);        // vps_video_parameter_set_id
    h264bits_u(&b, 1, 1);        // vps_base_layer_internal_flag
    h264bits_u(&b, 1, 1);        // vps_base_layer_available_flag
    h264bits_u(&b, 0, 6);        // vps_max_layers_minus1
    h264bits_u(&b, 0, 3);        // vps_max_sub_layers_minus1
    h264bits_u(&b, 1, 1);        // vps_temporal_id_nesting_flag
    h264bits_u(&b, 0xffff, 16);  // vps_reserved_0xffff_16bits
    profileTierLevel(&b, c);
    h264bits_u(&b, 0, 1);  // vps_sub_layer_ordering_info_present_flag
    h264bits_ue(&b, 1);    // vps_max_dec_pic_buffering_minus1: the picture and its reference
    h264bits_ue(&b, 0);    // vps_max_num_reorder_pics
    h264bits_ue(&b, 0);    // vps_max_latency_increase_plus1
    h264bits_u(&b, 0, 6);  // vps_max_layer_id
    h264bits_ue(&b, 0);    // vps_num_layer_sets_minus1
    h264bits_u(&b, 0, 1);  // vps_timing_info_present_flag (the SPS's VUI carries it)
    h264bits_u(&b, 0, 1);  // vps_extension_flag
    return finish(&b, NAL_VPS, out, cap);
}

static void vui(H264Bits *b, const HEVCEncConfig *c)
{
    h264bits_u(b, 0, 1);  // aspect_ratio_info_present_flag (square samples)
    h264bits_u(b, 0, 1);  // overscan_info_present_flag
    h264bits_u(b, 1, 1);  // video_signal_type_present_flag
    h264bits_u(b, 5, 3);  // video_format: unspecified
    h264bits_u(b, c->full_range, 1);
    h264bits_u(b, c->colour_description, 1);
    if (c->colour_description) {
        h264bits_u(b, c->colour_primaries, 8);
        h264bits_u(b, c->transfer, 8);
        h264bits_u(b, c->matrix, 8);
    }
    h264bits_u(b, 0, 1);  // chroma_loc_info_present_flag
    h264bits_u(b, 0, 1);  // neutral_chroma_indication_flag
    h264bits_u(b, 0, 1);  // field_seq_flag
    h264bits_u(b, 0, 1);  // frame_field_info_present_flag
    h264bits_u(b, 0, 1);  // default_display_window_flag
    const bool timing = c->fps_num && c->fps_den;
    h264bits_u(b, timing, 1);
    if (timing) {
        h264bits_u(b, c->fps_den, 32);  // vui_num_units_in_tick
        h264bits_u(b, c->fps_num, 32);  // vui_time_scale: one tick a picture
        h264bits_u(b, 0, 1);            // vui_poc_proportional_to_timing_flag
        h264bits_u(b, 0, 1);            // vui_hrd_parameters_present_flag
    }
    h264bits_u(b, 1, 1);  // bitstream_restriction_flag
    h264bits_u(b, 0, 1);  // tiles_fixed_structure_flag
    h264bits_u(b, 1, 1);  // motion_vectors_over_pic_boundaries_flag
    h264bits_u(b, 1, 1);  // restricted_ref_pic_lists_flag
    h264bits_ue(b, 0);    // min_spatial_segmentation_idc
    h264bits_ue(b, 0);    // max_bytes_per_pic_denom
    h264bits_ue(b, 0);    // max_bits_per_min_cu_denom
    h264bits_ue(b, 15);   // log2_max_mv_length_horizontal
    h264bits_ue(b, 15);   // log2_max_mv_length_vertical
}

size_t hevcenc_write_sps(const HEVCEncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t rbsp[160];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    const uint32_t w = hevcenc_coded_width(c), h = hevcenc_coded_height(c);
    h264bits_u(&b, 0, 4);  // sps_video_parameter_set_id
    h264bits_u(&b, 0, 3);  // sps_max_sub_layers_minus1
    h264bits_u(&b, 1, 1);  // sps_temporal_id_nesting_flag
    profileTierLevel(&b, c);
    h264bits_ue(&b, 0);  // sps_seq_parameter_set_id
    h264bits_ue(&b, 1);  // chroma_format_idc: 4:2:0
    h264bits_ue(&b, w);
    h264bits_ue(&b, h);
    const uint32_t cropRight = (w - c->width) / 2, cropBottom = (h - c->height) / 2;  // in chroma samples
    h264bits_u(&b, cropRight || cropBottom, 1);                                       // conformance_window_flag
    if (cropRight || cropBottom) {
        h264bits_ue(&b, 0);
        h264bits_ue(&b, cropRight);
        h264bits_ue(&b, 0);
        h264bits_ue(&b, cropBottom);
    }
    h264bits_ue(&b, 0);  // bit_depth_luma_minus8
    h264bits_ue(&b, 0);  // bit_depth_chroma_minus8
    h264bits_ue(&b, HEVCENC_LOG2_MAX_POC_LSB - 4);
    h264bits_u(&b, 0, 1);  // sps_sub_layer_ordering_info_present_flag
    h264bits_ue(&b, 1);    // sps_max_dec_pic_buffering_minus1
    h264bits_ue(&b, 0);    // sps_max_num_reorder_pics
    h264bits_ue(&b, 0);    // sps_max_latency_increase_plus1
    h264bits_ue(&b, c->log2_min_cb - 3);
    h264bits_ue(&b, c->log2_ctb - c->log2_min_cb);
    h264bits_ue(&b, c->log2_min_tb - 2);
    h264bits_ue(&b, c->log2_max_tb - c->log2_min_tb);
    h264bits_ue(&b, c->max_tr_depth_inter);
    h264bits_ue(&b, c->max_tr_depth_intra);
    h264bits_u(&b, 0, 1);  // scaling_list_enabled_flag
    h264bits_u(&b, c->amp, 1);
    h264bits_u(&b, c->sao, 1);
    h264bits_u(&b, 0, 1);  // pcm_enabled_flag
    h264bits_ue(&b, 1);    // num_short_term_ref_pic_sets: st_ref_pic_set(0), the picture before
    h264bits_ue(&b, 1);    // num_negative_pics
    h264bits_ue(&b, 0);    // num_positive_pics
    h264bits_ue(&b, 0);    // delta_poc_s0_minus1
    h264bits_u(&b, 1, 1);  // used_by_curr_pic_s0_flag
    h264bits_u(&b, 0, 1);  // long_term_ref_pics_present_flag
    h264bits_u(&b, c->temporal_mvp, 1);
    h264bits_u(&b, c->strong_intra_smoothing, 1);
    h264bits_u(&b, 1, 1);  // vui_parameters_present_flag
    vui(&b, c);
    h264bits_u(&b, 0, 1);  // sps_extension_present_flag
    return finish(&b, NAL_SPS, out, cap);
}

size_t hevcenc_write_pps(const HEVCEncConfig *c, uint8_t *out, size_t cap)
{
    (void)c;
    uint8_t rbsp[32];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    h264bits_ue(&b, 0);    // pps_pic_parameter_set_id
    h264bits_ue(&b, 0);    // pps_seq_parameter_set_id
    h264bits_u(&b, 0, 1);  // dependent_slice_segments_enabled_flag
    h264bits_u(&b, 0, 1);  // output_flag_present_flag
    h264bits_u(&b, 0, 3);  // num_extra_slice_header_bits
    h264bits_u(&b, 0, 1);  // sign_data_hiding_enabled_flag
    h264bits_u(&b, 0, 1);  // cabac_init_present_flag
    h264bits_ue(&b, 0);    // num_ref_idx_l0_default_active_minus1
    h264bits_ue(&b, 0);    // num_ref_idx_l1_default_active_minus1
    h264bits_se(&b, 0);    // init_qp_minus26
    h264bits_u(&b, 0, 1);  // constrained_intra_pred_flag
    h264bits_u(&b, 1, 1);  // transform_skip_enabled_flag
    h264bits_u(&b, 0, 1);  // cu_qp_delta_enabled_flag: one QP a picture
    h264bits_se(&b, 0);    // pps_cb_qp_offset
    h264bits_se(&b, 0);    // pps_cr_qp_offset
    h264bits_u(&b, 0, 1);  // pps_slice_chroma_qp_offsets_present_flag
    h264bits_u(&b, 0, 1);  // weighted_pred_flag
    h264bits_u(&b, 0, 1);  // weighted_bipred_flag
    h264bits_u(&b, 0, 1);  // transquant_bypass_enabled_flag
    h264bits_u(&b, 0, 1);  // tiles_enabled_flag
    h264bits_u(&b, 0, 1);  // entropy_coding_sync_enabled_flag
    h264bits_u(&b, 1, 1);  // pps_loop_filter_across_slices_enabled_flag
    h264bits_u(&b, 0, 1);  // deblocking_filter_control_present_flag
    h264bits_u(&b, 0, 1);  // pps_scaling_list_data_present_flag
    h264bits_u(&b, 0, 1);  // lists_modification_present_flag
    h264bits_ue(&b, 0);    // log2_parallel_merge_level_minus2
    h264bits_u(&b, 0, 1);  // slice_segment_header_extension_present_flag
    h264bits_u(&b, 0, 1);  // pps_extension_present_flag
    return finish(&b, NAL_PPS, out, cap);
}

uint8_t hevcenc_nal_type(const HEVCEncPicture *p)
{
    return p->idr ? NAL_IDR_W_RADL : NAL_TRAIL_R;
}

void hevcenc_slice_header(const HEVCEncConfig *c, const HEVCEncPicture *p, H264Bits *b)
{
    h264bits_u(b, 1, 1);  // first_slice_segment_in_pic_flag
    if (p->idr)
        h264bits_u(b, 0, 1);  // no_output_of_prior_pics_flag
    h264bits_ue(b, 0);        // slice_pic_parameter_set_id
    h264bits_ue(b, p->idr ? SLICE_I : SLICE_P);
    if (!p->idr) {
        h264bits_u(b, p->poc & ((1u << HEVCENC_LOG2_MAX_POC_LSB) - 1), HEVCENC_LOG2_MAX_POC_LSB);
        h264bits_u(b, 1, 1);  // short_term_ref_pic_set_sps_flag: the SPS's only set, so no index
        if (c->temporal_mvp)
            h264bits_u(b, 1, 1);  // slice_temporal_mvp_enabled_flag
    }
    if (c->sao) {
        h264bits_u(b, 1, 1);  // slice_sao_luma_flag
        h264bits_u(b, 1, 1);  // slice_sao_chroma_flag
    }
    if (!p->idr) {
        h264bits_u(b, 0, 1);  // num_ref_idx_active_override_flag
        h264bits_ue(b, 0);    // five_minus_max_num_merge_cand
    }
    h264bits_se(b, (int32_t)p->qp - 26);  // slice_qp_delta
    h264bits_u(b, 1, 1);                  // slice_loop_filter_across_slices_enabled_flag (deblocking is on)
}

size_t hevcenc_write_hvcc(const HEVCEncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t sets[3][192];
    const size_t sizes[3] = {
        hevcenc_write_vps(c, sets[0], sizeof(sets[0])),
        hevcenc_write_sps(c, sets[1], sizeof(sets[1])),
        hevcenc_write_pps(c, sets[2], sizeof(sets[2])),
    };
    static const uint8_t kTypes[3] = {NAL_VPS, NAL_SPS, NAL_PPS};
    if (!sizes[0] || !sizes[1] || !sizes[2] || cap < 23 + 3 * 5 + sizes[0] + sizes[1] + sizes[2])
        return 0;
    size_t n = 0;
    out[n++] = 1;  // configurationVersion
    out[n++] = PROFILE_MAIN;
    out[n++] = 0x60, out[n++] = 0, out[n++] = 0, out[n++] = 0;  // general_profile_compatibility_flags
    out[n++] = 0x90;                                            // progressive, frame only
    memset(out + n, 0, 5), n += 5;
    out[n++] = c->level_idc;
    out[n++] = 0xf0, out[n++] = 0x00;  // min_spatial_segmentation_idc 0
    out[n++] = 0xfc;                   // parallelismType 0
    out[n++] = 0xfd;                   // chromaFormat 1
    out[n++] = 0xf8;                   // bitDepthLumaMinus8 0
    out[n++] = 0xf8;                   // bitDepthChromaMinus8 0
    out[n++] = 0, out[n++] = 0;        // avgFrameRate
    out[n++] = 0x0f;                   // constantFrameRate 0, one temporal layer, nested, 4-byte lengths
    out[n++] = 3;                      // numOfArrays
    for (int i = 0; i < 3; i++) {
        out[n++] = (uint8_t)(0x80 | kTypes[i]);  // array_completeness
        out[n++] = 0, out[n++] = 1;              // numNalus
        out[n++] = (uint8_t)(sizes[i] >> 8), out[n++] = (uint8_t)sizes[i];
        memcpy(out + n, sets[i], sizes[i]);
        n += sizes[i];
    }
    return n;
}
