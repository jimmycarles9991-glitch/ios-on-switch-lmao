// SPDX-License-Identifier: BSD-2-Clause-Patent
// The converted plane's size: no more pixels than the source has on either axis (a layer shown larger is scaled up by
// the window, not by VIC), a plane kept while still content moves or shrinks to half wholly on the screen, and a new
// one once it grows past it or shrinks below half; a layer wholly off the screen is shown as nothing, not dropped.
#include "switch_plane_conversion_harness.h"

namespace {

// 128x64 shown at 256x128: VIC writes 128x64 (pitch 512, 32 KiB) where a plane the size on the screen took 128 KiB, and
// the window fetches the same quarter.
TEST_F(PlaneConversion, AnUpscaledVideoIsConvertedAtItsSourceSizeAndScaledUpByTheWindow)
{
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(lastOperation.srcW, 128u << 16);
    EXPECT_EQ(lastOperation.dstW, 128u);
    EXPECT_EQ(lastOperation.dstH, 64u);
    EXPECT_EQ(image.size, (128u << 16) | 256u);
    EXPECT_EQ(image.dda, 0x08000800u);
}

// Uneven on each axis: across shown larger (VIC keeps the source's 128), down shown smaller (VIC makes the 40 rows).
TEST_F(PlaneConversion, EachAxisTakesTheSmallerOfTheScreenAndTheSource)
{
    layer.dst = {100, 200, 300, 40};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(lastOperation.dstW, 128u);
    EXPECT_EQ(lastOperation.dstH, 40u);
    EXPECT_EQ(image.size, (40u << 16) | 300u);
}

TEST_F(PlaneConversion, StillContentMovedOrShrunkWhollyOnTheScreenKeepsItsPlane)
{
    const int slot = convert();
    ASSERT_GE(slot, 0);
    display->busy[slot] = true;
    for (const native_planes::Rect dst :
         {native_planes::Rect{300, 400, 256, 128}, native_planes::Rect{100, 200, 192, 96},
          native_planes::Rect{0, 0, 64, 32}, native_planes::Rect{10, 10, 80, 40}}) {
        layer.dst = dst;
        SCOPED_TRACE(testing::Message() << dst.x << "," << dst.y << " " << dst.w << "x" << dst.h);
        EXPECT_EQ(offer(), slot);
        EXPECT_EQ(image.size, (uint32_t(dst.h) << 16) | uint32_t(dst.w));
        EXPECT_EQ(image.position, (uint32_t(dst.y) << 16) | uint32_t(dst.x));
    }
    EXPECT_EQ(conversions, 1u);
    EXPECT_EQ(allocations, 1u);
}

// Below half its plane's size, the layer gets a plane its own size: window B fetches, and admission charges, no more
// than twice the pixels each way.
TEST_F(PlaneConversion, AShrinkBelowHalfConvertsAgain)
{
    const int slot = convert();
    ASSERT_GE(slot, 0);
    display->busy[slot] = true;
    layer.dst = {100, 200, 48, 24};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 2u);
    EXPECT_EQ(lastOperation.dstW, 48u);
    EXPECT_EQ(lastOperation.dstH, 24u);
}

// A plane made for the layer shown small is not stretched over the layer shown larger: VIC makes the larger one.
TEST_F(PlaneConversion, StillContentThatGrowsPastItsPlaneConvertsAgain)
{
    layer.dst = {100, 200, 32, 16};
    const int slot = convert();
    ASSERT_GE(slot, 0);
    display->busy[slot] = true;
    EXPECT_EQ(lastOperation.dstW, 32u);
    layer.dst = {100, 200, 64, 32};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 2u);
    EXPECT_EQ(lastOperation.dstW, 64u);
    EXPECT_EQ(lastOperation.dstH, 32u);
    EXPECT_EQ(image.dda, 0x10001000u);
}

// Cut by an edge the plane is the part on the screen, so moving the layer along the edge converts again.
TEST_F(PlaneConversion, ALayerCutByAnEdgeKeepsItsPlaneOnlyForTheSamePart)
{
    layer.dst = {-64, 200, 256, 128};
    const int slot = convert();
    ASSERT_GE(slot, 0);
    display->busy[slot] = true;
    EXPECT_EQ(lastOperation.srcX, 32u << 16);
    EXPECT_EQ(lastOperation.dstW, 96u);
    layer.dst = {-64, 300, 256, 128};
    EXPECT_EQ(offer(), slot) << "the same part of the source";
    layer.dst = {-32, 200, 256, 128};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 2u);
    EXPECT_EQ(lastOperation.srcX, 16u << 16);
}

TEST_F(PlaneConversion, ALayerWhollyOffTheScreenIsShownAsNothingNotDropped)
{
    layer.dst = {800, 200, 256, 128};
    EXPECT_EQ(offer(), 0);
    EXPECT_FALSE(image.visible);
    EXPECT_EQ(conversions, 0u);
    EXPECT_EQ(allocations, 0u);
}

}  // namespace
