// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264 parameter sets and slice headers, as far as NVDEC's picture setup and the reference bookkeeping need them
// (ITU-T H.264 (08/2021) 7.3.2.1.1 seq_parameter_set_data, 7.3.2.2 pic_parameter_set_rbsp, 7.3.3 slice_header,
// E.1.1 vui_parameters). Plain C with no Apple dependency, so the host tests build it too.
#ifndef H264_SYNTAX_H
#define H264_SYNTAX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    H264_NAL_SLICE = 1,
    H264_NAL_IDR = 5,
    H264_NAL_SEI = 6,
    H264_NAL_SPS = 7,
    H264_NAL_PPS = 8
};

enum {
    H264_SLICE_P = 0,
    H264_SLICE_B = 1,
    H264_SLICE_I = 2,
    H264_SLICE_SP = 3,
    H264_SLICE_SI = 4
};

typedef struct {
    bool valid;
    uint8_t profile_idc, constraint_flags, level_idc;
    uint8_t chroma_format_idc, bit_depth_luma, bit_depth_chroma;
    bool separate_colour_plane, transform_bypass, scaling_matrix_present;
    uint8_t scaling4[6][16], scaling8[2][64];  // raster order (7.4.2.1.1 with fall-back rule A applied)
    uint8_t log2_max_frame_num, poc_type, log2_max_poc_lsb;
    bool delta_pic_order_always_zero;
    int32_t offset_for_non_ref_pic, offset_for_top_to_bottom_field;
    uint8_t num_ref_frames_in_poc_cycle;
    int32_t offset_for_ref_frame[255];
    uint8_t max_num_ref_frames;
    bool gaps_in_frame_num_allowed;
    uint16_t width_mbs, height_map_units;
    uint16_t height_mbs;  // FrameHeightInMbs (7-18): (2 - frame_mbs_only_flag) * PicHeightInMapUnits
    bool frame_mbs_only, mb_adaptive_frame_field, direct_8x8_inference;
    uint32_t crop_left, crop_right, crop_top, crop_bottom;  // in luma samples
    // VUI
    bool full_range, colour_description, chroma_location;
    uint32_t chroma_top, chroma_bottom;
    uint8_t colour_primaries, transfer, matrix;
    bool bitstream_restriction;
    uint8_t max_num_reorder_frames, max_dec_frame_buffering;
} H264SPS;

typedef struct {
    bool valid;
    uint8_t sps_id;
    bool entropy_coding_mode, bottom_field_pic_order_present;
    uint8_t num_slice_groups;
    uint8_t num_ref_idx_active[2];
    bool weighted_pred;
    uint8_t weighted_bipred_idc;
    int8_t pic_init_qp, chroma_qp_index_offset, second_chroma_qp_index_offset;
    bool deblocking_filter_control_present, constrained_intra_pred, redundant_pic_cnt_present;
    bool transform_8x8_mode;
    uint8_t scaling4[6][16], scaling8[2][64];  // after fall-back rule B against its SPS
} H264PPS;

typedef struct {
    uint8_t type;  // MMCO 1..6
    uint32_t difference_of_pic_nums_minus1, long_term_pic_num, long_term_frame_idx, max_long_term_frame_idx_plus1;
} H264MMCO;

typedef struct {
    uint8_t nal_unit_type, nal_ref_idc;
    uint32_t first_mb_in_slice;
    uint8_t slice_type, pps_id;
    uint32_t frame_num;
    bool field_pic, bottom_field;
    uint32_t idr_pic_id;
    uint32_t poc_lsb;
    int32_t delta_poc_bottom, delta_poc[2];
    // dec_ref_pic_marking
    bool no_output_of_prior_pics, long_term_reference, adaptive_ref_pic_marking;
    uint8_t mmco_count;
    H264MMCO mmco[32];
} H264Slice;

typedef struct {
    H264SPS sps[32];
    H264PPS pps[256];
} H264ParamSets;

// Each takes one NAL unit including its one-byte header, with emulation prevention bytes still in place.
// Return 0 on success, -1 on malformed or unsupported syntax.
int h264_parse_sps(H264ParamSets *sets, const uint8_t *nal, size_t size, uint8_t *idOut);
int h264_parse_pps(H264ParamSets *sets, const uint8_t *nal, size_t size, uint8_t *idOut);
int h264_parse_slice(const H264ParamSets *sets, const uint8_t *nal, size_t size, H264Slice *out);
// 7.4.1.2.4: whether `next` starts another primary coded picture than `previous`'s (or restarts at macroblock 0).
bool h264_slice_new_picture(const H264Slice *previous, const H264Slice *next);

#endif
