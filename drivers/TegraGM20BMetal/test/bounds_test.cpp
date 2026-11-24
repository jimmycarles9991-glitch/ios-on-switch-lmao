// SPDX-License-Identifier: MIT
// bounds.h on the host: the box and the rectangle of a draw of Core Animation's vertex transform, over
// synthetic quads, triangles and clipped vertices (harness/boundsoracle.mm checks the same against a GPU's coverage);
// and region.h's fetch barrier region test over such draws, and when one covers a render area (clear elision).
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "../bounds.h"
#include "../region.h"

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;
const gm20b::PixelBox kTarget = {0, 0, kWidth, kHeight};
const gm20b::WindowViewport kViewport = {0, 0, kWidth, kHeight};

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

bool bounds(gm20b::DrawBounds &b, const std::vector<float> &p, uint32_t components = 4)
{
    const gm20b::PositionStream s = {reinterpret_cast<const uint8_t *>(p.data()), components * 4, 0, components};
    bool ok = true;
    for (uint32_t v = 0; v < p.size() / components; v++)
        ok &= b.add(s, v);
    return ok;
}

void expectBox(const gm20b::PixelBox &b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    EXPECT_EQ(b.x0, x0);
    EXPECT_EQ(b.y0, y0);
    EXPECT_EQ(b.x1, x1);
    EXPECT_EQ(b.y1, y1);
}

const Corner kA = {10, 30}, kB = {20, 30}, kC = {10, 50}, kD = {20, 50};

TEST(GM20BBounds, PixelAlignedStripIsItsRectangleExactly)
{
    gm20b::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD})));
    gm20b::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
    expectBox(possible, 10, 30, 20, 50);
    expectBox(b.pixels(kTarget), 9, 29, 21, 51);
}

TEST(GM20BBounds, FoldedStripIsNoRectangle)
{
    gm20b::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kD, kC})));
    gm20b::PixelBox certain, possible;
    EXPECT_FALSE(b.rectangle(false, certain, possible));
    expectBox(b.pixels(kTarget), 9, 29, 21, 51);
}

TEST(GM20BBounds, TwoTriangleListsAlongEitherDiagonal)
{
    gm20b::PixelBox certain, possible;
    for (const std::vector<Corner> &list :
         {std::vector<Corner>{kA, kB, kC, kB, kD, kC}, std::vector<Corner>{kA, kB, kD, kA, kD, kC}}) {
        gm20b::DrawBounds b(kPixels, kViewport);
        ASSERT_TRUE(bounds(b, positions(list)));
        ASSERT_TRUE(b.rectangle(true, certain, possible));
        expectBox(certain, 10, 30, 20, 50);
    }
    gm20b::DrawBounds twice(kPixels, kViewport);
    ASSERT_TRUE(bounds(twice, positions({kA, kB, kC, kA, kB, kC})));
    EXPECT_FALSE(twice.rectangle(true, certain, possible));
}

TEST(GM20BBounds, HalfPixelCornersLeaveTheirEdgesInDoubt)
{
    gm20b::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{10.5f, 30}, {20.5f, 30}, {10.5f, 50}, {20.5f, 50}})));
    gm20b::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 11, 30, 20, 50);
    expectBox(possible, 10, 30, 21, 50);
}

TEST(GM20BBounds, TriangleBoxHoldsItsVertices)
{
    gm20b::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{100.25f, 7}, {300, 200.75f}, {50, 400}})));
    expectBox(b.pixels(kTarget), 49, 6, 301, 401);
    gm20b::PixelBox certain, possible;
    EXPECT_FALSE(b.rectangle(false, certain, possible));
}

TEST(GM20BBounds, NormalizedYGrowsUpFromTheViewportsTop)
{
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    gm20b::DrawBounds b(identity, {100, 40, 200, 100});
    ASSERT_TRUE(bounds(b, positions({{-1, 1}, {-0.5f, 1}, {-1, 0.6f}})));
    expectBox(b.pixels(kTarget), 99, 39, 151, 61);
}

TEST(GM20BBounds, FloatTwoPositionsTakeZeroAndOne)
{
    gm20b::DrawBounds b(kPixels, kViewport);
    const std::vector<float> p = {10, 30, 20, 30, 10, 50, 20, 50};
    ASSERT_TRUE(bounds(b, p, 2));
    gm20b::PixelBox certain, possible;
    EXPECT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
}

TEST(GM20BBounds, PerspectiveScalesByW)
{
    gm20b::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD}, 0.5f, 4)));
    gm20b::PixelBox certain, possible;
    ASSERT_TRUE(b.rectangle(false, certain, possible));
    expectBox(certain, 10, 30, 20, 50);
}

TEST(GM20BBounds, AVertexAtOrBehindWZeroLeavesNoBox)
{
    gm20b::DrawBounds behind(kPixels, kViewport);
    EXPECT_FALSE(bounds(behind, positions({kA, kB, kC}, 0.5f, -1)));
    gm20b::DrawBounds at(kPixels, kViewport);
    EXPECT_FALSE(bounds(at, {10, 30, 0.5f, 0}));
    gm20b::DrawBounds nan(kPixels, kViewport);
    EXPECT_FALSE(bounds(nan, {NAN, 30, 0.5f, 1}));
    gm20b::DrawBounds subnormalW(kPixels, kViewport);
    EXPECT_FALSE(bounds(subnormalW, {10, 30, 0.5f, FLT_MIN * 0.5f}));
    const float bigScale[16] = {1e20f, 0, 0, 0, 0, 1e20f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    gm20b::DrawBounds overflow(bigScale, kViewport);
    EXPECT_FALSE(bounds(overflow, {1e20f, 0, 0.5f, 1}));
}

TEST(GM20BBounds, DepthClippedIsABoxButNoRectangle)
{
    gm20b::PixelBox certain, possible;
    for (const float z : {-0.25f, 1.25f}) {
        gm20b::DrawBounds b(kPixels, kViewport);
        ASSERT_TRUE(bounds(b, positions({kA, kB, kC, kD}, z)));
        EXPECT_FALSE(b.rectangle(false, certain, possible));
        expectBox(b.pixels(kTarget), 9, 29, 21, 51);
    }
}

TEST(GM20BBounds, OffTargetClampsToTheLimit)
{
    gm20b::DrawBounds b(kPixels, kViewport);
    ASSERT_TRUE(bounds(b, positions({{-500, -20}, {1500, -20}, {-500, 900}, {1500, 900}})));
    expectBox(b.pixels(kTarget), 0, 0, kWidth, kHeight);
    expectBox(b.pixels({100, 200, 300, 400}), 100, 200, 300, 400);
}

// A draw of a pixel rectangle as a strip, over vertex memory the test keeps
gm20b::DrawGeometry quad(std::vector<float> &memory, float x0, float y0, float x1, float y1)
{
    memory = positions({{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}});
    gm20b::DrawGeometry g;
    g.bounded = true;
    g.matrix = reinterpret_cast<const uint8_t *>(kPixels);
    g.positions = {reinterpret_cast<const uint8_t *>(memory.data()), 16, 0, 4};
    g.positionBytes = memory.size() * sizeof(float);
    g.count = 4;
    g.viewport = kViewport;
    g.limit = kTarget;
    return g;
}

// The same draw scissored to its rectangle: what the region test knows of a draw whose vertices it does not read
gm20b::DrawGeometry scissored(std::vector<float> &memory, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    gm20b::DrawGeometry g = quad(memory, float(x0), float(y0), float(x1), float(y1));
    g.limit = {x0, y0, x1, y1};
    return g;
}

// A draw whose vertices were bounded before it was added (a pass's first draw, for its cover test)
gm20b::WrittenRegion::Analysis bounded(const gm20b::DrawGeometry &g)
{
    gm20b::WrittenRegion::Analysis analysis(g);
    gm20b::PixelBox box;
    analysis.bounds(box);
    return analysis;
}

TEST(GM20BRegion, AFetchMeetsOnlyTheWritesItsPixelLimitReaches)
{
    std::vector<float> a, b, c, d;
    gm20b::WrittenRegion written;
    written.add(scissored(a, 0, 0, 100, 100));
    written.add(scissored(b, 200, 0, 300, 100));
    EXPECT_FALSE(written.meets(scissored(c, 120, 0, 180, 100)));
    EXPECT_TRUE(written.meets(scissored(d, 90, 0, 150, 100)));
    written.clear();
    EXPECT_FALSE(written.meets(scissored(d, 90, 0, 150, 100)));
}

TEST(GM20BRegion, BoundedDrawsResolveTheirProjectedBoxLazilyOnMeets)
{
    std::vector<float> a, b, c;
    gm20b::WrittenRegion written;
    gm20b::WrittenRegion::Analysis analysis(quad(a, 0, 0, 100, 100));
    written.add(analysis);
    EXPECT_FALSE(analysis.evaluated());
    EXPECT_FALSE(written.meets(quad(b, 200, 200, 300, 300)));
    EXPECT_TRUE(written.meets(quad(c, 50, 50, 150, 150)));
}

TEST(GM20BRegion, ABoundedWriteKeepsAGuardPixelBetweenTouchingRectangles)
{
    std::vector<float> a, b, c;
    gm20b::WrittenRegion written;
    written.add(bounded(quad(a, 0, 0, 100, 100)));
    EXPECT_TRUE(written.meets(scissored(b, 100, 0, 200, 100)));
    EXPECT_FALSE(written.meets(scissored(c, 102, 0, 200, 100)));
}

TEST(GM20BRegion, AnUnboundedDrawCoversItsScissor)
{
    std::vector<float> a, b, c;
    gm20b::WrittenRegion written;
    gm20b::DrawGeometry other = quad(a, 0, 0, 10, 10);
    other.bounded = false;
    other.limit = {0, 0, 640, 720};
    written.add(other);
    EXPECT_TRUE(written.meets(scissored(b, 600, 0, 700, 100)));
    EXPECT_FALSE(written.meets(scissored(c, 700, 0, 800, 100)));
}

TEST(GM20BRegion, VerticesPastTheBoundBytesLeaveNoBox)
{
    std::vector<float> a, b;
    gm20b::WrittenRegion written;
    gm20b::DrawGeometry g = quad(a, 0, 0, 10, 10);
    g.positionBytes = 3 * 16;
    g.limit = {0, 0, 640, 720};
    written.add(bounded(g));
    EXPECT_TRUE(written.meets(scissored(b, 600, 0, 700, 100)));
}

TEST(GM20BRegion, TileMaskElidesDisjointWritesPastExactBoxCapacity)
{
    const uint32_t count = gm20b::WrittenRegion::kMaxDraws + 20;
    std::vector<std::vector<float>> memory(count + 2);
    gm20b::WrittenRegion written;
    for (uint32_t i = 0; i < count; i++)
        written.add(quad(memory[i], float((i % 16) * 4), float((i / 16) * 4), float((i % 16) * 4 + 2),
                         float((i / 16) * 4 + 2)));
    EXPECT_FALSE(written.meets(quad(memory[count], 1000, 600, 1100, 700)));
    EXPECT_TRUE(written.meets(quad(memory[count + 1], 10, 10, 20, 20)));
}

TEST(GM20BRegion, SubTileDisjointWritesElideWithinExactBoxCapacity)
{
    std::vector<float> a, b, c;
    gm20b::WrittenRegion written;
    written.add(quad(a, 2, 2, 10, 10));
    EXPECT_FALSE(written.meets(quad(b, 14, 14, 24, 24)));
    EXPECT_TRUE(written.meets(quad(c, 8, 8, 20, 20)));
}

TEST(GM20BRegion, OutOfGridWritesStillTrackConservatively)
{
    std::vector<float> a, b, c;
    gm20b::WrittenRegion written;
    gm20b::DrawGeometry big = quad(a, 0, 0, 10, 10);
    big.bounded = false;
    big.limit = {2100, 100, 2300, 200};
    written.add(big);
    gm20b::DrawGeometry far = quad(b, 0, 0, 10, 10);
    far.bounded = false;
    far.limit = {100, 100, 200, 200};
    EXPECT_FALSE(written.meets(far));
    gm20b::DrawGeometry hit = quad(c, 0, 0, 10, 10);
    hit.bounded = false;
    hit.limit = {2200, 150, 2400, 250};
    EXPECT_TRUE(written.meets(hit));
}

TEST(GM20BRegion, PendingOverflowNeverEvaluatesWithoutMeetsAndFoldsLimitConservatively)
{
    const uint32_t count = gm20b::WrittenRegion::kMaxPending + 10;
    std::vector<std::vector<float>> memory(count + 2);
    gm20b::GeometryBudget budget{4096, 4096};
    gm20b::GeometryCache cache;
    gm20b::WrittenRegion written;
    std::vector<gm20b::WrittenRegion::Analysis> analyses;
    analyses.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        gm20b::DrawGeometry g = quad(memory[i], 0, 0, 10, 10);
        g.limit = i < gm20b::WrittenRegion::kMaxPending ? gm20b::PixelBox{0, 0, 100, 100}
                                                        : gm20b::PixelBox{600, 100, 700, 200};
        analyses.push_back(cache.analysis(g, i + 1, budget));
        written.add(analyses.back());
    }
    EXPECT_EQ(budget.vertices, 4096u);
    EXPECT_EQ(budget.pairs, 4096u);
    for (auto &a : analyses)
        EXPECT_FALSE(a.snapshot().evaluated);

    EXPECT_TRUE(written.meets(scissored(memory[count], 620, 120, 660, 160)));
    EXPECT_FALSE(written.meets(scissored(memory[count + 1], 1000, 500, 1100, 600)));
}

TEST(GM20BRegion, NonOverlappingQuadAcceptsConvexQuadsAndRejectsFoldedOrDegenerate)
{
    std::vector<float> stripMem, rotMem, listMem, foldedMem, concaveMem, dupMem, badWMem, hugeMem, guardMem;
    gm20b::WrittenRegion::Analysis axisStrip(quad(stripMem, 10, 20, 210, 140));
    EXPECT_TRUE(gm20b::WrittenRegion::nonOverlappingQuad(axisStrip, false));

    rotMem = positions({{100, 20}, {180, 70}, {40, 110}, {120, 160}});
    gm20b::DrawGeometry rot = quad(stripMem, 0, 0, 10, 10);
    rot.positions.base = reinterpret_cast<const uint8_t *>(rotMem.data());
    rot.positionBytes = rotMem.size() * sizeof(float);
    gm20b::WrittenRegion::Analysis rotAnalysis(rot);
    EXPECT_TRUE(gm20b::WrittenRegion::nonOverlappingQuad(rotAnalysis, false));

    gm20b::DrawGeometry list = quad(listMem, 10, 20, 210, 140);
    const uint16_t pair[6] = {0, 1, 2, 1, 3, 2};
    list.indices = reinterpret_cast<const uint8_t *>(pair);
    list.indexSize = 2;
    list.count = 6;
    list.strip = false;
    gm20b::WrittenRegion::Analysis listAnalysis(list);
    EXPECT_TRUE(gm20b::WrittenRegion::nonOverlappingQuad(listAnalysis, true));

    gm20b::DrawGeometry folded = quad(foldedMem, 10, 20, 210, 140);
    std::swap(foldedMem[8], foldedMem[12]);
    std::swap(foldedMem[9], foldedMem[13]);
    gm20b::WrittenRegion::Analysis foldedAnalysis(folded);
    EXPECT_FALSE(gm20b::WrittenRegion::nonOverlappingQuad(foldedAnalysis, false));

    concaveMem = positions({{0, 0}, {100, 0}, {50, 100}, {100, 0}, {50, 100}, {20, 40}});
    gm20b::DrawGeometry overlap = quad(stripMem, 0, 0, 10, 10);
    overlap.positions.base = reinterpret_cast<const uint8_t *>(concaveMem.data());
    overlap.positionBytes = concaveMem.size() * sizeof(float);
    overlap.count = 6;
    overlap.strip = false;
    gm20b::WrittenRegion::Analysis overlapAnalysis(overlap);
    EXPECT_FALSE(gm20b::WrittenRegion::nonOverlappingQuad(overlapAnalysis, true));

    gm20b::DrawGeometry dup = quad(dupMem, 10, 20, 210, 140);
    const uint16_t same[6] = {0, 1, 2, 0, 1, 2};
    dup.indices = reinterpret_cast<const uint8_t *>(same);
    dup.indexSize = 2;
    dup.count = 6;
    dup.strip = false;
    gm20b::WrittenRegion::Analysis dupAnalysis(dup);
    EXPECT_FALSE(gm20b::WrittenRegion::nonOverlappingQuad(dupAnalysis, true));

    gm20b::DrawGeometry badW = quad(badWMem, 10, 20, 210, 140);
    badWMem[3] = 0.0f;
    gm20b::WrittenRegion::Analysis badWAnalysis(badW);
    EXPECT_FALSE(gm20b::WrittenRegion::nonOverlappingQuad(badWAnalysis, false));

    gm20b::DrawGeometry huge = quad(hugeMem, -20000, 0, 20000, 100);
    gm20b::WrittenRegion::Analysis hugeAnalysis(huge);
    EXPECT_FALSE(gm20b::WrittenRegion::nonOverlappingQuad(hugeAnalysis, false));

    const float smallPixels[16] = {2.0f / 16.0f, 0, 0, 0, 0, -2.0f / 16.0f, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    guardMem = positions({{0, 0}, {2500, 0}, {0, 10}, {2500, 10}});
    gm20b::DrawGeometry outsideGuard = quad(stripMem, 0, 0, 10, 10);
    outsideGuard.matrix = reinterpret_cast<const uint8_t *>(smallPixels);
    outsideGuard.positions.base = reinterpret_cast<const uint8_t *>(guardMem.data());
    outsideGuard.positionBytes = guardMem.size() * sizeof(float);
    outsideGuard.viewport = {0, 0, 16, 16};
    gm20b::WrittenRegion::Analysis outsideGuardAnalysis(outsideGuard);
    EXPECT_FALSE(gm20b::WrittenRegion::nonOverlappingQuad(outsideGuardAnalysis, false));

    gm20b::DrawGeometry unbounded = quad(stripMem, 10, 20, 210, 140);
    unbounded.bounded = false;
    gm20b::WrittenRegion::Analysis unboundedAnalysis(unbounded);
    EXPECT_FALSE(gm20b::WrittenRegion::nonOverlappingQuad(unboundedAnalysis, false));
}

TEST(GM20BRegion, IndexedDrawsReadTheirIndices)
{
    std::vector<float> a, b;
    gm20b::WrittenRegion written;
    gm20b::DrawGeometry g = quad(a, 0, 0, 100, 100);
    const uint16_t indices[6] = {0, 1, 2, 1, 3, 2};
    g.indices = reinterpret_cast<const uint8_t *>(indices);
    g.indexSize = 2;
    g.count = 6;
    written.add(bounded(g));
    EXPECT_FALSE(written.meets(scissored(b, 150, 0, 200, 100)));
    EXPECT_TRUE(written.meets(scissored(b, 50, 50, 200, 100)));
}

TEST(GM20BRegion, AFullTargetRectangleCoversTheRenderArea)
{
    std::vector<float> a, b;
    EXPECT_TRUE(gm20b::WrittenRegion::covers(quad(a, 0, 0, kWidth, kHeight), false, kWidth, kHeight));
    EXPECT_TRUE(gm20b::WrittenRegion::covers(quad(b, -40, -40, kWidth + 40, kHeight + 40), false, kWidth, kHeight));
}

TEST(GM20BRegion, ARectangleShortOfAnEdgeCoversNothing)
{
    std::vector<float> a, b, c;
    EXPECT_FALSE(gm20b::WrittenRegion::covers(quad(a, 1, 0, kWidth, kHeight), false, kWidth, kHeight));
    EXPECT_FALSE(gm20b::WrittenRegion::covers(quad(b, 0, 0, kWidth, kHeight - 0.5f), false, kWidth, kHeight));
    EXPECT_FALSE(gm20b::WrittenRegion::covers(quad(c, 0, 0, kWidth - 1, kHeight), false, kWidth, kHeight));
}

TEST(GM20BRegion, ARectangleCoversOnlyWhereItsScissorReaches)
{
    std::vector<float> a;
    gm20b::DrawGeometry g = quad(a, 0, 0, kWidth, kHeight);
    g.limit = {0, 0, kWidth, kHeight - 1};
    EXPECT_FALSE(gm20b::WrittenRegion::covers(g, false, kWidth, kHeight));
    g.limit = kTarget;
    g.bounded = false;
    EXPECT_FALSE(gm20b::WrittenRegion::covers(g, false, kWidth, kHeight));
}

TEST(GM20BRegion, OnlyAPlainQuadCovers)
{
    std::vector<float> a, b, c;
    gm20b::DrawGeometry folded = quad(a, 0, 0, kWidth, kHeight);
    std::swap(a[8], a[12]);
    std::swap(a[9], a[13]);
    EXPECT_FALSE(gm20b::WrittenRegion::covers(folded, false, kWidth, kHeight));
    gm20b::DrawGeometry list = quad(b, 0, 0, kWidth, kHeight);
    const uint16_t pair[6] = {0, 1, 2, 1, 3, 2}, restart[5] = {0, 1, 0xffff, 2, 3};
    list.indices = reinterpret_cast<const uint8_t *>(pair);
    list.indexSize = 2;
    list.count = 6;
    EXPECT_TRUE(gm20b::WrittenRegion::covers(list, true, kWidth, kHeight));
    EXPECT_FALSE(gm20b::WrittenRegion::covers(list, false, kWidth, kHeight));
    gm20b::DrawGeometry strips = quad(c, 0, 0, kWidth, kHeight);
    strips.indices = reinterpret_cast<const uint8_t *>(restart);
    strips.indexSize = 2;
    strips.count = 5;
    EXPECT_FALSE(gm20b::WrittenRegion::covers(strips, false, kWidth, kHeight));
}

TEST(GM20BRegion, AClippedCornerCoversNothing)
{
    std::vector<float> a;
    gm20b::DrawGeometry g = quad(a, 0, 0, kWidth, kHeight);
    a[2] = -0.25f;
    EXPECT_FALSE(gm20b::WrittenRegion::covers(g, false, kWidth, kHeight));
    a[2] = 0.5f;
    a[3] = 0;
    EXPECT_FALSE(gm20b::WrittenRegion::covers(g, false, kWidth, kHeight));
}

TEST(GM20BRegion, AnIndexedDrawReadsItsIndicesFromTheBaseVertex)
{
    // the indices name the second quad's vertices from the base vertex 4: the first quad's pixels are not written
    std::vector<float> a, b, c;
    gm20b::DrawGeometry g = quad(a, 0, 0, 100, 100);
    const std::vector<float> second = positions({{300, 0}, {400, 0}, {300, 100}, {400, 100}});
    a.insert(a.end(), second.begin(), second.end());
    g.positions.base = reinterpret_cast<const uint8_t *>(a.data());
    g.positionBytes = a.size() * sizeof(float);
    const uint16_t indices[6] = {0, 1, 2, 1, 3, 2};
    g.indices = reinterpret_cast<const uint8_t *>(indices);
    g.indexSize = 2;
    g.first = 4;
    g.count = 6;
    gm20b::WrittenRegion written;
    written.add(bounded(g));
    EXPECT_FALSE(written.meets(scissored(b, 0, 0, 100, 100)));
    EXPECT_TRUE(written.meets(scissored(c, 350, 50, 450, 150)));
}

TEST(GM20BRegion, OneAnalysisSuppliesBoundsCoverageAndWrittenPixels)
{
    std::vector<float> a, b;
    gm20b::DrawGeometry g = quad(a, 0, 0, kWidth, kHeight);
    gm20b::WrittenRegion::Analysis analysis(g);
    EXPECT_FALSE(analysis.evaluated());
    gm20b::PixelBox box;
    ASSERT_TRUE(analysis.bounds(box));
    EXPECT_TRUE(analysis.evaluated());
    g.matrix = nullptr;
    g.positions.base = nullptr;
    EXPECT_TRUE(analysis.covers(false, kWidth, kHeight));
    gm20b::PixelBox again;
    ASSERT_TRUE(analysis.bounds(again));
    EXPECT_EQ(again.x0, box.x0);
    EXPECT_EQ(again.y0, box.y0);
    EXPECT_EQ(again.x1, box.x1);
    EXPECT_EQ(again.y1, box.y1);
    gm20b::WrittenRegion written;
    written.add(analysis);
    EXPECT_TRUE(written.meets(quad(b, 100, 100, 200, 200)));
}

TEST(GM20BRegion, FailedAnalysisRetainsTheConservativeScissor)
{
    std::vector<float> a, b;
    gm20b::DrawGeometry g = quad(a, 0, 0, 100, 100);
    g.limit = {0, 0, 500, 500};
    g.positionBytes = 1;
    gm20b::WrittenRegion::Analysis analysis(g);
    gm20b::PixelBox box;
    EXPECT_FALSE(analysis.bounds(box));
    g.matrix = nullptr;
    g.positions.base = nullptr;
    EXPECT_FALSE(analysis.bounds(box));
    EXPECT_FALSE(analysis.covers(false, 500, 500));
    gm20b::WrittenRegion written;
    written.add(analysis);
    EXPECT_TRUE(written.meets(quad(b, 400, 400, 450, 450)));
}

TEST(GM20BRegion, AnUnqueriedAnalysisAnswersByItsLimit)
{
    std::vector<float> a, b;
    gm20b::DrawGeometry g = scissored(a, 0, 0, 100, 100);
    gm20b::WrittenRegion::Analysis analysis(g);
    gm20b::WrittenRegion written;
    written.add(analysis);
    EXPECT_FALSE(analysis.evaluated());
    EXPECT_FALSE(written.meets(scissored(b, 200, 200, 300, 300)));
}

TEST(GM20BRegion, ANewDrawAnalysisObservesChangedGeometry)
{
    std::vector<float> a;
    gm20b::DrawGeometry g = quad(a, 0, 0, kWidth, kHeight);
    gm20b::WrittenRegion::Analysis first(g);
    ASSERT_TRUE(first.covers(false, kWidth, kHeight));
    a[3] = 0;
    gm20b::WrittenRegion::Analysis second(g);
    EXPECT_FALSE(second.covers(false, kWidth, kHeight));
    EXPECT_TRUE(first.covers(false, kWidth, kHeight));
}

TEST(GM20BRegion, TenThousandDrawsReuseOneSnapshotUntilGenerationChanges)
{
    std::vector<float> memory;
    auto g = quad(memory, 0, 0, kWidth, kHeight);
    gm20b::GeometryBudget budget{4, 1};
    gm20b::GeometryCache cache;
    auto first = cache.analysis(g, 7, budget);
    ASSERT_TRUE(first.covers(false, kWidth, kHeight));
    EXPECT_EQ(budget.vertices, 0u);
    // the recording's copy does not change under a snapshot; changing it here shows later draws take the snapshot
    memory[3] = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        auto draw = cache.analysis(g, 7, budget);
        ASSERT_TRUE(draw.covers(false, kWidth, kHeight));
    }
    budget.vertices = 4;
    auto changed = cache.analysis(g, 8, budget);
    EXPECT_FALSE(changed.covers(false, kWidth, kHeight));
    EXPECT_EQ(budget.vertices, 0u);
}

TEST(GM20BRegion, CachedProjectionClipsToEachDrawsOwnScissor)
{
    std::vector<float> memory;
    auto g = quad(memory, 0, 0, kWidth, kHeight);
    gm20b::GeometryBudget budget{4, 1};
    gm20b::GeometryCache cache;
    auto whole = cache.analysis(g, 1, budget);
    gm20b::PixelBox box;
    ASSERT_TRUE(whole.bounds(box));
    g.limit = {100, 200, 150, 250};
    auto clipped = cache.analysis(g, 1, budget);
    ASSERT_TRUE(clipped.bounds(box));
    EXPECT_EQ(box.x0, 100u);
    EXPECT_EQ(box.x1, 150u);
    EXPECT_EQ(box.y0, 200u);
    EXPECT_EQ(box.y1, 250u);
    EXPECT_EQ(budget.vertices, 0u);
    g.viewport.originX += 1;
    auto moved = cache.analysis(g, 1, budget);
    EXPECT_FALSE(moved.bounds(box));  // different viewport needs new projection work; budget is exhausted
}

TEST(GM20BRegion, UnavailableAndOverBudgetInputsKeepTheEntireScissorWithoutReading)
{
    gm20b::DrawGeometry unavailable;
    unavailable.matrix = reinterpret_cast<const uint8_t *>(1);
    unavailable.positions = {reinterpret_cast<const uint8_t *>(1), 16, 0, 4};
    unavailable.positionBytes = 1024;
    unavailable.count = 6;
    unavailable.limit = {5, 10, 500, 700};
    gm20b::GeometryCache cache;
    gm20b::GeometryBudget budget{0, 0};
    gm20b::PixelBox box;
    auto missing = cache.analysis(unavailable, 0, budget);
    EXPECT_FALSE(missing.bounds(box));
    EXPECT_EQ(missing.cachedBox().x1, 500u);
    unavailable.bounded = true;
    auto exhausted = cache.analysis(unavailable, 1, budget);
    EXPECT_FALSE(exhausted.bounds(box));  // deliberately invalid pointers must never be dereferenced
    EXPECT_EQ(exhausted.cachedBox().y1, 700u);
}

}  // namespace
