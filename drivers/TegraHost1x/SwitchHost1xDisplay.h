// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchHost1xFence.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchHost1xDisplayStream {
    static constexpr int kCancelled = -5;
    static constexpr uint32_t kProducers = 4;
    switch_host1x_fence fence = {};
    uint32_t syncpt = 0, count = 0;
    switch_gpu_backing_token producers[kProducers] = {};
    int status = 0;
    bool restore = false;
    void (*completed)(void *, int) = nullptr;
    void *context = nullptr;
};

int switch_host1x_display_start(SwitchHost1xDisplayStream *stream);
int switch_host1x_display_submit(SwitchHost1xDisplayStream *stream, const switch_gpu_backing_token *producers,
                                 uint32_t count, const uint32_t *registers, const uint32_t *values, uint32_t stores,
                                 uint32_t update, uint32_t request, void (*completed)(void *, int), void *context);
// Stops every outstanding controller write before returning; the existing fence still owns its completion.
int switch_host1x_display_cancel(SwitchHost1xDisplayStream *stream);
