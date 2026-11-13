// SPDX-License-Identifier: BSD-2-Clause-Patent
// VP9 on NVDEC 2.0; see VP9NVDEC.h.
#include "VP9NVDEC.h"
#include <stddef.h>
#include "NVDECRing.h"
#include <stdlib.h>
#include <string.h>
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"
#include "../../TegraNVDEC/TegraNVDECCheck.h"

#define MAX_SURFACES 10  // eight slots, the frame being decoded, and one spare while a slot changes hands
#define ALIGN(v, a)  (((v) + (a) - 1) / (a) * (a))

typedef struct {
    uint32_t handle;
    uint8_t *va;
    uint32_t pitch, rows, luma_bytes, bytes;  // capacity
    uint16_t width, height;                   // the frame it holds
    uint8_t refs;                             // slots holding it
} Surface;

// A frame's backward adaptation, waiting for the counts its job writes: done before the next frame's headers are read
// (they start from the contexts it refreshes) and before the next job overwrites the counts.
typedef struct {
    bool waiting, intra, lastKey, refresh, allowHP;
    uint8_t interpFilter, txMode, contextIdx;
    uint32_t fence;
    VP9FrameContext pre, fc;
} Adaptation;

struct VP9Decoder {
    H264Backend b;
    Adaptation adaptation;
    VP9State state;
    VP9FrameContext fc;
    VP9FrameHeader header;
    Surface surf[MAX_SURFACES];
    int8_t slot[8];  // surface of each reference slot, -1: empty
    // Shared buffers, sized for max_width x max_height
    uint32_t max_width, max_height;
    NVDECRing ring;  // each frame's status record, probability table and tile data
    uint32_t aux, aux_bytes;
    uint8_t *aux_va;
    // seg_off[kSegClear] is never written: the map a frame that resets past state reads (no CPU write into a buffer a
    // queued job may still be using).
    uint32_t seg_off[3], colmv_off[2], counters_off, filter_off, seg_bytes, filter_rows;
    uint8_t seg_read, colmv_write;
    // The previous decoded frame (libvpx last_*)
    bool have_last, last_show, last_key;
    uint16_t last_width, last_height;
    uint32_t picture_index;
};

enum {
    kStatusOff = 0,
    kProbOff = 256,  // sizeof(nvdec_status_s) fits below
    kTilesOff = kProbOff + ALIGN(sizeof(nvdec_vp9EntropyProbs_t), 256),
    kTilesBytes = 0x700,
    kBitsOff = kTilesOff + kTilesBytes,
    kSegClear = 2,
};

VP9Decoder *vp9_decoder_create(const H264Backend *backend)
{
    VP9Decoder *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->b = *backend;
    memset(d->slot, -1, sizeof(d->slot));
    return d;
}

static void freeSurface(VP9Decoder *d, Surface *s)
{
    if (s->handle)
        d->b.free(d->b.ctx, s->handle);
    memset(s, 0, sizeof(*s));
}

static void releaseBuffers(VP9Decoder *d)
{
    if (d->aux)
        d->b.free(d->b.ctx, d->aux);
    nvdec_ring_release(&d->ring, &d->b);
    d->aux = 0;
    d->aux_va = NULL;
}

void vp9_decoder_destroy(VP9Decoder *d)
{
    if (!d)
        return;
    for (int i = 0; i < MAX_SURFACES; i++)
        freeSurface(d, &d->surf[i]);
    releaseBuffers(d);
    free(d);
}

const VP9FrameContext *vp9_decoder_frame_context(const VP9Decoder *d)
{
    return &d->fc;
}

const VP9State *vp9_decoder_state(const VP9Decoder *d)
{
    return &d->state;
}

const VP9FrameHeader *vp9_decoder_last_header(const VP9Decoder *d)
{
    return &d->header;
}

// averne's sizes (superblocks of the 16-aligned frame); the filter buffer by height, BSD control data at 912/988 of it.
static uint32_t superblocks(uint32_t w, uint32_t h)
{
    return ((ALIGN(w, 16) + 63) / 64) * ((ALIGN(h, 16) + 63) / 64);
}

// The tile size table's tail entries as averne's driver sets them (meaning undocumented); a frame's table is shorter.
static void tileTail(uint8_t *input)
{
    ((uint16_t *)(input + kTilesOff))[0x37a] = 9;
    ((uint16_t *)(input + kTilesOff))[0x37b] = 1;
}

int vp9_decoder_configure(VP9Decoder *d, uint32_t width, uint32_t height)
{
    if (width <= d->max_width && height <= d->max_height && d->aux)
        return 0;
    if (width < d->max_width)
        width = d->max_width;
    if (height < d->max_height)
        height = d->max_height;
    releaseBuffers(d);
    const uint32_t sb = superblocks(width, height);
    d->seg_bytes = ALIGN(sb * 32, 256);
    const uint32_t colmv = ALIGN(sb * 1024, 256);
    d->filter_rows = ALIGN(height, 64);
    d->seg_off[0] = 0;
    d->seg_off[1] = d->seg_bytes;
    d->seg_off[kSegClear] = 2 * d->seg_bytes;
    d->counters_off = 3 * d->seg_bytes;
    d->colmv_off[0] = d->counters_off + ALIGN(sizeof(nvdec_vp9EntropyCounts_t), 256);
    d->colmv_off[1] = d->colmv_off[0] + colmv;
    d->filter_off = d->colmv_off[1] + colmv;
    d->aux_bytes = d->filter_off + d->filter_rows * 988;
    void *va;
    if (d->b.alloc(d->b.ctx, d->aux_bytes, &d->aux, &va))
        return -3;
    d->aux_va = va;  // zeroed: segmentation maps, motion vectors and counters start clear
    if (nvdec_ring_alloc(&d->ring, &d->b, kBitsOff + ALIGN((uint64_t)width * height / 2 + 0x10000, 0x10000))) {
        releaseBuffers(d);
        return -3;
    }
    for (uint32_t i = 0; i < d->ring.count; i++)
        tileTail(d->ring.input[i].va);
    d->max_width = width;
    d->max_height = height;
    d->seg_read = kSegClear;
    d->colmv_write = 0;
    return 0;
}

// A surface no slot holds, big enough for width x height (NV12 block linear, 64-byte pitch, rows to whole superblocks:
// the engine writes 64x64 blocks).
static int takeSurface(VP9Decoder *d, uint32_t width, uint32_t height)
{
    const uint32_t pitch = ALIGN(width, 64), rows = ALIGN(height, 64);
    int empty = -1;
    for (int i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &d->surf[i];
        if (s->refs)
            continue;
        if (s->handle && s->pitch >= pitch && s->rows >= rows) {
            if (d->b.recycle) {
                void *address = s->va;
                if (d->b.recycle(d->b.ctx, s->handle, s->bytes, &address))
                    return -1;
                s->va = address;
            }
            return i;
        }
        if (empty < 0 || !d->surf[empty].handle)
            empty = i;
    }
    if (empty < 0)
        return -1;
    Surface *s = &d->surf[empty];
    freeSurface(d, s);
    s->pitch = pitch;
    s->rows = rows;
    s->luma_bytes = pitch * rows;
    s->bytes = s->luma_bytes + s->luma_bytes / 2;
    void *va;
    if (d->b.alloc(d->b.ctx, s->bytes, &s->handle, &va)) {
        memset(s, 0, sizeof(*s));
        return -1;
    }
    s->va = va;
    return empty;
}

static void fillProbs(nvdec_vp9EntropyProbs_t *p, const VP9FrameContext *fc, const VP9State *s)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            memcpy(p->kf_bmode_prob[i][j], vp9_kf_y_mode_prob[i][j], 8);
            p->kf_bmode_probB[i][j][0] = vp9_kf_y_mode_prob[i][j][8];
        }
        memcpy(p->kf_uv_mode_prob[i], vp9_kf_uv_mode_prob[i], 8);
        p->kf_uv_mode_probB[i][0] = vp9_kf_uv_mode_prob[i][8];
    }
    memcpy(p->mb_segment_tree_probs, s->seg.tree_probs, 7);
    if (s->seg.temporal_update)
        memcpy(p->segment_pred_probs, s->seg.pred_probs, 3);
    else
        memset(p->segment_pred_probs, 255, 3);
    nvdec_vp9AdaptiveEntropyProbs_t *a = &p->a;
    for (int i = 0; i < 7; i++)
        memcpy(a->inter_mode_prob[i], fc->inter_mode_probs[i], 3);
    memcpy(a->intra_inter_prob, fc->intra_inter_prob, 4);
    for (int i = 0; i < 10; i++) {
        memcpy(a->uv_mode_prob[i], fc->uv_mode_prob[i], 8);
        a->uv_mode_probB[i][0] = fc->uv_mode_prob[i][8];
    }
    for (int i = 0; i < 2; i++) {
        a->tx8x8_prob[i][0] = fc->tx.p8x8[i][0];
        memcpy(a->tx16x16_prob[i], fc->tx.p16x16[i], 2);
        memcpy(a->tx32x32_prob[i], fc->tx.p32x32[i], 3);
    }
    for (int i = 0; i < 4; i++) {
        memcpy(a->sb_ymode_prob[i], fc->y_mode_prob[i], 8);
        a->sb_ymode_probB[i][0] = fc->y_mode_prob[i][8];
    }
    for (int i = 0; i < 16; i++) {
        memcpy(a->partition_prob[0][i], vp9_kf_partition_probs[i], 3);
        memcpy(a->partition_prob[1][i], fc->partition_prob[i], 3);
    }
    memcpy(a->switchable_interp_prob, fc->switchable_interp_prob, sizeof(a->switchable_interp_prob));
    memcpy(a->comp_inter_prob, fc->comp_inter_prob, 5);
    memcpy(a->mbskip_probs, fc->skip_probs, 3);
    memcpy(a->nmvc.joints, fc->nmvc.joints, 3);
    for (int i = 0; i < 2; i++) {
        const VP9MVComponent *c = &fc->nmvc.comps[i];
        a->nmvc.sign[i] = c->sign;
        a->nmvc.class0[i][0] = c->class0[0];
        memcpy(a->nmvc.fp[i], c->fp, 3);
        a->nmvc.class0_hp[i] = c->class0_hp;
        a->nmvc.hp[i] = c->hp;
        memcpy(a->nmvc.classes[i], c->classes, 10);
        memcpy(a->nmvc.class0_fp[i], c->class0_fp, 6);
        memcpy(a->nmvc.bits[i], c->bits, 10);
    }
    memcpy(a->single_ref_prob, fc->single_ref_prob, sizeof(a->single_ref_prob));
    memcpy(a->comp_ref_prob, fc->comp_ref_prob, 5);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 6; k++)
                for (int l = 0; l < 6; l++) {
                    memcpy(a->probCoeffs[i][j][k][l], fc->coef_probs[0][i][j][k][l], 3);
                    memcpy(a->probCoeffs8x8[i][j][k][l], fc->coef_probs[1][i][j][k][l], 3);
                    memcpy(a->probCoeffs16x16[i][j][k][l], fc->coef_probs[2][i][j][k][l], 3);
                    memcpy(a->probCoeffs32x32[i][j][k][l], fc->coef_probs[3][i][j][k][l], 3);
                }
}

// The engine's counters (nvdec_vp9EntropyCounts_t) as libvpx's FRAME_COUNTS. Inter modes come per tree node:
// [ctx][0][0] ZEROMV, [ctx][1][0] NEARESTMV, [ctx][2][0] NEARMV, [ctx][2][1] NEWMV.
static void readCounts(VP9Counts *c, const nvdec_vp9EntropyCounts_t *n)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < 7; i++) {
        c->inter_mode[i][0] = n->inter_mode_counts[i][1][0];
        c->inter_mode[i][1] = n->inter_mode_counts[i][2][0];
        c->inter_mode[i][2] = n->inter_mode_counts[i][0][0];
        c->inter_mode[i][3] = n->inter_mode_counts[i][2][1];
    }
    memcpy(c->y_mode, n->sb_ymode_counts, sizeof(c->y_mode));
    memcpy(c->uv_mode, n->uv_mode_counts, sizeof(c->uv_mode));
    memcpy(c->partition, n->partition_counts, sizeof(c->partition));
    memcpy(c->switchable_interp, n->switchable_interp_counts, sizeof(c->switchable_interp));
    memcpy(c->intra_inter, n->intra_inter_count, sizeof(c->intra_inter));
    memcpy(c->comp_inter, n->comp_inter_count, sizeof(c->comp_inter));
    memcpy(c->single_ref, n->single_ref_count, sizeof(c->single_ref));
    memcpy(c->comp_ref, n->comp_ref_count, sizeof(c->comp_ref));
    memcpy(c->tx.p32x32, n->tx32x32_count, sizeof(c->tx.p32x32));
    memcpy(c->tx.p16x16, n->tx16x16_count, sizeof(c->tx.p16x16));
    memcpy(c->tx.p8x8, n->tx8x8_count, sizeof(c->tx.p8x8));
    memcpy(c->skip, n->mbskip_count, sizeof(c->skip));
    memcpy(c->mv.joints, n->nmvcount.joints, sizeof(c->mv.joints));
    for (int i = 0; i < 2; i++) {
        VP9MVComponentCounts *m = &c->mv.comps[i];
        memcpy(m->sign, n->nmvcount.sign[i], sizeof(m->sign));
        memcpy(m->classes, n->nmvcount.classes[i], sizeof(m->classes));
        memcpy(m->class0, n->nmvcount.class0[i], sizeof(m->class0));
        memcpy(m->bits, n->nmvcount.bits[i], sizeof(m->bits));
        memcpy(m->class0_fp, n->nmvcount.class0_fp[i], sizeof(m->class0_fp));
        memcpy(m->fp, n->nmvcount.fp[i], sizeof(m->fp));
        memcpy(m->class0_hp, n->nmvcount.class0_hp[i], sizeof(m->class0_hp));
        memcpy(m->hp, n->nmvcount.hp[i], sizeof(m->hp));
    }
    memcpy(c->coef[0], n->countCoeffs, sizeof(c->coef[0]));
    memcpy(c->coef[1], n->countCoeffs8x8, sizeof(c->coef[1]));
    memcpy(c->coef[2], n->countCoeffs16x16, sizeof(c->coef[2]));
    memcpy(c->coef[3], n->countCoeffs32x32, sizeof(c->coef[3]));
    memcpy(c->eob_branch, n->countEobs, sizeof(c->eob_branch));
}

_Static_assert(offsetof(nvdec_vp9_pic_s, ref0_stride) == offsetof(nvdec_vp9_pic_s, ref0_width) + 4, "ref0 group");
_Static_assert(offsetof(nvdec_vp9_pic_s, ref1_stride) == offsetof(nvdec_vp9_pic_s, ref1_width) + 4, "ref1 group");
_Static_assert(offsetof(nvdec_vp9_pic_s, ref2_stride) == offsetof(nvdec_vp9_pic_s, ref2_width) + 4, "ref2 group");

// NVDEC numbers the frame's interpolation filter as the header's 2-bit literal does: 0 smooth, 1 regular, 2 sharp,
// 3 bilinear, 4 switchable (FFmpeg's FilterMode, averne's mapping), not libvpx's INTERP_FILTER (0 regular, 1 smooth).
// On hardware, clip A's frame 7 (fixed regular filter) sent as libvpx's 0 decoded to luma CRC 0xbde1bb6d, which is
// libvpx's output with that frame's literal rewritten to 0 (smooth); frames with switchable filters matched.
static const char kNvdecFilter[5] = {1, 0, 2, 3, 4};  // indexed by libvpx EIGHTTAP, SMOOTH, SHARP, BILINEAR, SWITCHABLE

static void fillSetup(const VP9Decoder *d, nvdec_vp9_pic_s *p, const VP9FrameHeader *h, const Surface *cur,
                      const Surface *refs[3], uint32_t streamLen)
{
    const VP9State *s = &d->state;
    memset(p, 0, sizeof(*p));
    p->stream_len = streamLen;
    p->Vp9BsdCtrlOffset = d->filter_rows * 912 / 256;
    // Each refN group is width, height, stride[2], adjacent shorts (asserted below).
    unsigned short *dims[3] = {&p->ref0_width, &p->ref1_width, &p->ref2_width};
    for (int i = 0; i < 3; i++) {
        dims[i][0] = refs[i]->width;
        dims[i][1] = refs[i]->height;
        dims[i][2] = dims[i][3] = (unsigned short)refs[i]->pitch;
    }
    p->width = h->width;
    p->height = h->height;
    p->framestride[0] = p->framestride[1] = (unsigned short)cur->pitch;
    p->keyFrame = h->key_frame;
    p->prevIsKeyFrame = d->have_last && d->last_key;
    p->resolutionChange = d->have_last && (h->width != d->last_width || h->height != d->last_height);
    p->errorResilient = h->error_resilient;
    p->prevShowFrame = d->have_last && d->last_show;
    p->intraOnly = h->intra_only;
    for (int i = 1; i < 4; i++)
        p->refFrameSignBias[i] = h->sign_bias[i];
    p->loopFilterLevel = (char)h->lf_level;
    p->loopFilterSharpness = (char)h->lf_sharpness;
    p->qpYAc = h->base_qindex;
    p->qpYDc = h->y_dc_delta_q;
    p->qpChAc = h->uv_ac_delta_q;
    p->qpChDc = h->uv_dc_delta_q;
    p->lossless = h->lossless;
    p->transform_mode = (char)h->tx_mode;
    p->allow_high_precision_mv = !h->key_frame && h->allow_high_precision_mv;
    p->mcomp_filter_type = kNvdecFilter[h->interp_filter];
    p->comp_pred_mode = (char)h->reference_mode;
    p->comp_fixed_ref = (char)h->comp_fixed_ref;
    p->comp_var_ref[0] = (char)h->comp_var_ref[0];
    p->comp_var_ref[1] = (char)h->comp_var_ref[1];
    p->log2_tile_columns = (char)h->log2_tile_cols;
    p->log2_tile_rows = (char)h->log2_tile_rows;
    p->segmentEnabled = s->seg.enabled;
    p->segmentMapUpdate = s->seg.update_map;
    p->segmentMapTemporalUpdate = s->seg.temporal_update;
    p->segmentFeatureMode = s->seg.abs_delta;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++)
            p->segmentFeatureEnable[i][j] = s->seg.feature_enabled[i][j];
        for (int j = 0; j < 3; j++)
            p->segmentFeatureData[i][j] = s->seg.feature_data[i][j];
    }
    p->modeRefLfEnabled = h->lf_delta_enabled;
    memcpy(p->mbRefLfDelta, s->ref_deltas, 4);
    memcpy(p->mbModeLfDelta, s->mode_deltas, 2);
}

static void tileSizes(uint16_t *sizes, const VP9FrameHeader *h)
{
    const uint32_t sbCols = (h->width + 63u) / 64, sbRows = (h->height + 63u) / 64;
    const unsigned rows = 1u << h->log2_tile_rows, cols = 1u << h->log2_tile_cols;
    for (unsigned i = 0; i < rows; i++)
        for (unsigned j = 0; j < cols; j++) {
            *sizes++ = (uint16_t)((sbCols * (j + 1) >> h->log2_tile_cols) - (sbCols * j >> h->log2_tile_cols));
            *sizes++ = (uint16_t)((sbRows * (i + 1) >> h->log2_tile_rows) - (sbRows * i >> h->log2_tile_rows));
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

static void output(VP9Decoder *d, const Surface *s, uint32_t job, void *token)
{
    const H264Picture picture = {
        .luma = s->va,
        .chroma = s->va + s->luma_bytes,
        .pitch = s->pitch,
        .handle = s->handle,
        .chroma_offset = s->luma_bytes,
        .rows = s->rows,
        .coded_width = s->width,
        .coded_height = s->height,
        .width = s->width,
        .height = s->height,
        .job = job,
        .token = token,
    };
    void *item = d->b.decoded(d->b.ctx, &picture);
    if (item)
        d->b.emit(d->b.ctx, item);
}

static void refresh(VP9Decoder *d, const VP9FrameContext *fc, bool contexts, uint8_t idx)
{
    if (contexts)
        d->state.contexts[idx] = *fc;
    d->fc = *fc;
}

int vp9_decoder_settle(VP9Decoder *d)
{
    Adaptation *a = &d->adaptation;
    if (!a->waiting)
        return 0;
    a->waiting = false;
    TegraNVDECWait wait = {.fence = a->fence, .invalidate_count = 1};
    wait.invalidate[0] = (TegraNVDECRange){d->aux, d->counters_off, sizeof(nvdec_vp9EntropyCounts_t), 0};
    if (d->b.wait(d->b.ctx, &wait))
        return -2;
    VP9Counts counts;
    readCounts(&counts, (const nvdec_vp9EntropyCounts_t *)(d->aux_va + d->counters_off));
    vp9_adapt_coef_probs(&a->fc, &a->pre, &counts, a->intra, a->lastKey);
    if (!a->intra) {
        vp9_adapt_mode_probs(&a->fc, &a->pre, &counts, a->interpFilter, a->txMode);
        vp9_adapt_mv_probs(&a->fc, &a->pre, &counts, a->allowHP);
    }
    refresh(d, &a->fc, a->refresh, a->contextIdx);
    return 0;
}

static int decodeFrame(VP9Decoder *d, const uint8_t *data, size_t size, void *token, bool *shown)
{
    VP9State *st = &d->state;
    VP9FrameHeader h;
    const int adapted = vp9_decoder_settle(d);
    if (adapted)
        return adapted;
    if (vp9_parse_uncompressed(st, data, size, &h))
        return -1;
    // One sample carries one frame token: a second shown frame in a superframe (not produced by encoders) is dropped.
    if (h.show_existing_frame) {  // libvpx: no decode and no state change, the slot's frame is shown again
        if (!*shown)
            output(d, &d->surf[d->slot[h.frame_to_show]], 0, token);
        *shown = true;
        return 0;
    }
    if (h.width < 144 || h.height < 144)
        return -1;  // NVDEC's VP9 floor (averne); the session start refuses it too
    if ((h.width > d->max_width || h.height > d->max_height) && vp9_decoder_configure(d, h.width, h.height))
        return -3;
    VP9FrameContext pre = st->contexts[h.frame_context_idx], fc = pre;
    const uint8_t *compressed = data + h.uncompressed_bytes, *tiles = compressed + h.compressed_bytes;
    const size_t tileBytes = size - h.uncompressed_bytes - h.compressed_bytes;
    if (vp9_parse_compressed(&h, compressed, h.compressed_bytes, &fc) || !tileBytes)
        return -1;

    const int curIdx = takeSurface(d, h.width, h.height);
    if (curIdx < 0)
        return -3;
    Surface *cur = &d->surf[curIdx];
    cur->width = h.width;
    cur->height = h.height;
    const bool intra = h.key_frame || h.intra_only;
    const Surface *refs[3];
    for (int i = 0; i < 3; i++)
        refs[i] = intra ? cur : &d->surf[d->slot[h.ref_idx[i]]];

    // vp9_setup_past_independence clears the segmentation map; NVDEC reads it from the read buffer.
    if (h.key_frame || h.intra_only || h.error_resilient)
        d->seg_read = kSegClear;
    if (kBitsOff + tileBytes + 64 > nvdec_ring_current(&d->ring)->bytes &&
        nvdec_ring_grow(&d->ring, &d->b, kBitsOff + tileBytes + 64, kBitsOff))
        return -3;
    const NVDECInput *input = nvdec_ring_current(&d->ring);
    memcpy(input->va + kBitsOff, tiles, tileBytes);
    memset(input->va + kBitsOff + tileBytes, 0, 64);
    fillProbs((nvdec_vp9EntropyProbs_t *)(input->va + kProbOff), &fc, st);
    tileSizes((uint16_t *)(input->va + kTilesOff), &h);

    TegraNVDECSubmit submit;
    memset(&submit, 0, sizeof(submit));
    submit.codec = TEGRA_NVDEC_CODEC_VP9;
    submit.setup_bytes = sizeof(nvdec_vp9_pic_s);
    fillSetup(d, (nvdec_vp9_pic_s *)submit.setup, &h, cur, refs, (uint32_t)tileBytes);
    const uint8_t segWrite = d->seg_read == 0 ? 1 : 0, colmvRead = d->colmv_write ^ 1;
    entry(&submit, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_VP9, 0);
    entry(&submit, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
          NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_VP9 | (1u << 4) /* GPTIMER_ON */ | (1u << 6) /* ERR_CONCEAL_ON */, 0);
    entry(&submit, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, d->picture_index++, 0);
    entry(&submit, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kBitsOff);
    entry(&submit, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kStatusOff);
    entry(&submit, NVC5B0_VP9_SET_PROB_TAB_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kProbOff);
    entry(&submit, NVC5B0_VP9_SET_CTX_COUNTER_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->counters_off);
    entry(&submit, NVC5B0_VP9_SET_TILE_SIZE_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input->handle, kTilesOff);
    entry(&submit, NVC5B0_VP9_SET_COL_MVWRITE_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux,
          d->colmv_off[d->colmv_write]);
    entry(&submit, NVC5B0_VP9_SET_COL_MVREAD_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->colmv_off[colmvRead]);
    entry(&submit, NVC5B0_VP9_SET_SEGMENT_READ_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->seg_off[d->seg_read]);
    entry(&submit, NVC5B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->seg_off[segWrite]);
    entry(&submit, NVC5B0_VP9_SET_FILTER_BUFFER_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->filter_off);
    const Surface *pics[4] = {refs[0], refs[1], refs[2], cur};  // LAST, GOLDEN, ALTREF, current (VP9_FRAME_SFC_ID)
    for (uint16_t i = 0; i < 4; i++) {
        entry(&submit, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, pics[i]->handle,
              0);
        entry(&submit, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, pics[i]->handle,
              pics[i]->luma_bytes);
    }
    entry(&submit, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8 /* AWAKEN */, 0);
    // The CPU wrote the probability table, the tile sizes and the tile data: one range of the input buffer.
    submit.clean[0] = (TegraNVDECRange){input->handle, kProbOff, (uint32_t)(kBitsOff - kProbOff + tileBytes + 64), 0};
    submit.clean_count = 1;
    uint32_t fence = 0;
    if (d->b.submit(d->b.ctx, &submit, &fence))
        return -2;
    nvdec_ring_advance(&d->ring);

    // Backward adaptation from the engine's counts (vp9_decode_frame's tail), then the context refresh: NVDEC 2.0
    // leaves the adaptation to the host, so the next frame waits for this one's counts (vp9_decoder_settle).
    if (!h.error_resilient && !h.frame_parallel)
        d->adaptation = (Adaptation){true,
                                     intra,
                                     d->have_last && d->last_key,
                                     h.refresh_frame_context,
                                     h.allow_high_precision_mv,
                                     h.interp_filter,
                                     h.tx_mode,
                                     h.frame_context_idx,
                                     fence,
                                     pre,
                                     fc};
    else
        refresh(d, &fc, h.refresh_frame_context, h.frame_context_idx);
    d->header = h;

    // The reference slots (swap_frame_buffers), then the buffers that alternate.
    for (int i = 0; i < 8; i++) {
        if (!(h.refresh_frame_flags & (1u << i)))
            continue;
        if (d->slot[i] >= 0)
            d->surf[d->slot[i]].refs--;
        d->slot[i] = (int8_t)curIdx;
        cur->refs++;
        st->ref_width[i] = h.width;
        st->ref_height[i] = h.height;
    }
    if (st->seg.enabled && st->seg.update_map)
        d->seg_read = segWrite;
    d->colmv_write ^= 1;
    d->have_last = true;
    d->last_show = h.show_frame;
    d->last_key = h.key_frame;
    d->last_width = h.width;
    d->last_height = h.height;
    if (h.show_frame) {
        if (!*shown)
            output(d, cur, fence, token);
        *shown = true;
    }
    return 0;
}

int vp9_decoder_decode(VP9Decoder *d, const uint8_t *data, size_t size, void *token)
{
    if (!d->aux)
        return -3;
    size_t offsets[8], sizes[8];
    const unsigned frames = vp9_superframe_split(data, size, offsets, sizes);
    if (!frames)
        return -1;
    bool shown = false;
    for (unsigned i = 0; i < frames; i++) {
        if (!sizes[i])
            continue;
        const int ret = decodeFrame(d, data + offsets[i], sizes[i], token, &shown);
        if (ret)
            return ret;
    }
    return shown ? 1 : 0;
}
