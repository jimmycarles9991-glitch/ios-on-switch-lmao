// SPDX-License-Identifier: MIT
// raw/runtime/raw_area_load.h on the host: the turn copies that move a turned plane loaded past the render area, run over a
// plane of unique texels, against the texels Metal's load and store left on an M4 Max (macOS 27.2): a plane of each rotation
// drawn with its frame texels' positions, then loaded by a pass of a smaller area whose draws write nothing.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "../raw/runtime/raw_area_load.h"
#include "../raw/runtime/raw_reach.h"

namespace {

using raw::Rect;
using raw::Rotation;

Rotation turned(uint8_t turn, uint32_t w, uint32_t h)
{
    Rotation r;
    r.turn = turn;
    r.width = w;
    r.height = h;
    return r;
}

struct FrameTexel {
    int32_t x = 0, y = 0;
};

// The plane, each texel the frame texel it holds, after raw::loadPastArea's copies (pack/turn.metal's masked copy over the
// destination's first width x height, every texel read from the plane as it was)
std::vector<FrameTexel> loadedPlane(const Rotation &own, uint32_t areaW, uint32_t areaH,
                                    const std::set<uint32_t> *reached)
{
    const uint32_t pw = own.planeWidth(), ph = own.planeHeight();
    const raw::TurnMap ofTexel = raw::frameToPlane(own);
    std::vector<FrameTexel> before(size_t(pw) * ph);
    for (uint32_t y = 0; y < ph; y++)
        for (uint32_t x = 0; x < pw; x++)
            before[size_t(y) * pw + x] = {ofTexel.ax * int32_t(x) + ofTexel.ay * int32_t(y) + ofTexel.cx,
                                          ofTexel.bx * int32_t(x) + ofTexel.by * int32_t(y) + ofTexel.cy};
    const uint32_t tilesX = raw::reachTiles(areaW), tilesY = raw::reachTiles(areaH);
    std::vector<uint8_t> mask(size_t(tilesX) * tilesY, reached ? 0 : 1);
    if (reached)
        for (uint32_t t : *reached)
            mask[t] = 1;
    const raw::TurnMask m = raw::turnMask({nullptr, tilesX, tilesY}, ofTexel);
    std::vector<FrameTexel> out = before;
    raw::AreaLoadCopy copies[4];
    for (uint32_t i = 0, n = raw::areaLoadCopies(raw::areaLoadOffset(own, areaW, areaH), copies); i < n; i++) {
        const raw::AreaLoadCopy &c = copies[i];
        const uint32_t w = c.width ? std::min(c.width, pw) : pw, h = c.height ? std::min(c.height, ph) : ph;
        for (int32_t y = 0; y < int32_t(h); y++)
            for (int32_t x = 0; x < int32_t(w); x++) {
                if (!raw::tileReached(m.tiles, mask.data(), x, y))
                    continue;
                const raw::TurnMap &a = c.map;
                const int32_t sx = a.ax * x + a.ay * y + a.cx, sy = a.bx * x + a.by * y + a.cy;
                if (sx < std::max(a.x0, 0) || sy < std::max(a.y0, 0) || sx >= std::min(a.x1, int32_t(pw)) ||
                    sy >= std::min(a.y1, int32_t(ph)))
                    continue;
                out[size_t(y) * pw + x] = before[size_t(sy) * pw + sx];
            }
    }
    return out;
}

// Each frame texel's offset to the frame texel it holds after the load, in the frame
std::vector<FrameTexel> offsets(const Rotation &own, uint32_t areaW, uint32_t areaH, const std::set<uint32_t> *reached)
{
    const std::vector<FrameTexel> plane = loadedPlane(own, areaW, areaH, reached);
    const raw::TurnMap ofFrame = raw::planeToFrame(own);
    std::vector<FrameTexel> out(size_t(own.width) * own.height);
    for (int32_t y = 0; y < int32_t(own.height); y++)
        for (int32_t x = 0; x < int32_t(own.width); x++) {
            const int32_t px = ofFrame.ax * x + ofFrame.ay * y + ofFrame.cx,
                          py = ofFrame.bx * x + ofFrame.by * y + ofFrame.cy;
            const FrameTexel held = plane[size_t(py) * own.planeWidth() + px];
            out[size_t(y) * own.width + x] = {held.x - x, held.y - y};
        }
    return out;
}

constexpr uint32_t kAny = UINT32_MAX;

struct Band {
    Rect frame;  // frame texels [x0, x1) x [y0, y1)
    int32_t dx, dy;
};

struct Measured {
    uint8_t turn;
    uint32_t w, h, areaW, areaH;
    std::vector<uint32_t> reached;  // empty: every tile
    std::vector<Band> bands;        // the offsets Metal left, each over its texels; every texel of no band kept (0, 0)
    uint32_t kept;                  // the texels holding their own (Metal's count; kAny where it printed none)
};

TEST(RawAreaLoad, TheCopiesLeaveEachTexelAsMetalsLoadAndStoreDid)
{
    const std::vector<Measured> metal = {
        {1, 96, 64, 64, 40, {}, {{{0, 0, 64, 40}, 0, 24}, {{0, 49, 64, 50}, 0, 14}, {{0, 55, 64, 56}, 0, 8}}, 2112},
        {1, 96, 64, 80, 48, {}, {{{0, 0, 96, 48}, 0, 16}, {{0, 49, 96, 50}, 0, 14}, {{0, 61, 96, 62}, 0, 2}}, kAny},
        {1, 96, 64, 50, 30, {}, {{{0, 0, 64, 30}, 0, 34}, {{0, 30, 64, 31}, 0, 33}, {{0, 31, 64, 32}, 0, 32}}, 4096},
        {1, 96, 64, 64, 64, {}, {}, 6144},
        {2, 96, 64, 64, 40, {}, {{{0, 0, 64, 40}, 32, 24}, {{0, 52, 64, 53}, 32, 11}, {{0, 53, 64, 54}, 32, 10}}, 2048},
        {2, 96, 64, 80, 48, {}, {{{0, 0, 80, 48}, 16, 16}, {{0, 56, 80, 57}, 16, 7}, {{0, 59, 80, 60}, 16, 4}}, kAny},
        {2, 96, 64, 50, 30, {}, {{{0, 0, 50, 30}, 46, 34}, {{0, 30, 50, 31}, 46, 33}, {{0, 31, 50, 32}, 46, 32}}, 4096},
        {2, 96, 64, 96, 40, {}, {{{0, 0, 96, 40}, 0, 24}, {{0, 49, 96, 50}, 0, 14}}, kAny},
        {2, 96, 64, 64, 64, {}, {{{0, 0, 64, 64}, 32, 0}}, 2048},
        {3, 96, 64, 64, 40, {}, {{{0, 0, 64, 64}, 32, 0}}, 2048},
        {3, 96, 64, 80, 48, {}, {{{0, 0, 80, 64}, 16, 0}, {{86, 0, 87, 64}, 9, 0}, {{94, 0, 95, 64}, 1, 0}}, 64},
        {3, 96, 64, 50, 30, {}, {{{0, 0, 50, 32}, 46, 0}, {{57, 0, 58, 32}, 38, 0}, {{58, 0, 59, 32}, 37, 0}}, 4096},
        {3, 96, 64, 96, 40, {}, {}, 6144},
        {1, 90, 60, 50, 30, {}, {{{0, 0, 64, 30}, 0, 30}, {{0, 30, 64, 31}, 0, 29}, {{0, 31, 64, 32}, 0, 28}}, 3352},
        {2, 90, 60, 50, 30, {}, {{{0, 0, 50, 30}, 40, 30}, {{0, 30, 50, 31}, 40, 29}, {{0, 31, 50, 32}, 40, 28}}, 3352},
        {3, 90, 60, 50, 30, {}, {{{0, 0, 50, 32}, 40, 0}, {{56, 0, 57, 32}, 33, 0}, {{58, 0, 59, 32}, 31, 0}}, 3352},
        {1, 96, 64, 80, 48, {1}, {{{32, 0, 64, 32}, 0, 16}}, 5120},
        {2, 96, 64, 80, 48, {1}, {{{32, 0, 64, 32}, 16, 16}}, 5120},
        {3, 96, 64, 80, 48, {1}, {{{32, 0, 64, 32}, 16, 0}}, 5120},
    };
    for (const Measured &c : metal) {
        const std::set<uint32_t> tiles(c.reached.begin(), c.reached.end());
        const Rotation own = turned(c.turn, c.w, c.h);
        const std::vector<FrameTexel> got = offsets(own, c.areaW, c.areaH, c.reached.empty() ? nullptr : &tiles);
        const auto name = [&] {
            return std::to_string(c.turn) + " " + std::to_string(c.w) + "x" + std::to_string(c.h) + " area " +
                   std::to_string(c.areaW) + "x" + std::to_string(c.areaH);
        };
        for (const Band &b : c.bands)
            for (uint32_t y = b.frame.y0; y < b.frame.y1; y++)
                for (uint32_t x = b.frame.x0; x < b.frame.x1; x++) {
                    const FrameTexel o = got[size_t(y) * c.w + x];
                    ASSERT_TRUE(o.x == b.dx && o.y == b.dy)
                        << name() << " (" << x << ", " << y << ") holds +(" << o.x << ", " << o.y << ")";
                }
        const auto inPlace = [](FrameTexel o) {
            return !o.x && !o.y;
        };
        const uint32_t kept = uint32_t(std::count_if(got.begin(), got.end(), inPlace));
        if (c.kept != kAny)
            EXPECT_EQ(kept, c.kept) << name();
    }
}

// A frame within the area moves nothing, and rotation 4's load is its transpose whatever the area
TEST(RawAreaLoad, OnlyTheAxesATurnReversesMove)
{
    EXPECT_EQ(raw::areaLoadOffset(turned(1, 96, 64), 96, 64).x, 0u);
    EXPECT_EQ(raw::areaLoadOffset(turned(1, 96, 64), 64, 64).x, 0u);
    EXPECT_EQ(raw::areaLoadOffset(turned(3, 96, 64), 96, 40).y, 0u);
    const raw::PlaneOffset four = raw::areaLoadOffset(turned(4, 96, 64), 50, 30);
    EXPECT_TRUE(!four.x && !four.y);
    const raw::PlaneOffset two = raw::areaLoadOffset(turned(2, 128, 80), 96, 64);
    EXPECT_TRUE(two.x == 32 && two.y == 16);
}

struct FakeTexture {
    uint8_t rotation = 0;
    uint32_t width = 0, height = 0;
    bool memoryless = false;
};

struct FakeAttachment {
    enum Load {
        DontCare,
        Load,
        Clear
    } load = DontCare;

    FakeTexture *texture = nullptr;
    bool store = true;
};

struct FakePass {
    FakeAttachment color[8];
    uint32_t count = 0;
};

// Measured: a pass with no draw call ran over every tile where a turned attachment was cleared and stored (a turned one loaded
// beside it moved, a plain one cleared over the area's tiles), and nothing ran beside a turned one loaded alone (a plain one
// cleared and stored was cleared whole)
TEST(RawAreaLoad, WhichPassesRunWithoutADrawAndWhichLoadPastTheArea)
{
    FakeTexture plane{2, 96, 64}, quarter{1, 64, 96}, plain{0, 50, 30}, small{1, 30, 50};
    FakePass p;
    p.color[0] = {FakeAttachment::Load, &plane};
    p.color[1] = {FakeAttachment::Clear, &plain};
    p.count = 2;
    EXPECT_FALSE(raw::runsWithoutDraws(p));
    EXPECT_TRUE(raw::loadsPastArea(p, 50, 30));
    EXPECT_FALSE(raw::loadsPastArea(p, 96, 64));
    p.color[1] = {FakeAttachment::Clear, &small};
    EXPECT_TRUE(raw::runsWithoutDraws(p));
    p.color[1].store = false;
    EXPECT_FALSE(raw::runsWithoutDraws(p));
    p.color[0] = {FakeAttachment::Load, &quarter};
    EXPECT_TRUE(raw::loadsPastArea(p, 96, 40));
    EXPECT_FALSE(raw::loadsPastArea(p, 40, 64));
    p.color[0].load = FakeAttachment::Clear;
    EXPECT_FALSE(raw::loadsPastArea(p, 50, 30));
}

}  // namespace
