// SPDX-License-Identifier: MIT
// raw/runtime/raw_quad_fill.h: which quads one bounding-box triangle can draw.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_quad_fill.h"

namespace {

TEST(RawQuadFill, AnAxisAlignedStripQuadQualifies)
{
    const float quad[8] = {-1, 1, 1, 1, -1, -1, 1, -1};
    EXPECT_TRUE(raw::axisAlignedQuad(quad));
}

TEST(RawQuadFill, TurnedOrDegenerateQuadsDoNot)
{
    const float turned[8] = {0, 1, 1, 0, -1, 0, 0, -1};
    const float flat[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_FALSE(raw::axisAlignedQuad(turned));
    EXPECT_FALSE(raw::axisAlignedQuad(flat));
}

TEST(RawQuadFill, TheRuleWaitsForTheDevice)
{
    EXPECT_FALSE(raw::fillViaTriangleBox()) << "gpu.x1.fill_bbox has not run on the chip";
}

}  // namespace
