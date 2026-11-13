// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include "switch_vic_bounce_pool_stubs.h"
#include "../SwitchVICBouncePool.cpp"

struct switch_gpu_backing {
    void *owner;
    void (*release)(void *);
};

struct SwitchVICMapping {
    switch_gpu_backing *backing;
};

namespace {
int allocations = 0, destroyed = 0, maps = 0;
bool failAllocation = false, failMap = false;

TegraVICSurfaceDesc shape(uint32_t width, uint32_t height)
{
    TegraVICSurfaceDesc d = {};
    d.fourcc = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    d.width = width;
    d.height = height;
    d.planeHeight[0] = height;
    d.planeBytesPerRow[0] = uint64_t(width) * 4;
    d.length = d.planeBytesPerRow[0] * height;
    return d;
}

class Pool : public testing::Test {
protected:
    void SetUp() override
    {
        allocations = destroyed = maps = 0;
        failAllocation = failMap = false;
    }

    void TearDown() override
    {
        for (auto &slot : gSlots) {
            EXPECT_FALSE(slot.busy);
            for (auto &buffer : slot.buffers)
                discard(buffer);
        }
        EXPECT_EQ(gBytes, 0u);
        EXPECT_EQ(allocations, destroyed);
        EXPECT_EQ(maps, 0);
    }
};
}

extern "C" const TegraVICTraits *tegra_vic_traits()
{
    return &kTegraVICTraits;
}

bool switch_dma_pages_alloc(switch_dma_pages *out, size_t bytes, IOOptionBits)
{
    if (failAllocation)
        return false;
    ++allocations;
    *out = {};
    out->memory = new IOBufferMemoryDescriptor;
    out->count = uint32_t(bytes / 4096);
    out->size = bytes;
    out->pages = new uint64_t[out->count]();
    return true;
}

void switch_dma_pages_free(switch_dma_pages *memory)
{
    ++destroyed;
    delete memory->memory;
    delete[] memory->pages;
    *memory = {};
}

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *, uint32_t, uint32_t, void *owner,
                                                           void (*release)(void *))
{
    return new switch_gpu_backing{owner, release};
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    backing->release(backing->owner);
    delete backing;
}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool)
{
    if (failMap)
        return nullptr;
    ++maps;
    return new SwitchVICMapping{backing};
}

void switch_vic_mapping_release(SwitchVICMapping *mapping)
{
    if (mapping) {
        --maps;
        delete mapping;
    }
}

TEST_F(Pool, WarmSlotReusesBackingAndMappingWithoutAllocation)
{
    auto d = shape(1920, 1080);
    SwitchVICBounceSlot *a = nullptr, *b = nullptr;
    ASSERT_EQ(switch_vic_bounce_acquire(&d, &d, &a), 0);
    auto *original = a->buffers[0].backing;
    switch_vic_bounce_release(a);
    const int cold = allocations;
    ASSERT_EQ(switch_vic_bounce_acquire(&d, &d, &b), 0);
    EXPECT_EQ(b->buffers[0].backing, original);
    EXPECT_EQ(allocations, cold);
    EXPECT_EQ(maps, 2);
    switch_vic_bounce_release(b);
}

TEST_F(Pool, PendingJobsUseDifferentPhysicalScratch)
{
    auto d = shape(256, 64);
    SwitchVICBounceSlot *a = nullptr, *b = nullptr;
    ASSERT_EQ(switch_vic_bounce_acquire(&d, &d, &a), 0);
    ASSERT_EQ(switch_vic_bounce_acquire(&d, &d, &b), 0);
    EXPECT_NE(a, b);
    EXPECT_NE(a->buffers[0].backing, b->buffers[0].backing);
    switch_vic_bounce_release(a);
    switch_vic_bounce_release(b);
}

TEST_F(Pool, CapacityPressureWakesOnlyAfterRetirementReleasesStorage)
{
    auto big = shape(8192, 4096), small = shape(256, 64);
    SwitchVICBounceSlot *a = nullptr;
    ASSERT_EQ(switch_vic_bounce_acquire(&big, &big, &a), 0);
    EXPECT_EQ(gBytes, kBudget);
    std::promise<void> entered;
    auto next = std::async(std::launch::async, [&] {
        entered.set_value();
        SwitchVICBounceSlot *b = nullptr;
        int error = switch_vic_bounce_acquire(&small, &small, &b);
        return std::make_pair(error, b);
    });
    entered.get_future().wait();
    EXPECT_EQ(next.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    switch_vic_bounce_release(a);
    auto [error, b] = next.get();
    ASSERT_EQ(error, 0);
    EXPECT_LE(gBytes, kBudget);
    switch_vic_bounce_release(b);
}

TEST_F(Pool, GrowAndMappingFailuresLeaveNoBusyOrLeakedAllocation)
{
    auto d = shape(256, 64);
    SwitchVICBounceSlot *slot = nullptr;
    failAllocation = true;
    EXPECT_EQ(switch_vic_bounce_acquire(&d, &d, &slot), -12);
    EXPECT_EQ(slot, nullptr);
    failAllocation = false;
    failMap = true;
    EXPECT_EQ(switch_vic_bounce_acquire(&d, &d, &slot), -12);
    EXPECT_EQ(slot, nullptr);
    failMap = false;
    ASSERT_EQ(switch_vic_bounce_acquire(&d, &d, &slot), 0);
    switch_vic_bounce_release(slot);
}

TEST_F(Pool, AnUnusedLargeSideDoesNotPreventAFreeSlotFromFittingBudget)
{
    auto big = shape(8192, 4096);
    SwitchVICBounceSlot *a = nullptr, *b = nullptr;
    ASSERT_EQ(switch_vic_bounce_acquire(&big, &big, &a), 0);
    switch_vic_bounce_release(a);
    auto bigger = shape(8192, 6144);
    ASSERT_EQ(switch_vic_bounce_acquire(&bigger, nullptr, &b), 0);
    EXPECT_LE(gBytes, kBudget);
    EXPECT_EQ(b->buffers[1].capacity, 0u);
    switch_vic_bounce_release(b);
}
