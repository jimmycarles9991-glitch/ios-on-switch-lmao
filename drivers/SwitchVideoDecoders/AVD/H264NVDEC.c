// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264 on NVDEC 2.0; see H264NVDEC.h. Clause numbers are ITU-T H.264 (08/2021).
#include "H264NVDEC.h"
#include <stdlib.h>
#include "H264DPB.h"
#include "NVDECRing.h"
#include <string.h>
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"

#define MAX_SURFACES 17
#define MAX_PENDING  34
#define ALIGN(v, a)  (((v) + (a) - 1) / (a) * (a))

typedef struct {
    int32_t poc;
    void *item;
} Pending;

struct H264Decoder {
    H264Backend b;
    H264ParamSets sets;
    H264SPS sps;  // the active SPS
    bool configured;
    // Buffers for the configured geometry
    uint32_t width_mbs, height_mbs, refs_allocated;
    uint32_t pitch, aligned_height, luma_bytes, surface_bytes, surfaces;
    uint32_t surface[MAX_SURFACES];
    uint8_t *surface_va[MAX_SURFACES];
    uint32_t aux, mbhist_off, mbhist_bytes, history_off, history_bytes;
    uint32_t slices_off, bits_off, max_slices;
    NVDECRing ring;  // each picture's status record, slice offsets and bitstream
    H264DPB dpb;
    uint8_t last_pic_idx;
    // A first field's sample and its job, until its frame goes out; refused: its second field's job was refused.
    void *held_token;
    uint32_t held_job;
    bool held_refused;
    // Display order
    Pending pending[MAX_PENDING];
    uint32_t pending_count, reorder;
    // The access unit being assembled
    uint32_t bits_len, slices;
    uint32_t picture_index;
};

H264Decoder *h264_decoder_create(const H264Backend *backend)
{
    H264Decoder *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->b = *backend;
    return d;
}

static void releaseBuffers(H264Decoder *d)
{
    for (uint32_t i = 0; i < d->surfaces; i++)
        d->b.free(d->b.ctx, d->surface[i]);
    if (d->aux)
        d->b.free(d->b.ctx, d->aux);
    nvdec_ring_release(&d->ring, &d->b);
    d->surfaces = d->aux = 0;
    d->configured = false;
}

void h264_decoder_destroy(H264Decoder *d)
{
    if (!d)
        return;
    for (uint32_t i = 0; i < d->pending_count; i++)
        d->b.discard(d->b.ctx, d->pending[i].item);
    releaseBuffers(d);
    free(d);
}

int h264_decoder_parameter_set(H264Decoder *d, const uint8_t *nal, size_t size)
{
    if (!size)
        return -1;
    switch (nal[0] & 0x1f) {
    case H264_NAL_SPS:
        return h264_parse_sps(&d->sets, nal, size, NULL);
    case H264_NAL_PPS:
        return h264_parse_pps(&d->sets, nal, size, NULL);
    default:
        return 0;
    }
}

const H264SPS *h264_decoder_sps(const H264Decoder *d)
{
    return d->configured ? &d->sps : NULL;
}

// Table A-1 MaxDpbMbs.
static uint32_t maxDpbFrames(const H264SPS *s)
{
    uint32_t mbs;
    switch (s->level_idc) {
    case 9:
    case 10:
        mbs = 396;
        break;
    case 11:
        mbs = (s->constraint_flags & 0x10) && s->profile_idc != 100 ? 396 : 900;
        break;  // level 1b
    case 12:
    case 13:
    case 20:
        mbs = 2376;
        break;
    case 21:
        mbs = 4752;
        break;
    case 22:
    case 30:
        mbs = 8100;
        break;
    case 31:
        mbs = 18000;
        break;
    case 32:
        mbs = 20480;
        break;
    case 40:
    case 41:
        mbs = 32768;
        break;
    case 42:
        mbs = 34816;
        break;
    case 50:
        mbs = 110400;
        break;
    case 51:
    case 52:
        mbs = 184320;
        break;
    default:
        mbs = 696320;
        break;
    }
    const uint32_t frames = mbs / ((uint32_t)s->width_mbs * s->height_mbs);
    return frames < 1 ? 1 : frames > 16 ? 16 : frames;
}

// How many decoded pictures may wait for display (C.4.5.3 with max_num_reorder_frames): the VUI's count, none for
// Baseline and intra-only profiles, else the level's whole DPB.
static uint32_t reorderDepth(const H264SPS *s)
{
    if (s->bitstream_restriction)
        return s->max_num_reorder_frames;
    if (s->profile_idc == 66 || ((s->constraint_flags & 0x10) && s->profile_idc >= 100))
        return 0;
    return maxDpbFrames(s);
}

static bool supported(const H264SPS *s)
{
    return s->valid && s->chroma_format_idc == 1 && s->bit_depth_luma == 8 && s->bit_depth_chroma == 8 &&
           !s->separate_colour_plane && s->width_mbs >= 3 && s->width_mbs * 16 <= 4096 && s->height_mbs * 16 <= 4096;
}

// Picture buffers as averne's decoder sizes them: block-linear NV12 surfaces (pitch 64-aligned, height 32-aligned),
// one colocated-motion area per surface, the MB history and the history buffer, and the input buffers (NVDECRing.h),
// each holding a picture's status record, slice offsets and bitstream.
static int allocate(H264Decoder *d, const H264SPS *s)
{
    releaseBuffers(d);
    d->width_mbs = s->width_mbs;
    d->height_mbs = s->height_mbs;
    d->refs_allocated = s->max_num_ref_frames;
    d->pitch = ALIGN(d->width_mbs * 16, 64);
    d->aligned_height = ALIGN(d->height_mbs * 16, 32);
    d->luma_bytes = d->pitch * d->aligned_height;
    d->surface_bytes = d->luma_bytes + d->luma_bytes / 2;
    const uint32_t count = s->max_num_ref_frames + 1 < 2 ? 2 : s->max_num_ref_frames + 1;
    for (uint32_t i = 0; i < count; i++) {
        void *va;
        if (d->b.alloc(d->b.ctx, d->surface_bytes, &d->surface[i], &va))
            return -1;
        d->surface_va[i] = va;
        d->surfaces = i + 1;
    }
    const uint32_t coloc = ALIGN(ALIGN(d->height_mbs, 2) * d->width_mbs * 64 - 63, 256);
    d->mbhist_bytes = ALIGN(d->width_mbs * 104, 256);
    d->history_bytes = ALIGN(d->width_mbs * 0x200 + 0x1100, 0x200);
    d->mbhist_off = coloc * count;
    d->history_off = d->mbhist_off + d->mbhist_bytes;
    void *va;
    if (d->b.alloc(d->b.ctx, d->history_off + d->history_bytes, &d->aux, &va))
        return -1;
    d->max_slices = d->width_mbs * d->height_mbs;
    d->slices_off = 256;
    d->bits_off = ALIGN(d->slices_off + (d->max_slices + 1) * 4, 256);
    return nvdec_ring_alloc(&d->ring, &d->b, d->bits_off + (d->width_mbs * d->height_mbs >= 3600 ? 0x100000 : 0x40000));
}

// The first SPS a PPS refers to (avcC carries one of each in practice).
int h264_decoder_configure(H264Decoder *d)
{
    for (unsigned i = 0; i < 256; i++) {
        if (!d->sets.pps[i].valid)
            continue;
        const H264SPS *s = &d->sets.sps[d->sets.pps[i].sps_id];
        if (!supported(s) || d->sets.pps[i].num_slice_groups != 1)
            return -1;
        d->sps = *s;
        if (allocate(d, s)) {
            releaseBuffers(d);
            return -3;
        }
        h264_dpb_reset(&d->dpb, s);
        d->configured = true;
        return 0;
    }
    return -1;
}

static void emitFirst(H264Decoder *d)
{
    uint32_t best = 0;
    for (uint32_t i = 1; i < d->pending_count; i++)
        if (d->pending[i].poc < d->pending[best].poc)
            best = i;
    void *item = d->pending[best].item;
    memmove(&d->pending[best], &d->pending[best + 1], (d->pending_count - best - 1) * sizeof(Pending));
    d->pending_count--;
    d->b.emit(d->b.ctx, item);
}

static void bump(H264Decoder *d, uint32_t keep)
{
    while (d->pending_count > keep)
        emitFirst(d);
}

static void discardPending(H264Decoder *d)
{
    for (uint32_t i = 0; i < d->pending_count; i++)
        d->b.discard(d->b.ctx, d->pending[i].item);
    d->pending_count = 0;
}

// How the frame's rows interleave in time: a field pair's first field first; a frame's earlier field first.
static uint8_t fieldOrder(const H264Decoder *d, uint8_t firstField, int32_t top, int32_t bottom)
{
    if (d->sps.frame_mbs_only)
        return AVD_FIELDS_UNSAID;
    if (firstField != H264_FRAME)
        return firstField == H264_TOP ? AVD_FIELDS_TOP_FIRST : AVD_FIELDS_BOTTOM_FIRST;
    return top < bottom ? AVD_FIELDS_TOP_FIRST : top > bottom ? AVD_FIELDS_BOTTOM_FIRST : AVD_FIELDS_PROGRESSIVE;
}

// The frame in surface picIdx to `decoded`, then into the display queue: decoded by `job` (and `firstField`'s job).
static void output(H264Decoder *d, uint8_t picIdx, void *token, int32_t poc, uint8_t fields, uint32_t job,
                   uint32_t firstField, bool refused)
{
    const H264Picture picture = {
        .luma = d->surface_va[picIdx],
        .chroma = d->surface_va[picIdx] + d->luma_bytes,
        .pitch = d->pitch,
        .handle = d->surface[picIdx],
        .chroma_offset = d->luma_bytes,
        .rows = d->aligned_height,
        .coded_width = d->width_mbs * 16,
        .coded_height = d->height_mbs * 16,
        .crop_x = d->sps.crop_left,
        .crop_y = d->sps.crop_top,
        .width = d->width_mbs * 16 - d->sps.crop_left - d->sps.crop_right,
        .height = d->height_mbs * 16 - d->sps.crop_top - d->sps.crop_bottom,
        .fields = fields,
        .poc = poc,
        .job = job,
        .first_field_job = firstField,
        .refused = refused,
        .token = token,
    };
    void *item = d->b.decoded(d->b.ctx, &picture);
    if (!item)
        return;
    if (d->pending_count == MAX_PENDING)
        emitFirst(d);
    d->pending[d->pending_count++] = (Pending){poc, item};
}

// A first field whose second field did not follow goes out alone, as its frame, under its own sample.
static void finishUnpaired(H264Decoder *d)
{
    H264DPB *dpb = &d->dpb;
    if (!dpb->pairing)
        return;
    dpb->pairing = false;
    output(d, dpb->pair_idx, d->held_token, dpb->pair_poc,
           fieldOrder(d, dpb->pair_parity, dpb->pair_poc, dpb->pair_poc), d->held_job, 0, d->held_refused);
}

void h264_decoder_flush(H264Decoder *d)
{
    finishUnpaired(d);
    bump(d, 0);
}

static void entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    TegraNVDECEntry *e = &s->entries[s->entry_count++];
    e->method = method;
    e->kind = kind;
    e->value = value;
    e->offset = offset;
}

static void fillSetup(const H264Decoder *d, nvdec_h264_pic_s *p, const H264Slice *sh, const H264Current *cur)
{
    const H264SPS *s = &d->sps;
    const H264PPS *pps = &d->sets.pps[sh->pps_id];
    memset(p, 0, sizeof(*p));
    p->stream_len = d->bits_len + 16;
    p->slice_count = d->slices;
    p->mbhist_buffer_size = d->mbhist_bytes;
    p->log2_max_pic_order_cnt_lsb_minus4 = s->poc_type == 0 ? s->log2_max_poc_lsb - 4 : 0;
    p->delta_pic_order_always_zero_flag = s->delta_pic_order_always_zero;
    p->frame_mbs_only_flag = s->frame_mbs_only;
    p->PicWidthInMbs = (int)d->width_mbs;
    p->FrameHeightInMbs = (int)d->height_mbs;
    p->tileFormat = 0;  // TBL
    p->gob_height = 0;  // GOB_2
    p->entropy_coding_mode_flag = pps->entropy_coding_mode;
    p->pic_order_present_flag = pps->bottom_field_pic_order_present;
    p->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_active[0] - 1;
    p->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_active[1] - 1;
    p->deblocking_filter_control_present_flag = pps->deblocking_filter_control_present;
    p->redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present;
    p->transform_8x8_mode_flag = pps->transform_8x8_mode;
    p->pitch_luma = p->pitch_chroma = d->pitch;
    p->HistBufferSize = d->history_bytes / 256;
    p->direct_8x8_inference_flag = s->direct_8x8_inference;
    p->weighted_pred_flag = pps->weighted_pred;
    p->constrained_intra_pred_flag = pps->constrained_intra_pred;
    p->ref_pic_flag = sh->nal_ref_idc != 0;
    p->log2_max_frame_num_minus4 = s->log2_max_frame_num - 4;
    p->chroma_format_idc = s->chroma_format_idc;
    p->pic_order_cnt_type = s->poc_type;
    p->pic_init_qp_minus26 = pps->pic_init_qp - 26;
    p->chroma_qp_index_offset = pps->chroma_qp_index_offset;
    p->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
    p->weighted_bipred_idc = pps->weighted_bipred_idc;
    // Both fields of a frame go into one surface and one colocated slot; the second names its first's surface with
    // second_field set and the other parity (libnvtvmr 0x18e18-0x18e40, 0x1a2a0-0x1a2dc; averne's nvtegra_h264.c:242).
    p->MbaffFrameFlag = s->mb_adaptive_frame_field && cur->structure == H264_FRAME;
    p->field_pic_flag = cur->structure != H264_FRAME;
    p->bottom_field_flag = cur->structure == H264_BOTTOM;
    p->second_field = cur->second;
    p->CurrPicIdx = cur->pic_idx;
    p->CurrColIdx = cur->pic_idx;
    p->frame_num = sh->frame_num;
    p->output_memory_layout = 0;  // NV12
    h264_dpb_fill(&d->dpb, cur, p->dpb, p->CurrFieldOrderCnt);
    memcpy(p->WeightScale, pps->scaling4, sizeof(p->WeightScale));
    memcpy(p->WeightScale8x8, pps->scaling8, sizeof(p->WeightScale8x8));
    p->lossless_ipred8x8_filter_enable = 1;
    p->qpprime_y_zero_transform_bypass_flag = s->transform_bypass;
}

// The method stream of averne's nvtegra_h264_prepare_cmdbuf, as relocations the kernel resolves.
static void fillSubmit(H264Decoder *d, TegraNVDECSubmit *s, uint8_t picIdx)
{
    s->codec = TEGRA_NVDEC_CODEC_H264;
    s->setup_bytes = sizeof(nvdec_h264_pic_s);
    entry(s, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_H264, 0);
    entry(s, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
          NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_H264 | (1u << 4) /* GPTIMER_ON */ | (1u << 6) /* ERR_CONCEAL_ON */, 0);
    entry(s, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, d->picture_index++, 0);
    const uint32_t input = nvdec_ring_current(&d->ring)->handle;
    entry(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input, d->bits_off);
    entry(s, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input, d->slices_off);
    entry(s, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, input, 0);
    entry(s, NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, 0);
    entry(s, NVC5B0_H264_SET_MBHIST_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->mbhist_off);
    entry(s, NVC5B0_SET_HISTORY_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->history_off);
    for (uint16_t i = 0; i < MAX_SURFACES; i++) {
        const uint32_t surface = d->surface[i < d->surfaces ? i : picIdx];
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, surface, 0);
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, surface,
              d->luma_bytes);
    }
    entry(s, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8 /* AWAKEN */, 0);
    s->clean[0] = (TegraNVDECRange){input, d->slices_off, (d->slices + 1) * 4, 0};
    s->clean[1] = (TegraNVDECRange){input, d->bits_off, d->bits_len + 16, 0};
    s->clean_count = 2;
}

static int freeSurface(const H264Decoder *d)
{
    for (uint32_t i = 0; i < d->surfaces; i++)
        if (!h264_dpb_holds(&d->dpb, (uint8_t)i))
            return (int)i;
    return -1;
}

// One picture: a frame, or one field. Returns 1 when the picture took `token` (a frame, or a first field whose frame
// goes out later under it), 0 when it did not (a second field: its frame goes out under its first field's sample),
// -1 for no surface, -2 when the NVDEC job failed, -3 when a surface could not be recycled.
static int decodePicture(H264Decoder *d, const H264Slice *sh, void *token, bool reorder)
{
    H264DPB *dpb = &d->dpb;
    const bool second = h264_dpb_second_field(dpb, sh);
    const uint32_t maxFrameNum = 1u << d->sps.log2_max_frame_num;
    if (!second)
        finishUnpaired(d);
    if (sh->nal_unit_type == H264_NAL_IDR) {
        if (sh->no_output_of_prior_pics)
            discardPending(d);
        else
            bump(d, 0);
        h264_dpb_reset(dpb, &d->sps);
    } else if (!second && sh->frame_num != dpb->prev_ref_frame_num &&
               sh->frame_num != (dpb->prev_ref_frame_num + 1) % maxFrameNum) {
        h264_dpb_fill_gap(dpb, sh->frame_num, d->last_pic_idx);
    }
    H264Current cur;
    h264_dpb_order(dpb, &d->sps, sh, &cur);

    int picIdx = second ? dpb->pair_idx : freeSurface(d);
    if (picIdx < 0) {  // more references than declared: make room
        h264_dpb_drop_oldest(dpb, sh->frame_num);
        if ((picIdx = freeSurface(d)) < 0)
            return -1;
    }
    if (!second && d->b.recycle) {
        void *address = d->surface_va[picIdx];
        if (d->b.recycle(d->b.ctx, d->surface[picIdx], d->surface_bytes, &address))
            return -3;
        d->surface_va[picIdx] = address;
    }
    cur.pic_idx = (uint8_t)picIdx;
    static const uint8_t endOfStream[16] = {0, 0, 1, 0x0b, 0, 0, 0, 0, 0, 0, 1, 0x0b, 0, 0, 0, 0};
    const NVDECInput *input = nvdec_ring_current(&d->ring);
    memcpy(input->va + d->bits_off + d->bits_len, endOfStream, sizeof(endOfStream));
    ((uint32_t *)(input->va + d->slices_off))[d->slices] = d->bits_len;

    TegraNVDECSubmit submit;
    memset(&submit, 0, sizeof(submit));
    fillSetup(d, (nvdec_h264_pic_s *)submit.setup, sh, &cur);
    fillSubmit(d, &submit, (uint8_t)picIdx);
    uint32_t fence = 0;
    if (d->b.submit(d->b.ctx, &submit, &fence)) {
        if (second) {  // the first field's frame goes out marked bad, so its sample is answered
            d->held_refused = true;
            finishUnpaired(d);
        }
        return -2;
    }
    nvdec_ring_advance(&d->ring);
    d->last_pic_idx = (uint8_t)picIdx;
    if (h264_dpb_finish(dpb, sh, &cur))
        bump(d, 0);  // C.4.4: memory_management_control_operation 5 infers no_output_of_prior_pics_flag 0
    if (dpb->pairing) {
        d->held_token = token;
        d->held_job = fence;
        d->held_refused = false;
        return 1;
    }
    const int32_t poc = cur.poc[0] < cur.poc[1] ? cur.poc[0] : cur.poc[1];
    const uint8_t first = second ? (uint8_t)(H264_FRAME ^ cur.structure) : H264_FRAME;
    output(d, (uint8_t)picIdx, second ? d->held_token : token, poc, fieldOrder(d, first, cur.poc[0], cur.poc[1]), fence,
           second ? d->held_job : 0, false);
    bump(d, reorder ? d->reorder : 0);
    return !second;
}

static int appendSlice(H264Decoder *d, const uint8_t *nal, size_t size)
{
    if (d->slices >= d->max_slices)
        return -1;
    const uint64_t needed = (uint64_t)d->bits_off + d->bits_len + 3 + size + 16;
    if (needed > nvdec_ring_current(&d->ring)->bytes &&
        nvdec_ring_grow(&d->ring, &d->b, needed, (uint64_t)d->bits_off + d->bits_len))
        return -1;
    uint8_t *input = nvdec_ring_current(&d->ring)->va;
    ((uint32_t *)(input + d->slices_off))[d->slices++] = d->bits_len;
    uint8_t *dst = input + d->bits_off + d->bits_len;
    dst[0] = dst[1] = 0;
    dst[2] = 1;
    memcpy(dst + 3, nal, size);
    d->bits_len += (uint32_t)size + 3;
    return 0;
}

static bool sameGeometry(const H264Decoder *d, const H264SPS *s)
{
    return s->width_mbs == d->width_mbs && s->height_mbs == d->height_mbs && s->max_num_ref_frames <= d->refs_allocated;
}

// The picture whose slices were collected, under its own SPS: decodePicture's answer.
static int finishPicture(H264Decoder *d, const H264Slice *first, void *token, bool reorder)
{
    const H264PPS *pps = &d->sets.pps[first->pps_id];
    const H264SPS *sps = &d->sets.sps[pps->sps_id];
    if (pps->num_slice_groups != 1 || !supported(sps))
        return -1;
    if (!sameGeometry(d, sps)) {  // new sequence with other dimensions: start over at its IDR
        if (first->nal_unit_type != H264_NAL_IDR)
            return -1;
        finishUnpaired(d);
        bump(d, 0);
        const uint32_t bits = d->bits_len, slices = d->slices;
        uint8_t *saved = malloc(bits + (slices + 1) * 4);
        if (!saved)
            return -1;
        const uint8_t *input = nvdec_ring_current(&d->ring)->va;
        memcpy(saved, input + d->bits_off, bits);
        memcpy(saved + bits, input + d->slices_off, (slices + 1) * 4);
        const int ret = allocate(d, sps);
        const NVDECInput *fresh = nvdec_ring_current(&d->ring);
        if (!ret && (uint64_t)d->bits_off + bits + 16 <= fresh->bytes && slices <= d->max_slices) {
            memcpy(fresh->va + d->bits_off, saved, bits);
            memcpy(fresh->va + d->slices_off, saved + bits, (slices + 1) * 4);
        }
        free(saved);
        if (ret) {
            releaseBuffers(d);
            return -3;
        }
        d->configured = true;
        d->bits_len = bits;
        d->slices = slices;
    }
    d->sps = *sps;
    h264_dpb_use_sps(&d->dpb, sps);
    d->reorder = reorderDepth(sps);
    return decodePicture(d, first, token, reorder);
}

// A sample holds one frame: a frame picture, or a field pair, and may first complete the frame a field of the
// previous sample started. VideoToolbox answers a sample whose token no picture took (0) as dropped, so the token
// goes to at most one frame and a picture that would start a second frame in the sample is not decoded. Once the token
// is taken, an error later in the sample returns 1: the token's frame goes out (marked bad if its second field failed).
int h264_decoder_decode(H264Decoder *d, const uint8_t *data, size_t size, unsigned lengthSize, void *token,
                        bool reorder)
{
    if (!d->configured || lengthSize < 1 || lengthSize > 4)
        return -1;
    H264Slice first, slice;
    bool have = false, taken = false;
    d->bits_len = d->slices = 0;
    for (size_t pos = 0; pos + lengthSize <= size;) {
        uint32_t length = 0;
        for (unsigned i = 0; i < lengthSize; i++)
            length = (length << 8) | data[pos + i];
        pos += lengthSize;
        if (!length || length > size - pos)
            return taken ? 1 : -1;
        const uint8_t *nal = data + pos;
        pos += length;
        const uint8_t type = nal[0] & 0x1f;
        const bool isSlice = type == H264_NAL_SLICE || type == H264_NAL_IDR;
        if (!isSlice && type != H264_NAL_SPS && type != H264_NAL_PPS)
            continue;
        if (isSlice && h264_parse_slice(&d->sets, nal, length, &slice))
            return taken ? 1 : -1;
        if (have && (!isSlice || h264_slice_new_picture(&first, &slice))) {
            const int ret = finishPicture(d, &first, token, reorder);
            if (ret < 0)
                return taken ? 1 : ret;
            taken |= ret == 1;
            have = false;
            d->bits_len = d->slices = 0;
            if (taken && isSlice && !h264_dpb_second_field(&d->dpb, &slice))
                return 1;
        }
        if (!isSlice) {
            if (h264_decoder_parameter_set(d, nal, length))
                return taken ? 1 : -1;
            continue;
        }
        if (!have)
            first = slice;
        have = true;
        if (appendSlice(d, nal, length))
            return taken ? 1 : -1;
    }
    if (have) {
        const int ret = finishPicture(d, &first, token, reorder);
        if (ret < 0)
            return taken ? 1 : ret;
        taken |= ret == 1;
    }
    return taken;
}
