// SPDX-License-Identifier: BSD-2-Clause-Patent
// See H264DPB.h. Clause numbers are ITU-T H.264 (08/2021).
#include "H264DPB.h"
#include <string.h>

static bool used(const H264RefFrame *f)
{
    return f->marking[0] || f->marking[1];
}

// The fields of `f` marked `kind`, as structure bits.
static uint8_t fieldsMarked(const H264RefFrame *f, uint8_t kind)
{
    return (f->marking[0] == kind ? H264_TOP : 0) | (f->marking[1] == kind ? H264_BOTTOM : 0);
}

static void unmark(H264RefFrame *f, uint8_t fields)
{
    for (unsigned p = 0; p < 2; p++)
        if (fields & (1u << p))
            f->marking[p] = H264_REF_NONE;
}

void h264_dpb_use_sps(H264DPB *dpb, const H264SPS *sps)
{
    dpb->log2_max_frame_num = sps->log2_max_frame_num;
    dpb->ref_limit = sps->max_num_ref_frames ? sps->max_num_ref_frames : 1;
}

void h264_dpb_reset(H264DPB *dpb, const H264SPS *sps)
{
    memset(dpb, 0, sizeof(*dpb));
    dpb->max_long_term_frame_idx = -1;
    h264_dpb_use_sps(dpb, sps);
}

uint8_t h264_slice_structure(const H264Slice *slice)
{
    return !slice->field_pic ? H264_FRAME : slice->bottom_field ? H264_BOTTOM : H264_TOP;
}

bool h264_dpb_second_field(const H264DPB *dpb, const H264Slice *slice)
{
    return dpb->pairing && slice->field_pic && h264_slice_structure(slice) != dpb->pair_parity &&
           slice->frame_num == dpb->pair_frame_num && slice->nal_unit_type != H264_NAL_IDR &&
           (slice->nal_ref_idc != 0) == dpb->pair_reference;
}

static int32_t frameNumWrap(const H264DPB *dpb, const H264RefFrame *f, uint32_t current)
{
    return f->frame_num > current ? (int32_t)f->frame_num - (1 << dpb->log2_max_frame_num) : (int32_t)f->frame_num;
}

static unsigned refCount(const H264DPB *dpb)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++)
        n += used(&dpb->refs[i]);
    return n;
}

bool h264_dpb_drop_oldest(H264DPB *dpb, uint32_t current)
{
    H264RefFrame *oldest = NULL;
    for (unsigned i = 0; i < 16; i++) {
        H264RefFrame *f = &dpb->refs[i];
        if (fieldsMarked(f, H264_REF_SHORT) &&
            (!oldest || frameNumWrap(dpb, f, current) < frameNumWrap(dpb, oldest, current)))
            oldest = f;
    }
    if (oldest)
        unmark(oldest, fieldsMarked(oldest, H264_REF_SHORT));
    return oldest != NULL;
}

// 8.2.5.3: counted in frame stores (a frame, a complementary field pair or a non-paired field each count once).
static void slidingWindow(H264DPB *dpb, uint32_t current, unsigned limit)
{
    while (refCount(dpb) >= limit && h264_dpb_drop_oldest(dpb, current)) {
    }
}

static H264RefFrame *addRef(H264DPB *dpb, const H264RefFrame *frame)
{
    bool taken[16] = {false};
    H264RefFrame *free = NULL;
    for (unsigned i = 0; i < 16; i++) {
        if (used(&dpb->refs[i]))
            taken[dpb->refs[i].slot] = true;
        else if (!free)
            free = &dpb->refs[i];
    }
    if (!free)
        return NULL;
    *free = *frame;
    for (uint8_t s = 0; s < 16; s++) {
        if (!taken[s]) {
            free->slot = s;
            break;
        }
    }
    return free;
}

// The decoded frame store holding surface `pic_idx` (a second field's first field).
static H264RefFrame *frameStore(H264DPB *dpb, uint8_t pic_idx)
{
    for (unsigned i = 0; i < 16; i++) {
        H264RefFrame *f = &dpb->refs[i];
        if (used(f) && !f->non_existing && f->pic_idx == pic_idx)
            return f;
    }
    return NULL;
}

bool h264_dpb_holds(const H264DPB *dpb, uint8_t pic_idx)
{
    for (unsigned i = 0; i < 16; i++) {
        const H264RefFrame *f = &dpb->refs[i];
        if (used(f) && !f->non_existing && f->pic_idx == pic_idx)
            return true;
    }
    return dpb->pairing && dpb->pair_idx == pic_idx;
}

// 8.2.4.1: the reference a memory management operation names. Decoding a frame, `number` is a frame store's
// FrameNumWrap (short-term) or LongTermFrameIdx (long-term) and names both fields; decoding a field, it is
// 2 * that + 1 for the field of the current parity and 2 * that for the other. *fields: the fields it names.
static H264RefFrame *findRef(H264DPB *dpb, int32_t number, uint8_t kind, uint32_t frameNum, uint8_t structure,
                             uint8_t *fields)
{
    int32_t id = number;
    uint8_t want = H264_FRAME;
    if (structure != H264_FRAME) {
        id = (number - (number & 1)) / 2;
        want = (number & 1) ? structure : (uint8_t)(H264_FRAME ^ structure);
    }
    for (unsigned i = 0; i < 16; i++) {
        H264RefFrame *f = &dpb->refs[i];
        const int32_t fid = kind == H264_REF_SHORT ? frameNumWrap(dpb, f, frameNum) : (int32_t)f->long_term_frame_idx;
        const uint8_t named = fieldsMarked(f, kind) & want;
        if (named && fid == id) {
            *fields = named;
            return f;
        }
    }
    return NULL;
}

// 8.2.5.4.3 and 8.2.5.4.6: LongTermFrameIdx `idx` leaves whatever holds it, except the other field of `keep`.
static void dropLong(H264DPB *dpb, uint32_t idx, const H264RefFrame *keep)
{
    for (unsigned i = 0; i < 16; i++) {
        H264RefFrame *f = &dpb->refs[i];
        if (f != keep && f->long_term_frame_idx == idx)
            unmark(f, fieldsMarked(f, H264_REF_LONG));
    }
}

// 8.2.5.4: adaptive marking. *marking and *longIdx: how the current picture is marked (memory management control
// operation 6 makes it long-term). Returns true for memory_management_control_operation 5.
static bool adaptive(H264DPB *dpb, const H264Slice *sh, const H264Current *c, uint8_t *marking, uint32_t *longIdx)
{
    const bool field = c->structure != H264_FRAME;
    const int32_t currPicNum = field ? 2 * (int32_t)sh->frame_num + 1 : (int32_t)sh->frame_num;
    const H264RefFrame *self = c->second ? frameStore(dpb, c->pic_idx) : NULL;
    bool mmco5 = false;
    for (unsigned i = 0; i < sh->mmco_count; i++) {
        const H264MMCO *m = &sh->mmco[i];
        const int32_t picNumX = currPicNum - (int32_t)(m->difference_of_pic_nums_minus1 + 1);
        uint8_t fields = 0;
        H264RefFrame *f;
        switch (m->type) {
        case 1:
            if ((f = findRef(dpb, picNumX, H264_REF_SHORT, sh->frame_num, c->structure, &fields)))
                unmark(f, fields);
            break;
        case 2:
            if ((f = findRef(dpb, (int32_t)m->long_term_pic_num, H264_REF_LONG, sh->frame_num, c->structure, &fields)))
                unmark(f, fields);
            break;
        case 3:
            if ((f = findRef(dpb, picNumX, H264_REF_SHORT, sh->frame_num, c->structure, &fields))) {
                dropLong(dpb, m->long_term_frame_idx, f);
                for (unsigned p = 0; p < 2; p++)
                    if (fields & (1u << p))
                        f->marking[p] = H264_REF_LONG;
                f->long_term_frame_idx = m->long_term_frame_idx;
            }
            break;
        case 4:
            dpb->max_long_term_frame_idx = (int32_t)m->max_long_term_frame_idx_plus1 - 1;
            for (unsigned j = 0; j < 16; j++)
                if ((int32_t)dpb->refs[j].long_term_frame_idx > dpb->max_long_term_frame_idx)
                    unmark(&dpb->refs[j], fieldsMarked(&dpb->refs[j], H264_REF_LONG));
            break;
        case 5:
            for (unsigned j = 0; j < 16; j++)
                unmark(&dpb->refs[j], H264_FRAME);
            dpb->max_long_term_frame_idx = -1;
            mmco5 = true;
            break;
        case 6:
            dropLong(dpb, m->long_term_frame_idx, self);
            *marking = H264_REF_LONG;
            *longIdx = m->long_term_frame_idx;
            break;
        }
    }
    return mmco5;
}

// 8.2.5.1 for a reference picture, after it is decoded. Returns true for memory_management_control_operation 5.
static bool mark(H264DPB *dpb, const H264Slice *sh, const H264Current *c)
{
    const unsigned parity = c->structure == H264_BOTTOM;
    uint8_t marking = H264_REF_SHORT;
    uint32_t longIdx = 0;
    bool mmco5 = false;
    if (sh->nal_unit_type == H264_NAL_IDR) {
        for (unsigned j = 0; j < 16; j++)
            unmark(&dpb->refs[j], H264_FRAME);
        dpb->max_long_term_frame_idx = sh->long_term_reference ? 0 : -1;
        marking = sh->long_term_reference ? H264_REF_LONG : H264_REF_SHORT;
    } else if (sh->adaptive_ref_pic_marking) {
        mmco5 = adaptive(dpb, sh, c, &marking, &longIdx);
    } else {
        const H264RefFrame *first = c->second ? frameStore(dpb, c->pic_idx) : NULL;
        if (!first || first->marking[!parity] != H264_REF_SHORT)  // 8.2.5.3: a pair's second field joins its first
            slidingWindow(dpb, sh->frame_num, dpb->ref_limit);
    }
    H264RefFrame *store = c->second ? frameStore(dpb, c->pic_idx) : NULL;
    if (store) {
        // 8.2.5.4.6 has the second field of a long-term first field repeat its index; one that does not keeps it.
        if (marking == H264_REF_SHORT && store->marking[!parity] == H264_REF_LONG) {
            marking = H264_REF_LONG;
            longIdx = store->long_term_frame_idx;
        }
        store->marking[parity] = marking;
        if (marking == H264_REF_LONG)
            store->long_term_frame_idx = longIdx;
        store->poc[parity] = c->poc[parity];
        return mmco5;
    }
    const uint32_t frameNum = mmco5 ? 0 : sh->frame_num;
    // A stream that keeps more references than it declared loses its oldest short-term ones.
    slidingWindow(dpb, frameNum, dpb->ref_limit);
    slidingWindow(dpb, frameNum, 16);
    H264RefFrame current = {
        .is_field = c->structure != H264_FRAME,
        .pic_idx = c->pic_idx,
        .frame_num = frameNum,
        .long_term_frame_idx = longIdx,
        .poc = {c->poc[0], c->poc[1]},
    };
    for (unsigned p = 0; p < 2; p++)
        if (c->structure & (1u << p))
            current.marking[p] = marking;
    addRef(dpb, &current);
    return mmco5;
}

void h264_dpb_fill_gap(H264DPB *dpb, uint32_t frameNum, uint8_t pic_idx)
{
    const uint32_t max = 1u << dpb->log2_max_frame_num;
    uint32_t unused = (dpb->prev_ref_frame_num + 1) % max;
    if ((frameNum - unused + max) % max > dpb->ref_limit) {
        for (unsigned i = 0; i < 16; i++)
            unmark(&dpb->refs[i], fieldsMarked(&dpb->refs[i], H264_REF_SHORT));
        unused = (frameNum + max - dpb->ref_limit) % max;
    }
    for (; unused != frameNum; unused = (unused + 1) % max) {
        slidingWindow(dpb, unused, dpb->ref_limit);
        const H264RefFrame filler = {
            .marking = {H264_REF_SHORT, H264_REF_SHORT}, .non_existing = true, .pic_idx = pic_idx, .frame_num = unused};
        addRef(dpb, &filler);
        if (unused < dpb->prev_frame_num)
            dpb->prev_frame_num_offset += max;
        dpb->prev_frame_num = dpb->prev_ref_frame_num = unused;
    }
}

void h264_dpb_order(const H264DPB *dpb, const H264SPS *s, const H264Slice *sh, H264Current *c)
{
    const bool idr = sh->nal_unit_type == H264_NAL_IDR;
    const uint32_t maxFrameNum = 1u << s->log2_max_frame_num;
    c->structure = h264_slice_structure(sh);
    c->second = h264_dpb_second_field(dpb, sh);
    c->poc_msb = 0;
    c->frame_num_offset = 0;
    int32_t top, bottom;
    if (s->poc_type == 0) {
        const int32_t maxLsb = 1 << s->log2_max_poc_lsb, lsb = (int32_t)sh->poc_lsb;
        const int32_t prevMsb = idr ? 0 : dpb->prev_poc_msb, prevLsb = idr ? 0 : dpb->prev_poc_lsb;
        int32_t msb = prevMsb;
        if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2)
            msb = prevMsb + maxLsb;
        else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2)
            msb = prevMsb - maxLsb;
        c->poc_msb = msb;
        top = bottom = msb + lsb;  // a field's count, whichever its parity (8-4, 8-6)
        if (c->structure == H264_FRAME)
            bottom = top + sh->delta_poc_bottom;
    } else {
        const uint32_t offset = idr                                   ? 0
                                : dpb->prev_frame_num > sh->frame_num ? dpb->prev_frame_num_offset + maxFrameNum
                                                                      : dpb->prev_frame_num_offset;
        c->frame_num_offset = offset;
        if (s->poc_type == 1) {
            uint32_t abs = s->num_ref_frames_in_poc_cycle ? offset + sh->frame_num : 0;
            if (!sh->nal_ref_idc && abs)
                abs--;
            int32_t expected = 0;
            if (abs) {
                int32_t perCycle = 0;
                for (unsigned i = 0; i < s->num_ref_frames_in_poc_cycle; i++)
                    perCycle += s->offset_for_ref_frame[i];
                const uint32_t cycle = (abs - 1) / s->num_ref_frames_in_poc_cycle;
                const uint32_t inCycle = (abs - 1) % s->num_ref_frames_in_poc_cycle;
                expected = (int32_t)cycle * perCycle;
                for (uint32_t i = 0; i <= inCycle; i++)
                    expected += s->offset_for_ref_frame[i];
            }
            if (!sh->nal_ref_idc)
                expected += s->offset_for_non_ref_pic;
            top = expected + sh->delta_poc[0];
            bottom = c->structure == H264_FRAME ? top + s->offset_for_top_to_bottom_field + sh->delta_poc[1]
                                                : expected + s->offset_for_top_to_bottom_field + sh->delta_poc[0];
        } else {
            top = bottom = idr ? 0 : 2 * (int32_t)(offset + sh->frame_num) - (sh->nal_ref_idc ? 0 : 1);
        }
    }
    c->poc[0] = top;
    c->poc[1] = bottom;
    if (c->structure != H264_FRAME) {
        const unsigned other = c->structure == H264_TOP;
        c->poc[other] = c->second ? dpb->pair_poc : 0;
    }
}

void h264_dpb_fill(const H264DPB *dpb, const H264Current *c, nvdec_dpb_entry_s out[16], int currPoc[2])
{
    memset(out, 0, 16 * sizeof(*out));
    for (unsigned i = 0; i < 16; i++) {
        const H264RefFrame *f = &dpb->refs[i];
        if (!used(f))
            continue;
        nvdec_dpb_entry_s *e = &out[f->slot];
        const bool longTerm = fieldsMarked(f, H264_REF_LONG) != 0;
        e->index = f->pic_idx;
        e->col_idx = f->pic_idx;
        e->state = (f->marking[0] != H264_REF_NONE) | (f->marking[1] != H264_REF_NONE) << 1;
        e->is_long_term = longTerm;
        e->not_existing = f->non_existing;
        e->is_field = f->is_field;
        e->top_field_marking = f->marking[0];
        e->bottom_field_marking = f->marking[1];
        e->FieldOrderCnt[0] = (unsigned)f->poc[0];
        e->FieldOrderCnt[1] = (unsigned)f->poc[1];
        if (c->second && !f->non_existing && f->pic_idx == c->pic_idx)
            e->FieldOrderCnt[0] = e->FieldOrderCnt[1] = (unsigned)dpb->pair_poc;
        e->FrameIdx = longTerm ? (int)f->long_term_frame_idx : (int)f->frame_num;
    }
    currPoc[0] = c->poc[0];
    currPoc[1] = c->poc[1];
}

bool h264_dpb_finish(H264DPB *dpb, const H264Slice *sh, H264Current *c)
{
    const bool mmco5 = sh->nal_ref_idc && mark(dpb, sh, c);
    const unsigned parity = c->structure == H264_BOTTOM;
    if (mmco5) {  // 8.2.1: tempPicOrderCnt, the picture's own fields counted from it
        if (c->structure == H264_FRAME) {
            const int32_t temp = c->poc[0] < c->poc[1] ? c->poc[0] : c->poc[1];
            c->poc[0] -= temp;
            c->poc[1] -= temp;
        } else {
            c->poc[parity] = 0;
        }
        H264RefFrame *store = frameStore(dpb, c->pic_idx);
        if (store)
            for (unsigned p = 0; p < 2; p++)
                if (c->structure & (1u << p))
                    store->poc[p] = c->poc[p];
    }
    const uint32_t frameNum = mmco5 ? 0 : sh->frame_num;
    if (sh->nal_ref_idc) {
        dpb->prev_poc_msb = mmco5 ? 0 : c->poc_msb;
        dpb->prev_poc_lsb = !mmco5 ? (int32_t)sh->poc_lsb : c->structure == H264_BOTTOM ? 0 : c->poc[0];
        dpb->prev_ref_frame_num = frameNum;
    }
    dpb->prev_frame_num_offset = mmco5 ? 0 : c->frame_num_offset;
    dpb->prev_frame_num = frameNum;
    dpb->pairing = c->structure != H264_FRAME && !c->second;
    if (dpb->pairing) {
        dpb->pair_parity = c->structure;
        dpb->pair_idx = c->pic_idx;
        dpb->pair_frame_num = frameNum;
        dpb->pair_reference = sh->nal_ref_idc != 0;
        dpb->pair_poc = c->poc[parity];
    }
    return mmco5;
}
