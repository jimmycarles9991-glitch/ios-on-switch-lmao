// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../fetch_order.h"

namespace {
using gm20b::FetchDraw;
using gm20b::FetchRefusal;

constexpr uint32_t kPoint = 0, kLine = 1, kLineStrip = 2, kTriangle = 3, kStrip = 4;

FetchRefusal refusal(bool fetchesOwn, FetchDraw draw, bool primitiveId = false)
{
    return gm20b::fetchRefusal(fetchesOwn, primitiveId, draw);
}

TEST(FetchOrder, OnlyAColorFetchedAndWrittenOutsideTheInterlockCounts)
{
    EXPECT_TRUE(gm20b::fetchesOwnColor(1, 1, 0));
    EXPECT_TRUE(gm20b::fetchesOwnColor(3, 2, 1));
    EXPECT_FALSE(gm20b::fetchesOwnColor(1, 2, 0));
    EXPECT_FALSE(gm20b::fetchesOwnColor(0, 1, 0));
    EXPECT_FALSE(gm20b::fetchesOwnColor(1, 1, 1));
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

// RenderBox's quad, an indexed strip of four vertices (its uber_fragment over an icon), is drawn a triangle at a time
// where its state's storage form cannot view the target; it is refused only where its faces decide its pixels, as a
// restart would leave the faces past it to its indices
TEST(FetchOrder, OnlyWhatOneTriangleADrawCannotKeepIsRefused)
{
    EXPECT_EQ(refusal(true, {kStrip, 4, 1, false, true}), FetchRefusal::None);
    EXPECT_EQ(gm20b::sequentialRefusal(false, {kStrip, 4, 1, false, true}), FetchRefusal::None);
    EXPECT_EQ(gm20b::sequentialRefusal(false, {kStrip, 4, 1, false, true, true}), FetchRefusal::Restart);
    EXPECT_EQ(gm20b::sequentialRefusal(false, {kStrip, 4, 1, false, false, true}), FetchRefusal::None);
    EXPECT_EQ(gm20b::sequentialRefusal(false, {kTriangle, 6, 1, false, true, true}), FetchRefusal::None);
    EXPECT_EQ(refusal(gm20b::fetchesOwnColor(1, 1, 1), {kStrip, 4, 1, false, true}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 3 * gm20b::kSequentialTriangles, 1}), FetchRefusal::None);
    EXPECT_EQ(refusal(true, {kTriangle, 3 * gm20b::kSequentialTriangles + 3, 1}), FetchRefusal::Long);
    EXPECT_EQ(refusal(true, {kStrip, gm20b::kSequentialTriangles + 3, 1}), FetchRefusal::Long);
    EXPECT_EQ(refusal(true, {kStrip, gm20b::kSequentialTriangles + 3, 1, false, true}), FetchRefusal::Long);
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
    EXPECT_STREQ(gm20b::fetchRefusalWhy(FetchRefusal::Indirect),
                 "indirect framebuffer-fetch draws require an interlocked program");
    for (FetchRefusal r : {FetchRefusal::Topology, FetchRefusal::Instances, FetchRefusal::Restart, FetchRefusal::Long,
                           FetchRefusal::PrimitiveId, FetchRefusal::Geometry})
        EXPECT_STREQ(gm20b::fetchRefusalWhy(r),
                     "framebuffer-fetch ordering requires an interlocked program or available bounded geometry");
}

TEST(FetchOrder, EveryReasonHasALogName)
{
    for (size_t r = 0; r < gm20b::kFetchRefusals; r++)
        EXPECT_NE(gm20b::kFetchRefusalNames[r], nullptr);
    EXPECT_STREQ(gm20b::kFetchRefusalNames[size_t(FetchRefusal::Restart)], "restart");
    EXPECT_STREQ(gm20b::kFetchRefusalNames[size_t(FetchRefusal::Geometry)], "geometry");
}

}  // namespace
