// SPDX-License-Identifier: BSD-2-Clause-Patent
// VP9 profile 0 on NVDEC 2.0: the decoder state a VLD engine leaves to its driver. NVDEC decodes the tiles; this keeps
// the eight reference slots and their surfaces, the four entropy contexts (compressed-header updates going in,
// backward adaptation from the engine's symbol counts coming out, VP9Probs.c), segmentation and loop filter state,
// splits superframes and fills nvdec_vp9_pic_s and NVDEC's probability table.
//
// NVDEC-specific facts (buffer set and sizes, method stream, probability table and counter layouts, the filter buffer's
// BSD control offset, the tile size table's tail entries) follow averne's FFmpeg nvtegra hwaccel
// (libavcodec/nvtegra_vp9.c, GPL; read, not copied) and NVIDIA's nvdec_drv.h / clc5b0.h, including the frame's
// interpolation filter in FFmpeg's numbering (proven on hardware, VP9NVDEC.c kNvdecFilter). Where averne's code and
// libvpx disagree on semantics, libvpx's are used: uv AC/DC deltas by their NVIDIA field names, compound-reference
// counts adapted, the segmentation map cleared when libvpx clears it, resolutionChange set.
//
// The plain-C backend (H264Backend's buffers, jobs and pictures) is shared with H264NVDEC, so the host tests drive this
// without a kernel.
#ifndef VP9_NVDEC_H
#define VP9_NVDEC_H
#include "H264NVDEC.h"
#include "VP9Syntax.h"

typedef struct VP9Decoder VP9Decoder;

VP9Decoder *vp9_decoder_create(const H264Backend *backend);
void vp9_decoder_destroy(VP9Decoder *decoder);

// Buffers for frames up to width x height (from the format description); larger key frames reallocate. 0, or -3 when
// NVDEC buffers could not be allocated.
int vp9_decoder_configure(VP9Decoder *decoder, uint32_t width, uint32_t height);

// One sample (a frame or a superframe). Returns 1 when a frame was shown (it reaches `decoded`, then `emit`), 0 when
// the sample showed none, -1 for unsupported or corrupt data, -2 when an NVDEC job failed, -3 when buffers failed.
int vp9_decoder_decode(VP9Decoder *decoder, const uint8_t *data, size_t size, void *token);

// For the host test: the entropy state after the last frame (libvpx's cm->fc and frame_contexts).
const VP9FrameContext *vp9_decoder_frame_context(const VP9Decoder *decoder);
// The last frame's backward adaptation, which waits for its job's entropy counts: done now. The next frame does it
// first itself; the frame context and the stored contexts include it only after. 0, or -2 when the job failed.
int vp9_decoder_settle(VP9Decoder *decoder);
const VP9State *vp9_decoder_state(const VP9Decoder *decoder);
const VP9FrameHeader *vp9_decoder_last_header(const VP9Decoder *decoder);

#endif
