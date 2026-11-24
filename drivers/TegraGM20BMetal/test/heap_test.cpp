// SPDX-License-Identifier: MIT
// heap.h: where an MTLHeap's resources lie in its one span. Metal's heap contract, which RenderBox's
// ResourceHeap relies on: placements never exceed the heap (a full heap answers nil), a range made aliasable or freed is
// taken by later placements, usedSize counts what is placed, and maxAvailableSizeWithAlignment is the largest placement.
#include "../heap.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kKiB = 1024, kBlock = 16 * kKiB;

TEST(GM20BHeap, PlacementsStayInsideTheHeap)
{
    gm20b::HeapPlacement h(256 * kKiB);
    uint64_t a, b, c;
    ASSERT_TRUE(h.place(128 * kKiB, kBlock, a));
    ASSERT_TRUE(h.place(128 * kKiB, kBlock, b));
    EXPECT_NE(a, b);
    EXPECT_EQ(h.used(), 256 * kKiB);
    EXPECT_FALSE(h.place(kBlock, kBlock, c)) << "a full heap answers nil";
    EXPECT_EQ(h.largestFree(1), 0u);
}

TEST(GM20BHeap, AnAliasableRangeIsTakenAgain)
{
    gm20b::HeapPlacement h(256 * kKiB);
    uint64_t first, second, again;
    ASSERT_TRUE(h.place(192 * kKiB, kBlock, first));
    ASSERT_TRUE(h.place(64 * kKiB, kBlock, second));
    h.release(first, 192 * kKiB);
    EXPECT_EQ(h.used(), 64 * kKiB);
    ASSERT_TRUE(h.place(160 * kKiB, kBlock, again));
    EXPECT_EQ(again, first) << "the memory a released texture held";
}

TEST(GM20BHeap, AlignmentIsKeptAndItsGapStaysFree)
{
    gm20b::HeapPlacement h(512 * kKiB);
    uint64_t small, aligned, gap;
    ASSERT_TRUE(h.place(4 * kKiB, 256, small));
    ASSERT_TRUE(h.place(64 * kKiB, 64 * kKiB, aligned));
    EXPECT_EQ(aligned % (64 * kKiB), 0u);
    EXPECT_EQ(h.used(), 68 * kKiB);
    ASSERT_TRUE(h.place(32 * kKiB, 256, gap));
    EXPECT_LT(gap, aligned) << "first fit takes the gap the alignment left";
}

TEST(GM20BHeap, ReleasedNeighboursCoalesce)
{
    gm20b::HeapPlacement h(192 * kKiB);
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

TEST(GM20BHeap, LargestFreeHonoursAlignment)
{
    gm20b::HeapPlacement h(128 * kKiB);
    uint64_t at;
    ASSERT_TRUE(h.place(4 * kKiB, 256, at));
    EXPECT_EQ(h.largestFree(256), 124 * kKiB);
    EXPECT_EQ(h.largestFree(64 * kKiB), 64 * kKiB);
}

TEST(GM20BHeap, ExplicitPlacementsMayOverlapWithoutConsumingAutomaticSpace)
{
    gm20b::HeapPlacement h(128 * kKiB, true);
    EXPECT_TRUE(h.placeAt(64 * kKiB, 256, 256));
    EXPECT_TRUE(h.placeAt(32 * kKiB, 256, 256));
    EXPECT_TRUE(h.placeAt(64 * kKiB, 256, 512));
    EXPECT_EQ(h.used(), 0u);
    EXPECT_EQ(h.largestFree(256), 0u);
    uint64_t automatic = UINT64_MAX;
    EXPECT_FALSE(h.place(256, 256, automatic));
    EXPECT_EQ(automatic, UINT64_MAX);
    h.release(256, 64 * kKiB);
    EXPECT_EQ(h.used(), 0u);
    EXPECT_TRUE(h.placeAt(64 * kKiB, 256, 256));
}

TEST(GM20BHeap, ExplicitPlacementChecksExactBoundsAlignmentAndOverflow)
{
    gm20b::HeapPlacement h(65537, true);
    EXPECT_TRUE(h.placeAt(1, 1, 65536));
    EXPECT_FALSE(h.placeAt(2, 1, 65536));
    EXPECT_FALSE(h.placeAt(256, 256, 1));
    EXPECT_FALSE(h.placeAt(0, 256, 0));
    EXPECT_FALSE(h.placeAt(256, 0, 0));
    EXPECT_FALSE(h.placeAt(256, 3, 0));
    EXPECT_FALSE(h.placeAt(256, 256, UINT64_MAX - 255));
    EXPECT_FALSE(h.placeAt(UINT64_MAX, 1, 1));
    gm20b::HeapPlacement automatic(65537);
    EXPECT_FALSE(automatic.placeAt(256, 256, 0));
}

TEST(GM20BHeap, KindRangesCrossOnlyOverlappingDifferentKinds)
{
    constexpr uint32_t kKindPitch = 0, kKindColor = 0xfe, kKindZeta = 0xdb;

    gm20b::HeapPlacement placement(128 * kKiB, true);
    bool crossed = true;
    ASSERT_TRUE(placement.placeAt(16 * kKiB, 512, 0, kKindColor, &crossed));
    EXPECT_FALSE(crossed);
    EXPECT_FALSE(placement.multiKind());

    ASSERT_TRUE(placement.placeAt(16 * kKiB, 512, 0, kKindColor, &crossed));
    EXPECT_FALSE(crossed) << "aliasing under the same PTE kind does not cross";
    EXPECT_FALSE(placement.multiKind());

    ASSERT_TRUE(placement.placeAt(16 * kKiB, 512, 16 * kKiB, kKindPitch, &crossed));
    EXPECT_FALSE(crossed) << "side-by-side GOB-aligned placements of different kinds do not overlap";
    EXPECT_FALSE(placement.multiKind());

    ASSERT_TRUE(placement.placeAt(16 * kKiB, 256, 0, kKindPitch, &crossed));
    EXPECT_TRUE(crossed) << "overlapping a block-linear range with a pitch buffer crosses kinds";
    EXPECT_TRUE(placement.multiKind());

    ASSERT_TRUE(placement.placeAt(256, 256, 32 * kKiB, kKindColor, &crossed));
    EXPECT_FALSE(crossed);
    ASSERT_TRUE(placement.placeAt(256, 256, 32 * kKiB + 256, kKindPitch, &crossed));
    EXPECT_TRUE(crossed) << "two halves of one 512-byte GOB share L2 GOB state";

    gm20b::HeapPlacement automatic(64 * kKiB);
    uint64_t a = 0, b = 0, c = 0;
    ASSERT_TRUE(automatic.place(16 * kKiB, kBlock, a, kKindColor, &crossed));
    EXPECT_FALSE(crossed);
    ASSERT_TRUE(automatic.place(16 * kKiB, kBlock, b, kKindZeta, &crossed));
    EXPECT_FALSE(crossed);
    EXPECT_FALSE(automatic.multiKind());
    automatic.release(a, 16 * kKiB);
    ASSERT_TRUE(automatic.place(16 * kKiB, kBlock, c, kKindPitch, &crossed));
    EXPECT_EQ(c, a);
    EXPECT_TRUE(crossed) << "reusing released automatic-heap memory under a different kind crosses";
    EXPECT_TRUE(automatic.multiKind());

    gm20b::KindRanges uses;
    uses.record(0, 4096, kKindColor, 1);
    uses.record(4096, 4096, kKindZeta, 3);
    EXPECT_TRUE(uses.overlapsOther(0, 4096, kKindPitch, 1));
    EXPECT_FALSE(uses.overlapsOther(0, 4096, kKindPitch, 2));
    uses.pruneBefore(2);
    EXPECT_FALSE(uses.overlapsOther(0, 4096, kKindPitch, 0));
    EXPECT_TRUE(uses.overlapsOther(4096, 4096, kKindPitch, 2));
}

}  // namespace
