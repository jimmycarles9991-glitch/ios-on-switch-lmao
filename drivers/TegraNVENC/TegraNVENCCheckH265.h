// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the kernel accepts in an H.265 TegraNVENCSubmit (NVENC 5.0's app 3; TegraNVENC/nvidia/SOURCE-H265): IDR
// pictures and P pictures with one reference, progressive 4:2:0 8-bit, 32x32 CTBs, one slice, one control of each
// kind, the setups NVIDIA's T210 driver builds. B pictures, temporal MVP (it needs colocated buffers), SAO, PCM,
// long-term references, explicit or other reference picture sets, list modification, weighted prediction, statistics
// dumps and two-pass RC are refused. Every size that steers the firmware's writes is bounded as for H.264
// (TegraNVENCCheck.h); a P picture's one reference is the only reference slot relocated, and bounded too.
#ifndef TEGRA_NVENC_CHECK_H265_H
#define TEGRA_NVENC_CHECK_H265_H
#include "TegraNVENCCheckCommon.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_NVENC_H265_SETUP_HEADER = (int)sizeof(nvenc_h265_drv_pic_setup_s),
    TEGRA_NVENC_H265_CTB = 32,
    TEGRA_NVENC_H265_MAX_CTBS = (4096 / 32) * (2304 / 32),  // TEGRA_NVENC_MAX_MBS's picture
    TEGRA_NVENC_H265_SLICE_STAT_OFFSET = 128,               // after nvenc_h265_pic_stat_s, as the driver places them
    TEGRA_NVENC_H265_TRAIL_R = 1,
    TEGRA_NVENC_H265_IDR_W_RADL = 19,
    TEGRA_NVENC_H265_IDR_N_LP = 20,
    TEGRA_NVENC_H265_PIC_P = 0,  // nvenc_h265_pic_control_s.pic_type
    TEGRA_NVENC_H265_PIC_IDR = 3,
    TEGRA_NVENC_H265_SLICE_P = 0,  // what the driver puts in slice_type for P (SOURCE-H265), and for I
    TEGRA_NVENC_H265_SLICE_I = 2,
};

// The bitstream buffer NVIDIA's T210 driver gives a picture: 3 bytes a luma sample of the coded picture, at least
// 32 KiB.
static inline uint32_t tegra_nvenc_h265_bitstream_bytes(uint32_t width, uint32_t height)
{
    const uint32_t bytes = (width / 16) * (height / 16) * 768;
    return bytes > 0x8000 ? bytes : 0x8000;
}

static inline const char *tegra_nvenc_h265_surface(const nvenc_h265_surface_cfg_s *c, uint32_t width, uint32_t height,
                                                   uint64_t luma, uint64_t chroma, bool reference)
{
    nvenc_h264_surface_cfg_s same;  // nvenc_drv.h gives both codecs one surface layout
    memcpy(&same, c, sizeof(same));
    return tegra_nvenc_check_surface(&same, width, height, luma, chroma, reference);
}

static inline const char *tegra_nvenc_h265_parameters(const nvenc_h265_drv_pic_setup_s *p)
{
    const nvenc_h265_sps_data_s *sps = &p->sps_data;
    if (sps->chroma_format_idc != 1 || sps->separate_colour_plane_flag || sps->bit_depth_minus_8 ||
        sps->pcm_enabled_flag || sps->long_term_ref_pics_present_flag || sps->num_long_term_ref_pics_sps ||
        sps->sample_adaptive_offset_enabled_flag || sps->sps_temporal_mvp_enabled_flag || sps->max_cu_size != 2 ||
        sps->min_cu_size > sps->max_cu_size || sps->min_tu_size > sps->max_tu_size ||
        sps->num_short_term_ref_pic_sets > 1)
        return "sequence parameters";
    const nvenc_h265_pps_data_s *pps = &p->pps_data;
    if (pps->num_ref_idx_l0_active_minus1 || pps->num_ref_idx_l1_active_minus1 || pps->weighted_pred_flag ||
        pps->weighted_bipred_flag || pps->transquant_bypass_enabled_flag || pps->cu_qp_delta_enabled_flag ||
        pps->lists_modification_present_flag || pps->dependent_slice_segments_enabled_flag ||
        pps->output_flag_present_flag)
        return "picture parameters";
    const nvenc_h265_rc_s *rc = &p->rate_control;
    for (int i = 0; i < 3; i++) {
        if (rc->QP[i] > 51 || rc->minQP[i] > 51 || rc->maxQP[i] > 51)
            return "rate control QP";
    }
    if (rc->two_pass_rc || rc->aqMode || rc->dump_aq_stats || rc->first_pass_source_half_scaled ||
        rc->ext_picture_rc_hints)
        return "rate control mode";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

static inline bool tegra_nvenc_h265_zero(const void *p, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)p;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i])
            return false;
    }
    return true;
}

// An IDR, or a P picture predicting from one reference through the SPS's one reference picture set: its list-0 entry
// a DPB slot << 1 (the low bit a field flag, 0x143dd), every other entry the driver's unused -2. `*slot` is the P
// picture's reference slot, -1 for an IDR.
static inline const char *tegra_nvenc_h265_picture(const nvenc_h265_pic_control_s *pc, uint32_t ctbs, int *slot)
{
    const bool idr = pc->pic_type == TEGRA_NVENC_H265_PIC_IDR;
    const bool p =
        pc->pic_type == TEGRA_NVENC_H265_PIC_P && pc->nal_unit_type == TEGRA_NVENC_H265_TRAIL_R && pc->ref_pic_flag;
    const bool idrNal =
        pc->nal_unit_type == TEGRA_NVENC_H265_IDR_W_RADL || pc->nal_unit_type == TEGRA_NVENC_H265_IDR_N_LP;
    if (!(idr ? idrNal : p) || pc->nuh_layer_id || pc->nuh_temporal_id_plus1 != 1)
        return "picture type";
    *slot = idr ? -1 : (int)((uint8_t)pc->l0[0] >> 1);
    if (!idr && (((uint8_t)pc->l0[0] & 1) || *slot >= TEGRA_NVENC_REFS))
        return "reference lists";
    for (uint32_t i = 0; i < 8; i++) {
        if ((signed char)pc->l1[i] != -2 || ((idr || i) && (signed char)pc->l0[i] != -2))
            return "reference lists";
    }
    if (!idr && (!pc->short_term_ref_pic_set_sps_flag || pc->short_term_ref_pic_set_idx))
        return "reference picture set";
    if (!tegra_nvenc_h265_zero(&pc->short_term_rps, sizeof(pc->short_term_rps)) ||
        !tegra_nvenc_h265_zero(&pc->long_term_rps, sizeof(pc->long_term_rps)))
        return "reference picture set";
    nvenc_h265_ref_pic_list_modification_s lists = pc->ref_pic_list_modification;
    if (lists.NumPocTotalCurr != (idr ? 0 : 1))
        return "reference picture count";
    lists.NumPocTotalCurr = 0;
    if (!tegra_nvenc_h265_zero(&lists, sizeof(lists)))
        return "list modification";
    if (pc->mode_decision_only_flag || pc->input_sub_sampling || pc->output_sub_sampling || pc->qpfifo ||
        pc->stats_fifo_enable || pc->enable_source_image_padding || pc->strips_in_frame > 1)
        return "picture features";
    if (pc->num_forced_slices_minus1 || pc->num_me_controls_minus1 || pc->num_md_controls_minus1 ||
        pc->num_q_controls_minus1 || pc->num_wp_controls_minus1)
        return "control counts";
    if (pc->slice_mode || pc->max_slice_size < ctbs)
        return "slice mode";
    if (!tegra_nvenc_h265_zero(&pc->mv_hevc_cfg, sizeof(pc->mv_hevc_cfg)))
        return "multiview";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

// The controls the app fetches by DMA, each aligned to its transfer (slice 64, ME 3 x 64, MD 128; the quant offset
// is copied and never read), all inside the setup.
static inline const char *tegra_nvenc_h265_controls(const TegraNVENCSubmit *s, const nvenc_h265_pic_control_s *pc,
                                                    uint32_t ctbs)
{
    const struct {
        uint32_t offset, size, align;
    } controls[] = {
        {pc->slice_control_offset, sizeof(nvenc_h265_slice_control_s), 64},
        {pc->me_control_offset, sizeof(nvenc_h264_me_control_s), 64},
        {pc->md_control_offset, sizeof(nvenc_h265_md_control_s), 128},
        {pc->q_control_offset, sizeof(nvenc_h265_quant_control_s), 4},
    };

    for (uint32_t i = 0; i < 4; i++) {  // NOLINT(modernize-loop-convert): C too (hevcnvenc_host.c)
        if (controls[i].offset < TEGRA_NVENC_H265_SETUP_HEADER || controls[i].offset % controls[i].align ||
            controls[i].offset + controls[i].size > s->setup_bytes)
            return "control offset";
    }
    const nvenc_h265_slice_control_s *slice = (const nvenc_h265_slice_control_s *)(s->setup + pc->slice_control_offset);
    // Exactly the picture: the app's end-of-picture wait (0xfd35) spins, with no way out, until the engine has
    // counted every CTB it was asked for (SOURCE-H265).
    const uint32_t type =
        pc->pic_type == TEGRA_NVENC_H265_PIC_IDR ? TEGRA_NVENC_H265_SLICE_I : TEGRA_NVENC_H265_SLICE_P;
    if (slice->num_ctus != ctbs || slice->ROI_enable || slice->dependent_slice_segment_flag ||
        slice->slice_type != type)
        return "slice control";
    // One reference, the PPS's default count, as the hardware codes num_ref_idx_active_override_flag (I[0x181c],
    // 0x10b9f): a larger count would name list entries that are -2.
    if (slice->num_ref_idx_active_override_flag || slice->num_ref_idx_l0_active_minus1 ||
        slice->num_ref_idx_l1_active_minus1 || slice->slice_temporal_mvp_enabled_flag)
        return "slice references";
    const nvenc_h264_me_control_s *me = (const nvenc_h264_me_control_s *)(s->setup + pc->me_control_offset);
    if (me->me_only_mode || me->mv_only_enable)
        return "ME-only";
    const nvenc_h265_md_control_s *md = (const nvenc_h265_md_control_s *)(s->setup + pc->md_control_offset);
    if (md->force_ipcm)
        return "mode decision";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

static inline const char *tegra_nvenc_h265_buffers(const nvenc_h265_pic_control_s *pc, const TegraNVENCRelocs *r,
                                                   uint32_t width, uint32_t height)
{
    if (pc->bitstream_buf_size < 4096 || pc->bitstream_buf_size > r->bits || pc->bitstream_start_pos >= 256 ||
        pc->bitstream_start_pos >= pc->bitstream_buf_size)
        return "bitstream";
    if (pc->hist_buf_size < tegra_nvenc_history_bytes(width, height) || pc->hist_buf_size > r->history)
        return "history";
    const uint64_t blocks = (uint64_t)(width / 16) * (height / 16);
    if (r->mepredIn && (r->mepredIn < blocks * TEGRA_NVENC_MEPRED_BYTES_PER_MB ||
                        r->mepredOut < blocks * TEGRA_NVENC_MEPRED_BYTES_PER_MB))
        return "ME predictor buffers";
    if (r->status < TEGRA_NVENC_STATUS_BYTES)
        return "status";
    if ((r->control & (1u << 11)) && (pc->slice_stat_offset < 128 || pc->slice_stat_offset + 16 * 4 > r->status))
        return "slice statistics";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

static inline const char *tegra_nvenc_check_h265(const TegraNVENCSubmit *s, const uint64_t *avail)
{
    if (s->entry_count < 1 || s->entry_count > TEGRA_NVENC_MAX_ENTRIES || s->clean_count > TEGRA_NVENC_MAX_RANGES)
        return "counts";
    if (s->setup_bytes < TEGRA_NVENC_H265_SETUP_HEADER || s->setup_bytes > TEGRA_NVENC_MAX_SETUP)
        return "setup size";
    TegraNVENCRelocs r;
    const char *why = tegra_nvenc_relocs(s, avail, &r);
    if (why)
        return why;
    if (!r.cur || !r.curChroma || !r.out || !r.bits || !r.status || !r.history)
        return "missing relocation";
    if ((r.mepredIn == 0u) != (r.mepredOut == 0u))
        return "ME predictors in without out, or out without in";
    if (r.colocOut || r.colocIn)
        return "colocated motion";
    // App 3 copies method words 0-0x12 only (nvenc.bin 0xf41a-0xf4bb): a reconstruction's or reference's chroma is at
    // its luma address plus the surface config's offset (0x142da-0x142fe, 0x14267-0x142a4), never a chroma method. The
    // OS keeps IN_REF_PIC0_CHROMA in the word that marks methods written (nvidia/SOURCE).
    if (r.outChroma)
        return "reconstruction chroma method";
    for (uint32_t i = 0; i < TEGRA_NVENC_REFS; i++) {
        if (r.refChroma[i])
            return "reference chroma method";
    }

    const nvenc_h265_drv_pic_setup_s *p = (const nvenc_h265_drv_pic_setup_s *)s->setup;
    if (p->magic != 0xd0b70006u)
        return "magic";  // nvenc_drv.h NV_NVENC_5_0, which app 3 checks too
    const uint32_t width = p->input_cfg.frame_width_minus1 + 1u;
    const uint32_t height = p->input_cfg.frame_height_minus1 + 1u;
    if (width < TEGRA_NVENC_MIN_DIMENSION || width > TEGRA_NVENC_MAX_DIMENSION || height < TEGRA_NVENC_MIN_DIMENSION ||
        height > TEGRA_NVENC_MAX_DIMENSION || width % TEGRA_NVENC_H265_CTB || height % TEGRA_NVENC_H265_CTB)
        return "dimensions";
    const uint32_t ctbs = (width / TEGRA_NVENC_H265_CTB) * (height / TEGRA_NVENC_H265_CTB);
    why = tegra_nvenc_h265_surface(&p->input_cfg, width, height, r.cur, r.curChroma, false);
    if (!why && (!p->outputpic_cfg.chroma_top_frm_offset || !p->refpic_cfg.chroma_top_frm_offset))
        why = "reconstruction chroma";
    if (!why)
        why = tegra_nvenc_h265_surface(&p->outputpic_cfg, width, height, r.out, r.out, true);
    if (!why)
        why = tegra_nvenc_h265_surface(&p->refpic_cfg, width, height, ~0ULL, ~0ULL, true);
    if (why)
        return why;
    const uint8_t *half = (const uint8_t *)&p->half_scaled_outputpic_cfg;
    for (uint32_t i = 0; i < sizeof(p->half_scaled_outputpic_cfg); i++) {
        if (half[i])
            return "half-scaled output";
    }
    why = tegra_nvenc_h265_parameters(p);
    int slot = -1;
    if (!why)
        why = tegra_nvenc_h265_picture(&p->pic_control, ctbs, &slot);
    if (!why && slot >= 0 && p->sps_data.num_short_term_ref_pic_sets != 1)
        why = "reference picture set";
    // Exactly the slot the list names: app 3 hands all 16 slots to the engine (0x13c5f), so a slot relocated but not
    // named is an address nothing should read, and one named but not relocated is whatever a previous job left.
    for (int i = 0; !why && i < TEGRA_NVENC_REFS; i++) {
        if ((r.refLuma[i] != 0u) != (i == slot))
            why = "reference slot";
        else if (i == slot)
            why = tegra_nvenc_h265_surface(&p->refpic_cfg, width, height, r.refLuma[i], r.refLuma[i], true);
    }
    if (!why)
        why = tegra_nvenc_h265_controls(s, &p->pic_control, ctbs);
    if (!why)
        why = tegra_nvenc_h265_buffers(&p->pic_control, &r, width, height);
    return why;
}

#ifdef __cplusplus
}
#endif

#endif
