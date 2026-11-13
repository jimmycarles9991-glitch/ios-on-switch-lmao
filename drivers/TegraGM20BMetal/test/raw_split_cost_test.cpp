// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_split_cost.h"
#include "../raw/runtime/raw_raster_area.h"
#include <gtest/gtest.h>
#include <cstring>

namespace {

raw::PackSplit fixture()
{
    raw::PackSplit split{};
    split.batchVersion = 1;
    split.batchThreads = 32;
    split.frontierBytes = 128;
    split.thresholdPixels = 10000;
    split.cost = {1, 0, 4000, 4000, 100, 100, 128, 16, 32};
    return split;
}

TEST(SplitCost, BatchAmortizesLaunchWhileTinyDrawsStayWhole)
{
    const auto split = fixture();
    raw::SplitPassPlan plan;
    EXPECT_FALSE(plan.accepts(&split, split, 1024));
    EXPECT_TRUE(plan.accepts(&split, split, 20000));
    plan.add(&split, split, 64, 1024);
    plan.finish();
    EXPECT_TRUE(plan.accepts(&split, split, 1024));
    EXPECT_FALSE(plan.accepts(&split, split, 4));
}

TEST(SplitCost, OneDrainIsSharedByDistinctProfitablePrograms)
{
    const auto split = fixture();
    raw::SplitPassPlan one;
    one.add(&split, split, 1, 10000);
    one.finish();
    EXPECT_FALSE(one.accepts(&split, split, 10000));
    raw::SplitPassPlan many;
    int keys[8]{};
    for (auto &key : keys)
        many.add(&key, split, 1, 10000);
    many.finish();
    for (auto &key : keys)
        EXPECT_TRUE(many.accepts(&key, split, 10000));
}

TEST(SplitCost, AccumulatesRasterGroupsAndRetainsHashAfterReuse)
{
    const auto split = fixture();
    raw::SplitPassPlan plan;
    int keys[100]{};
    for (auto &key : keys) {
        plan.add(&key, split, 32, 1024);
        plan.add(&key, split, 32, 1024);
    }
    plan.finish();
    for (auto &key : keys)
        EXPECT_TRUE(plan.accepts(&key, split, 1024));
    plan.clear();
    plan.add(&keys[0], split, 1, 4);
    plan.finish();
    EXPECT_FALSE(plan.accepts(&keys[0], split, 1024));
    EXPECT_FALSE(plan.accepts(&keys[1], split, 1024));
    EXPECT_TRUE(plan.accepts(&keys[1], split, 20000));
}

TEST(SplitCost, LargeFrontierAndPrologueCanOutweighSavedInstructions)
{
    auto split = fixture();
    split.cost.prologueWarpCycles = 100000;
    split.frontierBytes = 4096;
    raw::SplitPassPlan plan;
    plan.add(&split, split, 64, 1024);
    plan.finish();
    EXPECT_FALSE(plan.accepts(&split, split, 1024));
}

TEST(SplitCost, SaturatingCountersCannotWrapIntoAnUnderpricedPass)
{
    auto split = fixture();
    split.cost.prologueWarpCycles = UINT32_MAX;
    split.cost.prologueInstructions = UINT32_MAX;
    split.cost.savedPixelInstructions = 1;
    split.cost.savedPixelCycles = 1;
    raw::SplitPassPlan plan;
    for (unsigned i = 0; i < 3; ++i)
        plan.add(&split, split, UINT32_MAX, UINT64_MAX);
    plan.finish();
    EXPECT_FALSE(plan.accepts(&split, split, 1024));
}

TEST(SplitCost, LegacyMetadataKeepsScalarThreshold)
{
    auto split = fixture();
    split.cost = {};
    raw::SplitPassPlan plan;
    plan.add(&split, split, 1000, 20000);
    plan.finish();
    EXPECT_FALSE(plan.accepts(&split, split, 9999));
    EXPECT_TRUE(plan.accepts(&split, split, 10000));
}

TEST(SplitCost, RasterEstimateClipsScissorAndFractionalViewport)
{
    raw::RasterState state;
    EXPECT_EQ(raw::rasterPixelLimit(state, 1280, 720), 1280u * 720);
    state.set = raw::RasterState::Scissor | raw::RasterState::Viewport;
    state.scissor = {20, 20, 200, 200};
    const double viewport[] = {40.5, 10.25, 100.0, 100.0, 0, 1};
    std::memcpy(state.viewport.data(), viewport, sizeof(viewport));
    EXPECT_EQ(raw::rasterPixelLimit(state, 1280, 720), 101u * 91);
    state.scissor = {UINT32_MAX - 10, 0, 100, 100};
    EXPECT_EQ(raw::rasterPixelLimit(state, 1280, 720), 0u);
}

}  // namespace
