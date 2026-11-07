// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchDMAPages.h"
#include <IOKit/IOLib.h>
#include "SwitchDcache.h"
#include "SwitchHwPage.h"

bool switch_dma_page_list(IOMemoryDescriptor *memory, uint64_t offset, uint64_t bytes, uint64_t *pages, uint32_t max,
                          uint32_t *count)
{
    *count = 0;
    const uint64_t first = offset & ~SWITCH_HW_PAGE_MASK;
    const uint64_t whole = (offset + bytes + SWITCH_HW_PAGE_MASK) & ~SWITCH_HW_PAGE_MASK;
    if (!memory || !bytes || (whole - first) / SWITCH_HW_PAGE_SIZE > max)
        return false;
    uint32_t n = 0;
    for (offset = first; offset < whole;) {
        IOByteCount length = 0;
        const addr64_t pa = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
        if (!pa || !length || (pa & SWITCH_HW_PAGE_MASK))
            return false;
        for (uint64_t at = 0; at < length && offset < whole; at += SWITCH_HW_PAGE_SIZE, offset += SWITCH_HW_PAGE_SIZE)
            pages[n++] = pa + at;
    }
    *count = n;
    return true;
}

// No physical mask: one would send IOBufferMemoryDescriptor to its contiguous allocator. Every RAM page is reachable
// through the SMMU (34-bit page frames), which the SMMU's user checks per page.
bool switch_dma_pages_alloc(switch_dma_pages *out, size_t size, IOOptionBits extraOptions)
{
    *out = {};
    const uint64_t whole = ((uint64_t)size + SWITCH_HW_PAGE_MASK) & ~SWITCH_HW_PAGE_MASK;
    if (!size || whole / SWITCH_HW_PAGE_SIZE > UINT32_MAX)
        return false;
    const IOOptionBits options = (IOOptionBits)kIODirectionInOut | extraOptions;
    IOBufferMemoryDescriptor *memory =
        IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, (vm_size_t)whole, PAGE_SIZE);
    if (!memory)
        return false;
    if (memory->prepare() != kIOReturnSuccess) {
        memory->release();
        return false;
    }
    const uint32_t count = (uint32_t)(whole / SWITCH_HW_PAGE_SIZE);
    uint64_t *pages = IONew(uint64_t, count);
    auto *va = static_cast<uint8_t *>(memory->getBytesNoCopy());
    uint32_t listed = 0;
    if (!pages || !va || !switch_dma_page_list(memory, 0, whole, pages, count, &listed) || listed != count) {
        if (pages)
            IODelete(pages, uint64_t, count);
        memory->complete();
        memory->release();
        return false;
    }
    bzero(va, whole);
    switch_dcache_clean_invalidate(va, whole);
    *out = {memory, va, pages, count, (size_t)whole};
    return true;
}

void switch_dma_pages_free(switch_dma_pages *buffer)
{
    if (buffer->pages)
        IODelete(buffer->pages, uint64_t, buffer->count);
    if (buffer->memory) {
        buffer->memory->complete();
        buffer->memory->release();
    }
    *buffer = {};
}
