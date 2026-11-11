// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the kernel accepts in a TegraNVENCSubmit (TegraNVENCABI.h), shared with the host tests so a picture the plug-in
// builds is checked by the same code before it ever reaches hardware.
//
// Every size that steers NVENC's writes is bounded here against the buffer its relocation points into: the
// reconstructed picture (16x16 tiled, written for every reference picture), the bitstream (bitstream_buf_size), the
// history (hist_buf_size, at least tegra_nvenc_history_bytes), the colocated motion data when given (64 bytes per
// macroblock, 2.2), the ME predictors (in and out, as NVIDIA's T210 driver passes them), and the status record with
// its slice statistics. Reads (input picture,
// references) are bounded by their buffers too. Surfaces are bounded for every reading of the chroma methods
// (TegraNVENCABI.h TEGRA_NVENC_CHROMA_*), and for block-linear heights up to four GOBs.
// Everything that would make the firmware write elsewhere is refused: ME-only and hybrid outputs, MPEC/AQ/stats-FIFO
// dumps, two-pass and picture-level RC state, weighted prediction tables, B pictures, fields, MVC, encryption.
// Every job must run with the engine's GP timer on and at most 1 << 27 cycles, as NVDEC's.
// Only progressive 4:2:0 8-bit H.264 frames, one slice, one control of each kind.
// Slicing is the one NVIDIA's own T210 driver uses (TegraNVENC/nvidia/SOURCE): slice_mode 0 (the firmware walks the
// slice control array, here one control of exactly the picture's macroblocks, as the H.265 checker requires of its
// CTBs) with max_slice_size at least the macroblock count.
// slice_mode 1 is refused: NVIDIA's T210 driver never sends it, NVENC 5.0's H.264 app then runs the picture as one
// hardware-chained run (target count the whole picture, each slice-done interrupt restarting the next, app 0x0fe1 and
// 0x38ea), and NVIDIA's Ada NVENC hangs on it (github.com/mickys/nvenc docs/reports/T2.05e.md, MIT). On this NVENC it
// was in an IDR that never finished but also in an IPCM job that did (TegraNVENCSelfTest.cpp).
#ifndef TEGRA_NVENC_CHECK_H
#define TEGRA_NVENC_CHECK_H
#include "TegraNVENCCheckCommon.h"
#include "TegraNVENCCheckH265.h"

#ifdef __cplusplus
extern "C" {
#endif

// avail[i]: bytes from entry i's relocated address to the end of its buffer (ignored for values).
// Returns NULL when the submit is acceptable, else the reason.
static inline const char *tegra_nvenc_check(const TegraNVENCSubmit *s, const uint64_t *avail)
{
    enum {
        REFS = 16
    };

    if (s->codec == TEGRA_NVENC_CODEC_H265)
        return tegra_nvenc_check_h265(s, avail);
    if (s->codec != TEGRA_NVENC_CODEC_H264)
        return "codec";
    if (s->entry_count < 1 || s->entry_count > TEGRA_NVENC_MAX_ENTRIES || s->clean_count > TEGRA_NVENC_MAX_RANGES)
        return "counts";
    if (s->setup_bytes < TEGRA_NVENC_SETUP_HEADER || s->setup_bytes > TEGRA_NVENC_MAX_SETUP)
        return "setup size";
    TegraNVENCRelocs r;
    const char *why = tegra_nvenc_relocs(s, avail, &r);
    if (why)
        return why;
    if (!r.cur || !r.curChroma || !r.out || !r.bits || !r.status || !r.history)
        return "missing relocation";
    if ((r.mepredIn == 0u) != (r.mepredOut == 0u))
        return "ME predictors in without out, or out without in";

    const nvenc_h264_drv_pic_setup_s *p = (const nvenc_h264_drv_pic_setup_s *)s->setup;
    if (p->magic != 0xd0b70006u)
        return "magic";  // nvenc_drv.h NV_NVENC_5_0
    if (!p->gpTimer_timeout_val || p->gpTimer_timeout_val > TEGRA_NVENC_GPTIMER_MAX)
        return "GP timer limit";
    const uint32_t width = p->input_cfg.frame_width_minus1 + 1u;
    const uint32_t height = p->input_cfg.frame_height_minus1 + 1u;
    // Whole macroblocks: the app fetches input_cfg's size rounded down and encodes outputpic_cfg's rounded up, so any
    // other height leaves the engine waiting for a macroblock row it never fetches (nvidia/SOURCE).
    if (width < TEGRA_NVENC_MIN_DIMENSION || width > TEGRA_NVENC_MAX_DIMENSION || height < TEGRA_NVENC_MIN_DIMENSION ||
        height > TEGRA_NVENC_MAX_DIMENSION || width % 16 || height % 16)
        return "dimensions";
    const uint32_t mbs = (width / 16) * (height / 16);
    if (mbs > TEGRA_NVENC_MAX_MBS)
        return "too many macroblocks";
    why = tegra_nvenc_check_surface(&p->input_cfg, width, height, r.cur, r.curChroma, false);
    if (why)
        return why;
    // A reference picture without a chroma relocation has its chroma inside the luma buffer at the config's offset,
    // as NVIDIA's T210 driver gives them.
    if (!r.outChroma && !p->outputpic_cfg.chroma_top_frm_offset)
        return "reconstruction chroma";
    why = tegra_nvenc_check_surface(&p->outputpic_cfg, width, height, r.out, r.outChroma ? r.outChroma : r.out, true);
    if (why)
        return why;
    why = tegra_nvenc_check_surface(&p->refpic_cfg, width, height, ~0ULL, ~0ULL, true);
    if (why)
        return why;
    const uint8_t *half = (const uint8_t *)&p->half_scaled_outputpic_cfg;
    for (uint32_t i = 0; i < sizeof(p->half_scaled_outputpic_cfg); i++) {
        if (half[i])
            return "half-scaled output";
    }

    const nvenc_h264_sps_data_s *sps = &p->sps_data;
    if ((sps->profile_idc != 66 && sps->profile_idc != 77 && sps->profile_idc != 100) || sps->chroma_format_idc != 1 ||
        !sps->frame_mbs_only || sps->stereo_mvc_enable || sps->separate_colour_plane_flag ||
        sps->lossless_qpprime_flag || (sps->pic_order_cnt_type != 0 && sps->pic_order_cnt_type != 2))
        return "sequence parameters";
    const nvenc_h264_pps_data_s *pps = &p->pps_data;
    if (pps->weighted_bipred_idc || pps->weighted_pred_flag || pps->num_ref_idx_l1_active_minus1 ||
        pps->num_ref_idx_l0_active_minus1 >= REFS || pps->reserved0)
        return "picture parameters";

    const nvenc_h264_rc_s *rc = &p->rate_control;
    for (int i = 0; i < 3; i++) {
        if (rc->QP[i] > 51 || rc->minQP[i] > 51 || rc->maxQP[i] > 51)
            return "rate control QP";
    }
    if (rc->two_pass_rc || rc->aqMode || rc->dump_aq_stats || rc->first_pass_source_half_scaled ||
        rc->ext_picture_rc_hints)
        return "rate control mode";

    const nvenc_h264_pic_control_s *pc = &p->pic_control;
    if (pc->pic_struct || pc->pic_type == 1)
        return "picture structure or type";
    if (pc->base_view != 0 || pc->view_id || pc->priority_id || pc->temporal_id || pc->anchor_pic_flag ||
        pc->inter_view_flag || pc->opm_enable)
        return "MVC or encryption";
    if (pc->me_candidates_output_enable || pc->me_candidates_output_struct || pc->me_lambda_fifo_enable ||
        pc->me_candidates_rdo_mode || pc->stats_fifo_enable || pc->ofs_mode || pc->segment_enable ||
        pc->input_sub_sampling || pc->output_sub_sampling || pc->temporal_hint8x8_enable || pc->qpfifo ||
        pc->bit_depth_minus_8 || pc->b_as_ref || pc->new_subframe || pc->strips_in_frame > 1 ||
        pc->enable_source_image_padding)
        return "picture features";
    if (pc->num_forced_slices_minus1 || pc->num_me_controls_minus1 || pc->num_md_controls_minus1 ||
        pc->num_q_controls_minus1 || pc->num_wp_controls_minus1)
        return "control counts";
    if (pc->slice_mode || pc->max_slice_size < mbs)
        return "slice mode";

    const struct {
        uint32_t offset, size;
    } controls[] = {
        {pc->slice_control_offset, sizeof(nvenc_h264_slice_control_s)},
        {pc->me_control_offset, sizeof(nvenc_h264_me_control_s)},
        {pc->md_control_offset, sizeof(nvenc_h264_md_control_s)},
        {pc->q_control_offset, sizeof(nvenc_h264_quant_control_s)},
        {pc->wp_control_offset, sizeof(nvenc_pred_weight_table_s)},
    };

    for (uint32_t i = 0; i < 5; i++) {
        if (i == 4 && !controls[i].offset)
            continue;  // no weighted prediction table
        if (controls[i].offset < TEGRA_NVENC_SETUP_HEADER || (controls[i].offset & 3) ||
            controls[i].offset + controls[i].size > s->setup_bytes)
            return "control offset";
    }
    const nvenc_h264_slice_control_s *slice = (const nvenc_h264_slice_control_s *)(s->setup + pc->slice_control_offset);
    if (slice->num_mb != mbs || slice->ROI_enable || slice->force_intra > 1)
        return "slice control";
    const nvenc_h264_me_control_s *me = (const nvenc_h264_me_control_s *)(s->setup + pc->me_control_offset);
    if (me->me_only_mode || me->mv_only_enable)
        return "ME-only";
    // List-1 and bi-predicted partition enables pass (NVIDIA's T210 driver sets them in IDR and P pictures): with B
    // pictures refused and every list-1 entry unused (below) they name no reference. Direct and B skip do not.
    const nvenc_h264_md_control_s *md = (const nvenc_h264_md_control_s *)(s->setup + pc->md_control_offset);
    if (md->bdirect_mode || md->bskip_enable || md->ssim_rdo || md->calc_ssim)
        return "mode decision";

    // References: every list-0 entry a P picture may use names a slot with both planes relocated. An unused entry is
    // -2, as NVIDIA's T210 driver writes it (the firmware keeps an entry's low 5 bits as its DPB index, H.264 app
    // 0x8951: -2 is 30, no slot this checker admits). Every relocated slot is bounded, used or not: the app hands all
    // 16 to the engine on every picture (0x80a1).
    const uint32_t refs = pc->pic_type == 0 ? pps->num_ref_idx_l0_active_minus1 + 1u : 0u;
    if (refs && slice->num_ref_idx_active_override_flag &&
        slice->num_ref_idx_l0_active_minus1 != pps->num_ref_idx_l0_active_minus1)
        return "reference count override";
    for (uint32_t i = 0; i < 8; i++) {
        if ((signed char)pc->l1[i] != -2)
            return "list 1";  // char's signedness is the ABI's
        if (i >= refs) {
            if ((signed char)pc->l0[i] != -2)
                return "list 0 length";
            continue;
        }
        const int slot = (signed char)pc->l0[i];  // NOLINT(bugprone-signed-char-misuse): signed, -2 is unused
        if (slot < 0 || slot >= REFS || !r.refLuma[slot] ||
            (!r.refChroma[slot] && !p->refpic_cfg.chroma_top_frm_offset))
            return "reference slot";
    }
    for (uint32_t slot = 0; slot < REFS; slot++) {
        if (!r.refLuma[slot] && !r.refChroma[slot])
            continue;
        if (!r.refLuma[slot])
            return "reference slot";
        const uint64_t chroma = r.refChroma[slot] ? r.refChroma[slot] : r.refLuma[slot];
        why = tegra_nvenc_check_surface(&p->refpic_cfg, width, height, r.refLuma[slot], chroma, true);
        if (why)
            return why;
    }

    if (pc->bitstream_buf_size < 4096 || pc->bitstream_buf_size > r.bits || pc->bitstream_start_pos >= 256 ||
        pc->bitstream_start_pos >= pc->bitstream_buf_size)
        return "bitstream";
    if (pc->hist_buf_size < tegra_nvenc_history_bytes(width, height) || pc->hist_buf_size > r.history)
        return "history";
    if (r.colocOut && r.colocOut < (uint64_t)mbs * sizeof(nvenc_h264_coloc_mb_s))
        return "colocated buffer";
    if (r.mepredIn && (r.mepredIn < (uint64_t)mbs * TEGRA_NVENC_MEPRED_BYTES_PER_MB ||
                       r.mepredOut < (uint64_t)mbs * TEGRA_NVENC_MEPRED_BYTES_PER_MB))
        return "ME predictor buffers";
    if (r.status < TEGRA_NVENC_STATUS_BYTES)
        return "status";
    if ((r.control & (1u << 11)) && (pc->slice_stat_offset < 128 || pc->slice_stat_offset + 16 * 4 > r.status))
        return "slice statistics";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

#ifdef __cplusplus
}
#endif

#endif
