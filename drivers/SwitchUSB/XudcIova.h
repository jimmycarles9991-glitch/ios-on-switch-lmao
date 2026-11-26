// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device-mode requests at IOVAs (SwitchXUDCSmmu.cpp): the runs the controller sees at RAM's fixed IOVAs, which
// requests a window mapping takes packets off the bounce pool for, the 4 KiB pages it maps, and the runs the controller
// then sees. No IOKit: test/xudc_iova_test.cpp drives the same code.
//
// The XUDC is behind the SMMU (XUSB_DEV) with all of RAM mapped once (UsbStaticMap.h), so every request is reachable at
// no cost per request. The window takes only what the planner would bounce for its layout: runs that meet on a page
// boundary, which consecutive IOVA pages join into one, and runs across a 64 KiB line. A request starts at a 64 KiB
// line, so one that fits in 64 KiB from its first byte's offset in its page crosses none; in a longer one the lines
// fall on packet boundaries when that offset is a multiple of the packet size. What a mapping cannot change stays
// bounced: two runs that break inside a page (IOVA pages are 4 KiB), an OUT packet whose cache line holds other memory,
// and a 64 KiB line inside a packet of a longer request at another offset.
#pragma once
#include <stdint.h>
#include "XudcTransfer.h"

namespace xudc {

constexpr uint64_t kIovaPage = 4096;
constexpr uint32_t kIovaAlignPages = kTrbMaxBytes / kIovaPage;
// A request goes into the window only when that saves copying at least this much. A mapping and its unmapping write
// and clear its PTEs (10.8 a mapped request in xudc_iova_test), clean them, and flush the SMMU's PTC and TLB twice,
// each flush two MC writes and two read-backs: about 1.2-1.7 us with a read-back assumed at 0.5 us. A bounced KiB costs
// about 0.5 us to copy, its cache lines a little more (xudc_transfer_test's cost model). The development stats line
// reports the measured map and flush times (UsbSmmuSpace::Cost) to set this from.
constexpr uint32_t kRemapWorthBytes = 3 * 1024;

// The runs at their fixed IOVAs (fixed(pa, length), UINT64_MAX when there is none), merged where one continues the
// other; how many, or 0 when a run has no fixed IOVA.
template <class Fixed> inline uint32_t fixedRuns(const Run *runs, uint32_t count, Fixed fixed, Run *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t iova = fixed(runs[i].pa, runs[i].length);
        if (iova == UINT64_MAX)
            return 0;
        if (n && out[n - 1].pa + out[n - 1].length == iova) {
            out[n - 1].length += runs[i].length;
        } else {
            out[n++] = {iova, runs[i].length};
        }
    }
    return n;
}

// Whether a window mapping can take a bounced packet of these runs off the pool: two runs that meet on a page
// boundary, or a run across a 64 KiB line.
inline bool remapHelps(const Run *runs, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t end = runs[i].pa + runs[i].length;
        if ((runs[i].pa >> 16) != ((end - 1) >> 16))
            return true;
        if (i + 1 < count && (end & (kIovaPage - 1)) == 0 && (runs[i + 1].pa & (kIovaPage - 1)) == 0)
            return true;
    }
    return false;
}

// page(i, physical page) for each 4 KiB page under the runs, in request order, a page a run shares with the one before
// it once; returns how many.
template <class Page> inline uint32_t runPages(const Run *runs, uint32_t count, Page page)
{
    uint32_t pages = 0;
    uint64_t last = ~0ULL;
    for (uint32_t i = 0; i < count; ++i) {
        for (uint64_t at = runs[i].pa & ~(kIovaPage - 1); at < runs[i].pa + runs[i].length; at += kIovaPage) {
            if (at == last)
                continue;
            page(pages++, at);
            last = at;
        }
    }
    return pages;
}

// The runs as the controller sees them with runPages' pages at consecutive IOVA pages from `base`, merged where one
// continues the other; returns how many (at most count).
inline uint32_t iovaRuns(const Run *runs, uint32_t count, uint64_t base, Run *out)
{
    uint32_t n = 0;
    uint32_t pages = 0;
    uint64_t last = ~0ULL;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t first = runs[i].pa & ~(kIovaPage - 1);
        const uint32_t page = first == last ? pages - 1 : pages;
        const uint64_t iova = base + page * kIovaPage + (runs[i].pa & (kIovaPage - 1));
        for (uint64_t at = first; at < runs[i].pa + runs[i].length; at += kIovaPage) {
            if (at != last)
                ++pages;
            last = at;
        }
        if (n && out[n - 1].pa + out[n - 1].length == iova) {
            out[n - 1].length += runs[i].length;
        } else {
            out[n++] = {iova, runs[i].length};
        }
    }
    return n;
}

}  // namespace xudc
