// SPDX-License-Identifier: BSD-2-Clause-Patent
// Core Animation's pages as window A gives them back, for the debug kernels' check that window A never scans a page
// Core Animation has back (SwitchScanReturnDisplay.h). IOMFBDisplay::can_update_ gives a page back once the swap after
// the one that showed it completed (SwitchRetireDisplay.h), so a retiring swap that shows a page on window A gives back
// the page the last such swap showed; that page is Core Animation's until a later swap shows it again. A swap that
// leaves window A alone gives nothing back, and one whose page is not known forgets the last, so the check can miss a
// hand-back but never invents one.
#pragma once
#include <stdint.h>

namespace switch_display {
class ScanReturn {
public:
    static constexpr unsigned kTaken = 8, kBack = 8;
    static inline const char unchanged = 0;
    static constexpr const void *kUnchanged = &unchanged;
    static constexpr const void *kUnknown = nullptr;

    // Swap seq was taken showing `page` on window A (kUnchanged: it leaves window A alone).
    void taken(uint64_t seq, const void *page)
    {
        TakenPage &slot = takenPages[nextTaken++ % kTaken];
        slot = {seq, page};
        if (page != kUnchanged)
            forget(page);
    }

    // Swap seq retired: the page it gives back, or null.
    const void *retired(uint64_t seq)
    {
        const void *page = kUnknown;
        for (TakenPage &slot : takenPages) {
            if (slot.seq == seq && seq) {
                page = slot.page;
                slot = {};
            }
        }
        if (page == kUnchanged)
            return nullptr;
        const void *back = last && page && last != page ? last : nullptr;
        last = page;
        if (back)
            backPages[nextBack++ % kBack] = back;
        return back;
    }

    bool back(const void *page) const
    {
        if (!page || page == kUnchanged)
            return false;
        for (const void *p : backPages)
            if (p == page)
                return true;
        return false;
    }

private:
    struct TakenPage {
        uint64_t seq = 0;
        const void *page = nullptr;
    };

    void forget(const void *page)
    {
        for (const void *&p : backPages)
            if (p == page)
                p = nullptr;
    }

    TakenPage takenPages[kTaken] = {};
    const void *backPages[kBack] = {};
    const void *last = nullptr;
    unsigned nextTaken = 0, nextBack = 0;
};
}  // namespace switch_display
