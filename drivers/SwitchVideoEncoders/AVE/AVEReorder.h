// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVE's frame reordering: H.264 frame sessions that allow it (AllowFrameReordering, true unless set false; not real
// time; not Baseline; MaxFrameDelayCount not 0) code B pictures between anchors as NVIDIA's T210 driver does
// (H264NVENC.h, TegraNVENC/nvidia/SOURCE), with AVE_B_PICTURES of them (IBBP) or MaxFrameDelayCount if fewer.
//
// A frame is held, its pixel buffer retained and nothing copied, until the frame that ends its group arrives; that one
// is coded as the anchor (a P picture predicting from the anchor before), then the held ones as B pictures, each
// emitted at once, in decode order. A key frame, CompleteFrames and the end of the session code the held frames first
// (the last as the anchor): no B picture follows an IDR in decode order. Decode timestamps are the presentation times
// received, in order: the n-th picture out decodes at the n-th frame's time, as Apple's hardware encoder stamps its
// samples (an anchor decodes before it is shown, the B pictures after it a frame later than shown).
//
// The buffers: three reconstructions, each with its colocated motion (64 bytes a macroblock), as the driver allocates
// maxNumRefFrames + 1. A picture is coded into the one no reference slot holds; anchor n since the IDR goes in slot
// n & 1. A B picture NVENC refuses is coded again as an I picture nothing predicts from, as are the rest of its group,
// and the session codes anchors only from then on; a refused anchor leaves the session intra only, as without
// reordering. Every frame is answered asynchronously (kVTEncodeInfo_Asynchronous), as Apple's encoder answers them.
#ifndef AVE_REORDER_H
#define AVE_REORDER_H
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <stdbool.h>
#include <stdint.h>
#include "VTEncoderSPI.h"

enum {
    AVE_B_PICTURES = 2,  // IBBP, chosen here: libnvtvmr codes whatever pattern its caller states
    AVE_REORDER_FRAMES = 3,
    AVE_DTS_RING = 8,
};

typedef struct {
    VTVideoEncoderFrame frame;
    CVPixelBufferRef image;
    CMTime pts, duration;
    uint32_t display;  // since the IDR
} AVEHeld;

typedef struct {
    bool on;            // the stream has B pictures (its SPS says so)
    uint8_t bPictures;  // B pictures between anchors now: 0 once NVENC refused one
    uint32_t recon[AVE_REORDER_FRAMES], coloc[AVE_REORDER_FRAMES];
    int8_t slot[2];             // the frame each reference slot holds, -1 none
    uint32_t anchors;           // anchors since the IDR (the IDR is 0)
    uint32_t display;           // the next frame's display index since the IDR
    uint32_t anchorDisplay[2];  // display index of the last anchor and the one before
    AVEHeld held[AVE_B_PICTURES];
    unsigned heldCount;
    CMTime pts[AVE_DTS_RING];  // presentation times received, oldest first
    unsigned ptsHead, ptsCount;
    CMTime spare;  // the decode time of a picture NVENC refused, for the one coded in its place
    bool spareDts;
} AVEReorder;

#endif
