// Host stand-ins for what kits/SwitchDMA/SwitchDMABuffer.cpp takes from the kernel: an IOBufferMemoryDescriptor that
// hands out host memory at a physical address the test chooses, and the counts of what the kit did with it.
#ifndef SWITCH_TEST_SWITCH_DMA_STUBS_H
#define SWITCH_TEST_SWITCH_DMA_STUBS_H

#include "dcache_recorder.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOReturn = int;
using mach_vm_size_t = uint64_t;
using mach_vm_address_t = uint64_t;
using task_t = void *;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNoResources = 0x2be;
constexpr IOOptionBits kIODirectionInOut = 3, kIOMemoryPhysicallyContiguous = 0x10, kIOMemoryMapperNone = 0x800,
                       kIOMapCacheMask = 0x700, kIOMapInhibitCache = 0x100, kIOMapWriteCombineCache = 0x400;
inline task_t kernel_task = nullptr;

inline void bzero(void *p, size_t n)
{
    std::memset(p, 0, n);
}

// What the next allocation does, and what the kit did with the allocations.
struct SwitchDmaModel {
    bool failAllocation = false;
    bool failPrepare = false;
    uint64_t pa = 0x80000000;
    uint64_t contiguous = ~0ull;
    IOOptionBits options = 0;
    uint64_t mask = 0;
    int live = 0, prepared = 0;
};

inline SwitchDmaModel gDma;

class IOBufferMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t, IOOptionBits options, mach_vm_size_t capacity,
                                                            mach_vm_address_t mask)
    {
        gDma.options = options;
        gDma.mask = mask;
        if (gDma.failAllocation)
            return nullptr;
        auto *md = new IOBufferMemoryDescriptor;
        md->bytes_ = static_cast<uint8_t *>(std::malloc(capacity));
        std::memset(md->bytes_, 0xa5, capacity);
        md->capacity_ = capacity;
        ++gDma.live;
        return md;
    }

    IOReturn prepare()
    {
        if (gDma.failPrepare)
            return kIOReturnNoResources;
        ++gDma.prepared;
        return kIOReturnSuccess;
    }

    IOReturn complete()
    {
        --gDma.prepared;
        return kIOReturnSuccess;
    }

    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, IOOptionBits)
    {
        *length = gDma.contiguous < capacity_ ? gDma.contiguous : capacity_ - offset;
        return gDma.pa + offset;
    }

    void *getBytesNoCopy()
    {
        return bytes_;
    }

    void release()
    {
        --gDma.live;
        std::free(bytes_);
        delete this;
    }

private:
    uint8_t *bytes_ = nullptr;
    uint64_t capacity_ = 0;
};

#endif
