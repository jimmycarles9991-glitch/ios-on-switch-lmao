#include <gtest/gtest.h>

#include "../SwitchSdWindow.h"
#include "../SwitchSdWriteFence.h"
#include "bazel/testing/require.h"

TEST(SdWindow, ChecksPartitionAndCardCommandWidth)
{
    const SwitchSdWindow left{128, 32}, right{160, 2}, final{UINT32_MAX - 1, 2}, empty{}, overflow{UINT64_MAX - 1, 8};
    REQUIRE(left.contains(128, 32) && left.contains(159, 1));
    REQUIRE(!left.contains(127, 2) && !left.contains(159, 2) && !left.contains(160, 1));
    REQUIRE(right.contains(160, 1) && !right.contains(160, 0));
    REQUIRE(final.contains(UINT32_MAX - 1, 2) && final.contains(UINT32_MAX, 1));
    REQUIRE(!final.contains(UINT32_MAX, 2) && !empty.contains(0, 1) && !overflow.contains(UINT64_MAX, 1));
}

TEST(SdWriteFence, FailedMergedWriteReachesOverlappingBarriersAndCanRecover)
{
    SwitchSdWriteFence fence, other;
    ++fence.submitted;
    SwitchSdWriteFence::Barrier first, second, later;
    fence.begin(first);
    ++fence.submitted;
    fence.begin(second);
    ++fence.submitted;
    fence.finish(3, true);
    EXPECT_TRUE(first.failed);
    EXPECT_TRUE(second.failed);
    EXPECT_EQ(other.completed, 0u);
    fence.end(first, false);
    fence.end(second, false);
    fence.begin(later);
    EXPECT_TRUE(later.failed);
    fence.end(later, true);
    fence.begin(later);
    EXPECT_TRUE(later.failed);
    fence.end(later, false);
    ++fence.submitted;
    fence.begin(later);
    fence.finish(1, false);
    EXPECT_EQ(fence.completed, later.target);
    EXPECT_FALSE(later.failed);
    fence.end(later, false);
}
