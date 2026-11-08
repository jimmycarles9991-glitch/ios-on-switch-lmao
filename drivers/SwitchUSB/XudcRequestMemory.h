// SPDX-License-Identifier: BSD-2-Clause-Patent
// The memory operations of SwitchXUDC.cpp's data requests (section "Data endpoints"): through prepare() and the
// descriptor, or, with //drivers/SwitchUSB:xudc_direct, for zone memory without prepare() (SwitchDMADirectMemory.h:
// same runs, same cache lines, same bytes copied).
//
// With the setting off the kernel must stay byte-for-byte what it was, and XNU's panic() records __LINE__: so
// SwitchXUDC.cpp calls these on the lines that made the descriptor calls (no line added above a panic), and passes the
// descriptor before the other arguments, the order the member calls they replace load them in.
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "XudcTransfer.h"
#if SWITCH_XUDC_DIRECT
#include "SwitchDMADirectMemory.h"
#endif

namespace xudc {

#if SWITCH_XUDC_DIRECT
struct RequestDirect {
    bool direct = false;
    uint32_t directRuns = 0;
};

// A request not prepared yet: its runs into `runs` when it can go without prepare().
template <class R> inline bool requestDirect(R &r, Run *runs, uint32_t maxRuns)
{
    r.directRuns = switch_dma_direct_map(r.md, r.length, runs, maxRuns);
    r.direct = r.directRuns != 0;
    return r.direct;
}

template <class R> inline uint32_t directRunCount(const R &r)
{
    return r.direct ? r.directRuns : 0;
}

template <class R> inline uint32_t directOffset(const R &r)
{
    return r.direct ? r.length : 0;
}

inline void requestRead(const RequestDirect &r, IOMemoryDescriptor *md, uint32_t offset, uint8_t *to, uint32_t length)
{
    if (r.direct)
        switch_dma_direct_copy(md, offset, to, length, true);
    else
        md->readBytes(offset, to, length);
}

inline void requestWrite(const RequestDirect &r, IOMemoryDescriptor *md, uint32_t offset, uint8_t *from,
                         uint32_t length)
{
    if (r.direct)
        switch_dma_direct_copy(md, offset, from, length, false);
    else
        md->writeBytes(offset, from, length);
}

inline void requestSync(const RequestDirect &r, IOMemoryDescriptor *md, uint32_t offset, uint32_t length,
                        bool invalidate)
{
    if (r.direct)
        switch_dma_direct_sync(md, offset, length, invalidate);
    else
        md->performOperation(invalidate ? kIOMemoryIncoherentIOFlush : kIOMemoryIncoherentIOStore, offset, length);
}
#else
struct RequestDirect {};

template <class R> constexpr bool requestDirect(R &, Run *, uint32_t)
{
    return false;
}

template <class R> constexpr uint32_t directRunCount(const R &)
{
    return 0;
}

template <class R> constexpr uint32_t directOffset(const R &)
{
    return 0;
}

inline void requestRead(const RequestDirect &, IOMemoryDescriptor *md, uint32_t offset, uint8_t *to, uint32_t length)
{
    md->readBytes(offset, to, length);
}

// NOLINTNEXTLINE(misc-const-correctness): the SWITCH_XUDC_DIRECT variant's signature, whose copy takes a writable buffer
inline void requestWrite(const RequestDirect &, IOMemoryDescriptor *md, uint32_t offset, uint8_t *from, uint32_t length)
{
    md->writeBytes(offset, from, length);
}

inline void requestSync(const RequestDirect &, IOMemoryDescriptor *md, uint32_t offset, uint32_t length,
                        bool invalidate)
{
    md->performOperation(invalidate ? kIOMemoryIncoherentIOFlush : kIOMemoryIncoherentIOStore, offset, length);
}
#endif

}  // namespace xudc
