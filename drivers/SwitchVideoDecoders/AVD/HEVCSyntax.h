// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVC (ITU-T H.265) syntax a VLD engine leaves to its driver: the SPS and PPS fields NVDEC's picture setup takes, the
// short-term reference picture sets (7.3.7 / 7.4.8, inter-set prediction included), scaling lists (7.3.4, Table 7-6
// defaults) and the start of each slice segment header up to the reference picture syntax, whose length is NVDEC's
// sw_hdr_skip_length. Written from the specification; FFmpeg's in-memory scaling list layout (raster order within
// each 4x4 / 8x8 matrix, which averne's nvtegra_hevc.c hands NVDEC as is) is matched by construction and checked by
// the host test. Profiles taken: Main, Main 10 and Main Still Picture (4:2:0, 8 or 10 bits).
#ifndef HEVC_SYNTAX_H
#define HEVC_SYNTAX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HEVC_NAL_TRAIL_N = 0,
    HEVC_NAL_RADL_N = 6,
    HEVC_NAL_RADL_R = 7,
    HEVC_NAL_RASL_N = 8,
    HEVC_NAL_RASL_R = 9,
    HEVC_NAL_BLA_W_LP = 16,
    HEVC_NAL_IDR_W_RADL = 19,
    HEVC_NAL_IDR_N_LP = 20,
    HEVC_NAL_CRA = 21,
    HEVC_NAL_VPS = 32,
    HEVC_NAL_SPS = 33,
    HEVC_NAL_PPS = 34,
    HEVC_NAL_EOS = 36,
    HEVC_NAL_EOB = 37,
};

enum {
    HEVC_SLICE_B = 0,
    HEVC_SLICE_P = 1,
    HEVC_SLICE_I = 2
};

typedef struct {
    uint8_t num_negative, num_positive;  // NumNegativePics, NumPositivePics (at most 16 together)
    int16_t delta[2][16];                // DeltaPocS0 (negative, nearest first), DeltaPocS1
    uint8_t used[2][16];                 // UsedByCurrPicS0 / S1
} HEVCShortTermRPS;

// FFmpeg's layout: sl[sizeId][matrixId] in raster order (16 entries for 4x4, 64 for the 8x8 base of the larger
// sizes), dc[sizeId - 2][matrixId] the 16x16 and 32x32 DC values.
typedef struct {
    uint8_t sl[4][6][64];
    uint8_t dc[2][6];
} HEVCScalingList;

typedef struct {
    bool valid;
    uint8_t profile_idc, level_idc;
    uint8_t chroma_format_idc, bit_depth, bit_depth_chroma;
    uint16_t width, height;                                 // pic_width/height_in_luma_samples (coded)
    uint16_t crop_left, crop_right, crop_top, crop_bottom;  // conformance window, in luma samples
    uint8_t log2_max_poc_lsb;
    uint8_t max_dec_pic_buffering, max_num_reorder;  // HighestTid's
    uint32_t max_latency_increase_plus1;
    uint8_t log2_min_cb, log2_diff_max_min_cb, log2_min_tb, log2_diff_max_min_tb;
    uint8_t max_th_depth_inter, max_th_depth_intra;
    bool scaling_list_enabled;
    HEVCScalingList scaling;  // the SPS's (Table 7-6 defaults when it carries none)
    bool amp, sao, pcm, pcm_loop_filter_disabled;
    uint8_t pcm_bit_depth, pcm_bit_depth_chroma, log2_min_pcm, log2_diff_max_min_pcm;
    uint8_t num_short_term_rps;
    HEVCShortTermRPS st_rps[64];
    bool long_term_present;
    uint8_t num_long_term_sps;
    uint16_t lt_poc_lsb_sps[32];
    uint8_t lt_used_sps[32];
    bool temporal_mvp, strong_intra_smoothing;
    bool full_range, colour_description, chroma_location;
    uint32_t chroma_top, chroma_bottom;
    uint8_t colour_primaries, transfer, matrix;
} HEVCSPS;

typedef struct {
    bool valid;
    uint8_t sps_id;
    bool dependent_slice_segments, output_flag_present, sign_data_hiding, cabac_init_present;
    uint8_t num_extra_slice_header_bits, num_ref_idx_l0_default, num_ref_idx_l1_default;
    int8_t init_qp_minus26, cb_qp_offset, cr_qp_offset, beta_offset, tc_offset;  // beta/tc: the _div2 values x 2
    bool constrained_intra_pred, transform_skip, cu_qp_delta, slice_chroma_qp_offsets_present;
    uint8_t diff_cu_qp_delta_depth;
    bool weighted_pred, weighted_bipred, transquant_bypass, tiles, entropy_coding_sync;
    uint8_t num_tile_columns, num_tile_rows;  // 1 when tiles are off
    bool uniform_spacing;
    uint16_t column_width[20], row_height[22];  // in CTBs (resolved against the SPS when it is parsed)
    bool loop_filter_across_tiles, loop_filter_across_slices, deblocking_control, deblocking_override,
        deblocking_disabled;
    bool scaling_list_present;
    HEVCScalingList scaling;
    bool lists_modification_present, slice_header_extension_present;
    uint8_t log2_parallel_merge_level;
    bool range_extension;  // a Main-profile PPS has none; set means the stream is not one this decoder takes
} HEVCPPS;

typedef struct {
    HEVCSPS sps[16];
    HEVCPPS pps[64];
} HEVCParams;

typedef struct {
    uint8_t nal_type, temporal_id;
    bool first_slice, no_output_of_prior_pics, dependent;
    uint8_t pps_id, slice_type;
    bool pic_output;
    uint16_t poc_lsb;
    bool st_rps_from_sps;
    HEVCShortTermRPS st_rps;  // the picture's: the SPS's set it names, or its own
    uint8_t num_lt;           // num_long_term_sps + num_long_term_pics

    struct {
        uint16_t poc_lsb;
        bool used, msb_present;
        uint32_t msb_cycle;  // DeltaPocMsbCycleLt (accumulated)
    } lt[32];

    uint16_t skip_bits;    // sw_hdr_skip_length: from the end of slice_type to the end of the reference picture syntax
    uint16_t st_rps_bits;  // the slice's own st_ref_pic_set() (0 when the SPS's is used)
} HEVCSliceHeader;

// Emulation prevention removed: at most `cap` bytes of RBSP from the NAL unit (header included). Returns the length.
size_t hevc_unescape(const uint8_t *nal, size_t size, uint8_t *out, size_t cap);
// 0, or -1 for a malformed or unsupported unit (the SPS's profile, chroma format or bit depth not Main/Main10/MSP).
int hevc_parse_sps(HEVCParams *params, const uint8_t *nal, size_t size);
int hevc_parse_pps(HEVCParams *params, const uint8_t *nal, size_t size);
// The slice segment header up to slice_temporal_mvp_enabled_flag. 0, or -1.
int hevc_parse_slice_header(const HEVCParams *params, const uint8_t *nal, size_t size, HEVCSliceHeader *out);
// The scaling lists a picture uses: the PPS's, else the SPS's.
const HEVCScalingList *hevc_scaling_list(const HEVCSPS *sps, const HEVCPPS *pps);
void hevc_default_scaling_list(HEVCScalingList *sl);

#endif
