// SPDX-License-Identifier: MIT
// raw/runtime/raw_overlap.h on the host: a framebuffer-fetching draw drawn through the texture unit, one triangle a run,
// each run keeping its first (provoking) vertex, a strip's odd triangles their winding, an indexed strip's runs index
// ranges like a list's; instanced draws are not split, and faces decide pixels only under culling or differing stencil
// faces.
#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

#include "../raw/runtime/raw_overlap.h"

namespace {

using Runs = std::vector<std::pair<uint32_t, uint32_t>>;

TEST(RawOverlap, AListAndAStripAreOneTriangleARun)
{
    Runs runs;
    std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
    ASSERT_TRUE(raw::TriangleRuns::sequential(3, 6, 1, runs, winding));
    EXPECT_EQ(runs, (Runs{{0, 3}, {3, 6}}));
    EXPECT_FALSE(winding[0]);
    EXPECT_FALSE(winding[1]);
    ASSERT_TRUE(raw::TriangleRuns::sequential(4, 4, 1, runs, winding));
    EXPECT_EQ(runs, (Runs{{0, 3}, {1, 4}}));
    EXPECT_FALSE(winding[0]);
    EXPECT_TRUE(winding[1]);
}

TEST(RawOverlap, SequentialPlanPreservesEveryPrimitiveAndItsProvokingVertex)
{
    for (uint32_t primitive : {3u, 4u}) {
        for (uint32_t count = 0; count <= 290; ++count) {
            Runs runs;
            std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
            const uint32_t expected = primitive == 3 ? count / 3 : (count > 2 ? count - 2 : 0);
            const bool admitted = raw::TriangleRuns::sequential(primitive, count, 1, runs, winding);
            ASSERT_EQ(admitted, expected <= raw::TriangleRuns::kMaxTriangles);
            if (!admitted) {
                EXPECT_TRUE(runs.empty());
                continue;
            }
            ASSERT_EQ(runs.size(), expected);
            for (uint32_t i = 0; i < expected; ++i) {
                const uint32_t first = primitive == 3 ? i * 3 : i;
                EXPECT_EQ(runs[i].first, first);  // same first/provoking vertex; only raster winding changes
                EXPECT_EQ(runs[i].second, first + 3);
                EXPECT_EQ(winding[i], primitive == 4 && (i & 1));
            }
        }
    }
}

TEST(RawOverlap, AnIndexedStripIsOneTriangleARunOfIndices)
{
    Runs runs;
    std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
    ASSERT_TRUE(raw::TriangleRuns::sequential(4, 4, 1, runs, winding));
    EXPECT_EQ(runs, (Runs{{0, 3}, {1, 4}}));
    EXPECT_FALSE(winding[0]);
    EXPECT_TRUE(winding[1]);
    ASSERT_TRUE(raw::TriangleRuns::sequential(3, 9, 1, runs, winding));
    EXPECT_EQ(runs, (Runs{{0, 3}, {3, 6}, {6, 9}}));
}

TEST(RawOverlap, SequentialPlanRefusesInstanceOrderingAndOtherTopologies)
{
    Runs runs{{0, 6}};
    std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
    EXPECT_FALSE(raw::TriangleRuns::sequential(3, 6, 2, runs, winding));
    EXPECT_FALSE(raw::TriangleRuns::sequential(2, 6, 1, runs, winding));
    EXPECT_FALSE(raw::TriangleRuns::sequential(4, UINT32_MAX, 1, runs, winding));
    EXPECT_TRUE(runs.empty());
}

struct Face {
    uint8_t compare = 7, fail = 0, depthFail = 0, pass = 0;
    uint32_t readMask = ~0u, writeMask = ~0u;
};

struct Stencil {
    Face front, back;
};

TEST(RawOverlap, FacesDecidePixelsOnlyUnderCullingOrDifferingStencilFaces)
{
    const uint32_t same[2] = {3, 3}, apart[2] = {3, 4}, saturated[2] = {300, 255};
    Stencil s;
    EXPECT_FALSE(raw::facingDecides(0, s, same, true));
    EXPECT_TRUE(raw::facingDecides(1, s, same, true));
    EXPECT_TRUE(raw::facingDecides(2, s, same, false));
    EXPECT_FALSE(raw::facingDecides(0, s, apart, true));
    s.back.pass = 6;  // MTLStencilOperationIncrementWrap on the back face alone, as a winding fill writes
    EXPECT_TRUE(raw::facingDecides(0, s, same, true));
    EXPECT_FALSE(raw::facingDecides(0, s, same, false));
    s.front.pass = 6;
    EXPECT_FALSE(raw::facingDecides(0, s, same, true));
    EXPECT_TRUE(raw::facingDecides(0, s, apart, true));
    EXPECT_FALSE(raw::facingDecides(0, s, apart, false));
    EXPECT_FALSE(raw::facingDecides(0, s, saturated, true));
    s.front.readMask = 0x7f;
    EXPECT_TRUE(raw::facingDecides(0, s, same, true));
    Stencil untouched;
    untouched.back.readMask = 0x0f;
    untouched.front.fail = 3;
    EXPECT_FALSE(raw::facingDecides(0, untouched, apart, true));
}

}  // namespace
