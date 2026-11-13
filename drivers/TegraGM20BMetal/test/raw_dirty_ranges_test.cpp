// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_dirty_ranges.h"
#include <gtest/gtest.h>
#include <array>
#include <random>

namespace {
void add(raw::DirtyRanges &ranges, uintptr_t address, uint64_t bytes, bool invalidate = false)
{
    ranges.add(reinterpret_cast<const void *>(address), bytes, invalidate);
}

TEST(DirtyRanges, InterleavedRepeatedBindingsCleanEachCacheLineOnce)
{
    raw::DirtyRanges ranges;
    for (unsigned i = 0; i < 1000; ++i) {
        add(ranges, 0x1001, 15);
        add(ranges, 0x2003, 16);
    }
    const auto &merged = ranges.merged();
    ASSERT_EQ(merged.size(), 2);
    EXPECT_EQ(merged[0].begin, 0x1000u);
    EXPECT_EQ(merged[0].end, 0x1040u);
    EXPECT_EQ(merged[1].begin, 0x2000u);
    EXPECT_EQ(merged[1].end, 0x2040u);
}

TEST(DirtyRanges, BridgingSpansMergeBothWaysAndPreserveInvalidate)
{
    raw::DirtyRanges ranges;
    add(ranges, 0x3000, 0x100, true);
    add(ranges, 0x1000, 0x100);
    add(ranges, 0x1100, 0x1f00);
    const auto &merged = ranges.merged();
    ASSERT_EQ(merged.size(), 1);
    EXPECT_EQ(merged[0].begin, 0x1000u);
    EXPECT_EQ(merged[0].end, 0x3100u);
    EXPECT_TRUE(merged[0].invalidate);
    ranges.clear();
    EXPECT_TRUE(ranges.merged().empty());
}

TEST(DirtyRanges, RandomOverlapsCoverExactlyTheRequestedCacheLines)
{
    raw::DirtyRanges ranges;
    std::array<uint8_t, 1024> expected{}, visits{};
    std::mt19937 random(0x57);
    for (unsigned i = 0; i < 200; ++i) {
        const uint32_t first = random() % (expected.size() * 64 - 128);
        const uint32_t bytes = 1 + random() % 128;
        const bool invalidate = random() & 1;
        add(ranges, 0x1000 + first, bytes, invalidate);
        for (uint32_t line = first / 64; line <= (first + bytes - 1) / 64; ++line)
            expected[line] |= invalidate ? 3 : 1;
    }
    for (const auto &range : ranges.merged())
        for (uintptr_t address = range.begin; address < range.end; address += 64) {
            const auto line = (address - 0x1000) / 64;
            ASSERT_LT(line, visits.size());
            EXPECT_EQ(visits[line]++, 0);
            EXPECT_NE(expected[line], 0);
            if (expected[line] & 2)
                EXPECT_TRUE(range.invalidate);
        }
    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(visits[i], expected[i] != 0);
}
}  // namespace
