// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../SwitchPower/SwitchDisplayMemory.h"

namespace switch_display {
class MemoryEnvelope {
    switch_display_memory_head mode = {}, held = {};
    switch_display_memory_window layers[4] = {};
    bool configured = false, applied = false;

    static bool same(const switch_display_memory_head &a, const switch_display_memory_head &b)
    {
        if (a.head != b.head || a.bandwidth_envelope != b.bandwidth_envelope || a.pixel_clock_hz != b.pixel_clock_hz ||
            a.active_width != b.active_width || a.active_height != b.active_height || a.total_width != b.total_width ||
            a.total_height != b.total_height || a.peak_bytes_per_second != b.peak_bytes_per_second)
            return false;
        for (unsigned i = 0; i != 3; ++i) {
            const auto &x = a.windows[i], &y = b.windows[i];
            if (x.active != y.active || x.peak_bytes_per_second != y.peak_bytes_per_second ||
                x.required_buffering_bytes != y.required_buffering_bytes ||
                x.latency_buffering_bytes != y.latency_buffering_bytes)
                return false;
        }
        return true;
    }

    switch_display_memory_head request(uint64_t peak) const
    {
        auto result = mode;
        result.peak_bytes_per_second = peak;
        return result;
    }

    template <class Apply> bool publish(switch_display_memory_head next, bool preserve, Apply apply)
    {
        if (!configured)
            return false;
        next.bandwidth_envelope = preserve ? 1u : 0u;
        if (preserve) {
            if (next.peak_bytes_per_second < held.peak_bytes_per_second)
                next.peak_bytes_per_second = held.peak_bytes_per_second;
            for (unsigned i = 0; i != 3; ++i)
                mergeMemoryWindow(next.windows[i], held.windows[i]);
        }
        if (applied && same(next, held))
            return true;
        if (apply(&next))
            return false;
        held = next;
        applied = true;
        return true;
    }

public:
    void invalidate()
    {
        configured = applied = false;
    }

    bool ready() const
    {
        return configured;
    }

    bool holding() const
    {
        return applied;
    }

    bool configure(const switch_display_memory_head &timing, uint32_t sourceWidth, uint32_t sourceHeight, bool rotated)
    {
        if (!timing.pixel_clock_hz || !timing.active_width || !timing.active_height ||
            timing.total_width <= timing.active_width || timing.total_height <= timing.active_height)
            return false;
        auto next = timing;
        for (auto &window : next.windows)
            window = {};
        const uint64_t bandwidth = uint64_t(timing.pixel_clock_hz) * 4;
        if (!memoryWindow(next.windows[0], bandwidth, sourceWidth, sourceHeight, timing.active_width,
                          timing.active_height, false, false, rotated))
            return false;
        switch_display_memory_window pitch;
        if (!memoryWindow(pitch, bandwidth, timing.active_width, timing.active_height, timing.active_width,
                          timing.active_height, false, true, false))
            return false;
        mergeMemoryWindow(next.windows[0], pitch);
        next.peak_bytes_per_second = bandwidth;
        mode = next;
        configured = true;
        applied = false;
        return true;
    }

    template <class Apply>
    bool admit(const switch_display_memory_window updated[4], const switch_display_memory_window active[2],
               uint64_t peak, Apply apply)
    {
        switch_display_memory_window nextLayers[4], worst = {};
        for (unsigned i = 0; i != 4; ++i) {
            nextLayers[i] = layers[i];
            mergeMemoryWindow(nextLayers[i], updated[i]);
            mergeMemoryWindow(worst, nextLayers[i]);
        }
        for (unsigned i = 0; i != 2; ++i)
            mergeMemoryWindow(worst, active[i]);
        auto next = request(peak);
        next.windows[1] = next.windows[2] = worst;
        if (!publish(next, true, apply))
            return false;
        for (unsigned i = 0; i != 4; ++i)
            layers[i] = nextLayers[i];
        return true;
    }

    template <class Apply> bool arm(const switch_display_memory_window active[2], uint64_t peak, Apply apply)
    {
        const switch_display_memory_window windows[3] = {mode.windows[0], active[0], active[1]};
        return armWindows(windows, peak, apply);
    }

    // Window A alone, unless a plan has been published since the mode was configured.
    template <class Apply> bool applyUiAlone(Apply apply)
    {
        if (applied)
            return true;
        return publish(request(mode.peak_bytes_per_second), false, apply);
    }

    template <class Apply> bool retire(const switch_display_memory_window active[2], uint64_t peak, Apply apply)
    {
        const switch_display_memory_window windows[3] = {mode.windows[0], active[0], active[1]};
        return retireWindows(windows, peak, apply);
    }

    // Windows A, B and C as a composition has them (the UI on A or, in landscape, on B: uiWindow()). Armed, the
    // reservation covers what is held too, so a request out keeps both its old and its new demand.
    template <class Apply> bool armWindows(const switch_display_memory_window windows[3], uint64_t peak, Apply apply)
    {
        auto next = request(peak);
        for (unsigned i = 0; i != 3; ++i)
            next.windows[i] = windows[i];
        return publish(next, true, apply);
    }

    template <class Apply> bool retireWindows(const switch_display_memory_window windows[3], uint64_t peak, Apply apply)
    {
        auto next = request(peak);
        for (unsigned i = 0; i != 3; ++i)
            next.windows[i] = windows[i];
        if (!publish(next, false, apply))
            return false;
        for (auto &layer : layers)
            layer = {};
        return true;
    }

    // The UI's own window as configure() sized it.
    const switch_display_memory_window &uiWindow() const
    {
        return mode.windows[0];
    }

    const switch_display_memory_head &reservation() const
    {
        return held;
    }

    uint32_t pixelClock() const
    {
        return mode.pixel_clock_hz;
    }
};
}
