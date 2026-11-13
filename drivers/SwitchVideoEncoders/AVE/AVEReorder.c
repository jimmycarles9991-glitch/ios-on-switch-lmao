// SPDX-License-Identifier: BSD-2-Clause-Patent
// See AVEReorder.h.
#include <string.h>
#include "AVEEncoder.h"

typedef enum {
    CODED,
    REFUSED,  // NVENC refused it; nothing emitted
    DROPPED,  // answered with an error
} Outcome;

bool ave_reorder_wanted(const Encoder *e)
{
    return e->codec == kCMVideoCodecType_H264 && !e->tileSession && e->allowReordering && !e->realTime &&
           e->allowTemporal && e->info.p_frames && e->maxFrameDelay != 0;
}

OSStatus ave_reorder_start(Encoder *e)
{
    AVEReorder *r = &e->reorder;
    memset(r, 0, sizeof(*r));
    r->recon[0] = e->handles.recon;
    r->recon[1] = e->handles.ref;
    if (ave_alloc_buffer(e, e->layout.recon.bytes, &r->recon[2]))
        return kVTAllocationFailedErr;
    for (int i = 0; i < AVE_REORDER_FRAMES; i++)
        if (ave_alloc_buffer(e, e->layout.coloc_bytes, &r->coloc[i]))
            return kVTAllocationFailedErr;
    r->slot[0] = r->slot[1] = -1;
    r->bPictures = e->maxFrameDelay >= 0 && e->maxFrameDelay < AVE_B_PICTURES ? (uint8_t)e->maxFrameDelay
                                                                              : (uint8_t)AVE_B_PICTURES;
    r->on = true;
    return noErr;
}

static void pushPts(AVEReorder *r, CMTime pts)
{
    if (r->ptsCount == AVE_DTS_RING) {
        r->ptsHead = (r->ptsHead + 1) % AVE_DTS_RING;
        r->ptsCount--;
    }
    r->pts[(r->ptsHead + r->ptsCount++) % AVE_DTS_RING] = pts;
}

static CMTime nextDts(AVEReorder *r)
{
    if (r->spareDts) {
        r->spareDts = false;
        return r->spare;
    }
    if (!r->ptsCount)
        return kCMTimeInvalid;
    const CMTime dts = r->pts[r->ptsHead];
    r->ptsHead = (r->ptsHead + 1) % AVE_DTS_RING;
    r->ptsCount--;
    return dts;
}

// A dropped frame takes its decode time with it, so the next picture out takes the next one.
static void drop(Encoder *e, const AVEHeld *h, OSStatus status, bool coded)
{
    AVEReorder *r = &e->reorder;
    if (r->spareDts)
        r->spareDts = false;
    else if (!coded)
        (void)nextDts(r);
    if (e->session)
        VTEncoderSessionEmitEncodedFrame(e->session, h->frame, status, kVTEncodeInfo_FrameDropped, NULL);
}

// The frame no reference slot holds.
static int freeFrame(const AVEReorder *r)
{
    int f = 0;
    while (f == r->slot[0] || f == r->slot[1])
        f++;
    return f;
}

static Outcome code(Encoder *e, const AVEHeld *h, uint8_t type)
{
    AVEReorder *r = &e->reorder;
    const int f = freeFrame(r);
    AVEPlan plan = {
        .frame = h->frame, .image = h->image, .pts = h->pts, .duration = h->duration, .handles = e->handles};
    plan.dts = nextDts(r);
    H264EncPicture *p = &plan.picture;
    H264EncHandles *b = &plan.handles;
    p->type = type;
    p->poc_lsb = type == H264ENC_IDR ? 0 : (2 * h->display) & 0xffff;
    b->recon = r->recon[f];
    b->coloc_out = r->coloc[f];
    b->ref = b->ref1 = b->coloc_in = 0;
    const uint32_t anchor = r->anchors + 1;
    if (type == H264ENC_IDR) {
        b->ref = r->recon[(f + 1) % AVE_REORDER_FRAMES];  // slot 0 on an IDR too (H264NVENC.c)
    } else if (type == H264ENC_P) {
        const int s = (int)(r->anchors & 1);
        p->frame_num = anchor & 0xffff;
        p->ref_slot[0] = (uint8_t)s;
        p->gap[0] = (uint8_t)(h->display - r->anchorDisplay[0] - 1);
        *(s ? &b->ref1 : &b->ref) = r->recon[r->slot[s]];
    } else if (type == H264ENC_I) {  // in place of a B picture NVENC refused: between the same anchors
        p->frame_num = anchor & 0xffff;
        p->unreferenced = true;
        b->ref = r->recon[r->slot[0]];
    } else {
        const int past = (int)((r->anchors - 1) & 1), future = (int)(r->anchors & 1);
        p->frame_num = anchor & 0xffff;
        p->ref_slot[0] = (uint8_t)past;
        p->ref_slot[1] = (uint8_t)future;
        p->gap[0] = (uint8_t)(h->display - r->anchorDisplay[1] - 1);
        p->gap[1] = (uint8_t)(r->anchorDisplay[0] - h->display - 1);
        b->ref = r->recon[r->slot[0]];
        b->ref1 = r->recon[r->slot[1]];
        b->coloc_in = r->coloc[r->slot[future]];
    }
    const OSStatus status = ave_encode_planned(e, &plan);
    if (plan.refused) {
        r->spare = plan.dts;
        r->spareDts = true;
        return REFUSED;
    }
    if (status) {
        drop(e, h, status, true);
        return DROPPED;
    }
    if (type == H264ENC_IDR) {
        r->slot[0] = (int8_t)f;
        r->slot[1] = -1;
        r->anchors = 0;
        r->anchorDisplay[0] = r->anchorDisplay[1] = 0;
        r->display = 1;
    } else if (type == H264ENC_P) {
        r->slot[anchor & 1] = (int8_t)f;
        r->anchors = anchor;
        r->anchorDisplay[1] = r->anchorDisplay[0];
        r->anchorDisplay[0] = h->display;
    }
    return CODED;
}

// `anchor`, then the held frames before it as B pictures.
static void codeGroup(Encoder *e, const AVEHeld *anchor)
{
    AVEReorder *r = &e->reorder;
    const Outcome outcome = code(e, anchor, H264ENC_P);
    if (outcome == REFUSED) {  // P pictures are off (encodePicture): every frame an IDR, in display order
        for (unsigned i = 0; i < r->heldCount; i++)
            if (code(e, &r->held[i], H264ENC_IDR) == REFUSED)
                drop(e, &r->held[i], kVTVideoEncoderMalfunctionErr, true);
        if (code(e, anchor, H264ENC_IDR) == REFUSED)
            drop(e, anchor, kVTVideoEncoderMalfunctionErr, true);
    } else if (outcome == DROPPED) {
        return;  // the held frames wait for the next anchor
    } else {
        for (unsigned i = 0; i < r->heldCount; i++) {
            if (r->bPictures && code(e, &r->held[i], H264ENC_B) != REFUSED)
                continue;
            r->bPictures = 0;
            if (code(e, &r->held[i], H264ENC_I) == REFUSED)
                drop(e, &r->held[i], kVTVideoEncoderMalfunctionErr, true);
        }
    }
    for (unsigned i = 0; i < r->heldCount; i++)
        CVPixelBufferRelease(r->held[i].image);
    r->heldCount = 0;
}

OSStatus ave_reorder_frame(Encoder *e, VTVideoEncoderFrame frame, CVPixelBufferRef image, CMTime pts, CMTime duration,
                           CFDictionaryRef options)
{
    AVEReorder *r = &e->reorder;
    if (CVPixelBufferGetWidth(image) != (size_t)e->size.width ||
        CVPixelBufferGetHeight(image) != (size_t)e->size.height || CVPixelBufferGetPlaneCount(image) != 2)
        return kVTPixelTransferNotSupportedErr;
    pushPts(r, pts);
    const bool key = ave_wants_key(e, pts, options, r->display ? r->display - 1 : 0);
    AVEHeld h = {.frame = frame, .image = image, .pts = pts, .duration = duration, .display = key ? 0 : r->display};
    if (key) {
        ave_reorder_flush(e);
        if (code(e, &h, H264ENC_IDR) == REFUSED)
            drop(e, &h, kVTVideoEncoderMalfunctionErr, true);
        return noErr;
    }
    r->display++;
    if (r->heldCount < r->bPictures) {
        h.image = CVPixelBufferRetain(image);
        r->held[r->heldCount++] = h;
        return noErr;
    }
    codeGroup(e, &h);
    return noErr;
}

// Until none is held: an anchor that fails leaves the frames before it for the next pass.
void ave_reorder_flush(Encoder *e)
{
    AVEReorder *r = &e->reorder;
    if (!e->session) {
        ave_reorder_release(e);
        return;
    }
    while (r->heldCount) {
        const AVEHeld anchor = r->held[--r->heldCount];
        codeGroup(e, &anchor);
        CVPixelBufferRelease(anchor.image);
    }
}

void ave_reorder_release(Encoder *e)
{
    AVEReorder *r = &e->reorder;
    for (unsigned i = 0; i < r->heldCount; i++)
        CVPixelBufferRelease(r->held[i].image);
    r->heldCount = 0;
}
