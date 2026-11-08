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
// slice control array, here one control covering every macroblock) with max_slice_size at least the macroblock count.
// slice_mode 1 is refused: NVIDIA's T210 driver never sends it, NVENC 5.0's H.264 app then runs the picture as one
// hardware-chained run (target count the whole picture, each slice-done interrupt restarting the next, app 0x0fe1 and
// 0x38ea), and NVIDIA's Ada NVENC hangs on it (github.com/mickys/nvenc docs/reports/T2.05e.md, MIT). On this NVENC it
// was in an IDR that never finished but also in an IPCM job that did (TegraNVENCSelfTest.cpp).
#ifndef TEGRA_NVENC_CHECK_H
#define TEGRA_NVENC_CHECK_H
#include <stddef.h>
#include <stdint.h>
#include "TegraNVENCABI.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "nvidia/clc5b7.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_NVENC_MAX_DIMENSION = 4096,
    TEGRA_NVENC_MAX_MBS = 36864,  // 4096x2304: bounds a job's time (TegraNVENC.cpp kJobBoundUs)
    TEGRA_NVENC_MIN_DIMENSION = 32,
    TEGRA_NVENC_GPTIMER_MAX = 1u << 27,
    TEGRA_NVENC_STATUS_BYTES = 128 + 16 * 4,  // nvenc_pic_stat_s, then slice statistics (one slice; margin)
    TEGRA_NVENC_SETUP_HEADER = (int)sizeof(nvenc_h264_drv_pic_setup_s),
    // ME predictor records a picture writes for the next one: NVIDIA's T210 driver gives each of its two buffers
    // 128 KiB at 1280x720 (3600 macroblocks); 64 bytes a macroblock covers that with margin.
    TEGRA_NVENC_MEPRED_BYTES_PER_MB = 64,
};

static inline uint32_t tegra_nvenc_align(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

// The history buffer: the larger of nvenc_drv.h 2.3's minimum, (((frame_width_minus1 + 16) / 16) + 16) * 128 bytes
// (one macroblock row of 128-byte records), and what NVIDIA's own T210 driver gives NVENC 5.0: HIST_BLOCK_SIZE (192)
// bytes per macroblock of the picture plus one row, (width_mbs * (height_mbs + 1)) * 192 (706560 bytes at 1280x720,
// TegraNVENC/nvidia/SOURCE). The firmware hands the size to the engine (hist_buf_size >> 7 into its history unit,
// H.264 app 0x3333), so a smaller buffer is one the engine can outrun.
static inline uint32_t tegra_nvenc_history_bytes(uint32_t width, uint32_t height)
{
    const uint32_t wmbs = (width + 15) / 16;
    const uint32_t hmbs = (height + 15) / 16;
    const uint32_t row = (((width - 1 + 16) / 16) + 16) * 128;
    const uint32_t picture = wmbs * (hmbs + 1) * HIST_BLOCK_SIZE;
    return row > picture ? row : picture;
}

// Luma rows a surface spans: its height rounded to 64, which covers 16x16 tiles and block-linear blocks of up to
// eight GOBs.
static inline uint32_t tegra_nvenc_surface_rows(uint32_t height)
{
    return tegra_nvenc_align(height, 64);
}

// A surface: luma at its relocation, chroma `chromaOffset` bytes into the luma buffer (surface config) or at its own
// relocation. Every reading is bounded: the config offset alone, the chroma relocation alone, and the two added.
static inline const char *tegra_nvenc_check_surface(const nvenc_h264_surface_cfg_s *c, uint32_t width, uint32_t height,
                                                    uint64_t lumaAvail, uint64_t chromaAvail, bool reference)
{
    if (c->frame_width_minus1 + 1u != width || c->frame_height_minus1 + 1u != height)
        return "surface dimensions";
    if (c->sfc_trans_mode || c->reserved2 || c->memory_mode || c->nv21_enable || c->input_bl_mode || c->reserved)
        return "surface mode";
    if (c->luma_top_frm_offset || c->luma_bot_offset || c->chroma_bot_offset)
        return "surface field offsets";
    if (reference ? (!c->tiled_16x16 || c->block_height) : (c->tiled_16x16 && c->block_height))
        return "surface tiling";
    if (c->block_height > 2)
        return "block height";
    const uint32_t pitch = c->sfc_pitch;
    if (pitch != c->sfc_pitch_chroma || (pitch & 63) || pitch < tegra_nvenc_align(width, 16) || pitch > 16384)
        return "surface pitch";
    const uint64_t rows = tegra_nvenc_surface_rows(height);
    const uint64_t luma = (uint64_t)pitch * rows;
    const uint64_t chroma = luma / 2;
    const uint64_t offset = (uint64_t)c->chroma_top_frm_offset * 256;
    if (offset && offset < luma)
        return "chroma overlaps luma";
    if (lumaAvail < luma || lumaAvail < offset + chroma || chromaAvail < offset + chroma)
        return "surface too small";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

// avail[i]: bytes from entry i's relocated address to the end of its buffer (ignored for values).
// Returns NULL when the submit is acceptable, else the reason.
static inline const char *tegra_nvenc_check(const TegraNVENCSubmit *s, const uint64_t *avail)
{
    enum {
        REFS = 16
    };

    if (s->codec != TEGRA_NVENC_CODEC_H264)
        return "codec";
    if (s->entry_count < 1 || s->entry_count > TEGRA_NVENC_MAX_ENTRIES || s->clean_count > TEGRA_NVENC_MAX_RANGES)
        return "counts";
    if (s->setup_bytes < TEGRA_NVENC_SETUP_HEADER || s->setup_bytes > TEGRA_NVENC_MAX_SETUP)
        return "setup size";
    uint64_t cur = 0;
    uint64_t curChroma = 0;
    uint64_t out = 0;
    uint64_t outChroma = 0;
    uint64_t bits = 0;
    uint64_t status = 0;
    uint64_t history = 0;
    uint64_t colocOut = 0;
    uint64_t mepredIn = 0;
    uint64_t mepredOut = 0;
    uint64_t refLuma[REFS] = {0};
    uint64_t refChroma[REFS] = {0};
    uint32_t seen = 0;
    uint32_t control = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVENC_ENTRY_VALUE) {
            uint32_t bit = 0;
            switch (m) {
            case NVC5B7_SET_APPLICATION_ID:  // first: the OS selects the app before methods are stored for it
                if (e->value != s->codec)
                    return "application id";
                if (i)
                    return "application id not first";
                bit = 0;
                break;
            // H.264, GPTIMER_ON (required), FORCE_OUTPIC, SLICE_STAT_ON, constant QP, nothing else.
            case NVC5B7_SET_CONTROL_PARAMS:
                if ((e->value & 0xf) != NVC5B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264 || (e->value & ~0x190fu) ||
                    !(e->value & (1u << 12)))
                    return "control params";
                control = e->value;
                bit = 1;
                break;
            case NVC5B7_SET_PICTURE_INDEX:
                bit = 2;
                break;
            case NVC5B7_SET_WATCHDOG_TIMER:
                bit = 3;
                break;
            case NVC5B7_EXECUTE:  // AWAKEN only: NOTIFY would release a semaphore at a raw address
                if (e->value & ~0x100u)
                    return "execute flags";
                if (i != s->entry_count - 1)
                    return "execute not last";
                bit = 4;
                break;
            default:
                return "value method";
            }
            if (seen & (1u << bit))
                return "repeated method";
            seen |= 1u << bit;
            continue;
        }
        if (e->kind != TEGRA_NVENC_ENTRY_RELOC)
            return "entry kind";
        if (e->offset & 0xff)
            return "relocation alignment";
        const uint64_t a = avail[i];
        uint64_t *slot = NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
        switch (m) {
        case NVC5B7_SET_IN_CUR_PIC:
            slot = &cur;
            break;
        case NVC5B7_SET_IN_CUR_PIC_CHROMA_U:
            slot = &curChroma;
            break;
        case NVC5B7_SET_OUT_REF_PIC_LUMA:
            slot = &out;
            break;
        case NVC5B7_SET_OUT_REF_PIC_CHROMA:
            slot = &outChroma;
            break;
        case NVC5B7_SET_OUT_BITSTREAM:
            slot = &bits;
            break;
        case NVC5B7_SET_OUT_ENC_STATUS:
            slot = &status;
            break;
        case NVC5B7_SET_IOHISTORY:
            slot = &history;
            break;
        case NVC5B7_SET_OUT_COLOC_DATA:
            slot = &colocOut;
            break;
        case NVC5B7_SET_IN_MEPRED_DATA:
            slot = &mepredIn;
            break;
        case NVC5B7_SET_OUT_MEPRED_DATA:
            slot = &mepredOut;
            break;
        default:
            if (m >= NVC5B7_SET_IN_REF_PIC0_LUMA && m < NVC5B7_SET_IN_REF_PIC0_LUMA + 4 * REFS && !(m & 3)) {
                slot = &refLuma[(m - NVC5B7_SET_IN_REF_PIC0_LUMA) / 4];
            } else if (m >= NVC5B7_SET_IN_REF_PIC0_CHROMA && m < NVC5B7_SET_IN_REF_PIC0_CHROMA + 4 * REFS && !(m & 3)) {
                slot = &refChroma[(m - NVC5B7_SET_IN_REF_PIC0_CHROMA) / 4];
            } else {
                return "relocated method";
            }
        }
        if (*slot)
            return "repeated relocation";
        *slot = a ? a : 1;  // a relocation exactly at a buffer's end still counts as present
    }
    if ((seen & 0x13) != 0x13)
        return "missing application id, control params or execute";
    if (!cur || !curChroma || !out || !bits || !status || !history)
        return "missing relocation";
    if ((mepredIn == 0u) != (mepredOut == 0u))
        return "ME predictors in without out, or out without in";

    const nvenc_h264_drv_pic_setup_s *p = (const nvenc_h264_drv_pic_setup_s *)s->setup;
    if (p->magic != 0xd0b70006u)
        return "magic";  // nvenc_drv.h NV_NVENC_5_0
    if (!p->gpTimer_timeout_val || p->gpTimer_timeout_val > TEGRA_NVENC_GPTIMER_MAX)
        return "GP timer limit";
    const uint32_t width = p->input_cfg.frame_width_minus1 + 1u;
    const uint32_t height = p->input_cfg.frame_height_minus1 + 1u;
    if (width < TEGRA_NVENC_MIN_DIMENSION || width > TEGRA_NVENC_MAX_DIMENSION || height < TEGRA_NVENC_MIN_DIMENSION ||
        height > TEGRA_NVENC_MAX_DIMENSION)
        return "dimensions";
    const uint32_t wmbs = (width + 15) / 16;
    const uint32_t hmbs = (height + 15) / 16;
    const uint32_t mbs = wmbs * hmbs;
    if (mbs > TEGRA_NVENC_MAX_MBS)
        return "too many macroblocks";
    const char *why = tegra_nvenc_check_surface(&p->input_cfg, width, height, cur, curChroma, false);
    if (why)
        return why;
    // A reference picture without a chroma relocation has its chroma inside the luma buffer at the config's offset,
    // as NVIDIA's T210 driver gives them.
    if (!outChroma && !p->outputpic_cfg.chroma_top_frm_offset)
        return "reconstruction chroma";
    why = tegra_nvenc_check_surface(&p->outputpic_cfg, width, height, out, outChroma ? outChroma : out, true);
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
    if (slice->num_mb < mbs || slice->ROI_enable || slice->force_intra > 1)
        return "slice control";
    const nvenc_h264_me_control_s *me = (const nvenc_h264_me_control_s *)(s->setup + pc->me_control_offset);
    if (me->me_only_mode || me->mv_only_enable)
        return "ME-only";
    // List-1 and bi-predicted partition enables pass (NVIDIA's T210 driver sets them in IDR and P pictures): with B
    // pictures refused and every list-1 entry unused (below) they name no reference. Direct and B skip do not.
    const nvenc_h264_md_control_s *md = (const nvenc_h264_md_control_s *)(s->setup + pc->md_control_offset);
    if (md->bdirect_mode || md->bskip_enable || md->ssim_rdo || md->calc_ssim)
        return "mode decision";

    // References: every list-0 entry a P picture may use names a slot with both planes relocated and big enough. An
    // unused entry is -2, as NVIDIA's T210 driver writes it (the firmware keeps an entry's low 5 bits as its DPB index,
    // H.264 app 0x8951: -2 is 30, no slot this checker admits).
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
        if (slot < 0 || slot >= REFS || !refLuma[slot] || (!refChroma[slot] && !p->refpic_cfg.chroma_top_frm_offset))
            return "reference slot";
        const uint64_t chroma = refChroma[slot] ? refChroma[slot] : refLuma[slot];
        why = tegra_nvenc_check_surface(&p->refpic_cfg, width, height, refLuma[slot], chroma, true);
        if (why)
            return why;
    }

    if (pc->bitstream_buf_size < 4096 || pc->bitstream_buf_size > bits || pc->bitstream_start_pos >= 256 ||
        pc->bitstream_start_pos >= pc->bitstream_buf_size)
        return "bitstream";
    if (pc->hist_buf_size < tegra_nvenc_history_bytes(width, height) || pc->hist_buf_size > history)
        return "history";
    if (colocOut && colocOut < (uint64_t)mbs * sizeof(nvenc_h264_coloc_mb_s))
        return "colocated buffer";
    if (mepredIn && (mepredIn < (uint64_t)mbs * TEGRA_NVENC_MEPRED_BYTES_PER_MB ||
                     mepredOut < (uint64_t)mbs * TEGRA_NVENC_MEPRED_BYTES_PER_MB))
        return "ME predictor buffers";
    if (status < TEGRA_NVENC_STATUS_BYTES)
        return "status";
    if ((control & (1u << 11)) && (pc->slice_stat_offset < 128 || pc->slice_stat_offset + 16 * 4 > status))
        return "slice statistics";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (ave_host.c)
}

#ifdef __cplusplus
}
#endif

#endif
