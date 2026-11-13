// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct switch_display_memory_window {
    uint64_t peak_bytes_per_second;
    uint32_t required_buffering_bytes;
    uint32_t latency_buffering_bytes;
    uint32_t active;
};

struct switch_display_memory_head {
    uint32_t head;
    uint32_t pixel_clock_hz;
    uint32_t active_width, active_height;
    uint32_t total_width, total_height;
    uint64_t peak_bytes_per_second;
    uint32_t bandwidth_envelope;
    struct switch_display_memory_window windows[3];
};

#ifdef __cplusplus
extern "C" {
#endif
int switch_emc_display_memory_reserve(const struct switch_display_memory_head *request);
// 1 while the EMC governor can admit display memory: its MC registers are mapped, the chip and DRAM are the qualified
// profile, and no rate change has left a reservation uncovered. Any context.
int switch_emc_display_memory_ready(void);
#ifdef __cplusplus
}

namespace switch_display {
inline bool memoryWindow(switch_display_memory_window &out, uint64_t bandwidth, uint32_t sourceWidth,
                         uint32_t sourceHeight, uint32_t destinationWidth, uint32_t destinationHeight, bool nv12,
                         bool pitch, bool rotated)
{
    if (!sourceWidth || !sourceHeight || !destinationWidth || !destinationHeight)
        return false;
    const uint32_t width = rotated ? sourceHeight : sourceWidth;
    const bool scaling = rotated ? sourceWidth != destinationWidth : sourceHeight != destinationHeight;
    const uint32_t lines = pitch ? 0 : rotated ? (nv12 ? 16 : 4) + uint32_t(scaling) : scaling ? 3 : 1;
    const uint64_t required = uint64_t(width) * lines * (nv12 ? rotated ? 2 : 3 : 4);
    const uint64_t latency = lines && rotated && !scaling ? uint64_t(width) * (nv12 ? 3 : 4) : 0;
    if (required > UINT32_MAX || latency > UINT32_MAX || !bandwidth)
        return false;
    out = {bandwidth, uint32_t(required), uint32_t(latency), 1};
    return true;
}

inline void mergeMemoryWindow(switch_display_memory_window &held, const switch_display_memory_window &next)
{
    if (!next.active)
        return;
    if (!held.active) {
        held = next;
        return;
    }
    if (held.peak_bytes_per_second < next.peak_bytes_per_second)
        held.peak_bytes_per_second = next.peak_bytes_per_second;
    if (held.required_buffering_bytes < next.required_buffering_bytes)
        held.required_buffering_bytes = next.required_buffering_bytes;
    if (held.latency_buffering_bytes > next.latency_buffering_bytes)
        held.latency_buffering_bytes = next.latency_buffering_bytes;
}
}  // namespace switch_display
#endif
