// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_fetch_order.h"

namespace {
using raw::FetchDraw;
using raw::FetchRefusal;

constexpr uint32_t kPoint = 0, kLine = 1, kLineStrip = 2, kTriangle = 3, kStrip = 4;

FetchRefusal refusal(bool fetchesOwn, FetchDraw draw, bool primitiveId = false)
{
    return raw::fetchRefusal(fetchesOwn, primitiveId, draw);
}

TEST(FetchOrder, OnlyAColorFetchedAndWrittenOutsideTheInterlockCounts)
{
    EXPECT_TRUE(raw::fetchesOwnColor(1, 1, 0));
    EXPECT_TRUE(raw::fetchesOwnColor(3, 2, 1));
    EXPECT_FALSE(raw::fetchesOwnColor(1, 2, 0));
    EXPECT_FALSE(raw::fetchesOwnColor(0, 1, 0));
    EXPECT_FALSE(raw::fetchesOwnColor(1, 1, 1));
}

TEST(FetchOrder, ADrawThatFetchesNoColorItWritesIsNeverRefused)
{
    for (uint32_t primitive : {kPoint, kLine, kLineStrip, kTriangle, kStrip})
        for (bool indirect : {false, true})
            for (bool indexed : {false, true})
                EXPECT_EQ(refusal(false, {primitive, 600, 7, indirect, indexed}), FetchRefusal::None);
}

TEST(FetchOrder, OnePrimitiveIsDrawnWhole)
{
    EXPECT_EQ(refusal(true, {kTriangle, 3, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kStrip, 3, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kPoint, 1, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kLine, 2, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kLineStrip, 2, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 0, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 6, 0}), FetchRefusal::None);
}

// A state with no storage form: lists and strips drawn a triangle at a time, no vertex read
TEST(FetchOrder, ListsAndStripsAreDrawnATriangleAtATime)
{
    EXPECT_EQ(refusal(true, {kTriangle, 6, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kStrip, 4, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 36, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 6, 1, false, true}), FetchRefusal::None);
}

// RenderBox's quad, an indexed strip of four vertices, is refused only where its state has no storage form: a
// state drawn inside the pixel interlock (fetchesOwnColor false) takes any shape whole
TEST(FetchOrder, OnlyWhatOneTriangleADrawCannotKeepIsRefused)
{
    EXPECT_EQ(refusal(true, {kStrip, 4, 1, false, true}), FetchRefusal::Restart);
    EXPECT_EQ(refusal(raw::fetchesOwnColor(1, 1, 1), {kStrip, 4, 1, false, true}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 3 * raw::kSequentialTriangles, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 3 * raw::kSequentialTriangles + 3, 1}), FetchRefusal::Long);
    EXPECT_EQ(refusal(true, {kStrip, raw::kSequentialTriangles + 3, 1}), FetchRefusal::Long);
    EXPECT_EQ(refusal(true, {kTriangle, 6, 1}, true), FetchRefusal::PrimitiveId);
    EXPECT_EQ(refusal(true, {kTriangle, 3, 1}, true), FetchRefusal::None);
}

TEST(FetchOrder, InstancesTopologyAndIndirectCountsAreRefusedAsRecorded)
{
    EXPECT_EQ(refusal(true, {kTriangle, 6, 2}), FetchRefusal::Instances);
    EXPECT_EQ(refusal(true, {kTriangle, 3, 2}), FetchRefusal::Instances);
    EXPECT_EQ(refusal(true, {kPoint, 2, 1}), FetchRefusal::Topology);
    EXPECT_EQ(refusal(true, {kLine, 4, 1}), FetchRefusal::Topology);
    EXPECT_EQ(refusal(true, {kLineStrip, 3, 1}), FetchRefusal::Topology);
    EXPECT_EQ(refusal(true, {kTriangle, 0, 1, true}), FetchRefusal::Indirect);
    EXPECT_EQ(refusal(true, {kPoint, 0, 1, true}), FetchRefusal::Indirect);
}

TEST(FetchOrder, TheReasonsKeepTheEncodeLinesWording)
{
    EXPECT_STREQ(raw::fetchRefusalWhy(FetchRefusal::Indirect),
                 "indirect framebuffer-fetch draws require an interlocked program");
    for (FetchRefusal r : {FetchRefusal::Topology, FetchRefusal::Instances, FetchRefusal::Restart, FetchRefusal::Long,
                           FetchRefusal::PrimitiveId, FetchRefusal::Geometry})
        EXPECT_STREQ(raw::fetchRefusalWhy(r),
                     "framebuffer-fetch ordering requires an interlocked program or available bounded geometry");
}

TEST(FetchOrder, EveryReasonHasALogName)
{
    for (size_t r = 0; r < raw::kFetchRefusals; r++)
        EXPECT_NE(raw::kFetchRefusalNames[r], nullptr);
    EXPECT_STREQ(raw::kFetchRefusalNames[size_t(FetchRefusal::Restart)], "restart");
    EXPECT_STREQ(raw::kFetchRefusalNames[size_t(FetchRefusal::Geometry)], "geometry");
}

}  // namespace
