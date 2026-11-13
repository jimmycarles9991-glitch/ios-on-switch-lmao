// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>
#include "switch_vic_backing_stubs.h"

[[noreturn]] void panic(const char *)
{
    throw std::runtime_error("mapping panic");
}

#include "../SwitchVICMapping.cpp"

namespace {
switch_gpu_backing backing;
switch_gpu_backing_info description;
std::array<uint64_t, 40000> pages;

struct Created {
    uint64_t address, first;
    uint32_t count;
    bool writable;
};

std::vector<Created> created;
std::vector<uint64_t> unmapped;
uint64_t nextAddress = 0x30000000;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *, switch_gpu_backing_info *info)
{
    *info = description;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *item)
{
    ++item->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *item)
{
    --item->refs;
}

extern "C" uint64_t switch_vic_mapping_create(const uint64_t *physical, uint32_t count, bool writable)
{
    nextAddress += 0x1000000;
    created.push_back({nextAddress, physical[0], count, writable});
    return nextAddress;
}

extern "C" void switch_vic_mapping_destroy(uint64_t address, uint32_t)
{
    unmapped.push_back(address);
}

namespace {
class SwitchVICMappingTest : public testing::Test {
    void SetUp() override
    {
        if (!gMappings.lock)
            ASSERT_TRUE(switch_vic_mapping_start());
        for (size_t i = 0; i < pages.size(); ++i)
            pages[i] = 0x80000000 + i * 4096;
        description = {};
        description.allocation_id = 42;
        description.allocation_generation = 7;
        description.pages = pages.data();
        description.page_count = pages.size();
        created.clear();
        unmapped.clear();
    }

    void TearDown() override
    {
        for (auto &mapping : gMappings.maps) {
            EXPECT_EQ(mapping.users, 0u);
            gMappings.evict(mapping);
        }
        EXPECT_EQ(gMappings.bytes, 0u);
        EXPECT_EQ(backing.refs, 1u);
    }
};

TEST_F(SwitchVICMappingTest, WarmPlaneRangesKeepTheSameIovaAndExactPhysicalSlice)
{
    auto *luma = switch_vic_mapping_acquire_range(&backing, true, 8, 12);
    auto *same = switch_vic_mapping_acquire_range(&backing, true, 8, 12);
    auto *chroma = switch_vic_mapping_acquire_range(&backing, true, 100, 7);
    ASSERT_NE(luma, nullptr);
    EXPECT_EQ(luma, same);
    ASSERT_NE(chroma, nullptr);
    EXPECT_NE(chroma, luma);
    ASSERT_EQ(created.size(), 2u);
    EXPECT_EQ(created[0].first, pages[8]);
    EXPECT_EQ(created[0].count, 12u);
    EXPECT_EQ(created[1].first, pages[100]);
    EXPECT_EQ(created[1].count, 7u);
    EXPECT_EQ(gMappings.bytes, 19u * 4096);
    switch_vic_mapping_release(luma);
    switch_vic_mapping_release(same);
    switch_vic_mapping_release(chroma);
}

TEST_F(SwitchVICMappingTest, GenerationAndAccessRightsArePartOfTheMappingIdentity)
{
    auto *read = switch_vic_mapping_acquire_range(&backing, false, 3, 5);
    auto *write = switch_vic_mapping_acquire_range(&backing, true, 3, 5);
    ++description.allocation_generation;
    auto *grown = switch_vic_mapping_acquire_range(&backing, false, 3, 5);
    ASSERT_NE(read, nullptr);
    ASSERT_NE(write, nullptr);
    ASSERT_NE(grown, nullptr);
    EXPECT_NE(read, write);
    EXPECT_NE(read, grown);
    EXPECT_EQ(created.size(), 3u);
    switch_vic_mapping_release(read);
    switch_vic_mapping_release(write);
    switch_vic_mapping_release(grown);
}

TEST_F(SwitchVICMappingTest, RangeAndCacheBudgetRejectBeforeChangingTheIommu)
{
    EXPECT_EQ(switch_vic_mapping_acquire_range(&backing, true, UINT32_MAX, 2), nullptr);
    EXPECT_EQ(switch_vic_mapping_acquire_range(&backing, true, 39999, 2), nullptr);
    EXPECT_EQ(switch_vic_mapping_acquire_range(&backing, true, 0, 0), nullptr);
    EXPECT_EQ(switch_vic_mapping_acquire(&backing, true), nullptr);
    EXPECT_TRUE(created.empty());
    auto *tile = switch_vic_mapping_acquire_range(&backing, true, 39999, 1);
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(created[0].first, pages[39999]);
    switch_vic_mapping_release(tile);
}

TEST_F(SwitchVICMappingTest, EvictionCannotUnmapAnActivePlane)
{
    auto *held = switch_vic_mapping_acquire_range(&backing, true, 0, 1);
    ASSERT_NE(held, nullptr);
    const uint64_t address = switch_vic_mapping_address(held);
    for (uint32_t i = 1; i < 32 + 10; ++i) {
        auto *mapping = switch_vic_mapping_acquire_range(&backing, true, i, 1);
        ASSERT_NE(mapping, nullptr);
        switch_vic_mapping_release(mapping);
    }
    EXPECT_EQ(switch_vic_mapping_address(held), address);
    EXPECT_EQ(std::count(unmapped.begin(), unmapped.end(), address), 0);
    switch_vic_mapping_release(held);
}
}  // namespace
