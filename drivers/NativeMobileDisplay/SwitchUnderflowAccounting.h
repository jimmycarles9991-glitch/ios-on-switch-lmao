// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace switch_display {
// Windows A, B and C's underflows, taken at DC A frame ends. DC_WINBUF_{A,B,C}D_UFLOW_STATUS counts a window's
// underflows until it is written 0, bit 31 set once the count overflowed (L4T dc.c tegra_dc_underflow_count). It counts
// whether or not WIN_x_UF_INT latches: on L4T the latch stayed clear while window A's count read 6. A frame end with a
// count or a latch is an underflowing frame. An episode opens at the first one and closes after kQuietFrames clean frame
// ends in a row, so its log lines are bounded by frames, however often the window underflows.
class UnderflowAccounting {
public:
    static constexpr unsigned kWindows = 3;
    static constexpr uint32_t kCountOverflowed = 1u << 31;
    static constexpr unsigned kQuietFrames = 60;

    enum class Edge : uint8_t {
        kNone,
        kOpened,
        kClosed
    };

    struct Totals {
        uint64_t counted = 0;
        uint32_t frames = 0;
        bool overflowed = false;

        void add(uint32_t status, bool underflowed)
        {
            counted += status & ~kCountOverflowed;
            frames += underflowed ? 1u : 0u;
            overflowed = overflowed || (status & kCountOverflowed) != 0;
        }
    };

    // A window's count and latch at one frame end; 0 and false for a window that did not scan for the driver.
    Edge frameEnd(unsigned window, uint32_t status, bool latched)
    {
        Window &w = windows[window];
        const bool underflowed = status != 0 || latched;
        w.stats.add(status, underflowed);
        if (underflowed) {
            const bool opened = !w.open;
            if (opened)
                w.episode = {};
            w.open = true;
            w.clean = 0;
            w.episode.add(status, true);
            return opened ? Edge::kOpened : Edge::kNone;
        }
        if (!w.open || ++w.clean < kQuietFrames)
            return Edge::kNone;
        w.open = false;
        return Edge::kClosed;
    }

    const Totals &episode(unsigned window) const
    {
        return windows[window].episode;
    }

    // The window's totals since the last take.
    Totals take(unsigned window)
    {
        const Totals totals = windows[window].stats;
        windows[window].stats = {};
        return totals;
    }

private:
    struct Window {
        Totals stats, episode;
        unsigned clean = 0;
        bool open = false;
    };

    Window windows[kWindows];
};
}
