// SPDX-License-Identifier: BSD-2-Clause-Patent
// An encoder's input picture written by VIC: a region of a client's IOSurface copied (or converted, from RGB) into an
// engine's picture buffer as NV12 in the layout that engine reads, one job, no CPU wait.
#pragma once
#include <kern/task.h>
#include <stdint.h>
#include "TegraVIC.h"

struct switch_gpu_backing;
struct switch_gpu_backing_token;

struct SwitchVICEncodeInput {
    switch_gpu_backing *destination;  // the picture buffer, registered by its engine
    uint64_t lumaOffset, chromaOffset;
    uint32_t pitch, lumaRows, chromaRows;  // both planes: bytes a row, and rows (whole blocks when block linear)
    uint32_t blockLinear, blockHeightLog2;
    uint32_t width, height;        // the destination picture (the coded size); the region goes at its origin
    uint32_t x, y, w, h;           // the region of the surface, in pixels
    uint32_t fourcc;               // '420v' or '420f': the destination's range; a Y'CbCr source keeps its own matrix
    uint32_t matrix;               // TEGRA_VIC_YCC_* for an RGB source
    const TegraVICAcquire *after;  // the engine's last job that read the buffer, or null
};

// True once VIC takes these jobs: published, and a block-linear destination proven when `blockLinear`.
bool switch_vic_encode_input_ready(bool blockLinear);
// Queues the job and returns once VIC has it: 0, VIC's fence (a threshold of tegra_vic_syncpt()) and the job's token
// (whose failure is the encoder's), `bounced` when the copy engine staged the source first, or a negative errno with
// `why` a literal for the caller's one line.
int switch_vic_encode_input(uint32_t surfaceID, task_t task, const SwitchVICEncodeInput *input, uint32_t *fence,
                            switch_gpu_backing_token *written, bool *bounced, const char **why);
