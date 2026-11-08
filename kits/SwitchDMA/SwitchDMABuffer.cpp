// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchDMABuffer.h"
#include "SwitchDcache.h"

// There is no system IOMapper on this platform: with kIOMemoryMapperNone the descriptor's physical segment is the
// bus address the device uses, and prepare() wires the pages once at allocation instead of per transfer.
bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t physicalMask, IOOptionBits extraOptions)
{
    *out = {};
    if (!size || !physicalMask)
        return false;
    const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                                 static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
                                 static_cast<IOOptionBits>(kIOMemoryMapperNone) | extraOptions;
    IOBufferMemoryDescriptor *memory =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, size, physicalMask);
    if (!memory)
        return false;
    if (memory->prepare() != kIOReturnSuccess) {
        memory->release();
        return false;
    }
    IOByteCount contiguous = 0;
    const uint64_t pa = memory->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    auto *va = static_cast<uint8_t *>(memory->getBytesNoCopy());
    const uint64_t ceiling = UINT64_MAX >> static_cast<unsigned>(__builtin_clzll(physicalMask));
    if (!pa || !va || contiguous < size || pa + (size - 1) > ceiling) {
        memory->complete();
        memory->release();
        return false;
    }
    bzero(va, size);
    switch_dcache_clean_invalidate(va, size);
    *out = {memory, va, pa, size};
    return true;
}

void switch_dma_free(switch_dma_buffer *buffer)
{
    if (buffer->memory) {
        buffer->memory->complete();
        buffer->memory->release();
    }
    *buffer = {};
}
