// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <new>
#include "../../TegraVIC/SwitchNativeScanout.h"
#include "../../TegraDisplayFlip/TegraScanoutRing.h"
#include "../../TegraGM20B/TegraGM20BLayout.h"
using IOByteCount = uint64_t;
using addr64_t = uint64_t;
constexpr int kIOMemoryMapperNone = 0, kIODirectionOut = 1, kIOReturnSuccess = 0;
#define IONewZero(type, count) new (std::nothrow) type[count]()
#define OSSafeReleaseNULL(object) \
    do {                          \
        if (object)               \
            object->release();    \
        object = nullptr;         \
    } while (0)

class OSObject {
public:
    unsigned refs = 1;

    void retain()
    {
        ++refs;
    }

    void release()
    {
        --refs;
    }
};

class IOMemoryDescriptor : public OSObject {
public:
    unsigned prepares = 0;
    bool fail = false;

    int prepare(int)
    {
        if (fail)
            return -1;
        ++prepares;
        return 0;
    }

    void complete(int)
    {
        --prepares;
    }

    uint64_t getPhysicalSegment(uint64_t at, IOByteCount *length, int)
    {
        *length = 4096;
        return 0x80000000 + at;
    }
};

void *tegra_gm20b_surface_hold(uint64_t, int *)
{
    return nullptr;
}

void tegra_gm20b_surface_unhold(void *)
{}

int tegra_gm20b_surface_layout(uint64_t, tgm_layout *)
{
    return 0;
}

namespace {
SwitchNativeScanout next;
unsigned leases;
int error;

struct Domain {
    unsigned maps = 0, unmaps = 0;
    uint64_t first = 0;
    bool fail = false;

    bool map(uint64_t, const uint64_t *pages, uint32_t, bool)
    {
        ++maps;
        first = *pages;
        return !fail;
    }

    void unmap(uint64_t, uint32_t)
    {
        ++unmaps;
    }
};

struct Display {
    static constexpr int kSlot0 = 3, kNoBuffer = -1;
    static constexpr unsigned kSlots = 5;
    static constexpr uint64_t kProbeSlot = 0x10000000, kSlotSpan = 4u << 20;

    struct DirectSlot {
        IOMemoryDescriptor *memory;
        OSObject *surface;
        void *layout;
        uint64_t stride, used;
        uint32_t pages;
        bool tiled;
        SwitchNativeScanout *read = nullptr;
        uint64_t allocationId = 0, allocationGeneration = 0, sourceOffset = 0;
    } slots[kSlots] = {};

    TegraScanoutRing ring;
    Domain probeDomain;
    uint64_t slotUses = 0;
    int gpuDirect = -1, gpuPresentedEntry = -1;

    void cleanDomain()
    {}

    void holdScanoutBacking(unsigned)
    {}

    void releaseScanoutBacking(unsigned)
    {}

    Display()
    {
        ring.reset(3);
    }

    ~Display()
    {
        for (unsigned k = 0; k < kSlots; ++k) {
            dropSlot(k);
            delete[] slots[k].read;
        }
    }

#include "../SwitchDirectSlots.h"
};
}

int switch_native_scanout_pitch(OSObject *, SwitchNativeScanout *out)
{
    *out = error ? SwitchNativeScanout{} : next;
    if (out->lease)
        ++leases;
    return error;
}

void switch_native_scanout_release(SwitchNativeScanout *snapshot)
{
    if (snapshot->lease) {
        EXPECT_GT(leases, 0u);
        --leases;
    }
    *snapshot = {};
}

namespace {
class DirectSlots : public testing::Test {
protected:
    std::array<uint64_t, 8> pages{};
    OSObject surface;
    IOMemoryDescriptor memory;
    Display *display;

    void SetUp() override
    {
        for (unsigned p = 0; p < pages.size(); ++p)
            pages[p] = 0x80000000ULL + p * 4096;
        next = {};
        next.lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
        next.allocation.pages = pages.data();
        next.allocation.page_count = unsigned(pages.size());
        next.allocation.allocation_id = 1;
        next.allocation.allocation_generation = 1;
        next.allocation.content_generation = 1;
        error = 0;
        leases = 0;
        display = new Display;
    }

    void TearDown() override
    {
        delete display;
        EXPECT_EQ(leases, 0u);
        EXPECT_EQ(memory.prepares, 0u);
        EXPECT_EQ(memory.refs, 1u);
        EXPECT_EQ(surface.refs, 1u);
    }

    int offer()
    {
        return display->directSlotFor(&surface, &memory, 256, 16384, false);
    }
};

TEST_F(DirectSlots, ReadSurvivesPresentCompletionAndReleasesOnlyAfterLatchReplacement)
{
    ASSERT_EQ(offer(), 3);
    EXPECT_EQ(leases, 1u);
    display->gpuDirect = 3;
    display->releaseIdleDirectReads();
    EXPECT_EQ(leases, 1u);
    display->gpuDirect = -1;
    display->gpuPresentedEntry = 3;
    display->releaseIdleDirectReads();
    EXPECT_EQ(leases, 1u);
    display->ring.ready(3);
    display->gpuPresentedEntry = -1;
    display->releaseIdleDirectReads();
    EXPECT_EQ(leases, 1u);
    ASSERT_EQ(display->ring.latch(3, false), TegraScanoutRing::kTaken);
    display->releaseIdleDirectReads();
    EXPECT_EQ(leases, 1u);
    display->ring.ready(1);
    ASSERT_EQ(display->ring.latch(1, false), TegraScanoutRing::kTaken);
    display->releaseIdleDirectReads();
    EXPECT_EQ(leases, 0u);
    EXPECT_EQ(display->probeDomain.maps, 1u);
    EXPECT_EQ(display->probeDomain.unmaps, 0u);
    next.allocation.content_generation = 2;
    ASSERT_EQ(offer(), 3);
    EXPECT_EQ(leases, 1u);
    EXPECT_EQ(display->probeDomain.maps, 1u);
}

TEST_F(DirectSlots, ActiveDuplicateKeepsOneLeaseAndRejectsChangedContent)
{
    ASSERT_EQ(offer(), 3);
    display->ring.ready(3);
    EXPECT_EQ(offer(), 3);
    EXPECT_EQ(leases, 1u);
    next.allocation.content_generation = 2;
    EXPECT_EQ(offer(), -1);
    EXPECT_EQ(leases, 1u);
}

TEST_F(DirectSlots, AllocationGrowthRemapsOnlyIdleSlotsAndPreservesActiveGeneration)
{
    ASSERT_EQ(offer(), 3);
    display->ring.ready(3);
    next.allocation.allocation_generation = 2;
    EXPECT_EQ(offer(), 4);
    EXPECT_EQ(display->probeDomain.maps, 2u);
    EXPECT_EQ(leases, 2u);
    EXPECT_EQ(display->slots[0].allocationGeneration, 1u);
}

TEST_F(DirectSlots, MappingAndPrepareFailureUnwindSnapshots)
{
    memory.fail = true;
    EXPECT_EQ(offer(), -1);
    EXPECT_EQ(leases, 0u);
    memory.fail = false;
    display->probeDomain.fail = true;
    EXPECT_EQ(offer(), -1);
    EXPECT_EQ(leases, 0u);
    EXPECT_EQ(memory.prepares, 0u);
    EXPECT_EQ(display->probeDomain.unmaps, 1u);
}

TEST_F(DirectSlots, AliasedOffsetMapsActualPagesAndRejectsOutOfRange)
{
    next.pitchOffset = 4096;
    EXPECT_EQ(offer(), 3);
    EXPECT_EQ(display->probeDomain.first, pages[1]);
    next.pitchOffset = 4096 * 6;
    EXPECT_EQ(offer(), -1);
    EXPECT_EQ(leases, 1u);
}
}
