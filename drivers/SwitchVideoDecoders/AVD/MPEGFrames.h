// SPDX-License-Identifier: BSD-2-Clause-Patent
// What MPEG-1/2 and MPEG-4 Part 2 on NVDEC share: four NV12 block-linear surfaces of one size (the picture being
// decoded, the past and future references, one spare while a reference changes hands), NVDEC's three picture slots
// (0 current, 1 past, 2 future, as averne's nvtegra_mpeg12.c and nvtegra_mpeg4.c bind them), the input buffers
// (NVDECRing.h: each job's status record, the codec's own tables such as MPEG-2's slice offsets, then the bitstream),
// and the picture out through the backend.
#ifndef MPEG_FRAMES_H
#define MPEG_FRAMES_H
#include "H264NVDEC.h"
#include "NVDECRing.h"

enum {
    MPEG_FRAMES_SURFACES = 4,
    MPEG_FRAMES_TABLES = 256,  // the codec's tables in the input buffer, past the status record
};

typedef struct {
    H264Backend b;
    uint32_t width_mbs, height_mbs, pitch, rows, luma_bytes, bytes;
    uint32_t handle[MPEG_FRAMES_SURFACES];
    uint8_t *va[MPEG_FRAMES_SURFACES];
    int8_t past, future;  // reference surfaces, -1 when none
    NVDECRing ring;
    uint32_t bits_off;  // the bitstream in each input buffer
    uint32_t bits_len;  // its bytes, end sequence included
    uint32_t picture_index;
} MPEGFrames;

// Surfaces for width_mbs x height_mbs macroblocks (pitch 64-aligned, rows to whole 32-row macroblock pairs) and input
// buffers with `tables` bytes after the status record; the references are dropped. 0, or -3 when NVDEC buffers
// could not be had.
int mpeg_frames_configure(MPEGFrames *f, uint32_t width_mbs, uint32_t height_mbs, uint32_t tables);
void mpeg_frames_release(MPEGFrames *f);

// A surface no reference holds, its backing renewed when the backend recycles (an exported picture keeps the old one).
int mpeg_frames_take(MPEGFrames *f);
// The picture just decoded into `index` becomes the future reference, the old future the past one.
void mpeg_frames_reference(MPEGFrames *f, int index);

// The input buffer the next job fills: its tables at MPEG_FRAMES_TABLES.
NVDECInput *mpeg_frames_input(MPEGFrames *f);
// Appends `size` bytes of bitstream (reset first when `reset`); -1 when the input buffer cannot grow to hold them.
int mpeg_frames_append(MPEGFrames *f, const uint8_t *data, size_t size, bool reset);

void mpeg_frames_entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset);
// The input relocations (bitstream, status) and the three picture slots.
void mpeg_frames_bind(MPEGFrames *f, TegraNVDECSubmit *s, int current, int past, int future);
// Submits with AWAKEN last: the job's fence, or -2 when it was refused. `clean` names an extra range the CPU wrote (or
// has length 0).
int64_t mpeg_frames_run(MPEGFrames *f, TegraNVDECSubmit *s, TegraNVDECRange clean);

// The picture in surface `index`, width x height from its top left (fields: AVD_FIELDS_*), decoded by job `job` in this
// call (0: none), to `decoded` and then `emit`.
void mpeg_frames_output(MPEGFrames *f, int index, uint32_t width, uint32_t height, uint8_t fields, uint32_t job,
                        void *token);

#endif
