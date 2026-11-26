// SPDX-License-Identifier: BSD-2-Clause-Patent
// How SwitchXUDCData.cpp's requests reach their memory (kits/SwitchDMA/SwitchDMADirect.h switch_dma_access): zone
// memory through the page tables and a buffer wired for its life through its own segments, neither prepare()d; anything
// else through prepare() and the descriptor.
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "../../kits/SwitchDMA/SwitchDMADirectMemory.h"
#include "XudcTransfer.h"

namespace xudc {

struct RequestDirect {
    switch_dma_access access;
    uint32_t directRuns;
};

// A request not prepared yet: true when it needs no prepare() (its runs into `runs` when they come from the page tables).
template <class R> inline bool requestDirect(R &r, Run *runs, uint32_t maxRuns)
{
    r.access = switch_dma_direct_access(r.md, r.length, runs, maxRuns, &r.directRuns);
    return r.access != SWITCH_DMA_ACCESS_PREPARE;
}

template <class R> inline uint32_t directRunCount(const R &r)
{
    return r.access == SWITCH_DMA_ACCESS_PAGE_TABLES ? r.directRuns : 0;
}

template <class R> inline uint32_t directOffset(const R &r)
{
    return r.access == SWITCH_DMA_ACCESS_PAGE_TABLES ? r.length : 0;
}

inline void requestRead(const RequestDirect &r, IOMemoryDescriptor *md, uint32_t offset, uint8_t *to, uint32_t length)
{
    if (r.access == SWITCH_DMA_ACCESS_PAGE_TABLES) {
        switch_dma_direct_copy(md, offset, to, length, true);
    } else {
        md->readBytes(offset, to, length);
    }
}

inline void requestWrite(const RequestDirect &r, IOMemoryDescriptor *md, uint32_t offset, uint8_t *from,
                         uint32_t length)
{
    if (r.access == SWITCH_DMA_ACCESS_PAGE_TABLES) {
        switch_dma_direct_copy(md, offset, from, length, false);
    } else {
        md->writeBytes(offset, from, length);
    }
}

// The controller does not snoop. Before it reads request bytes (IN): clean. Before it writes them and again after
// (OUT): invalidate, as Linux arm64's dcache_inval_poc does, which cleans and invalidates a line the range shares with
// other data and only invalidates a line that is all the request's: a packet whose line reaches past its run is bounced
// (XudcTransfer.h plan), so the controller never writes a shared line, and the other data's dirty bytes in it are
// written back, never dropped. Bytes the controller did not write read whatever memory held; a dirty line of the
// request is never written back over what it did write.
inline void requestSync(const RequestDirect &r, IOMemoryDescriptor *md, uint32_t offset, uint32_t length,
                        bool fromDevice)
{
    const switch_dcache_op op = fromDevice ? SWITCH_DCACHE_INVALIDATE : SWITCH_DCACHE_CLEAN;
    if (r.access == SWITCH_DMA_ACCESS_PAGE_TABLES) {
        switch_dma_direct_sync(md, offset, length, op);
    } else {
        switch_dma_segments_sync(md, offset, length, op);
    }
}

}  // namespace xudc
