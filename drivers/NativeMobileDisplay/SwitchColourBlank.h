// SPDX-License-Identifier: BSD-2-Clause-Patent
// A blank swap the colour unit shows (NativeLegacyDisplay's colour blank): no frame is filled and window A keeps
// fetching the page before it while the CMU blacks it out. IOMFBDisplay::can_update_ gives that page back to Core
// Animation when the blank swap retires, so the swap retires once the blank is on screen: at the first frame end after
// the one at which the colour unit settled the blank's generation (the CMU_ENABLE written then latches at the next frame
// start, TegraDisplayColorSequence.h). From then until the next flip latches, window A shows none of the page. Plain
// logic: test/switch_scan_return_test.cpp.
#pragma once
#include <stdint.h>

namespace switch_display {
class ColourBlank {
public:
    // Swap `seq` shows the blank, which the colour unit publishes as `generation`. A blank already on screen stays.
    void kicked(uint64_t seq, uint32_t generation)
    {
        pending = seq;
        blankGeneration = generation;
        settledAt = 0;
    }

    bool holds(uint64_t seq) const
    {
        return pending && pending == seq;
    }

    bool waiting() const
    {
        return pending != 0;
    }

    uint32_t generation() const
    {
        return blankGeneration;
    }

    // A frame end, after the colour unit's action: the blank swap to retire now, or 0.
    uint64_t frameEnd(uint64_t frameEnds, bool settled)
    {
        if (!pending)
            return 0;
        if (!settledAt) {
            if (settled)
                settledAt = frameEnds;
            return 0;
        }
        if (frameEnds <= settledAt)
            return 0;
        const uint64_t seq = pending;
        pending = 0;
        shown = true;
        return seq;
    }

    // The flip after the blank latched (window A left the page), or the scanout stopped: the blank swap still waiting,
    // which retires with it, or 0.
    uint64_t ended()
    {
        const uint64_t seq = pending;
        pending = settledAt = 0;
        shown = false;
        return seq;
    }

    // Window A scans the page before the blank, with nothing of it on screen.
    bool hidesWindowA() const
    {
        return shown;
    }

private:
    uint64_t pending = 0, settledAt = 0;
    uint32_t blankGeneration = 0;
    bool shown = false;
};
}  // namespace switch_display
