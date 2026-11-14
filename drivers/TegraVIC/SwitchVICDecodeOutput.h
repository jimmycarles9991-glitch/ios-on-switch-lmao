// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decoder's planar picture written by VIC into a client's semi-planar IOSurface: NVJPG's 4:2:2 or 4:4:4 decode (its Y,
// Cb and Cr planes in a buffer of its own) re-packed into a '422f' or '444f' destination, every sample kept (the same
// subsampling, range, standard and siting on both sides: VIC's Y'CbCr pass-through). One job, queued while the decoder
// still runs: it waits for the decoder's syncpoint on the hardware, so no CPU stands between the two engines.
#pragma once
#include <stdint.h>
#include "TegraVIC.h"
#include "TegraVICSurface.h"

struct switch_gpu_backing;

struct SwitchVICDecodeOutput {
    switch_gpu_backing *source;    // the decoder's buffer, registered by it
    uint64_t planeOffset[3];       // Y, Cb, Cr
    uint32_t pitch[3], rows[3];    // pitches a multiple of kTegraVICTraits.sourcePitchAlign
    uint32_t chroma444;            // 1: 4:4:4; 0: 4:2:2, Cb and Cr halved across
    uint32_t width, height;        // the picture
    uint32_t x, y;                 // where it goes in the destination
    const TegraVICAcquire *after;  // the decoder's job that writes the buffer
};

bool switch_vic_decode_output_ready(void);
// Queues the job: 0, and `completion` runs once with VIC's status for the destination's write; or a negative errno with
// nothing queued and `why` a literal for the caller's one line.
int switch_vic_decode_output(const TegraVICSurface *destination, const SwitchVICDecodeOutput *output,
                             void (*completion)(void *, int), void *context, const char **why);
