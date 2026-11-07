// SPDX-License-Identifier: BSD-2-Clause-Patent
// See HEVCNVDEC.h. Clause numbers are ITU-T H.265 (V8).
#include "HEVCNVDEC.h"
#include <stdlib.h>
#include <string.h>
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"
#include "../../TegraNVDEC/TegraNVDECCheck.h"

#define SLOTS        16   // NVDEC's picture slots; the DPB (current picture included) never holds more
#define MAX_SEGMENTS 600  // slice segments in one picture
#define ALIGN(v, a)  (((v) + (a) - 1) / (a) * (a))

enum {
    kStatusOff = 0,
    kTilesOff = 256,  // tile sizes: 0x900 bytes (pairs from entry 0, boundaries from entry 0x380)
    kTilesBytes = 0x900,
    kScalingOff = kTilesOff + kTilesBytes,  // nvdec_hevc_scaling_list_s, 0x400 reserved
    kBitsOff = 0x1000,
    kFilterBytesPerRow = 480 + 3840 + 60,  // deblocking, SAO, BSD control (averne's FILTER/SAO/BSD_SIZE)
};

typedef struct {
    bool used;                    // in the DPB
    bool ref, long_term, output;  // marking; needed for output
    int32_t poc;
    uint32_t latency;  // PicLatencyCount
    void *item;        // the decoded picture, waiting to be bumped out
    uint32_t handle;   // its surface (kept while the slot is free, for the next picture)
    uint8_t *va;
} Entry;

struct HEVCDecoder {
    H264Backend b;
    HEVCParams params;
    int first_sps;
    bool allow10;
    Entry dpb[SLOTS];
    uint32_t width, height, pitch, rows, luma_bytes, surface_bytes;  // the surfaces' geometry
    uint32_t input, aux, colmv_bytes, filter_off, aux_bytes;
    uint8_t *input_va, *aux_va;
    uint64_t input_bytes;
    bool decoded_any, stream_start, skip_rasl, have_prev;
    int32_t prev_tid0_poc, last_poc;
    uint32_t picture_index;
    unsigned missing;
};

HEVCDecoder *hevc_decoder_create(const H264Backend *backend)
{
    HEVCDecoder *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->b = *backend;
    d->first_sps = -1;
    d->stream_start = true;
    return d;
}

static void freeSurfaces(HEVCDecoder *d)
{
    for (int i = 0; i < SLOTS; i++) {
        if (d->dpb[i].handle)
            d->b.free(d->b.ctx, d->dpb[i].handle);
        d->dpb[i].handle = 0;
        d->dpb[i].va = NULL;
    }
}

static void releaseBuffers(HEVCDecoder *d)
{
    if (d->aux)
        d->b.free(d->b.ctx, d->aux);
    if (d->input)
        d->b.free(d->b.ctx, d->input);
    d->aux = d->input = 0;
    d->aux_va = d->input_va = NULL;
}

static void dropAll(HEVCDecoder *d)  // the DPB emptied without output
{
    for (int i = 0; i < SLOTS; i++) {
        Entry *e = &d->dpb[i];
        if (e->item)
            d->b.discard(d->b.ctx, e->item);
        e->item = NULL;
        e->used = e->ref = e->long_term = e->output = false;
    }
}

void hevc_decoder_destroy(HEVCDecoder *d)
{
    if (!d)
        return;
    dropAll(d);
    freeSurfaces(d);
    releaseBuffers(d);
    free(d);
}

int hevc_decoder_parameter_set(HEVCDecoder *d, const uint8_t *nal, size_t size)
{
    if (size < 3)
        return -1;
    const int type = (nal[0] >> 1) & 0x3f;
    if (type == HEVC_NAL_SPS) {
        if (hevc_parse_sps(&d->params, nal, size))
            return -1;
        if (d->first_sps < 0) {  // its id: the SPS just parsed is the only valid one with this content
            for (int i = 0; i < 16; i++)
                if (d->params.sps[i].valid)
                    d->first_sps = i;
        }
        return 0;
    }
    if (type == HEVC_NAL_PPS)
        return hevc_parse_pps(&d->params, nal, size);
    return 0;  // VPS, SEI: nothing NVDEC needs
}

const HEVCSPS *hevc_decoder_sps(const HEVCDecoder *d)
{
    return d->first_sps >= 0 ? &d->params.sps[d->first_sps] : NULL;
}

void hevc_decoder_allow_10bit(HEVCDecoder *d, bool allow)
{
    d->allow10 = allow;
}

int32_t hevc_decoder_last_poc(const HEVCDecoder *d)
{
    return d->last_poc;
}

unsigned hevc_decoder_missing_refs(const HEVCDecoder *d)
{
    return d->missing;
}

// Buffers for the SPS's coded size. Only called with an empty DPB when the geometry changes (an IRAP that starts a
// coded video sequence; a new SPS mid-sequence with another size is refused).
static int configure(HEVCDecoder *d, const HEVCSPS *sps)
{
    const uint32_t pitch = ALIGN(sps->width, 64), rows = ALIGN(sps->height, 64);
    if (d->aux && pitch == d->pitch && rows == d->rows)
        return 0;
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used)
            return -1;
    freeSurfaces(d);
    releaseBuffers(d);
    d->width = sps->width, d->height = sps->height, d->pitch = pitch, d->rows = rows;
    d->luma_bytes = pitch * rows;
    d->surface_bytes = d->luma_bytes + d->luma_bytes / 2;
    // Colocated motion: 17 slots of (64-aligned width x height) / 16 bytes; the filter buffer: deblocking, SAO and
    // BSD control rows over the 64-aligned height (averne's nvtegra_hevc.c sizes).
    d->colmv_bytes = pitch * rows / 16;
    d->filter_off = ALIGN(17 * d->colmv_bytes, 256);
    d->aux_bytes = d->filter_off + kFilterBytesPerRow * rows;
    void *va;
    if (d->b.alloc(d->b.ctx, d->aux_bytes, &d->aux, &va))
        return -3;
    d->aux_va = va;
    d->input_bytes = kBitsOff + ALIGN((uint64_t)sps->width * sps->height / 2 + 0x10000, 0x10000);
    if (d->b.alloc(d->b.ctx, d->input_bytes, &d->input, &va)) {
        releaseBuffers(d);
        return -3;
    }
    d->input_va = va;
    return 0;
}

int hevc_decoder_configure(HEVCDecoder *d)
{
    const HEVCSPS *sps = hevc_decoder_sps(d);
    if (!sps)
        return -1;
    const int ret = configure(d, sps);
    if (ret)
        return ret;
    // Surfaces for the DPB the SPS declares (the current picture included), so NVDEC's memory is known to hold them.
    for (unsigned i = 0; i < sps->max_dec_pic_buffering && i < SLOTS; i++) {
        Entry *e = &d->dpb[i];
        void *va = NULL;
        if (e->handle)
            continue;
        if (d->b.alloc(d->b.ctx, d->surface_bytes, &e->handle, &va)) {
            e->handle = 0;
            freeSurfaces(d);
            releaseBuffers(d);
            return -3;
        }
        e->va = va;
    }
    return 0;
}

static int growInput(HEVCDecoder *d, uint64_t needed)
{
    uint64_t bytes = d->input_bytes;
    while (bytes < needed)
        bytes *= 2;
    if (bytes > (32u << 20))
        return -1;
    uint32_t handle;
    void *va;
    if (d->b.alloc(d->b.ctx, bytes, &handle, &va))
        return -1;
    d->b.free(d->b.ctx, d->input);
    d->input = handle;
    d->input_va = va;
    d->input_bytes = bytes;
    return 0;
}

static unsigned countWhere(const HEVCDecoder *d, bool output)
{
    unsigned n = 0;
    for (int i = 0; i < SLOTS; i++)
        n += d->dpb[i].used && (!output || d->dpb[i].output);
    return n;
}

static void removeUnneeded(HEVCDecoder *d)
{
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used && !d->dpb[i].output && !d->dpb[i].ref)
            d->dpb[i].used = d->dpb[i].long_term = false;
}

// C.5.2.4: the picture needed for output with the smallest POC goes out.
static bool bump(HEVCDecoder *d)
{
    int best = -1;
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used && d->dpb[i].output && (best < 0 || d->dpb[i].poc < d->dpb[best].poc))
            best = i;
    if (best < 0)
        return false;
    Entry *e = &d->dpb[best];
    e->output = false;
    if (e->item)
        d->b.emit(d->b.ctx, e->item);
    e->item = NULL;
    removeUnneeded(d);
    return true;
}

static bool latencyDue(const HEVCDecoder *d, const HEVCSPS *sps)
{
    if (!sps->max_latency_increase_plus1)
        return false;
    const uint64_t limit =
        (uint64_t)sps->max_num_reorder + sps->max_latency_increase_plus1 - 1;  // SpsMaxLatencyPictures
    for (int i = 0; i < SLOTS; i++)
        if (d->dpb[i].used && d->dpb[i].output && d->dpb[i].latency >= limit)
            return true;
    return false;
}

void hevc_decoder_flush(HEVCDecoder *d)
{
    while (bump(d)) {
    }
}

static void entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    TegraNVDECEntry *e = &s->entries[s->entry_count++];
    e->method = method;
    e->kind = kind;
    e->value = value;
    e->offset = offset;
}

// Tile columns and rows in CTBs (6.5.1). False for a PPS whose explicit sizes do not fit this SPS.
static bool tileGrid(const HEVCSPS *sps, const HEVCPPS *pps, uint16_t *col, uint16_t *row)
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

static void tileTable(uint16_t *t, const HEVCSPS *sps, const HEVCPPS *pps, const uint16_t *col, const uint16_t *row)
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

static void scalingTable(nvdec_hevc_scaling_list_s *out, const HEVCScalingList *sl)
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

typedef struct {
    int8_t before[16], after[16], lt[16];  // RefPicSetStCurrBefore / StCurrAfter / LtCurr as slots (-1: missing)
    uint8_t nb, na, nl;
} RefSets;

static void fillSetup(const HEVCDecoder *d, nvdec_hevc_pic_s *p, const HEVCSPS *sps, const HEVCPPS *pps,
                      const HEVCSliceHeader *sh, int cur, int32_t poc, const RefSets *r, int scratch, uint32_t length)
{
    memset(p, 0, sizeof(*p));
    p->stream_len = length;
    p->sw_start_code_e = 1;
    // 8-bit, or 10-bit truncated to 8 (BT.2020 matrices by their own table)
    p->disp_output_mode = sps->bit_depth == 8                                                  ? 0
                          : sps->colour_description && (sps->matrix == 9 || sps->matrix == 10) ? 3
                                                                                               : 2;
    p->framestride[0] = p->framestride[1] = d->pitch;
    p->colMvBuffersize = d->colmv_bytes / 256;
    p->HevcSaoBufferOffset = 480 * d->rows / 256;
    p->HevcBsdCtrlOffset = (480 + 3840) * d->rows / 256;
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

typedef struct {
    const uint8_t *data;
    size_t size;
} Span;

static int findRef(const HEVCDecoder *d, int32_t poc, bool lsbOnly, int32_t mask, bool shortOnly, const bool *taken)
{
    for (int i = 0; i < SLOTS; i++) {
        const Entry *e = &d->dpb[i];
        if (!e->used || !e->ref || taken[i] || (shortOnly && e->long_term))
            continue;
        if (lsbOnly ? (e->poc & mask) == poc : e->poc == poc)
            return i;
    }
    return -1;
}

static int decodePicture(HEVCDecoder *d, const Span *seg, int count, const HEVCSliceHeader *sh, void *token)
{
    const HEVCPPS *pps = &d->params.pps[sh->pps_id];
    const HEVCSPS *sps = &d->params.sps[pps->sps_id];
    if (pps->range_extension || sps->width < 144 || sps->height < 144 || sps->width > 4096 || sps->height > 4096 ||
        (sps->bit_depth == 10 && !d->allow10))
        return -1;
    const uint8_t nal = sh->nal_type;
    const bool irap = nal >= HEVC_NAL_BLA_W_LP && nal <= 23,
               idr = nal == HEVC_NAL_IDR_W_RADL || nal == HEVC_NAL_IDR_N_LP;
    const bool noRaslOutput = irap && (nal != HEVC_NAL_CRA || d->stream_start);
    if (irap)
        d->skip_rasl = noRaslOutput;
    if ((nal == HEVC_NAL_RASL_N || nal == HEVC_NAL_RASL_R) && d->skip_rasl)
        return 0;  // 8.1.3: not decodable
    if (!irap && !d->decoded_any)
        return 0;  // a stream that starts without an IRAP picture: wait for one

    // 8.3.1: the picture order count.
    const int32_t maxLsb = 1 << sps->log2_max_poc_lsb;
    int32_t msb = 0;
    if (!(irap && noRaslOutput)) {
        const int32_t prevLsb = d->prev_tid0_poc & (maxLsb - 1), prevMsb = d->prev_tid0_poc - prevLsb;
        const int32_t lsb = sh->poc_lsb;
        msb = lsb < prevLsb && prevLsb - lsb >= maxLsb / 2  ? prevMsb + maxLsb
              : lsb > prevLsb && lsb - prevLsb > maxLsb / 2 ? prevMsb - maxLsb
                                                            : prevMsb;
    }
    const int32_t poc = msb + sh->poc_lsb;

    // 8.3.2: the reference picture set; references it does not name are no longer references.
    RefSets r;
    memset(&r, 0, sizeof(r));
    bool keep[SLOTS] = {false}, lt[SLOTS] = {false};
    if (irap && noRaslOutput)
        for (int i = 0; i < SLOTS; i++)
            d->dpb[i].ref = false;
    if (!idr) {
        for (unsigned i = 0; i < sh->num_lt; i++) {
            int32_t value = sh->lt[i].poc_lsb;
            if (sh->lt[i].msb_present)
                value += poc - (int32_t)sh->lt[i].msb_cycle * maxLsb - (poc & (maxLsb - 1));
            const int j = findRef(d, value, !sh->lt[i].msb_present, maxLsb - 1, false, keep);
            if (j >= 0)
                keep[j] = lt[j] = true;
            if (sh->lt[i].used && r.nl < 16)
                r.lt[r.nl++] = (int8_t)j;
        }
        for (int s = 0; s < 2; s++) {
            const unsigned n = s ? sh->st_rps.num_positive : sh->st_rps.num_negative;
            for (unsigned i = 0; i < n; i++) {
                const int j = findRef(d, poc + sh->st_rps.delta[s][i], false, 0, true, keep);
                if (j >= 0)
                    keep[j] = true;
                if (!sh->st_rps.used[s][i])
                    continue;
                if (s && r.na < 16)
                    r.after[r.na++] = (int8_t)j;
                if (!s && r.nb < 16)
                    r.before[r.nb++] = (int8_t)j;
            }
        }
        for (int i = 0; i < SLOTS; i++) {
            if (!keep[i])
                d->dpb[i].ref = false;
            if (lt[i])
                d->dpb[i].long_term = true;
        }
    }

    // C.5.2.2: output and removal before the current picture.
    if (irap && noRaslOutput && d->decoded_any) {
        // NoOutputOfPriorPicsFlag: the flag of an IDR or BLA picture; a CRA that starts a sequence after an end of
        // sequence outputs what came before it (as FFmpeg does) rather than dropping it.
        if (nal != HEVC_NAL_CRA && sh->no_output_of_prior_pics)
            dropAll(d);
        hevc_decoder_flush(d);
        removeUnneeded(d);
    } else {
        removeUnneeded(d);
        while (countWhere(d, true) > sps->max_num_reorder || latencyDue(d, sps) ||
               countWhere(d, false) >= sps->max_dec_pic_buffering)
            if (!bump(d))
                break;
    }
    const int configured = configure(d, sps);
    if (configured)
        return configured;

    int cur = -1;
    for (int i = 0; i < SLOTS && cur < 0; i++)
        if (!d->dpb[i].used)
            cur = i;
    if (cur < 0)
        return -1;  // a DPB larger than its SPS allows
    Entry *e = &d->dpb[cur];
    if (!e->handle) {
        void *va;
        if (d->b.alloc(d->b.ctx, d->surface_bytes, &e->handle, &va)) {
            e->handle = 0;
            return -3;
        }
        e->va = va;
    }
    int scratch = -1;
    const int8_t *sets[3] = {r.before, r.after, r.lt};
    const uint8_t counts[3] = {r.nb, r.na, r.nl};
    for (int s = 0; s < 3; s++)
        for (unsigned k = 0; k < counts[s]; k++) {
            if (sets[s][k] < 0)
                d->missing++;
            else if (scratch < 0)
                scratch = sets[s][k];
        }
    if (scratch < 0)
        scratch = cur;

    // The bitstream: every slice segment of the picture after a 4-byte start code (sw_start_code_e).
    uint64_t length = 0;
    for (int i = 0; i < count; i++)
        length += 4 + seg[i].size;
    if (kBitsOff + length + 64 > d->input_bytes && growInput(d, kBitsOff + length + 64))
        return -3;
    uint8_t *bits = d->input_va + kBitsOff;
    for (int i = 0; i < count; i++) {
        bits[0] = bits[1] = bits[2] = 0, bits[3] = 1;
        memcpy(bits + 4, seg[i].data, seg[i].size);
        bits += 4 + seg[i].size;
    }
    memset(bits, 0, 64);
    uint16_t col[20], row[22];
    if (!tileGrid(sps, pps, col, row))
        return -1;
    tileTable((uint16_t *)(d->input_va + kTilesOff), sps, pps, col, row);
    scalingTable((nvdec_hevc_scaling_list_s *)(d->input_va + kScalingOff), hevc_scaling_list(sps, pps));

    TegraNVDECSubmit submit;
    memset(&submit, 0, sizeof(submit));
    submit.codec = TEGRA_NVDEC_CODEC_HEVC;
    submit.setup_bytes = sizeof(nvdec_hevc_pic_s);
    fillSetup(d, (nvdec_hevc_pic_s *)submit.setup, sps, pps, sh, cur, poc, &r, scratch, (uint32_t)length);
    entry(&submit, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_HEVC, 0);
    entry(&submit, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
          NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_HEVC | (1u << 4) /* GPTIMER_ON */ | (1u << 6) /* ERR_CONCEAL_ON */, 0);
    entry(&submit, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, d->picture_index++, 0);
    entry(&submit, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->input, kBitsOff);
    entry(&submit, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->input, kStatusOff);
    entry(&submit, NVC5B0_HEVC_SET_SCALING_LIST_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->input, kScalingOff);
    entry(&submit, NVC5B0_HEVC_SET_TILE_SIZES_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->input, kTilesOff);
    entry(&submit, NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->filter_off);
    entry(&submit, NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, 0);
    for (uint16_t i = 0; i < SLOTS; i++) {
        const Entry *pic = i == cur || (d->dpb[i].used && d->dpb[i].ref) ? &d->dpb[i] : &d->dpb[scratch];
        entry(&submit, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, pic->handle, 0);
        entry(&submit, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, pic->handle,
              d->luma_bytes);
    }
    entry(&submit, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8 /* AWAKEN */, 0);
    submit.clean[0] = (TegraNVDECRange){d->input, kTilesOff, (uint32_t)(kBitsOff - kTilesOff + length + 64), 0};
    submit.clean_count = 1;
    TegraNVDECWait wait = {.invalidate_count = 1};
    wait.invalidate[0] = (TegraNVDECRange){d->input, kStatusOff, sizeof(nvdec_status_s), 0};
    if (!d->b.cpu_reads || d->b.cpu_reads(d->b.ctx))
        wait.invalidate[wait.invalidate_count++] = (TegraNVDECRange){e->handle, 0, d->surface_bytes, 0};
    if (d->b.submit(d->b.ctx, &submit, &wait.fence) || d->b.wait(d->b.ctx, &wait))
        return -2;
    const nvdec_status_s *status = (const nvdec_status_s *)(d->input_va + kStatusOff);
    const int error = status->error_status && !status->mbs_correctly_decoded ? (int)status->error_status : 0;

    // 8.3.1's prevTid0Pic; the current picture becomes a short-term reference (8.3.2).
    if (sh->temporal_id == 0 && !(nal >= HEVC_NAL_RADL_N && nal <= HEVC_NAL_RASL_R) && !(nal <= 14 && !(nal & 1)))
        d->prev_tid0_poc = poc;
    d->decoded_any = true;
    d->stream_start = false;
    d->last_poc = poc;
    e->used = e->ref = true;
    e->long_term = false;
    e->poc = poc;
    e->latency = 0;
    e->output = sh->pic_output;
    e->item = NULL;
    if (e->output) {
        const H264Picture picture = {
            .luma = e->va,
            .chroma = e->va + d->luma_bytes,
            .pitch = d->pitch,
            .handle = e->handle,
            .chroma_offset = d->luma_bytes,
            .rows = d->rows,
            .coded_width = sps->width,
            .coded_height = sps->height,
            .crop_x = sps->crop_left,
            .crop_y = sps->crop_top,
            .width = (uint32_t)(sps->width - sps->crop_left - sps->crop_right),
            .height = (uint32_t)(sps->height - sps->crop_top - sps->crop_bottom),
            .poc = poc,
            .status = error,
            .token = token,
        };
        e->item = d->b.decoded(d->b.ctx, &picture);
        if (!e->item)
            e->output = false;
    }
    // C.5.2.3: latency counts, then the additional bumping.
    for (int i = 0; i < SLOTS; i++)
        if (i != cur && d->dpb[i].used && d->dpb[i].output)
            d->dpb[i].latency++;
    while (countWhere(d, true) > sps->max_num_reorder || latencyDue(d, sps))
        if (!bump(d))
            break;
    return sh->pic_output ? 1 : 0;  // a picture not output is answered at once, as a dropped frame
}

int hevc_decoder_decode(HEVCDecoder *d, const uint8_t *data, size_t size, unsigned lengthSize, void *token)
{
    if (lengthSize < 1 || lengthSize > 4)
        return -1;
    Span seg[MAX_SEGMENTS];  // 9.6 KB of stack: each session's decode thread its own
    int count = 0;
    bool eos = false;
    HEVCSliceHeader first;
    for (size_t at = 0; at < size;) {
        if (size - at < lengthSize)
            return -1;
        size_t n = 0;
        for (unsigned i = 0; i < lengthSize; i++)
            n = n << 8 | data[at + i];
        at += lengthSize;
        if (n < 2 || n > size - at)
            return -1;
        const uint8_t *nal = data + at;
        at += n;
        const int type = (nal[0] >> 1) & 0x3f;
        if (type == HEVC_NAL_SPS || type == HEVC_NAL_PPS) {
            if (hevc_decoder_parameter_set(d, nal, n))
                return -1;
        } else if (type == HEVC_NAL_EOS || type == HEVC_NAL_EOB) {
            eos = true;
        } else if (type <= HEVC_NAL_CRA && (type <= HEVC_NAL_RASL_R || type >= HEVC_NAL_BLA_W_LP)) {
            if (!count && (hevc_parse_slice_header(&d->params, nal, n, &first) || !first.first_slice))
                return -1;
            if (count == MAX_SEGMENTS)
                return -1;
            seg[count].data = nal, seg[count++].size = n;
        }
    }
    const int ret = count ? decodePicture(d, seg, count, &first, token) : 0;
    if (eos)
        d->stream_start = true;  // 8.1.3: the next IRAP picture starts a coded video sequence anew
    return ret;
}
