// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC's HEVC picture setup (nvdec_hevc_pic_s) and its tile size and scaling list tables; see HEVCState.h. Clause
// numbers are ITU-T H.265 (V8).
#include "HEVCState.h"
#include <string.h>

// Tile columns and rows in CTBs (6.5.1). False for a PPS whose explicit sizes do not fit this SPS.
bool hevc_tile_grid(const HEVCSPS *sps, const HEVCPPS *pps, uint16_t *col, uint16_t *row)
{
    const unsigned ctb = sps->log2_min_cb + sps->log2_diff_max_min_cb;
    const uint32_t wc = (sps->width + (1u << ctb) - 1) >> ctb, hc = (sps->height + (1u << ctb) - 1) >> ctb;
    const unsigned cols = pps->num_tile_columns, rows = pps->num_tile_rows;
    if (cols > wc || rows > hc)
        return false;
    uint32_t sw = 0, sh = 0;
    for (unsigned i = 0; i < cols; i++) {
        col[i] = (uint16_t)(pps->uniform_spacing || !pps->tiles ? (i + 1) * wc / cols - i * wc / cols
                            : i + 1 < cols                      ? pps->column_width[i]
                                                                : wc - sw);
        sw += col[i];
    }
    for (unsigned i = 0; i < rows; i++) {
        row[i] = (uint16_t)(pps->uniform_spacing || !pps->tiles ? (i + 1) * hc / rows - i * hc / rows
                            : i + 1 < rows                      ? pps->row_height[i]
                                                                : hc - sh);
        sh += row[i];
    }
    return sw == wc && sh == hc && col[cols - 1] && row[rows - 1] && col[cols - 1] <= wc && row[rows - 1] <= hc;
}

void hevc_tile_table(uint16_t *t, const HEVCSPS *sps, const HEVCPPS *pps, const uint16_t *col, const uint16_t *row)
{
    memset(t, 0, kTilesBytes);
    const unsigned shift = sps->log2_min_cb + sps->log2_diff_max_min_cb - 4;  // boundaries in 16-sample units
    if (!pps->tiles) {
        t[0] = col[0], t[1] = row[0];
        return;
    }
    uint16_t *bound = t + 0x380, sum = 0;
    for (unsigned i = 0; i < pps->num_tile_columns; i++)
        *bound++ = (uint16_t)((sum += col[i]) << shift);
    sum = 0;
    for (unsigned i = 0; i < pps->num_tile_rows; i++)
        *bound++ = (uint16_t)((sum += row[i]) << shift);
    for (unsigned i = 0; i < pps->num_tile_rows; i++)
        for (unsigned j = 0; j < pps->num_tile_columns; j++)
            *t++ = col[j], *t++ = row[i];
}

void hevc_scaling_table(nvdec_hevc_scaling_list_s *out, const HEVCScalingList *sl)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 6; i++)
        out->ScalingListDCCoeff16x16[i] = sl->dc[0][i];
    for (int i = 0; i < 2; i++)
        out->ScalingListDCCoeff32x32[i] = sl->dc[1][i * 3];
    for (int i = 0; i < 6; i++) {
        memcpy(out->ScalingList4x4[i], sl->sl[0][i], 16);
        memcpy(out->ScalingList8x8[i], sl->sl[1][i], 64);
        memcpy(out->ScalingList16x16[i], sl->sl[2][i], 64);
    }
    memcpy(out->ScalingList32x32[0], sl->sl[3][0], 64);
    memcpy(out->ScalingList32x32[1], sl->sl[3][3], 64);
}

static int8_t clip8(int32_t v)
{
    return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

void hevc_fill_setup(const HEVCDecoder *d, nvdec_hevc_pic_s *p, const HEVCSPS *sps, const HEVCPPS *pps,
                     const HEVCSliceHeader *sh, int cur, int32_t poc, const RefSets *r, int scratch, uint32_t length)
{
    memset(p, 0, sizeof(*p));
    p->stream_len = length;
    p->sw_start_code_e = 1;
    // 8-bit samples, or Main 10's in 16 bits (mode 1: P010, the 10 bits high, as 'x420' holds them), or those and an
    // 8-bit display buffer (mode 2)
    p->disp_output_mode = d->sample_bytes == 2 ? (hevc_display_buffer(d) ? 2 : 1) : 0;
    // framestride counts samples: in mode 1 the engine's rows are framestride x 2 bytes (averne's nvtegra_hevc.c
    // halves P010's linesize). Given in bytes, every block row lands at twice its offset.
    p->framestride[0] = p->framestride[1] = d->pitch / d->sample_bytes;
    p->colMvBuffersize = d->colmv_bytes / 256;
    p->HevcSaoBufferOffset = kDeblockBytesPerRow * d->sample_bytes * d->rows / 256;
    p->HevcBsdCtrlOffset = (kDeblockBytesPerRow + kSaoBytesPerRow) * d->sample_bytes * d->rows / 256;
    p->pic_width_in_luma_samples = sps->width;
    p->pic_height_in_luma_samples = sps->height;
    p->chroma_format_idc = 1;
    p->bit_depth_luma = p->bit_depth_chroma = sps->bit_depth;
    p->log2_min_luma_coding_block_size = sps->log2_min_cb;
    p->log2_max_luma_coding_block_size = sps->log2_min_cb + sps->log2_diff_max_min_cb;
    p->log2_min_transform_block_size = sps->log2_min_tb;
    p->log2_max_transform_block_size = sps->log2_min_tb + sps->log2_diff_max_min_tb;
    p->max_transform_hierarchy_depth_inter = sps->max_th_depth_inter;
    p->max_transform_hierarchy_depth_intra = sps->max_th_depth_intra;
    p->scalingListEnable = sps->scaling_list_enabled;
    p->amp_enable_flag = sps->amp;
    p->sample_adaptive_offset_enabled_flag = sps->sao;
    p->pcm_enabled_flag = sps->pcm;
    if (sps->pcm) {
        p->pcm_sample_bit_depth_luma = sps->pcm_bit_depth;
        p->pcm_sample_bit_depth_chroma = sps->pcm_bit_depth_chroma;
        p->log2_min_pcm_luma_coding_block_size = sps->log2_min_pcm;
        p->log2_max_pcm_luma_coding_block_size = sps->log2_min_pcm + sps->log2_diff_max_min_pcm;
    }
    p->pcm_loop_filter_disabled_flag = sps->pcm_loop_filter_disabled;
    p->sps_temporal_mvp_enabled_flag = sps->temporal_mvp;
    p->strong_intra_smoothing_enabled_flag = sps->strong_intra_smoothing;
    p->dependent_slice_segments_enabled_flag = pps->dependent_slice_segments;
    p->output_flag_present_flag = pps->output_flag_present;
    p->num_extra_slice_header_bits = pps->num_extra_slice_header_bits;
    p->sign_data_hiding_enabled_flag = pps->sign_data_hiding;
    p->cabac_init_present_flag = pps->cabac_init_present;
    p->num_ref_idx_l0_default_active = pps->num_ref_idx_l0_default;
    p->num_ref_idx_l1_default_active = pps->num_ref_idx_l1_default;
    p->init_qp = (unsigned)(pps->init_qp_minus26 + 26 + 6 * (sps->bit_depth - 8));
    p->constrained_intra_pred_flag = pps->constrained_intra_pred;
    p->transform_skip_enabled_flag = pps->transform_skip;
    p->cu_qp_delta_enabled_flag = pps->cu_qp_delta;
    p->diff_cu_qp_delta_depth = pps->diff_cu_qp_delta_depth;
    p->pps_cb_qp_offset = (char)pps->cb_qp_offset;
    p->pps_cr_qp_offset = (char)pps->cr_qp_offset;
    p->pps_beta_offset = (char)pps->beta_offset;
    p->pps_tc_offset = (char)pps->tc_offset;
    p->pps_slice_chroma_qp_offsets_present_flag = pps->slice_chroma_qp_offsets_present;
    p->weighted_pred_flag = pps->weighted_pred;
    p->weighted_bipred_flag = pps->weighted_bipred;
    p->transquant_bypass_enabled_flag = pps->transquant_bypass;
    p->tiles_enabled_flag = pps->tiles;
    p->entropy_coding_sync_enabled_flag = pps->entropy_coding_sync;
    p->num_tile_columns = pps->tiles ? pps->num_tile_columns : 0;
    p->num_tile_rows = pps->tiles ? pps->num_tile_rows : 0;
    p->loop_filter_across_tiles_enabled_flag = pps->tiles && pps->loop_filter_across_tiles;
    p->loop_filter_across_slices_enabled_flag = pps->loop_filter_across_slices;
    p->deblocking_filter_control_present_flag = pps->deblocking_control;
    p->deblocking_filter_override_enabled_flag = pps->deblocking_override;
    p->pps_deblocking_filter_disabled_flag = pps->deblocking_disabled;
    p->lists_modification_present_flag = pps->lists_modification_present;
    p->log2_parallel_merge_level = pps->log2_parallel_merge_level;
    p->slice_segment_header_extension_present_flag = pps->slice_header_extension_present;
    p->num_ref_frames = (unsigned char)(r->nb + r->na + r->nl);  // NumPicTotalCurr
    p->IDR_picture_flag = sh->nal_type == HEVC_NAL_IDR_W_RADL || sh->nal_type == HEVC_NAL_IDR_N_LP;
    p->RAP_picture_flag = sh->nal_type >= HEVC_NAL_BLA_W_LP && sh->nal_type <= 23;
    p->curr_pic_idx = (unsigned char)cur;
    p->pattern_id = 2;  // the dithering tables off (averne: 2 for 8-bit output modes; a 10-bit source as well here)
    p->sw_hdr_skip_length = sh->skip_bits;
    const int32_t scratchDiff = scratch == cur ? 0 : clip8(poc - d->dpb[scratch].poc);
    for (int i = 0; i < SLOTS; i++) {
        if (i == cur)
            continue;
        const Entry *e = &d->dpb[i];
        if (e->used && e->ref) {
            p->RefDiffPicOrderCnts[i] = clip8(poc - e->poc);
            if (e->long_term)
                p->longtermflag |= (unsigned short)(1u << (15 - i));
        } else {
            p->RefDiffPicOrderCnts[i] = (short)scratchDiff;
        }
    }
    // The initial reference picture lists (8.3.4), cycled to 16 entries; NVDEC applies the slices' modifications.
    const unsigned total = r->nb + r->na + r->nl;
    if (!total)
        return;
    const int8_t *order[2][3] = {{r->before, r->after, r->lt}, {r->after, r->before, r->lt}};
    const uint8_t counts[2][3] = {{r->nb, r->na, r->nl}, {r->na, r->nb, r->nl}};
    unsigned char *lists[2] = {p->initreflistidxl0, p->initreflistidxl1};
    for (int l = 0; l < 2; l++)
        for (unsigned i = 0; i < 16;)
            for (int s = 0; s < 3 && i < 16; s++)
                for (unsigned k = 0; k < counts[l][s] && i < 16; k++)
                    lists[l][i++] = (unsigned char)(order[l][s][k] >= 0 ? order[l][s][k] : scratch);
}
