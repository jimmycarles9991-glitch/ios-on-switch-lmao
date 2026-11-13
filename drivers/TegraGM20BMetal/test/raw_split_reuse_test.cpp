// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_split_reuse.h"
#include <gtest/gtest.h>

namespace {

TEST(RawSplitReuse, IdenticalInputsReuseOneFrontierWithoutGrowing)
{
    raw::SplitReuse cache;
    raw::SplitInputs inputs;
    inputs.program = &cache;
    inputs.buffers[1] = 0x800000;
    inputs.bytes[1] = 256;
    EXPECT_EQ(cache.find(inputs), 0u);
    cache.remember(inputs, 0x200000);
    const auto capacity = cache.capacity();
    for (unsigned i = 0; i < 10000; ++i)
        EXPECT_EQ(cache.find(inputs), 0x200000u);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.capacity(), capacity);
}

TEST(RawSplitReuse, EveryProgramPushAddressAndExtentAffectsReuse)
{
    raw::SplitReuse cache;
    raw::SplitInputs original;
    cache.remember(original, 0x10000);
    auto other = original;
    other.program = &cache;
    EXPECT_EQ(cache.find(other), 0u);
    for (size_t i = 0; i < original.push.size(); ++i) {
        other = original;
        other.push[i] = 1u << (i % 32);
        EXPECT_EQ(cache.find(other), 0u);
    }
    for (size_t i = 0; i < original.buffers.size(); ++i) {
        other = original;
        other.buffers[i] = 0x100000000ULL;
        EXPECT_EQ(cache.find(other), 0u);
        other = original;
        other.bytes[i] = 256;
        EXPECT_EQ(cache.find(other), 0u);
    }
    EXPECT_EQ(cache.find(original), 0x10000u);
}

TEST(RawSplitReuse, GrowthPreservesInterleavedInputsAndResetDropsOldGpuAddresses)
{
    raw::SplitReuse cache;
    for (uint32_t i = 0; i < 2000; ++i) {
        raw::SplitInputs inputs;
        inputs.push[17] = i;
        cache.remember(inputs, 0x80000 + uint64_t(i) * 256);
    }
    const auto capacity = cache.capacity();
    for (uint32_t n = 0; n < 2000; ++n) {
        const uint32_t i = n * 997 % 2000;
        raw::SplitInputs inputs;
        inputs.push[17] = i;
        EXPECT_EQ(cache.find(inputs), 0x80000 + uint64_t(i) * 256);
    }
    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.capacity(), capacity);
    for (uint32_t i = 0; i < 2000; ++i) {
        raw::SplitInputs inputs;
        inputs.push[17] = i;
        EXPECT_EQ(cache.find(inputs), 0u);
        cache.remember(inputs, 0x200000 + uint64_t(i) * 256);
        EXPECT_EQ(cache.find(inputs), 0x200000 + uint64_t(i) * 256);
    }
    EXPECT_EQ(cache.capacity(), capacity);
}

TEST(RawSplitReuse, CacheCapacityIsBoundedAndExcessDrawsRemainUncached)
{
    raw::SplitReuse cache;
    for (uint32_t i = 0; i < 10000; ++i) {
        raw::SplitInputs inputs;
        inputs.push[0] = i;
        cache.remember(inputs, uint64_t(i + 1) * 256);
    }
    EXPECT_EQ(cache.size(), 4096u);
    EXPECT_EQ(cache.capacity(), 8192u);
    raw::SplitInputs inputs;
    inputs.push[0] = 9000;
    EXPECT_EQ(cache.find(inputs), 0u);
    inputs.push[0] = 123;
    EXPECT_EQ(cache.find(inputs), 124u * 256);
}

}  // namespace
