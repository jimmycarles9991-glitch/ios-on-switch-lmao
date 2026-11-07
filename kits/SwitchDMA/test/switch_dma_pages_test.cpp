// SwitchDMAPages.cpp against stand-in descriptors: page lists in descriptor order across scattered segments, the
// refusals (a segment off a page boundary, a list too long), and buffers that start zeroed, out of the caches, and
// leave nothing behind on any failure.
#include "kits/SwitchDMA/SwitchDMAPages.h"

#include <gtest/gtest.h>

namespace {
class SwitchDMAPages : public ::testing::Test {
protected:
    void SetUp() override
    {
        gPages = {};
        switch_dcache_forget();
    }

    void TearDown() override
    {
        EXPECT_EQ(gPages.live, 0);
        EXPECT_EQ(gPages.prepared, 0);
    }
};

IOMemoryDescriptor descriptor(std::vector<SwitchDmaSegment> segments)
{
    IOMemoryDescriptor md;
    md.segments_ = std::move(segments);
    return md;
}
}  // namespace

TEST_F(SwitchDMAPages, ListsEveryHardwarePageInDescriptorOrder)
{
    IOMemoryDescriptor md = descriptor({{0x123450000, 0x4000}, {0x80008000, 0x2000}, {0x9000c000, 0x4000}});
    uint64_t pages[16] = {};
    uint32_t count = 0;
    ASSERT_TRUE(switch_dma_page_list(&md, 0, 0x9001, pages, 16, &count));
    const uint64_t want[] = {0x123450000, 0x123451000, 0x123452000, 0x123453000, 0x80008000,
                             0x80009000,  0x9000c000,  0x9000d000,  0x9000e000,  0x9000f000};
    ASSERT_EQ(count, 10u);
    for (uint32_t i = 0; i < count; ++i)
        EXPECT_EQ(pages[i], want[i]) << i;
}

TEST_F(SwitchDMAPages, StopsAtTheRequestedExtent)
{
    IOMemoryDescriptor md = descriptor({{0x80000000, 0x10000}});
    uint64_t pages[16] = {};
    uint32_t count = 0;
    ASSERT_TRUE(switch_dma_page_list(&md, 0, 0x1000, pages, 16, &count));
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(pages[0], 0x80000000u);
}

TEST_F(SwitchDMAPages, StartsAtThePageHoldingTheOffset)
{
    IOMemoryDescriptor md = descriptor({{0x80000000, 0x4000}, {0x90000000, 0x4000}});
    uint64_t pages[4] = {};
    uint32_t count = 0;
    ASSERT_TRUE(switch_dma_page_list(&md, 0x3800, 0x1000, pages, 4, &count));
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(pages[0], 0x80003000u);
    EXPECT_EQ(pages[1], 0x90000000u);
    EXPECT_FALSE(switch_dma_page_list(&md, 0x3800, 0x1000, pages, 1, &count)) << "two pages, room for one";
}

TEST_F(SwitchDMAPages, CountsAShortTailAsItsWholePage)
{
    IOMemoryDescriptor md = descriptor({{0x80000000, 0x1800}});
    uint64_t pages[4] = {};
    uint32_t count = 0;
    ASSERT_TRUE(switch_dma_page_list(&md, 0, 0x1800, pages, 4, &count));
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(pages[1], 0x80001000u);
}

TEST_F(SwitchDMAPages, RefusesMisalignedSegmentsAndShortLists)
{
    uint64_t pages[4] = {};
    uint32_t count = 7;
    IOMemoryDescriptor misaligned = descriptor({{0x80000800, 0x4000}});
    EXPECT_FALSE(switch_dma_page_list(&misaligned, 0, 0x1000, pages, 4, &count));
    EXPECT_EQ(count, 0u);
    IOMemoryDescriptor partial = descriptor({{0x80000000, 0x1000}, {0x80004000, 0x800}, {0x80005000, 0x1000}});
    EXPECT_FALSE(switch_dma_page_list(&partial, 0, 0x3000, pages, 4, &count));
    IOMemoryDescriptor large = descriptor({{0x80000000, 0x10000}});
    EXPECT_FALSE(switch_dma_page_list(&large, 0, 0x5000, pages, 4, &count));
    IOMemoryDescriptor hole = descriptor({{0x80000000, 0x1000}});
    EXPECT_FALSE(switch_dma_page_list(&hole, 0, 0x2000, pages, 4, &count));
    EXPECT_FALSE(switch_dma_page_list(&large, 0, 0, pages, 4, &count));
}

TEST_F(SwitchDMAPages, AllocatesScatteredZeroedAndCleanBuffers)
{
    gPages.segments = {{0x100004000, 0x4000}, {0x80010000, 0xc000}};
    switch_dma_pages buffer;
    ASSERT_TRUE(switch_dma_pages_alloc(&buffer, 0xf001, kIOMemoryKernelUserShared));
    EXPECT_EQ(gPages.options, kIODirectionInOut | kIOMemoryKernelUserShared);
    EXPECT_EQ(gPages.alignment, PAGE_SIZE);
    EXPECT_EQ(buffer.size, 0x10000u);
    ASSERT_EQ(buffer.count, 16u);
    EXPECT_EQ(buffer.pages[0], 0x100004000u);
    EXPECT_EQ(buffer.pages[3], 0x100007000u);
    EXPECT_EQ(buffer.pages[4], 0x80010000u);
    EXPECT_EQ(buffer.pages[15], 0x8001b000u);
    EXPECT_EQ(gPages.prepared, 1);
    for (size_t i = 0; i < buffer.size; ++i)
        ASSERT_EQ(buffer.va[i], 0) << i;

    const switch_dcache_event *events;
    const size_t count = switch_dcache_recorded(&events);
    ASSERT_EQ(count, buffer.size / 64 + 1);
    EXPECT_STREQ(events[0].op, "civac");
    EXPECT_EQ(events[0].address, (uintptr_t)buffer.va);
    EXPECT_STREQ(events[count - 1].op, "dsb");

    switch_dma_pages_free(&buffer);
    EXPECT_EQ(buffer.memory, nullptr);
    EXPECT_EQ(buffer.pages, nullptr);
    EXPECT_EQ(buffer.count, 0u);
}

TEST_F(SwitchDMAPages, LeavesNothingBehindOnFailure)
{
    switch_dma_pages buffer;
    EXPECT_FALSE(switch_dma_pages_alloc(&buffer, 0));
    gPages.failAllocation = true;
    EXPECT_FALSE(switch_dma_pages_alloc(&buffer, 0x1000));
    gPages.failAllocation = false;
    gPages.failPrepare = true;
    EXPECT_FALSE(switch_dma_pages_alloc(&buffer, 0x1000));
    gPages.failPrepare = false;
    gPages.segments = {{0x80000400, 0x4000}};
    EXPECT_FALSE(switch_dma_pages_alloc(&buffer, 0x4000));
    EXPECT_EQ(buffer.memory, nullptr);
    switch_dma_pages_free(&buffer);
}
