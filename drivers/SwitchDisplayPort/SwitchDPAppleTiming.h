// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchDPModes.h"

// IOAVFamily's ElementData on both pinned builds; the native 1080p60 timing and RGB8 color tables use these layouts.
struct switch_dp_apple_axis {
    uint32_t total, active, sync_width, back_porch, front_porch;
    int32_t precise_sync_rate;
    uint32_t sync_polarity, pixel_repetition;
};

struct switch_dp_apple_timing {
    uint32_t flags;
    struct switch_dp_apple_axis horizontal, vertical;
    uint32_t pixel_clock_hz, aspect_ratio, scan_information;
};

struct switch_dp_apple_color {
    uint32_t depth, pixel_encoding, dynamic_range, colorimetry, eotf, flags, reserved[2];
};

static inline struct switch_dp_apple_timing switch_dp_apple_timing(const struct switch_dp_mode *m)
{
    const uint64_t total = (uint64_t)switch_dp_mode_htotal(m) * switch_dp_mode_vtotal(m);
    const int32_t refresh = total ? (int32_t)(((uint64_t)m->pixel_khz * 1000u * 65536u + total / 2) / total) : 0;
    const struct switch_dp_apple_timing timing = {
        0,
        {switch_dp_mode_htotal(m), m->hactive, m->hsync, m->hback, m->hfront, 0, m->hsync_high ? 1u : 0u, 0},
        {switch_dp_mode_vtotal(m), m->vactive, m->vsync, m->vback, m->vfront, refresh, m->vsync_high ? 1u : 0u, 0},
        m->pixel_khz * 1000u,
        0,
        0,
    };
    return timing;
}

static inline struct switch_dp_apple_color switch_dp_apple_rgb8(void)
{
    const struct switch_dp_apple_color color = {8, 0, 0, 10, 0, 0, {0, 0}};
    return color;
}
