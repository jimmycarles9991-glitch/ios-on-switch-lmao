// SPDX-License-Identifier: BSD-2-Clause-Patent
// See H264NVENC.h. Syntax: ITU-T H.264 (08/2021) 7.3.2.1.1 seq_parameter_set_data, E.1.1 vui_parameters, 7.3.2.2
// pic_parameter_set_rbsp, 7.3.3 slice_header, 7.4.1 emulation prevention, Table A-1 level limits; ISO/IEC 14496-15
// 5.3.3.1 AVCDecoderConfigurationRecord. Setup: NVIDIA's nvenc_drv.h and clc5b7.h (MIT, drivers/TegraNVENC/nvidia).
#include "H264NVENC.h"
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "../../TegraNVENC/nvidia/clc5b7.h"
#include "../../TegraNVENC/TegraNVENCCheck.h"

#define ALIGN(v, a) (((v) + (a) - 1) & ~((uint32_t)(a) - 1))

enum {
    LOG2_MAX_FRAME_NUM = 16,
    LOG2_MAX_POC_LSB = 16,
    SLICE_OFF = 512,
    ME_OFF = 640,
    MD_OFF = 832,
    Q_OFF = 960
};

enum {
    SETUP_BYTES = Q_OFF + 192
};

void h264bits_init(H264Bits *b, uint8_t *data, size_t cap)
{
    memset(b, 0, sizeof(*b));
    b->data = data;
    b->cap = cap;
}

void h264bits_u(H264Bits *b, uint32_t value, unsigned n)
{
    for (unsigned i = n; i-- > 0;) {
        b->acc = (b->acc << 1) | ((value >> i) & 1);
        if (++b->count == 8) {
            if (b->bytes < b->cap)
                b->data[b->bytes] = (uint8_t)b->acc;
            else
                b->overflow = true;
            b->bytes++;
            b->acc = 0;
            b->count = 0;
        }
    }
}

void h264bits_ue(H264Bits *b, uint32_t value)
{
    const uint64_t v = (uint64_t)value + 1;
    unsigned bits = 0;
    while ((v >> bits) > 1)
        bits++;
    h264bits_u(b, 0, bits);
    h264bits_u(b, (uint32_t)v, bits + 1);
}

void h264bits_se(H264Bits *b, int32_t value)
{
    h264bits_ue(b, value > 0 ? (uint32_t)(2 * value - 1) : (uint32_t)(-2 * (int64_t)value));
}

bool h264bits_aligned(const H264Bits *b)
{
    return b->count == 0;
}

void h264bits_align_zero(H264Bits *b)
{
    while (b->count)
        h264bits_u(b, 0, 1);
}

void h264bits_trailing(H264Bits *b)
{
    h264bits_u(b, 1, 1);
    h264bits_align_zero(b);
}

size_t h264bits_size(const H264Bits *b)
{
    return b->overflow ? 0 : b->bytes;
}

size_t h264enc_nal(uint8_t header, const uint8_t *rbsp, size_t size, uint8_t *out, size_t cap)
{
    size_t n = 0;
    unsigned zeros = 0;
    if (cap < 1)
        return 0;
    out[n++] = header;
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

uint32_t h264enc_width_mbs(const H264EncConfig *c)
{
    return (c->width + 15) / 16;
}

uint32_t h264enc_height_mbs(const H264EncConfig *c)
{
    return (c->height + 15) / 16;
}

int h264enc_configure(H264EncConfig *c)
{
    if (c->width < TEGRA_NVENC_MIN_DIMENSION || c->height < TEGRA_NVENC_MIN_DIMENSION ||
        c->width > TEGRA_NVENC_MAX_DIMENSION || c->height > TEGRA_NVENC_MAX_DIMENSION || (c->width & 1) ||
        (c->height & 1))
        return -1;
    const uint32_t mbs = h264enc_width_mbs(c) * h264enc_height_mbs(c);
    if (mbs > TEGRA_NVENC_MAX_MBS)
        return -1;
    if (c->profile_idc != H264_PROFILE_BASELINE && c->profile_idc != H264_PROFILE_MAIN &&
        c->profile_idc != H264_PROFILE_HIGH)
        return -1;
    if (c->profile_idc == H264_PROFILE_BASELINE)
        c->cabac = c->reorder = false;
    if (c->profile_idc != H264_PROFILE_HIGH)
        c->transform8x8 = false;
    if (c->input_layout >= TEGRA_NVENC_LAYOUTS || c->chroma_mode > TEGRA_NVENC_CHROMA_METHOD)
        return -1;
    if (!c->level_idc) {
        static const struct {
            uint8_t level;
            uint32_t maxFs, maxMbps;
        } kLevels[] = {
            {20, 396, 11880},    {21, 792, 19800},    {22, 1620, 20250},    {30, 1620, 40500},
            {31, 3600, 108000},  {32, 5120, 216000},  {40, 8192, 245760},   {42, 8704, 522240},
            {50, 22080, 589824}, {51, 36864, 983040}, {52, 36864, 2073600},
        };

        const uint64_t fps = c->fps_num && c->fps_den ? ((uint64_t)c->fps_num + c->fps_den - 1) / c->fps_den : 60;
        const uint32_t wmbs = h264enc_width_mbs(c), hmbs = h264enc_height_mbs(c);
        c->level_idc = 52;
        for (unsigned i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
            // A.3.1: frame size, macroblock rate, and neither side longer than sqrt(8 * MaxFS).
            const uint32_t side = (uint32_t)(8u * kLevels[i].maxFs);
            if (mbs <= kLevels[i].maxFs && (uint64_t)mbs * fps <= kLevels[i].maxMbps && wmbs * wmbs <= side &&
                hmbs * hmbs <= side) {
                c->level_idc = kLevels[i].level;
                break;
            }
        }
    }
    return 0;
}

void h264enc_surface(uint32_t layout, uint32_t width, uint32_t height, H264EncSurface *s)
{
    const uint32_t blockRows =
        tegra_nvenc_block_rows(layout == TEGRA_NVENC_LAYOUT_TILED16, tegra_nvenc_layout_gobs(layout));
    s->pitch = ALIGN(width, 64);
    s->rows = tegra_nvenc_luma_rows(height, blockRows);
    s->chroma_offset = s->pitch * s->rows;
    s->bytes = s->chroma_offset + s->pitch * tegra_nvenc_chroma_rows(height, blockRows);
}

void h264enc_layout(const H264EncConfig *c, H264EncLayout *l)
{
    memset(l, 0, sizeof(*l));
    const uint32_t wmbs = h264enc_width_mbs(c), hmbs = h264enc_height_mbs(c), mbs = wmbs * hmbs;
    h264enc_surface(c->input_layout, wmbs * 16, hmbs * 16, &l->input);
    h264enc_surface(TEGRA_NVENC_LAYOUT_TILED16, wmbs * 16, hmbs * 16, &l->recon);
    // At most 3200 bits per macroblock (A.3.1 item j, the I_PCM bound) plus headers: NVENC's output never outgrows it.
    l->bitstream_bytes = ALIGN(mbs * 400 + 4096, 65536);
    l->status_bytes = 4096;
    l->history_bytes = ALIGN(tegra_nvenc_history_bytes(c->width, c->height), 256);
    l->history_offset = l->status_bytes;
    l->aux_bytes = l->history_offset + l->history_bytes;
    l->mepred_bytes = ALIGN(mbs * TEGRA_NVENC_MEPRED_BYTES_PER_MB, 65536);
    l->coloc_bytes = ALIGN(mbs * (uint32_t)sizeof(nvenc_h264_coloc_mb_s), 256);
}

static size_t finish(H264Bits *b, uint8_t header, uint8_t *out, size_t cap)
{
    h264bits_trailing(b);
    const size_t n = h264bits_size(b);
    return n ? h264enc_nal(header, b->data, n, out, cap) : 0;
}

size_t h264enc_write_sps(const H264EncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t rbsp[128];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    const uint32_t wmbs = h264enc_width_mbs(c), hmbs = h264enc_height_mbs(c);
    h264bits_u(&b, c->profile_idc, 8);
    // constraint_set1 for Baseline: constrained baseline (no FMO/ASO/redundant slices), which this stream is.
    h264bits_u(&b, c->profile_idc == H264_PROFILE_BASELINE ? 0x40 : 0, 8);
    h264bits_u(&b, c->level_idc, 8);
    h264bits_ue(&b, 0);  // seq_parameter_set_id
    if (c->profile_idc == H264_PROFILE_HIGH) {
        h264bits_ue(&b, 1);    // chroma_format_idc 4:2:0
        h264bits_ue(&b, 0);    // bit_depth_luma_minus8
        h264bits_ue(&b, 0);    // bit_depth_chroma_minus8
        h264bits_u(&b, 0, 1);  // qpprime_y_zero_transform_bypass_flag
        h264bits_u(&b, 0, 1);  // seq_scaling_matrix_present_flag
    }
    h264bits_ue(&b, LOG2_MAX_FRAME_NUM - 4);
    h264bits_ue(&b, 0);  // pic_order_cnt_type
    h264bits_ue(&b, LOG2_MAX_POC_LSB - 4);
    h264bits_ue(&b, c->reorder ? 2 : 1);  // max_num_ref_frames: the two anchors a B picture reads
    h264bits_u(&b, 0, 1);                 // gaps_in_frame_num_value_allowed_flag
    h264bits_ue(&b, wmbs - 1);
    h264bits_ue(&b, hmbs - 1);
    h264bits_u(&b, 1, 1);  // frame_mbs_only_flag
    h264bits_u(&b, 1, 1);  // direct_8x8_inference_flag
    const uint32_t cropRight = (wmbs * 16 - c->width) / 2, cropBottom = (hmbs * 16 - c->height) / 2;
    h264bits_u(&b, cropRight || cropBottom, 1);
    if (cropRight || cropBottom) {
        h264bits_ue(&b, 0);
        h264bits_ue(&b, cropRight);
        h264bits_ue(&b, 0);
        h264bits_ue(&b, cropBottom);
    }
    h264bits_u(&b, 1, 1);  // vui_parameters_present_flag
    h264bits_u(&b, 0, 1);  // aspect_ratio_info_present_flag (square samples)
    h264bits_u(&b, 0, 1);  // overscan_info_present_flag
    h264bits_u(&b, 1, 1);  // video_signal_type_present_flag
    h264bits_u(&b, 5, 3);  // video_format: unspecified
    h264bits_u(&b, c->full_range, 1);
    h264bits_u(&b, c->colour_description, 1);
    if (c->colour_description) {
        h264bits_u(&b, c->colour_primaries, 8);
        h264bits_u(&b, c->transfer, 8);
        h264bits_u(&b, c->matrix, 8);
    }
    h264bits_u(&b, 0, 1);  // chroma_loc_info_present_flag
    const bool timing = c->fps_num && c->fps_den;
    h264bits_u(&b, timing, 1);
    if (timing) {
        h264bits_u(&b, c->fps_den, 32);      // num_units_in_tick
        h264bits_u(&b, 2 * c->fps_num, 32);  // time_scale (two ticks a frame)
        h264bits_u(&b, 0, 1);                // fixed_frame_rate_flag
    }
    h264bits_u(&b, 0, 1);  // nal_hrd_parameters_present_flag
    h264bits_u(&b, 0, 1);  // vcl_hrd_parameters_present_flag
    h264bits_u(&b, 0, 1);  // pic_struct_present_flag
    // bitstream_restriction_flag: decoders hold no picture without reordering, one with it (an anchor waits for the B
    // pictures before it).
    h264bits_u(&b, 1, 1);
    h264bits_u(&b, 1, 1);                 // motion_vectors_over_pic_boundaries_flag
    h264bits_ue(&b, 0);                   // max_bytes_per_pic_denom
    h264bits_ue(&b, 0);                   // max_bits_per_mb_denom
    h264bits_ue(&b, 15);                  // log2_max_mv_length_horizontal
    h264bits_ue(&b, 15);                  // log2_max_mv_length_vertical
    h264bits_ue(&b, c->reorder ? 1 : 0);  // max_num_reorder_frames
    h264bits_ue(&b, c->reorder ? 2 : 1);  // max_dec_frame_buffering
    return finish(&b, 0x67, out, cap);    // nal_ref_idc 3, SPS
}

size_t h264enc_write_pps(const H264EncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t rbsp[32];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    h264bits_ue(&b, 0);  // pic_parameter_set_id
    h264bits_ue(&b, 0);  // seq_parameter_set_id
    h264bits_u(&b, c->cabac, 1);
    h264bits_u(&b, 0, 1);  // bottom_field_pic_order_in_frame_present_flag
    h264bits_ue(&b, 0);    // num_slice_groups_minus1
    h264bits_ue(&b, 0);    // num_ref_idx_l0_default_active_minus1
    h264bits_ue(&b, 0);    // num_ref_idx_l1_default_active_minus1
    h264bits_u(&b, 0, 1);  // weighted_pred_flag
    h264bits_u(&b, 0, 2);  // weighted_bipred_idc
    h264bits_se(&b, 0);    // pic_init_qp_minus26
    h264bits_se(&b, 0);    // pic_init_qs_minus26
    h264bits_se(&b, 0);    // chroma_qp_index_offset
    h264bits_u(&b, 1, 1);  // deblocking_filter_control_present_flag
    h264bits_u(&b, 0, 1);  // constrained_intra_pred_flag
    h264bits_u(&b, 0, 1);  // redundant_pic_cnt_present_flag
    if (c->profile_idc == H264_PROFILE_HIGH) {
        h264bits_u(&b, c->transform8x8, 1);
        h264bits_u(&b, 0, 1);  // pic_scaling_matrix_present_flag
        h264bits_se(&b, 0);    // second_chroma_qp_index_offset
    }
    return finish(&b, 0x68, out, cap);  // nal_ref_idc 3, PPS
}

size_t h264enc_write_avcc(const H264EncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t sps[160], pps[48];
    const size_t spsSize = h264enc_write_sps(c, sps, sizeof(sps)), ppsSize = h264enc_write_pps(c, pps, sizeof(pps));
    const size_t n = 6 + 2 + spsSize + 1 + 2 + ppsSize + (c->profile_idc == H264_PROFILE_HIGH ? 4 : 0);
    if (!spsSize || !ppsSize || n > cap)
        return 0;
    size_t i = 0;
    out[i++] = 1;         // configurationVersion
    out[i++] = sps[1];    // AVCProfileIndication
    out[i++] = sps[2];    // profile_compatibility
    out[i++] = sps[3];    // AVCLevelIndication
    out[i++] = 0xfc | 3;  // lengthSizeMinusOne = 3
    out[i++] = 0xe0 | 1;  // one SPS
    out[i++] = (uint8_t)(spsSize >> 8);
    out[i++] = (uint8_t)spsSize;
    memcpy(out + i, sps, spsSize);
    i += spsSize;
    out[i++] = 1;  // one PPS
    out[i++] = (uint8_t)(ppsSize >> 8);
    out[i++] = (uint8_t)ppsSize;
    memcpy(out + i, pps, ppsSize);
    i += ppsSize;
    if (c->profile_idc == H264_PROFILE_HIGH) {  // 5.3.3.1.2: chroma format and bit depths, no SPS extensions
        out[i++] = 0xfc | 1;
        out[i++] = 0xf8 | 0;
        out[i++] = 0xf8 | 0;
        out[i++] = 0;
    }
    return i;
}

void h264enc_slice_header(const H264EncConfig *c, const H264EncPicture *p, H264Bits *b)
{
    const bool idr = p->type == H264ENC_IDR, intra = idr || p->type == H264ENC_I, bPicture = p->type == H264ENC_B;
    h264bits_ue(b, 0);                             // first_mb_in_slice
    h264bits_ue(b, intra ? 7 : bPicture ? 6 : 5);  // slice_type: every slice of the picture I (7), B (6) or P (5)
    h264bits_ue(b, 0);                             // pic_parameter_set_id
    h264bits_u(b, p->frame_num & 0xffff, LOG2_MAX_FRAME_NUM);
    if (idr)
        h264bits_ue(b, p->idr_pic_id);
    h264bits_u(b, p->poc_lsb & 0xffff, LOG2_MAX_POC_LSB);
    if (bPicture)
        h264bits_u(b, 1, 1);  // direct_spatial_mv_pred_flag (the MD control's bdirect_mode 1)
    if (!intra) {
        h264bits_u(b, 0, 1);  // num_ref_idx_active_override_flag
        h264bits_u(b, 0, 1);  // ref_pic_list_modification_flag_l0
    }
    if (bPicture)
        h264bits_u(b, 0, 1);  // ref_pic_list_modification_flag_l1
    if (idr) {
        h264bits_u(b, 0, 1);  // no_output_of_prior_pics_flag
        h264bits_u(b, 0, 1);  // long_term_reference_flag
    } else if (!bPicture && !p->unreferenced) {
        h264bits_u(b, 0, 1);  // adaptive_ref_pic_marking_mode_flag: sliding window
    }
    if (c->cabac && !intra)
        h264bits_ue(b, 0);                // cabac_init_idc
    h264bits_se(b, (int32_t)p->qp - 26);  // slice_qp_delta
    h264bits_ue(b, 0);                    // disable_deblocking_filter_idc: on
    h264bits_se(b, 0);                    // slice_alpha_c0_offset_div2
    h264bits_se(b, 0);                    // slice_beta_offset_div2
}

// Every surface is the coded size, as NVIDIA's T210 driver gives it (1920x1088 for 1080p; the SPS crops). NVENC 5.0's
// H.264 app sizes the input fetch from input_cfg rounding down (app 0x85d6-0x861b, I/O 0xe88) and the macroblock
// pipeline from outputpic_cfg rounding up (0x818c, I/O 0x1808 and 0x191c): a height of 1080 fetches 67 macroblock rows
// for a picture of 68, and the engine stalls on the missing row (TegraNVENC/nvidia/SOURCE).
static void surfaceConfig(const H264EncConfig *c, const H264EncSurface *s, nvenc_h264_surface_cfg_s *cfg, bool input)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->frame_width_minus1 = (unsigned short)(h264enc_width_mbs(c) * 16 - 1);
    cfg->frame_height_minus1 = (unsigned short)(h264enc_height_mbs(c) * 16 - 1);
    cfg->sfc_pitch = cfg->sfc_pitch_chroma = (unsigned short)s->pitch;
    cfg->chroma_top_frm_offset = c->chroma_mode == TEGRA_NVENC_CHROMA_OFFSET ? s->chroma_offset / 256 : 0;
    if (!input) {
        cfg->tiled_16x16 = 1;  // references are always 16x16 tiled (nvenc_drv.h 1.5)
        return;
    }
    cfg->tiled_16x16 = c->input_layout == TEGRA_NVENC_LAYOUT_TILED16;
    cfg->block_height = tegra_nvenc_layout_gobs(c->input_layout);
}

// The slice, motion search, mode decision and quantisation controls: the values NVIDIA's own T210 driver gives NVENC
// 5.0 (its H.264 setups for 1280x720 High profile CABAC, IDR and P, captured on a Switch and published by
// github.com/TomasUribe/switch-frame-tap, logs/m79-runE-setups/setup_3.bin (IDR) and setup_5.bin (P); decoded with this
// tree's nvenc_drv.h, see TegraNVENC/nvidia/SOURCE). The driver's ME, MD and quant controls are the same for IDR and P
// pictures except intra 4x4, which its P pictures leave off. NVENC 5.0's H.264 app reads the MD control up to byte 0x3a
// only (priority_ipred_type_ip1; intra_ssd_cnt_*, rdo_level and on are later NVENCs'), and the ME control up to the
// mbc_mb_size word (0x90); the rest is filled as the driver fills it anyway. Nothing here makes NVENC write outside the
// picture, bitstream, history, colocated and status buffers the checker bounds.
static void controls(const H264EncConfig *c, const H264EncPicture *p, uint8_t *setup)
{
    const uint32_t mbs = h264enc_width_mbs(c) * h264enc_height_mbs(c);
    const bool intra = p->type == H264ENC_IDR || p->type == H264ENC_I, bPicture = p->type == H264ENC_B;
    // One slice: the one control, under slice_mode 0, covers every macroblock (h264enc_fill_submit).
    nvenc_h264_slice_control_s *slice = (nvenc_h264_slice_control_s *)(setup + SLICE_OFF);
    slice->num_mb = mbs;
    slice->qp_avr = p->qp;
    slice->qp_slice_min = 1;  // the driver's; RateControl.h never asks below 8 (QP 10..48, key frames 2 finer)
    slice->qp_slice_max = 51;
    slice->disable_deblocking_filter_idc = 0;
    slice->cabac_init_idc = 0;
    slice->force_intra = 0;  // the driver's: the picture type alone makes an IDR intra

    // Motion search: spatial and constant (zero) vector hints, integer search over the driver's five stamps then
    // quarter-pel refinement, vectors limited to H.264's range (-2048..2047.75 x -512..511.75); the ME's reference
    // cache (MBC) 110 macroblocks. NVENC 5.0 writes mbc_mb_size straight to the MBC (low 8 bits to I/O 0x1010, app
    // 0x842a; the picture size in macroblocks goes to I/O 0x100c, app 0x82ec). 110 is the driver's at 1280x720, the
    // only size captured; NVIDIA's desktop drivers send 0 to NVENC 6 and later, which do not read it
    // (github.com/mickys/nvenc src/h264enctmpl.h).
    nvenc_h264_me_control_s *me = (nvenc_h264_me_control_s *)(setup + ME_OFF);
    me->refinement_mode = 1;  // quarter pel
    me->lambda_mode = 1;      // constant lambda,
    me->const_lambda = 4;     // U8.3: 0.5
    me->refine_on_search_enable = 1;
    me->fps_mvcost = 1;
    me->sps_mvcost = 1;
    me->sps_cost_func = 1;
    me->limit_mv.mv_limit_enable = 1;
    me->limit_mv.left_mvx_int = -2048;
    me->limit_mv.right_mvx_int = 2047;
    me->limit_mv.right_mvx_frac = 3;
    me->limit_mv.top_mvy_int = -512;
    me->limit_mv.bottom_mvy_int = 511;
    me->limit_mv.bottom_mvy_frac = 3;
    me->predsrc.self_spatial_stamp_l0 = me->predsrc.self_spatial_stamp_l1 = 1;
    me->predsrc.self_spatial_search = 1;
    me->predsrc.self_spatial_enable = 1;
    me->predsrc.const_mv_stamp_l0 = me->predsrc.const_mv_stamp_l1 = bPicture ? 3 : 2;
    me->predsrc.const_mv_search = 1;
    me->predsrc.const_mv_enable = 1;
    me->predsrc.coloc_enable = bPicture;  // the list-1 reference's colocated vectors as a hint
    static const uint32_t kShapes[5][2] = {
        {0x0c0c0000u, 0x00000000u}, {0x1c1c0000u, 0x0000001cu}, {0x3e3e0000u, 0x0000003eu},
        {0x7f3e0800u, 0x0000083eu}, {0x7f1c1c89u, 0x81081c1cu},
    };
    nvenc_h264_me_stamp_s *shape = &me->shape0;
    for (int i = 0; i < 5; i++) {
        shape[i].bitmask[0] = kShapes[i][0];
        shape[i].bitmask[1] = kShapes[i][1];
    }
    shape[0].hor_adjust = shape[0].ver_adjust = 1;
    me->mbc_mb_size = 110;
    me->partDecisionMadeByFPP = 1;
    me->spatial_hint_pattern = 0xf;
    me->temporal_hint_pattern = 0x3f;
    me->fbm_op_winner_num_p_frame = me->fbm_op_winner_num_b_frame_l0 = me->fbm_op_winner_num_b_frame_l1 = 1;
    me->fbm_select_best_cu32_parttype_num = 3;
    me->sps_evaluate_merge_cand_num = 5;
    me->external_hint_order = me->coloc_hint_order = 1;
    me->hint_type0 = 0;  // nvenc_drv.h: the five must cover 0x1f
    me->hint_type1 = 1;
    me->hint_type2 = 2;
    me->hint_type3 = 3;
    me->hint_type4 = 4;

    // Mode decision: every intra mode, the 4x4/8x8/16x16 search with early termination (8x8 first with the 8x8
    // transform, nvenc_drv.h: priority_ipred_type_ip1 must be within ip_search_mode), early intra on 16x16 DC/H/V, all
    // list-0 partitions and P skip, the driver's intra biases. The driver also enables the list-1 and bi-predicted
    // partitions in IDR and P pictures, where they select nothing (no list 1); copied, so the block is the driver's.
    nvenc_h264_md_control_s *md = (nvenc_h264_md_control_s *)(setup + MD_OFF);
    md->intra_luma4x4_mode_enable = intra || !c->transform8x8 ? 0x1ff : 0;  // the driver's P pictures: 8x8, 16x16
    md->intra_luma8x8_mode_enable = c->transform8x8 ? 0x1ff : 0;
    md->intra_luma16x16_mode_enable = 0xf;
    md->intra_chroma_mode_enable = 0xf;
    md->l0_part_16x16_enable = md->l0_part_16x8_enable = md->l0_part_8x16_enable = md->l0_part_8x8_enable = 1;
    md->l0_part_8x4_enable = md->l0_part_4x8_enable = md->l0_part_4x4_enable = 1;
    md->l1_part_16x16_enable = md->l1_part_16x8_enable = md->l1_part_8x16_enable = md->l1_part_8x8_enable = 1;
    md->bi_part_16x16_enable = md->bi_part_16x8_enable = md->bi_part_8x16_enable = md->bi_part_8x8_enable = 1;
    md->pskip_enable = 1;
    md->bdirect_mode = bPicture ? 1 : 0;  // spatial, NVIDIA's default where B pictures are on
    md->intra_nxn_bias_multiplier = 24;
    md->intra_most_prob_bias_multiplier = 4;
    md->mv_cost_enable = 1;
    md->early_intra_mode_control = 1;
    md->early_intra_mode_type_16x16dc = md->early_intra_mode_type_16x16h = md->early_intra_mode_type_16x16v = 1;
    md->ip_search_mode = c->transform8x8 ? 7 : 5;  // 4x4, (8x8,) 16x16
    md->early_termination_ip1 = 1;
    md->priority_ipred_type_ip1 = c->transform8x8 ? 3 : 1;
    md->intra_ssd_cnt_4x4 = md->intra_ssd_cnt_8x8 = md->intra_ssd_cnt_16x16 = 1;  // later NVENCs hang on 0 (T2.05e)
    md->rdo_level = 1;
    md->tu_search_num = 2;

    // Quantisation: the driver's coefficient-cost vectors and dead zones (rounding offsets: the firmware scales each to
    // 10 bits and writes them in pairs into the quantiser, app 0x5540; 0 would truncate every coefficient towards
    // zero). Intra 4x4 luma by position from 1023 (DC) down to 410, inter from 682 down to 274; chroma and 8x8 luma
    // intra 682, inter 342: intra twice inter, as the H.264 reference model's 1/3 and 1/6 of a step.
    nvenc_h264_quant_control_s *q = (nvenc_h264_quant_control_s *)(setup + Q_OFF);
    static const unsigned short kDz4x4Intra[16] = {1023, 878, 820, 682, 878, 820, 682, 512,
                                                   820,  682, 512, 410, 682, 512, 410, 410};
    static const unsigned short kDz4x4Inter[16] = {682, 586, 546, 456, 586, 546, 456, 342,
                                                   546, 456, 342, 292, 456, 342, 292, 274};
    q->qpp_run_vector_4x4 = 0x056b;
    q->qpp_run_vector_8x8[0] = 0xaaff;
    q->qpp_run_vector_8x8[1] = 0xffaa;
    q->qpp_run_vector_8x8[2] = 0x000f;
    q->qpp_luma8x8_cost = 4;
    q->qpp_luma16x16_cost = 6;
    q->qpp_chroma_cost = 7;
    memcpy(q->dz_4x4_YI, kDz4x4Intra, sizeof(kDz4x4Intra));
    memcpy(q->dz_4x4_YP, kDz4x4Inter, sizeof(kDz4x4Inter));
    q->dz_4x4_CI = 682;
    q->dz_4x4_CP = 342;
    for (int i = 0; i < 16; i++) {
        q->dz_8x8_YI[i] = 682;
        q->dz_8x8_YP[i] = 342;
    }
}

void h264enc_fill_submit(const H264EncConfig *c, const H264EncLayout *l, const H264EncPicture *p,
                         const H264EncHandles *h, TegraNVENCSubmit *s)
{
    memset(s, 0, sizeof(*s));
    s->codec = TEGRA_NVENC_CODEC_H264;
    s->setup_bytes = SETUP_BYTES;
    nvenc_h264_drv_pic_setup_s *setup = (nvenc_h264_drv_pic_setup_s *)s->setup;
    const bool bPicture = p->type == H264ENC_B, inter = bPicture || p->type == H264ENC_P;
    setup->magic = 0xd0b70006u;  // NV_NVENC_5_0
    surfaceConfig(c, &l->recon, &setup->refpic_cfg, false);
    surfaceConfig(c, &l->input, &setup->input_cfg, true);
    surfaceConfig(c, &l->recon, &setup->outputpic_cfg, false);
    nvenc_h264_sps_data_s *sps = &setup->sps_data;
    sps->profile_idc = c->profile_idc;
    sps->level_idc = c->level_idc;
    sps->chroma_format_idc = 1;
    sps->pic_order_cnt_type = 0;
    sps->log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4;
    sps->log2_max_pic_order_cnt_lsb_minus4 = LOG2_MAX_POC_LSB - 4;
    sps->frame_mbs_only = 1;
    nvenc_h264_pps_data_s *pps = &setup->pps_data;
    pps->entropy_coding_mode_flag = c->cabac;
    pps->deblocking_filter_control_present_flag = 1;
    pps->transform_8x8_mode_flag = c->transform8x8;
    nvenc_h264_rc_s *rc = &setup->rate_control;  // constant QP: the plug-in's controller sets it per picture
    rc->QP[0] = rc->QP[1] = rc->QP[2] = p->qp;
    rc->maxQP[0] = rc->maxQP[1] = rc->maxQP[2] = 51;
    rc->framerate = c->fps_num && c->fps_den ? (int)(c->fps_num / c->fps_den) : 30;
    rc->gop_length = 0xffffffffu;
    // The lists and distances as NVIDIA's T210 driver writes them (TegraNVENC/nvidia/SOURCE): unused list entries -2,
    // unused distances and scale factors -1; an entry is its slot << 1. A P picture's one reference ahead with scale
    // factor 256; a B picture's references ahead and behind, the scale factor of that pair (8-203).
    nvenc_h264_pic_control_s *pc = &setup->pic_control;
    memset(pc->l0, -2, sizeof(pc->l0));
    memset(pc->l1, -2, sizeof(pc->l1));
    memset(pc->temp_dist_l0, -1, sizeof(pc->temp_dist_l0));
    memset(pc->temp_dist_l1, -1, sizeof(pc->temp_dist_l1));
    memset(pc->dist_scale_factor, -1, sizeof(pc->dist_scale_factor));
    if (inter) {
        pc->l0[0] = (char)(p->ref_slot[0] << 1);
        pc->temp_dist_l0[0] = (char)(p->gap[0] + 1);
    }
    if (bPicture) {
        pc->l1[0] = (char)(p->ref_slot[1] << 1);
        pc->temp_dist_l1[0] = (char)-(p->gap[1] + 1);
        pc->dist_scale_factor[0][0] = (short)tegra_nvenc_h264_dist_scale(p->gap[0] + 1, -(p->gap[1] + 1));
    } else if (inter) {
        for (int i = 0; i < 8; i++)
            pc->dist_scale_factor[i][0] = 256;
    }
    pc->frame_num = (unsigned short)p->frame_num;
    pc->pic_order_cnt_lsb = (unsigned short)p->poc_lsb;
    pc->idr_pic_id = (unsigned short)p->idr_pic_id;
    pc->slice_control_offset = SLICE_OFF;
    pc->me_control_offset = ME_OFF;
    pc->md_control_offset = MD_OFF;
    pc->q_control_offset = Q_OFF;
    pc->hist_buf_size = l->history_bytes;
    pc->bitstream_buf_size = l->bitstream_bytes;
    pc->bitstream_start_pos = 0;
    pc->max_byte_count_before_resid_zero = l->bitstream_bytes - 4096;
    pc->pic_struct = 0;
    pc->pic_type = p->type;
    pc->ref_pic_flag = !bPicture && !p->unreferenced;
    // Slicing as NVIDIA's T210 driver does it: slice_mode 0, the firmware walking the slice control array (one control,
    // every macroblock), max_slice_size the macroblock count (TegraNVENCCheck.h refuses slice_mode 1).
    pc->slice_mode = 0;
    pc->max_slice_size = h264enc_width_mbs(c) * h264enc_height_mbs(c);
    pc->cabac_zero_word_enable = 1;  // the driver's (7.4.2.10 bin-to-bit limit); a no-op with CAVLC
    pc->e4byteStartCode = 1;
    setup->gpTimer_timeout_val = TEGRA_NVENC_GPTIMER_MAX;
    controls(c, p, s->setup);

    const bool method = c->chroma_mode == TEGRA_NVENC_CHROMA_METHOD;
    const uint32_t inChroma = method ? l->input.chroma_offset : 0, refChroma = method ? l->recon.chroma_offset : 0;
    TegraNVENCEntry *e = s->entries;
    uint32_t n = 0;
#define VALUE(m, v)              e[n++] = (TegraNVENCEntry){(uint16_t)(m), TEGRA_NVENC_ENTRY_VALUE, (v), 0}
#define RELOC(m, handle, offset) e[n++] = (TegraNVENCEntry){(uint16_t)(m), TEGRA_NVENC_ENTRY_RELOC, (handle), (offset)}
    VALUE(NVC5B7_SET_APPLICATION_ID, NVC5B7_SET_APPLICATION_ID_ID_NVENC_H264);
    // NVIDIA's T210 driver's stream (switch-frame-tap m79-runE): FORCE_OUTPIC with the GP timer, the ME predictor
    // pair and reference slot 0 (the other reconstruction) in every job captured, the reference's chroma at its config
    // offset (no chroma methods in that reading), no colocated data in that P-only session. With B pictures that driver
    // names the slots the lists use, every picture's colocated output and a B picture's colocated input (libnvtvmr,
    // TegraNVENC/nvidia/SOURCE). Slot 0 goes in IDRs too: the app hands all 16 reference slots to the engine on every
    // picture (H.264 app 0x80a1), and an IDR without it would hand it 0 or the address a previous job left there.
    VALUE(NVC5B7_SET_CONTROL_PARAMS,
          NVC5B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264 | (1u << 8) /* FORCE_OUTPIC */ | (1u << 12) /* GPTIMER_ON */);
    VALUE(NVC5B7_SET_PICTURE_INDEX, p->picture_index);
    RELOC(NVC5B7_SET_IN_CUR_PIC, h->input, 0);
    RELOC(NVC5B7_SET_IN_CUR_PIC_CHROMA_U, h->input, inChroma);
    RELOC(NVC5B7_SET_OUT_REF_PIC_LUMA, h->recon, 0);
    if (method)
        RELOC(NVC5B7_SET_OUT_REF_PIC_CHROMA, h->recon, refChroma);
    RELOC(NVC5B7_SET_IN_MEPRED_DATA, h->mepred[p->picture_index & 1], 0);
    RELOC(NVC5B7_SET_OUT_MEPRED_DATA, h->mepred[~p->picture_index & 1], 0);
    if (h->ref)
        RELOC(NVC5B7_SET_IN_REF_PIC0_LUMA, h->ref, 0);
    if (h->ref && method)
        RELOC(NVC5B7_SET_IN_REF_PIC0_CHROMA, h->ref, refChroma);
    if (h->ref1)
        RELOC(NVC5B7_SET_IN_REF_PIC1_LUMA, h->ref1, 0);
    if (h->ref1 && method)
        RELOC(NVC5B7_SET_IN_REF_PIC1_CHROMA, h->ref1, refChroma);
    if (h->coloc_in)
        RELOC(NVC5B7_SET_IN_COLOC_DATA, h->coloc_in, 0);
    if (h->coloc_out)
        RELOC(NVC5B7_SET_OUT_COLOC_DATA, h->coloc_out, 0);
    RELOC(NVC5B7_SET_OUT_BITSTREAM, h->bitstream, 0);
    RELOC(NVC5B7_SET_OUT_ENC_STATUS, h->aux, 0);
    RELOC(NVC5B7_SET_IOHISTORY, h->aux, l->history_offset);
    VALUE(NVC5B7_EXECUTE, 1u << 8 /* AWAKEN */);
#undef VALUE
#undef RELOC
    s->entry_count = n;
    s->clean[0] = (TegraNVENCRange){h->input, 0, l->input.bytes, 0};
    s->clean_count = 1;
}

// A 64-byte x 8-row GOB of 16-byte x 2-row sectors (the layout NVDEC writes and BlockLinear.c reads).
static size_t gob(uint32_t x, uint32_t y)
{
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}

static size_t blockLinear(uint32_t gobs, uint32_t pitch, uint32_t x, uint32_t y)
{
    const uint32_t blockRows = 8 * gobs;
    return (size_t)(y / blockRows) * pitch * blockRows + (size_t)(x / 64) * 512 * gobs + ((y % blockRows) / 8) * 512 +
           gob(x, y);
}

size_t h264enc_surface_offset(uint32_t layout, uint32_t pitch, uint32_t x, uint32_t y)
{
    if (layout == TEGRA_NVENC_LAYOUT_TILED16)
        return (size_t)(y / 16) * pitch * 16 + (x / 16) * 256 + (y % 16) * 16 + x % 16;
    const uint32_t gobs = tegra_nvenc_layout_gobs(layout);
    return gobs ? blockLinear(gobs, pitch, x, y) : (size_t)y * pitch + x;
}

// One plane: `rows` x `bytes` samples from `src` (edge-replicated past `w` x `h`), 16 bytes at a time: each 16-byte
// run of a row is contiguous in every layout.
static void putPlane(uint32_t layout, uint32_t pitch, uint8_t *dst, const uint8_t *src, size_t stride, uint32_t w,
                     uint32_t h, uint32_t codedW, uint32_t codedH, uint32_t pairs)
{
    for (uint32_t y = 0; y < codedH; y++) {
        const uint8_t *row = src + (size_t)(y < h ? y : h - 1) * stride;
        if (layout == TEGRA_NVENC_LAYOUT_PITCH) {
            memcpy(dst + (size_t)y * pitch, row, w);
            for (uint32_t x = w; x < codedW; x++)
                dst[(size_t)y * pitch + x] = row[pairs ? w - 2 + (x & 1) : w - 1];
            continue;
        }
        for (uint32_t x = 0; x < codedW; x += 16) {
            uint8_t *out = dst + h264enc_surface_offset(layout, pitch, x, y);
            if (x + 16 <= w) {
                memcpy(out, row + x, 16);
                continue;
            }
            for (uint32_t i = 0; i < 16; i++) {
                const uint32_t sx = x + i;
                out[i] = row[sx < w ? sx : (pairs ? w - 2 + (sx & 1) : w - 1)];
            }
        }
    }
}

void h264enc_put_input(const H264EncConfig *c, const H264EncLayout *l, uint8_t *buffer, const uint8_t *luma,
                       size_t lumaStride, const uint8_t *chroma, size_t chromaStride)
{
    h264enc_put_region(c->input_layout, &l->input, buffer, luma, lumaStride, chroma, chromaStride, c->width, c->height,
                       h264enc_width_mbs(c) * 16, h264enc_height_mbs(c) * 16);
}

void h264enc_put_region(uint32_t layout, const H264EncSurface *input, uint8_t *buffer, const uint8_t *luma,
                        size_t lumaStride, const uint8_t *chroma, size_t chromaStride, uint32_t width, uint32_t height,
                        uint32_t codedWidth, uint32_t codedHeight)
{
    putPlane(layout, input->pitch, buffer, luma, lumaStride, width, height, codedWidth, codedHeight, 0);
    putPlane(layout, input->pitch, buffer + input->chroma_offset, chroma, chromaStride, (width + 1) & ~1u,
             (height + 1) / 2, codedWidth, codedHeight / 2, 1);
}

size_t h264enc_annexb_to_avcc(const uint8_t *in, size_t size, uint8_t *out, size_t cap)
{
    size_t n = 0, i = 0;
    while (i + 3 <= size) {
        if (!(in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1)) {
            i++;
            continue;
        }
        const size_t start = i + 3;
        size_t end = start;
        while (end + 3 <= size && !(in[end] == 0 && in[end + 1] == 0 && (in[end + 2] == 1 || in[end + 2] == 0)))
            end++;
        if (end + 3 > size)
            end = size;
        while (end > start && in[end - 1] == 0)
            end--;  // trailing_zero_8bits and the next start code's zero byte
        const size_t length = end - start;
        if (length) {
            if (n + 4 + length > cap)
                return 0;
            out[n++] = (uint8_t)(length >> 24);
            out[n++] = (uint8_t)(length >> 16);
            out[n++] = (uint8_t)(length >> 8);
            out[n++] = (uint8_t)length;
            memcpy(out + n, in + start, length);
            n += length;
        }
        i = end;
    }
    return n;
}
