// SwitchDMABuffer.cpp against a stand-in IOBufferMemoryDescriptor: the options and mask it asks for, the address
// ceiling it enforces, that nothing leaks on any failure, and that a buffer starts zeroed and out of the caches.
#include "kits/SwitchDMA/SwitchDMABuffer.h"

#include <gtest/gtest.h>

#include <string>

namespace {
class SwitchDMABuffer : public ::testing::Test {
protected:
    void SetUp() override
    {
        gDma = {};
        switch_dcache_forget();
    }

    void TearDown() override
    {
        EXPECT_EQ(gDma.live, 0);
        EXPECT_EQ(gDma.prepared, 0);
    }
};

TEST_F(SwitchDMABuffer, AllocatesContiguousUnmappedBelowTheMask)
{
    switch_dma_buffer buffer;
    ASSERT_TRUE(switch_dma_alloc(&buffer, 200));
    EXPECT_EQ(gDma.options, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone);
    EXPECT_EQ(gDma.mask, kSwitchDMABelow4G);
    EXPECT_EQ(buffer.pa, 0x80000000u);
    EXPECT_EQ(buffer.size, 200u);
    EXPECT_EQ(gDma.prepared, 1);
    for (size_t i = 0; i < buffer.size; ++i)
        ASSERT_EQ(buffer.va[i], 0) << i;

    const switch_dcache_event *events;
    const size_t count = switch_dcache_recorded(&events);
    ASSERT_GE(count, 2u);
    EXPECT_STREQ(events[0].op, "civac");
    EXPECT_STREQ(events[count - 1].op, "dsb");

    switch_dma_free(&buffer);
    EXPECT_EQ(buffer.memory, nullptr);
    EXPECT_EQ(buffer.va, nullptr);
}

TEST_F(SwitchDMABuffer, PassesExtraOptionsAndMask)
{
    switch_dma_buffer buffer;
    ASSERT_TRUE(switch_dma_alloc(&buffer, 4096, 0xffff0000ull, kIOMapWriteCombineCache));
    EXPECT_EQ(gDma.options & kIOMapWriteCombineCache, kIOMapWriteCombineCache);
    EXPECT_EQ(gDma.mask, 0xffff0000ull);
    const switch_dcache_event *events;
    const size_t count = switch_dcache_recorded(&events);
    ASSERT_EQ(count, 1u);
    EXPECT_STREQ(events[0].op, "oshst");
    switch_dma_free(&buffer);
}

TEST_F(SwitchDMABuffer, RefusesWhatTheDeviceCannotReach)
{
    switch_dma_buffer buffer;
    gDma.pa = 0xfffff000;
    EXPECT_FALSE(switch_dma_alloc(&buffer, 0x2000));
    EXPECT_EQ(buffer.memory, nullptr);

    gDma.pa = 0xffffe000;
    EXPECT_TRUE(switch_dma_alloc(&buffer, 0x2000));
    switch_dma_free(&buffer);

    gDma.pa = 0x80000000;
    gDma.contiguous = 0x1000;
    EXPECT_FALSE(switch_dma_alloc(&buffer, 0x2000));
}

TEST_F(SwitchDMABuffer, FailuresLeakNothing)
{
    switch_dma_buffer buffer;
    gDma.failAllocation = true;
    EXPECT_FALSE(switch_dma_alloc(&buffer, 64));
    gDma.failAllocation = false;
    gDma.failPrepare = true;
    EXPECT_FALSE(switch_dma_alloc(&buffer, 64));
    EXPECT_FALSE(switch_dma_alloc(&buffer, 0));
    EXPECT_FALSE(switch_dma_alloc(&buffer, 64, 0));
}

TEST_F(SwitchDMABuffer, FreeingAnEmptyBufferIsHarmless)
{
    switch_dma_buffer buffer = {};
    switch_dma_free(&buffer);
}
}  // namespace
