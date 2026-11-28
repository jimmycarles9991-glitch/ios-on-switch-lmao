// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchHost1xFence.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraGM20B/SwitchGpuBridge.h"

struct SwitchHost1xDisplayStream {
    static constexpr int kCancelled = -5;
    static constexpr uint32_t kProducers = 4;
    switch_host1x_fence fence = {};
    uint32_t syncpt = 0, count = 0;
    switch_gpu_backing_token producers[kProducers] = {};
    // A GPU producer's: the GPU-to-host1x bridge the stream waits on first, owned from a successful submit until the
    // completion has its status.
    switch_gpu_bridge *bridge = nullptr;
    uint32_t bridgeSyncpt = 0, bridgeThreshold = 0;
    // Why the bridge's producers failed (switch_gpu_bridge_why), read before the completion lets the bridge go; 0 when
    // none did. The completion's caller judges a set the stream already wrote by it.
    int bridgeWhy = 0;
    int status = 0;
    bool restore = false;
    void (*completed)(void *, int) = nullptr;
    void *context = nullptr;
};

int switch_host1x_display_start(SwitchHost1xDisplayStream *stream);
// producers: VIC and NVDEC tokens the stream WAITs on. bridge, when not null, is a GPU producer's jobless bridge
// (switch_gpu_bridge_jobless), whose failure cancels a stream queued on it: on success the stream owns it; on failure the
// caller still does. A failed bridge's completion carries its producers' reason.
int switch_host1x_display_submit(SwitchHost1xDisplayStream *stream, const switch_gpu_backing_token *producers,
                                 uint32_t count, const uint32_t *registers, const uint32_t *values, uint32_t stores,
                                 uint32_t update, uint32_t request, void (*completed)(void *, int), void *context,
                                 switch_gpu_bridge *bridge = nullptr);
// Stops every outstanding controller write before returning; the existing fence still owns its completion.
int switch_host1x_display_cancel(SwitchHost1xDisplayStream *stream);
