// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_execution_plan.h"

namespace {
using raw::ExecutionKind;

TEST(ExecutionAttribution, NativeRangesStayNativeAcrossEvents)
{
    raw::ExecutionAttribution route;
    route.add(true, false);
    route.add(false, true);
    route.add(true, false);
    EXPECT_EQ(route.native, 2u);
    EXPECT_EQ(route.compatibility, 0u);
    EXPECT_FALSE(route.mixed());
    EXPECT_FALSE(route.onlyCompatibility());
}

TEST(ExecutionAttribution, CompatibilityRangesStayCompatibilityAcrossEvents)
{
    raw::ExecutionAttribution route;
    route.add(false, false);
    route.add(false, true);
    route.add(false, false);
    EXPECT_EQ(route.native, 0u);
    EXPECT_EQ(route.compatibility, 2u);
    EXPECT_FALSE(route.mixed());
    EXPECT_TRUE(route.onlyCompatibility());
    route.add(true, false);
    EXPECT_TRUE(route.mixed());
    EXPECT_FALSE(route.onlyCompatibility());
}

TEST(ExecutionAttribution, EmptyAndEventOnlyHaveNoCompatibilityWork)
{
    raw::ExecutionAttribution route;
    for (unsigned count = 0; count != 3; ++count) {
        EXPECT_EQ(route.native, 0u);
        EXPECT_EQ(route.compatibility, 0u);
        EXPECT_FALSE(route.mixed());
        EXPECT_FALSE(route.onlyCompatibility());
        route.add(false, true);
    }
}

TEST(ExecutionPlan, AdjacentCompatibleEncodersShareOneSubmission)
{
    raw::ExecutionPlan plan;
    plan.append(0, 8, ExecutionKind::native);
    plan.append(8, 12, ExecutionKind::native);
    plan.append(12, 18, ExecutionKind::compatibility);
    plan.append(18, 24, ExecutionKind::native);
    const auto &ranges = plan.ranges();
    ASSERT_EQ(ranges.size(), 3u);
    EXPECT_EQ(ranges[0].from, 0u);
    EXPECT_EQ(ranges[0].to, 12u);
    EXPECT_EQ(ranges[1].kind, ExecutionKind::compatibility);
    EXPECT_EQ(ranges[2].from, 18u);
}

TEST(ExecutionPlan, EventsNeverMergeWithWorkOrEachOther)
{
    raw::ExecutionPlan plan;
    plan.append(0, 4, ExecutionKind::native);
    plan.append(4, 5, ExecutionKind::signal);
    plan.append(5, 6, ExecutionKind::wait);
    plan.append(6, 10, ExecutionKind::native);
    plan.append(10, 11, ExecutionKind::signal);
    ASSERT_EQ(plan.ranges().size(), 5u);
    for (size_t i = 1; i < plan.ranges().size(); ++i)
        EXPECT_EQ(plan.ranges()[i - 1].to, plan.ranges()[i].from);
}

TEST(ExecutionPlan, EmptyAndEventOnlyCommandBuffersKeepExactPositions)
{
    raw::ExecutionPlan empty;
    empty.append(0, 0, ExecutionKind::native);
    EXPECT_TRUE(empty.ranges().empty());
    empty.append(0, 1, ExecutionKind::signal);
    empty.append(1, 2, ExecutionKind::signal);
    ASSERT_EQ(empty.ranges().size(), 2u);
    EXPECT_EQ(empty.ranges()[1].from, 1u);
}

TEST(ExecutionPlan, EveryWaitIsOneTheGpuMayTake)
{
    raw::ExecutionPlan plan;
    plan.append(0, 1, ExecutionKind::wait);
    plan.append(1, 2, ExecutionKind::wait);
    plan.append(2, 5, ExecutionKind::native);
    plan.append(5, 6, ExecutionKind::wait);
    plan.append(6, 7, ExecutionKind::signal);
    plan.append(7, 8, ExecutionKind::wait);
    plan.append(8, 10, ExecutionKind::compatibility);
    plan.append(10, 11, ExecutionKind::wait);
    const auto &ranges = plan.ranges();
    ASSERT_EQ(ranges.size(), 8);
    for (const raw::ExecutionRange &range : ranges)
        EXPECT_EQ(range.gpuWait, range.kind == ExecutionKind::wait) << range.from;
}

raw::ExecutionAttribution routeOf(const std::vector<raw::ExecutionRange> &ranges)
{
    raw::ExecutionAttribution route;
    for (const auto &range : ranges)
        route.add(range.kind == ExecutionKind::native, false);
    return route;
}

// Three render encoders recorded raw share one submission; the middle one's draw is refused while it is encoded
TEST(ExecutionPlan, ARefusedEncoderAloneGoesToCompatibilityAndItsNeighboursStayNative)
{
    raw::ExecutionPlan plan;
    plan.append(0, 5, ExecutionKind::native);
    plan.append(5, 9, ExecutionKind::native);
    plan.append(9, 14, ExecutionKind::native);
    ASSERT_EQ(plan.ranges().size(), 1u);
    const auto parts = raw::isolate(plan.ranges()[0], 5, 9);
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0].from, 0u);
    EXPECT_EQ(parts[0].to, 5u);
    EXPECT_EQ(parts[0].kind, ExecutionKind::native);
    EXPECT_EQ(parts[1].from, 5u);
    EXPECT_EQ(parts[1].to, 9u);
    EXPECT_EQ(parts[1].kind, ExecutionKind::compatibility);
    EXPECT_EQ(parts[2].from, 9u);
    EXPECT_EQ(parts[2].to, 14u);
    EXPECT_EQ(parts[2].kind, ExecutionKind::native);
    const auto route = routeOf(parts);
    EXPECT_EQ(route.native, 2u);
    EXPECT_EQ(route.compatibility, 1u);
    EXPECT_TRUE(route.mixed());
}

TEST(ExecutionPlan, ARefusedFirstOrLastEncoderLeavesOneNativeSide)
{
    const raw::ExecutionRange range{0, 14, ExecutionKind::native};
    const auto first = raw::isolate(range, 0, 5);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[0].kind, ExecutionKind::compatibility);
    EXPECT_EQ(first[0].to, 5u);
    EXPECT_EQ(first[1].kind, ExecutionKind::native);
    EXPECT_EQ(first[1].from, 5u);
    EXPECT_EQ(first[1].to, 14u);
    const auto last = raw::isolate(range, 9, 14);
    ASSERT_EQ(last.size(), 2u);
    EXPECT_EQ(last[0].kind, ExecutionKind::native);
    EXPECT_EQ(last[0].to, 9u);
    EXPECT_EQ(last[1].kind, ExecutionKind::compatibility);
    EXPECT_EQ(last[1].from, 9u);
}

TEST(ExecutionPlan, ARefusalNoEncoderOwnsSendsTheWholeRangeToCompatibility)
{
    const raw::ExecutionRange range{3, 14, ExecutionKind::native};
    for (const auto &[from, to] : std::vector<std::pair<size_t, size_t>>{{14, 14}, {0, 5}, {9, 20}, {6, 6}}) {
        const auto parts = raw::isolate(range, from, to);
        ASSERT_EQ(parts.size(), 1u);
        EXPECT_EQ(parts[0].from, 3u);
        EXPECT_EQ(parts[0].to, 14u);
        EXPECT_EQ(parts[0].kind, ExecutionKind::compatibility);
        EXPECT_TRUE(routeOf(parts).onlyCompatibility());
    }
}

// A refused range that held one encoder is replaced by that encoder alone: nothing native is left to split
TEST(ExecutionPlan, ARefusedSingleEncoderRangeIsOneCompatibilityRange)
{
    const auto parts = raw::isolate({5, 9, ExecutionKind::native}, 5, 9);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].kind, ExecutionKind::compatibility);
    EXPECT_EQ(parts[0].from, 5u);
    EXPECT_EQ(parts[0].to, 9u);
}
}  // namespace
