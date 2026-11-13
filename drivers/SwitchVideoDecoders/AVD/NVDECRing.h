// SPDX-License-Identifier: BSD-2-Clause-Patent
// The input buffers a decoder cycles: each picture's bitstream, its tables and its status record go into the next one,
// which no job still reads. A decoder given TegraNVDECInfo.jobs_in_flight + 1 of them (H264Backend.inputs) never
// waits to write one: the job that last used it ended before the Submit of the job after it returned
// (TegraNVDECABI.h, Queue room).
#ifndef NVDEC_RING_H
#define NVDEC_RING_H
#include "H264NVDEC.h"

#define NVDEC_RING_MAX 16

typedef struct {
    uint32_t handle;
    uint8_t *va;
    uint64_t bytes;
} NVDECInput;

typedef struct {
    NVDECInput input[NVDEC_RING_MAX];
    uint32_t count, next;
} NVDECRing;

// Every input at `bytes`; -1 when one could not be had (none is then held).
int nvdec_ring_alloc(NVDECRing *ring, const H264Backend *b, uint64_t bytes);
void nvdec_ring_release(NVDECRing *ring, const H264Backend *b);
// The input the next job fills.
NVDECInput *nvdec_ring_current(NVDECRing *ring);
// The current input grown to at least `needed` bytes (doubling, at most 32 MiB), its first `keep` bytes kept.
int nvdec_ring_grow(NVDECRing *ring, const H264Backend *b, uint64_t needed, uint64_t keep);
// Its job submitted: the next input becomes current.
void nvdec_ring_advance(NVDECRing *ring);

#endif
