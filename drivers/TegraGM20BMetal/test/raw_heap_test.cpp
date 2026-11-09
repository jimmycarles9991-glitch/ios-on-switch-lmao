// SPDX-License-Identifier: MIT
// raw/runtime/raw_heap.h: where an MTLHeap's resources lie in its one span. Metal's heap contract, which RenderBox's
// ResourceHeap relies on: placements never exceed the heap (a full heap answers nil), a range made aliasable or freed is
// taken by later placements, usedSize counts what is placed, and maxAvailableSizeWithAlignment is the largest placement.
#include "../raw/runtime/raw_heap.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kKiB = 1024, kBlock = 16 * kKiB;

TEST(RawHeap, PlacementsStayInsideTheHeap)
{
    raw::HeapPlacement h(256 * kKiB);
    uint64_t a, b, c;
    ASSERT_TRUE(h.place(128 * kKiB, kBlock, a));
    ASSERT_TRUE(h.place(128 * kKiB, kBlock, b));
    EXPECT_NE(a, b);
    EXPECT_EQ(h.used(), 256 * kKiB);
    EXPECT_FALSE(h.place(kBlock, kBlock, c)) << "a full heap answers nil";
    EXPECT_EQ(h.largestFree(1), 0u);
}

TEST(RawHeap, AnAliasableRangeIsTakenAgain)
{
    raw::HeapPlacement h(256 * kKiB);
    uint64_t first, second, again;
    ASSERT_TRUE(h.place(192 * kKiB, kBlock, first));
    ASSERT_TRUE(h.place(64 * kKiB, kBlock, second));
    h.release(first, 192 * kKiB);
    EXPECT_EQ(h.used(), 64 * kKiB);
    ASSERT_TRUE(h.place(160 * kKiB, kBlock, again));
    EXPECT_EQ(again, first) << "the memory a released texture held";
}

TEST(RawHeap, AlignmentIsKeptAndItsGapStaysFree)
{
    raw::HeapPlacement h(512 * kKiB);
    uint64_t small, aligned, gap;
    ASSERT_TRUE(h.place(4 * kKiB, 256, small));
    ASSERT_TRUE(h.place(64 * kKiB, 64 * kKiB, aligned));
    EXPECT_EQ(aligned % (64 * kKiB), 0u);
    EXPECT_EQ(h.used(), 68 * kKiB);
    ASSERT_TRUE(h.place(32 * kKiB, 256, gap));
    EXPECT_LT(gap, aligned) << "first fit takes the gap the alignment left";
}

TEST(RawHeap, ReleasedNeighboursCoalesce)
{
    raw::HeapPlacement h(192 * kKiB);
    uint64_t a, b, c, whole;
    ASSERT_TRUE(h.place(64 * kKiB, kBlock, a));
    ASSERT_TRUE(h.place(64 * kKiB, kBlock, b));
    ASSERT_TRUE(h.place(64 * kKiB, kBlock, c));
    h.release(a, 64 * kKiB);
    h.release(c, 64 * kKiB);
    EXPECT_EQ(h.largestFree(kBlock), 64 * kKiB);
    h.release(b, 64 * kKiB);
    EXPECT_EQ(h.used(), 0u);
    EXPECT_EQ(h.largestFree(kBlock), 192 * kKiB);
    ASSERT_TRUE(h.place(192 * kKiB, kBlock, whole));
    EXPECT_EQ(whole, 0u);
}

TEST(RawHeap, LargestFreeHonoursAlignment)
{
    raw::HeapPlacement h(128 * kKiB);
    uint64_t at;
    ASSERT_TRUE(h.place(4 * kKiB, 256, at));
    EXPECT_EQ(h.largestFree(256), 124 * kKiB);
    EXPECT_EQ(h.largestFree(64 * kKiB), 64 * kKiB);
}

}  // namespace
