// Host stand-ins for what kits/SwitchDMA/SwitchDMAPages.cpp takes from the kernel: descriptors whose physical segments
// the test lays out (scattered, as the VM hands out pages), and counts of what the kit did with them.
#ifndef SWITCH_TEST_SWITCH_DMA_PAGES_STUBS_H
#define SWITCH_TEST_SWITCH_DMA_PAGES_STUBS_H

#include "dcache_recorder.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using IOReturn = int;
using addr64_t = uint64_t;
using vm_size_t = uint64_t;
using vm_offset_t = uint64_t;
using task_t = void *;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNoResources = 0x2be;
constexpr IOOptionBits kIODirectionInOut = 3, kIOMemoryMapperNone = 0x800, kIOMemoryKernelUserShared = 0x2000;
constexpr uint64_t PAGE_SIZE = 16384;
inline task_t kernel_task = nullptr;

inline void bzero(void *p, size_t n)
{
    std::memset(p, 0, n);
}

#define IONew(type, count)             static_cast<type *>(std::calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)

// One physical segment: where it starts and how long it is.
struct SwitchDmaSegment {
    uint64_t pa, length;
};

// What the next allocation does, and what the kit did with the allocations.
struct SwitchDmaPagesModel {
    bool failAllocation = false;
    bool failPrepare = false;
    std::vector<SwitchDmaSegment> segments;  // the next allocation's layout; empty: one segment at 0x80000000
    IOOptionBits options = 0;
    uint64_t alignment = 0;
    int live = 0, prepared = 0;
};

inline SwitchDmaPagesModel gPages;

class IOMemoryDescriptor {
public:
    virtual ~IOMemoryDescriptor() = default;

    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, IOOptionBits)
    {
        uint64_t at = 0;
        for (const SwitchDmaSegment &s : segments_) {
            if (offset < at + s.length) {
                *length = s.length - (offset - at);
                return s.pa + (offset - at);
            }
            at += s.length;
        }
        *length = 0;
        return 0;
    }

    std::vector<SwitchDmaSegment> segments_;
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *inTaskWithOptions(task_t, IOOptionBits options, vm_size_t capacity,
                                                       vm_offset_t alignment)
    {
        gPages.options = options;
        gPages.alignment = alignment;
        if (gPages.failAllocation)
            return nullptr;
        auto *md = new IOBufferMemoryDescriptor;
        md->bytes_ = static_cast<uint8_t *>(std::malloc(capacity));
        std::memset(md->bytes_, 0xa5, capacity);
        md->segments_ =
            gPages.segments.empty() ? std::vector<SwitchDmaSegment>{{0x80000000, capacity}} : gPages.segments;
        ++gPages.live;
        return md;
    }

    IOReturn prepare()
    {
        if (gPages.failPrepare)
            return kIOReturnNoResources;
        ++gPages.prepared;
        return kIOReturnSuccess;
    }

    IOReturn complete()
    {
        --gPages.prepared;
        return kIOReturnSuccess;
    }

    void *getBytesNoCopy()
    {
        return bytes_;
    }

    void release()
    {
        --gPages.live;
        std::free(bytes_);
        delete this;
    }

private:
    uint8_t *bytes_ = nullptr;
};

#endif
