// SPDX-License-Identifier: MIT
// raw/runtime/raw_bounds.h on the host: the box and the rectangle of a draw of Core Animation's vertex transform, over
// synthetic quads, triangles and clipped vertices (raw/test/boundsoracle.mm checks the same against a GPU's coverage);
// and raw_region.h's fetch barrier region test over such draws, and when one covers a render area (clear elision).
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "../raw/runtime/raw_bounds.h"
#include "../raw/runtime/raw_region.h"

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;
const raw::PixelBox kTarget = {0, 0, kWidth, kHeight};
const raw::WindowViewport kViewport = {0, 0, kWidth, kHeight};

// Core Animation's projection of a layer's pixels: x right and y down over the viewport
const float kPixels[16] = {2.0f / kWidth, 0, 0, 0, 0, -2.0f / kHeight, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};

struct Corner {
    float x, y;
};

std::vector<float> positions(const std::vector<Corner> &corners, float z = 0.5f, float w = 1)
{
    std::vector<float> out;
    for (const Corner &c : corners)
        out.insert(out.end(), {c.x * w, c.y * w, z * w, w});
    return out;
}

bool bounds(raw::DrawBounds &b, const std::vector<float> &p, uint32_t components = 4)
{
    const raw::PositionStream s = {reinterpret_cast<const uint8_t *>(p.data()), components * 4, 0, components};
    bool ok = true;
    for (uint32_t v = 0; v < p.size() / components; v++)
        ok &= b.add(s, v);
    return ok;
}

void expectBox(const raw::PixelBox &b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    EXPECT_EQ(b.x0, x0);
    EXPECT_EQ(b.y0, y0);
    EXPECT_EQ(b.x1, x1);
    EXPECT_EQ(b.y1, y1);
}

const Corner kA = {10, 30}, kB = {20, 30}, kC = {10, 50}, kD = {20, 50};

TEST(RawBounds, PixelAlignedStripIsItsRectangleExactly)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD})));
    raw::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
    expectBox(possible, 10, 30, 20, 50);
    expectBox(b.pixels(kTarget), 9, 29, 21, 51);
}

TEST(RawBounds, FoldedStripIsNoRectangle)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kD, kC})));
    raw::PixelBox certain, possible;
    EXPECT_FALSE(b.rectangle(false, certain, possible));
    expectBox(b.pixels(kTarget), 9, 29, 21, 51);
}

TEST(RawBounds, TwoTriangleListsAlongEitherDiagonal)
{
    raw::PixelBox certain, possible;
    for (const std::vector<Corner> &list :
         {std::vector<Corner>{kA, kB, kC, kB, kD, kC}, std::vector<Corner>{kA, kB, kD, kA, kD, kC}}) {
        raw::DrawBounds b(kPixels, kViewport);
        ASSERT_TRUE(bounds(b, positions(list)));
        ASSERT_TRUE(b.rectangle(true, certain, possible));
        expectBox(certain, 10, 30, 20, 50);
    }
    raw::DrawBounds twice(kPixels, kViewport);
    ASSERT_TRUE(bounds(twice, positions({kA, kB, kC, kA, kB, kC})));
    EXPECT_FALSE(twice.rectangle(true, certain, possible));
}

TEST(RawBounds, HalfPixelCornersLeaveTheirEdgesInDoubt)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{10.5f, 30}, {20.5f, 30}, {10.5f, 50}, {20.5f, 50}})));
    raw::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 11, 30, 20, 50);
    expectBox(possible, 10, 30, 21, 50);
}

TEST(RawBounds, TriangleBoxHoldsItsVertices)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{100.25f, 7}, {300, 200.75f}, {50, 400}})));
    expectBox(b.pixels(kTarget), 49, 6, 301, 401);
    raw::PixelBox certain, possible;
    EXPECT_FALSE(b.rectangle(false, certain, possible));
}

TEST(RawBounds, NormalizedYGrowsUpFromTheViewportsTop)
{
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    raw::DrawBounds b(identity, {100, 40, 200, 100});
    ASSERT_TRUE(bounds(b, positions({{-1, 1}, {-0.5f, 1}, {-1, 0.6f}})));
    expectBox(b.pixels(kTarget), 99, 39, 151, 61);
}

TEST(RawBounds, FloatTwoPositionsTakeZeroAndOne)
{
    raw::DrawBounds b(kPixels, kViewport);
    const std::vector<float> p = {10, 30, 20, 30, 10, 50, 20, 50};
    ASSERT_TRUE(bounds(b, p, 2));
    raw::PixelBox certain, possible;
    EXPECT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
}

TEST(RawBounds, PerspectiveScalesByW)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD}, 0.5f, 4)));
    raw::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
}

TEST(RawBounds, AVertexAtOrBehindWZeroLeavesNoBox)
{
    raw::DrawBounds behind(kPixels, kViewport);
    EXPECT_FALSE(bounds(behind, positions({kA, kB, kC}, 0.5f, -1)));
    raw::DrawBounds at(kPixels, kViewport);
    EXPECT_FALSE(bounds(at, {10, 30, 0.5f, 0}));
    raw::DrawBounds nan(kPixels, kViewport);
    EXPECT_FALSE(bounds(nan, {NAN, 30, 0.5f, 1}));
}

TEST(RawBounds, DepthClippedIsABoxButNoRectangle)
{
    raw::PixelBox certain, possible;
    for (const float z : {-0.25f, 1.25f}) {
        raw::DrawBounds b(kPixels, kViewport);
        ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD}, z)));
        EXPECT_FALSE(b.rectangle(false, certain, possible));
        expectBox(b.pixels(kTarget), 9, 29, 21, 51);
    }
}

TEST(RawBounds, OffTargetClampsToTheLimit)
{
    raw::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{-500, -20}, {1500, -20}, {-500, 900}, {1500, 900}})));
    expectBox(b.pixels(kTarget), 0, 0, kWidth, kHeight);
    expectBox(b.pixels({100, 200, 300, 400}), 100, 200, 300, 400);
}

// A draw of a pixel rectangle as a strip, over vertex memory the test keeps
raw::DrawGeometry quad(std::vector<float> &memory, float x0, float y0, float x1, float y1)
{
    memory = positions({{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}});
    raw::DrawGeometry g;
    g.bounded = true;
    g.matrix = reinterpret_cast<const uint8_t *>(kPixels);
    g.positions = {reinterpret_cast<const uint8_t *>(memory.data()), 16, 0, 4};
    g.positionBytes = memory.size() * sizeof(float);
    g.count = 4;
    g.viewport = kViewport;
    g.limit = kTarget;
    return g;
}

TEST(RawRegion, AFetchMeetsOnlyTheWritesItsPixelsReach)
{
    std::vector<float> a, b, c, d;
    raw::WrittenRegion written;
    written.add(quad(a, 0, 0, 100, 100));
    written.add(quad(b, 200, 0, 300, 100));
    EXPECT_FALSE(written.meets(quad(c, 120, 0, 180, 100)));
    EXPECT_TRUE(written.meets(quad(d, 90, 0, 150, 100)));
    written.clear();
    EXPECT_FALSE(written.meets(quad(d, 90, 0, 150, 100)));
}

TEST(RawRegion, AGuardPixelSeparatesTouchingRectangles)
{
    std::vector<float> a, b, c;
    raw::WrittenRegion written;
    written.add(quad(a, 0, 0, 100, 100));
    EXPECT_TRUE(written.meets(quad(b, 100, 0, 200, 100)));
    EXPECT_FALSE(written.meets(quad(c, 102, 0, 200, 100)));
}

TEST(RawRegion, AnUnboundedDrawCoversItsScissor)
{
    std::vector<float> a, b, c;
    raw::WrittenRegion written;
    raw::DrawGeometry other = quad(a, 0, 0, 10, 10);
    other.bounded = false;
    other.limit = {0, 0, 640, 720};
    written.add(other);
    EXPECT_TRUE(written.meets(quad(b, 600, 0, 700, 100)));
    EXPECT_FALSE(written.meets(quad(c, 700, 0, 800, 100)));
}

TEST(RawRegion, VerticesPastTheBoundBytesLeaveNoBox)
{
    std::vector<float> a, b;
    raw::WrittenRegion written;
    raw::DrawGeometry g = quad(a, 0, 0, 10, 10);
    g.positionBytes = 3 * 16;
    g.limit = {0, 0, 640, 720};
    written.add(g);
    EXPECT_TRUE(written.meets(quad(b, 600, 0, 700, 100)));
}

TEST(RawRegion, MoreWritesThanItHoldsMeetEverything)
{
    std::vector<std::vector<float>> memory(raw::WrittenRegion::kMaxDraws + 2);
    raw::WrittenRegion written;
    for (uint32_t i = 0; i <= raw::WrittenRegion::kMaxDraws; i++)
        written.add(quad(memory[i], float(i), 0, float(i) + 1, 1));
    EXPECT_TRUE(written.meets(quad(memory.back(), 1000, 600, 1100, 700)));
}

TEST(RawRegion, IndexedDrawsReadTheirIndices)
{
    std::vector<float> a, b;
    raw::WrittenRegion written;
    raw::DrawGeometry g = quad(a, 0, 0, 100, 100);
    const uint16_t indices[6] = {0, 1, 2, 1, 3, 2};
    g.indices = reinterpret_cast<const uint8_t *>(indices);
    g.indexSize = 2;
    g.count = 6;
    written.add(g);
    EXPECT_FALSE(written.meets(quad(b, 150, 0, 200, 100)));
    EXPECT_TRUE(written.meets(quad(b, 50, 50, 200, 100)));
}

TEST(RawRegion, AFullTargetRectangleCoversTheRenderArea)
{
    std::vector<float> a, b;
    EXPECT_TRUE(raw::WrittenRegion::covers(quad(a, 0, 0, kWidth, kHeight), false, kWidth, kHeight));
    EXPECT_TRUE(raw::WrittenRegion::covers(quad(b, -40, -40, kWidth + 40, kHeight + 40), false, kWidth, kHeight));
}

TEST(RawRegion, ARectangleShortOfAnEdgeCoversNothing)
{
    std::vector<float> a, b, c;
    EXPECT_FALSE(raw::WrittenRegion::covers(quad(a, 1, 0, kWidth, kHeight), false, kWidth, kHeight));
    EXPECT_FALSE(raw::WrittenRegion::covers(quad(b, 0, 0, kWidth, kHeight - 0.5f), false, kWidth, kHeight));
    EXPECT_FALSE(raw::WrittenRegion::covers(quad(c, 0, 0, kWidth - 1, kHeight), false, kWidth, kHeight));
}

TEST(RawRegion, ARectangleCoversOnlyWhereItsScissorReaches)
{
    std::vector<float> a;
    raw::DrawGeometry g = quad(a, 0, 0, kWidth, kHeight);
    g.limit = {0, 0, kWidth, kHeight - 1};
    EXPECT_FALSE(raw::WrittenRegion::covers(g, false, kWidth, kHeight));
    g.limit = kTarget;
    g.bounded = false;
    EXPECT_FALSE(raw::WrittenRegion::covers(g, false, kWidth, kHeight));
}

TEST(RawRegion, OnlyAPlainQuadCovers)
{
    std::vector<float> a, b, c;
    raw::DrawGeometry folded = quad(a, 0, 0, kWidth, kHeight);
    std::swap(a[8], a[12]);
    std::swap(a[9], a[13]);
    EXPECT_FALSE(raw::WrittenRegion::covers(folded, false, kWidth, kHeight));
    raw::DrawGeometry list = quad(b, 0, 0, kWidth, kHeight);
    const uint16_t pair[6] = {0, 1, 2, 1, 3, 2}, restart[5] = {0, 1, 0xffff, 2, 3};
    list.indices = reinterpret_cast<const uint8_t *>(pair);
    list.indexSize = 2;
    list.count = 6;
    EXPECT_TRUE(raw::WrittenRegion::covers(list, true, kWidth, kHeight));
    EXPECT_FALSE(raw::WrittenRegion::covers(list, false, kWidth, kHeight));
    raw::DrawGeometry strips = quad(c, 0, 0, kWidth, kHeight);
    strips.indices = reinterpret_cast<const uint8_t *>(restart);
    strips.indexSize = 2;
    strips.count = 5;
    EXPECT_FALSE(raw::WrittenRegion::covers(strips, false, kWidth, kHeight));
}

TEST(RawRegion, AClippedCornerCoversNothing)
{
    std::vector<float> a;
    raw::DrawGeometry g = quad(a, 0, 0, kWidth, kHeight);
    a[2] = -0.25f;
    EXPECT_FALSE(raw::WrittenRegion::covers(g, false, kWidth, kHeight));
    a[2] = 0.5f;
    a[3] = 0;
    EXPECT_FALSE(raw::WrittenRegion::covers(g, false, kWidth, kHeight));
}

}  // namespace
