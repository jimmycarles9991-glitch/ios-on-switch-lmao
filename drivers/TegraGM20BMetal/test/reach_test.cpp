// SPDX-License-Identifier: MIT
// reach.h on the host. The reach pass's scissor and viewport clip, against the tiles Metal reached on an M4 Max
// (macOS 27.2.1, a 128x96 frame); and the masked turn copies, which must leave a rotation-4 plane, or an attachment of another
// frame through its stand-in, as Metal's load and store do: transposed in the reached tiles, every other texel in place.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "../reach.h"

namespace {

using gm20b::Rect;
using gm20b::Rotation;
using Tiles = std::set<std::pair<uint32_t, uint32_t>>;

Rotation turned(uint8_t turn, uint32_t w, uint32_t h)
{
    Rotation r;
    r.turn = turn;
    r.width = w;
    r.height = h;
    return r;
}

struct Scissor {
    uint32_t x, y, w, h;
};

struct Viewport {
    double x, y, w, h;
};

// What the reach pass reaches of a rectangle drawn in frame pixels [x0, x1] x [y0, y1]: the texels of its target (reachStep
// frame pixels each, the frame's extent) the conservative rasteriser covers (each the rectangle touches, its far edges included,
// as Metal's tiles), within the rounded-out scissor (an empty one where it lies outside the viewport, as RenderEncoder's
// setScissor takes it) and viewport clip, as their tiles
Tiles reached(float x0, float y0, float x1, float y1, Scissor s, Viewport v, uint32_t w, uint32_t h)
{
    const uint32_t step = gm20b::reachStep(w, h);
    if (gm20b::scissorOutsideViewport(s.x, s.y, s.w, s.h, v.x, v.y, v.w, v.h))
        s.w = s.h = 0;
    const Rect sc = gm20b::reachScissor(s.x, s.y, s.w, s.h, w, h, step);
    const Rect vc = gm20b::reachViewportClip(v.x, v.y, v.w, v.h, step);
    const float k = float(step);
    Tiles out;
    for (uint32_t j = 0; j < h / step; j++)
        for (uint32_t i = 0; i < w / step; i++) {
            const bool touched = i <= x1 / k && i + 1 > x0 / k && j <= y1 / k && j + 1 > y0 / k;
            const bool inside = i >= sc.x0 && i < sc.x1 && j >= sc.y0 && j < sc.y1 && i >= vc.x0 && i < vc.x1 &&
                                j >= vc.y0 && j < vc.y1;
            if (touched && inside)
                out.insert({i * step / gm20b::kReachTile, j * step / gm20b::kReachTile});
        }
    return out;
}

TEST(GM20BReach, ScissorsAndViewportsBoundTheReachInWholeTilesAsMetalDoes)
{
    const Scissor full{0, 0, 128, 96};
    const Viewport frame{0, 0, 128, 96};

    struct Case {
        const char *name;
        float x0, y0, x1, y1;
        Scissor scissor;
        Viewport viewport;
        Tiles metal;
    };

    const Case cases[] = {
        {"rect", 40, 4, 60, 24, full, frame, {{1, 0}}},
        {"aligned", 32, 32, 64, 64, full, frame, {{1, 1}, {2, 1}, {1, 2}, {2, 2}}},
        {"aligned short", 32, 32, 63.99f, 63.99f, full, frame, {{1, 1}}},
        {"outside the scissor in a tile it touches", 44, 10, 60, 20, {0, 0, 40, 96}, frame, {{1, 0}}},
        {"scissor to a tile edge", 20, 10, 60, 20, {0, 0, 32, 96}, frame, {{0, 0}}},
        {"scissor from a tile edge", 20, 10, 70, 20, {32, 0, 32, 96}, frame, {{1, 0}}},
        {"empty scissor", 20, 10, 60, 20, {10, 10, 0, 10}, frame, {{0, 0}}},
        {"empty scissor on a tile edge", 20, 10, 60, 20, {32, 10, 0, 10}, frame, {{0, 0}}},
        {"scissor far from it", 70, 10, 80, 20, {0, 0, 40, 96}, frame, {}},
        {"outside the viewport in a tile it touches", 44, 10, 60, 20, full, {0, 0, 40, 96}, {{1, 0}}},
        {"viewport far from it", 70, 10, 80, 20, full, {0, 0, 40, 96}, {}},
        {"viewport to its right", 10, 10, 20, 20, full, {64, 0, 40, 96}, {}},
        // measured under macOS 27.2: a scissor wholly outside the viewport reaches as an empty one, though both touch a tile
        {"scissor left of the viewport", 40, 0, 64, 48, {0, 0, 40, 20}, {40, 0, 24, 48}, {}},
        {"scissor a pixel into the viewport", 40, 0, 64, 48, {0, 0, 41, 20}, {40, 0, 24, 48}, {{1, 0}}},
        {"scissor above the viewport", 0, 7, 64, 48, {0, 0, 32, 7}, {0, 7, 64, 41}, {{0, 0}}},
        {"scissor above the viewport, primitive elsewhere", 47, 11, 53, 66, {35, 1, 7, 6}, {8, 7, 79, 59}, {}},
    };
    for (const Case &c : cases)
        EXPECT_EQ(reached(c.x0, c.y0, c.x1, c.y1, c.scissor, c.viewport, 128, 96), c.metal) << c.name;
    // a frame of no whole tiles: its own edge clips what lies past it in the tiles that overhang it
    const Scissor odd{0, 0, 120, 90};
    const Viewport oddFrame{0, 0, 120, 90};
    EXPECT_EQ(reached(121, 10, 127, 20, odd, oddFrame, 120, 90), Tiles{}) << "past the frame in its last tile";
    EXPECT_EQ(reached(100, 10, 127, 20, odd, oddFrame, 120, 90), (Tiles{{3, 0}})) << "across the frame's edge";
    EXPECT_EQ(reached(100, 100, 300, 200, {0, 0, 1280, 720}, {0, 0, 1280, 720}, 1280, 720).size(), 7u * 4u);
}

TEST(GM20BReach, AFrameOfNoWholeTilesDrawsAtTheLargestStepThatDividesIt)
{
    EXPECT_EQ(gm20b::reachStep(96, 64), 32u);
    EXPECT_EQ(gm20b::reachStep(1280, 720), 16u);
    EXPECT_EQ(gm20b::reachStep(90, 60), 2u);
    EXPECT_EQ(gm20b::reachStep(1279, 720), 1u);
}

TEST(GM20BReach, TheFramesResolutionTakesTheSameTiles)
{
    for (const auto &[x, y, w, h] :
         {std::array<uint32_t, 4>{0, 0, 128, 96}, {40, 4, 20, 20}, {33, 31, 0, 5}, {90, 60, 400, 9}}) {
        const Rect tiles = gm20b::reachScissor(x, y, w, h, 120, 90, gm20b::kReachTile);
        const Rect pixels = gm20b::reachScissor(x, y, w, h, 120, 90, 1);
        EXPECT_EQ(pixels, (Rect{tiles.x0 * 32, tiles.y0 * 32, tiles.x1 * 32, tiles.y1 * 32})) << x << " " << y;
    }
    const Rect a = gm20b::reachViewportClip(-8.5, 3.25, 50, 40, gm20b::kReachTile),
               b = gm20b::reachViewportClip(-8.5, 3.25, 50, 40, 1);
    EXPECT_EQ(a, (Rect{0, 0, 2, 2}));
    EXPECT_EQ(b, (Rect{0, 0, 64, 64}));
    EXPECT_EQ(gm20b::reachViewportClip(5, 5, 0, 10, 1), Rect{});
}

// pack/turn.metal's masked copy over whole images: each destination texel the source's at `map` where its frame tile is reached
void maskedCopy(std::vector<uint32_t> &dst, uint32_t dw, uint32_t dh, const std::vector<uint32_t> &src, uint32_t sw,
                uint32_t sh, const gm20b::TurnMap &map, const gm20b::TileMap &tiles, const std::vector<uint8_t> *mask)
{
    for (int32_t y = 0; y < int32_t(dh); y++)
        for (int32_t x = 0; x < int32_t(dw); x++) {
            if (mask && !gm20b::tileReached(tiles, mask->data(), x, y))
                continue;
            const int32_t sx = map.ax * x + map.ay * y + map.cx, sy = map.bx * x + map.by * y + map.cy;
            if (sx < std::max(map.x0, 0) || sy < std::max(map.y0, 0) || sx >= std::min(map.x1, int32_t(sw)) ||
                sy >= std::min(map.y1, int32_t(sh)))
                continue;
            dst[size_t(y) * dw + x] = src[size_t(sy) * sw + sx];
        }
}

std::vector<uint32_t> unique(uint32_t w, uint32_t h)
{
    std::vector<uint32_t> v(size_t(w) * h);
    for (size_t i = 0; i < v.size(); i++)
        v[i] = uint32_t(i) * 2654435761u + 1;
    return v;
}

std::vector<uint8_t> someTiles(uint32_t tilesX, uint32_t tilesY, uint32_t seed)
{
    std::vector<uint8_t> m(size_t(tilesX) * tilesY);
    for (size_t i = 0; i < m.size(); i++)
        m[i] = ((i * 7 + seed) % 3) == 0;
    return m;
}

// Metal's rotation-4 load and store of a W x H frame: a reached frame texel (x, y) loads plane (y, x) and stores at
// (H - 1 - y, x); every other texel stays
std::vector<uint32_t> metalRotation4(const std::vector<uint32_t> &plane, uint32_t h, const Rect &area,
                                     const std::vector<uint8_t> &mask)
{
    std::vector<uint32_t> out = plane;
    const uint32_t pw = h;
    for (uint32_t y = 0; y < area.y1; y++)
        for (uint32_t x = 0; x < area.x1; x++)
            if (mask[(y / 32) * gm20b::reachTiles(area.x1) + x / 32])
                out[size_t(x) * pw + (h - 1 - y)] = plane[size_t(x) * pw + y];
    return out;
}

TEST(GM20BReach, TheMaskedMirrorTurnsTheReachedTilesAlone)
{
    for (const auto &[w, h] : {std::pair<uint32_t, uint32_t>{96, 64}, {90, 60}, {128, 96}})
        for (uint32_t seed = 0; seed < 4; seed++) {
            const Rotation r = turned(1, w, h);
            const uint32_t tx = gm20b::reachTiles(w), ty = gm20b::reachTiles(h);
            std::vector<uint8_t> mask = seed == 3 ? std::vector<uint8_t>(size_t(tx) * ty, 1) : someTiles(tx, ty, seed);
            const std::vector<uint32_t> plane = unique(r.planeWidth(), r.planeHeight());
            std::vector<uint32_t> got = plane;
            const gm20b::TurnMask m = gm20b::turnMask({nullptr, tx, ty}, gm20b::frameToPlane(r));
            maskedCopy(got, r.planeWidth(), r.planeHeight(), plane, r.planeWidth(), r.planeHeight(),
                       gm20b::planeMirror(r.planeWidth()), m.tiles, &mask);
            EXPECT_EQ(got, metalRotation4(plane, h, {0, 0, w, h}, mask)) << w << "x" << h << " " << seed;
        }
}

// A rotation-4 attachment of another frame beside a turned one: its stand-in takes every texel by the store map's inverse, then
// the reached tiles transposed; the pass stores the stand-in back over the render area. Its plane must end as Metal leaves it.
TEST(GM20BReach, AStandInOfAnotherFrameLoadsTransposedInTheReachedTilesAlone)
{
    const auto run = [](uint8_t passTurn, uint32_t pw, uint32_t ph, uint32_t w4, uint32_t h4, uint32_t seed) {
        const Rotation pass = turned(passTurn, pw, ph), own = turned(4, w4, h4);
        const Rect area{0, 0, std::min(pw, w4), std::min(ph, h4)};
        const uint32_t tx = gm20b::reachTiles(area.x1), ty = gm20b::reachTiles(area.y1);
        const std::vector<uint8_t> mask = someTiles(tx, ty, seed);
        const std::vector<uint32_t> t = unique(own.planeWidth(), own.planeHeight());
        const gm20b::TurnMap intoPlane = gm20b::frameToPlane(pass);
        const gm20b::TurnMap keep = gm20b::after(gm20b::planeLoad(own, false), intoPlane),
                             transposed = gm20b::after(gm20b::planeLoad(own, true), intoPlane),
                             back = gm20b::within(gm20b::after(gm20b::planeToFrame(pass), gm20b::frameToPlane(own)),
                                                  pass.rect(area));
        std::vector<uint32_t> standIn(size_t(pass.planeWidth()) * pass.planeHeight(), 0);
        maskedCopy(standIn, pass.planeWidth(), pass.planeHeight(), t, own.planeWidth(), own.planeHeight(), keep, {},
                   nullptr);
        const gm20b::TurnMask m = gm20b::turnMask({nullptr, tx, ty}, intoPlane);
        maskedCopy(standIn, pass.planeWidth(), pass.planeHeight(), t, own.planeWidth(), own.planeHeight(), transposed,
                   m.tiles, &mask);
        std::vector<uint32_t> got = t;
        maskedCopy(got, own.planeWidth(), own.planeHeight(), standIn, pass.planeWidth(), pass.planeHeight(), back, {},
                   nullptr);
        return got == metalRotation4(t, h4, area, mask);
    };
    for (uint8_t passTurn = 1; passTurn <= 3; passTurn++)
        for (const auto &[w4, h4] : {std::pair<uint32_t, uint32_t>{96, 64}, {128, 80}, {64, 40}, {90, 60}})
            for (uint32_t seed = 0; seed < 3; seed++)
                EXPECT_TRUE(run(passTurn, 96, 64, w4, h4, seed))
                    << int(passTurn) << " " << w4 << "x" << h4 << " " << seed;
}

struct FakeTexture {
    uint8_t rotation = 0;
    bool memoryless = false;
    uint32_t width = 0, height = 0;
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

struct FakeDepthStencil {
    FakeTexture *texture = nullptr;
};

struct FakePass {
    FakeAttachment color[8];
    uint32_t count = 0;
    FakeDepthStencil depthStencil;
};

TEST(GM20BReach, WhichPassesReachEveryTileAndWhichLoadTransposed)
{
    FakeTexture plane{4}, other{0}, transient{0, true};
    FakePass p;
    p.color[0] = {FakeAttachment::Load, &plane};
    p.count = 1;
    EXPECT_TRUE(gm20b::loadsRotation4(p));
    EXPECT_FALSE(gm20b::reachesEveryTile(p));
    p.color[1] = {FakeAttachment::Clear, &other};
    p.count = 2;
    EXPECT_TRUE(gm20b::reachesEveryTile(p));
    p.color[1].store = false;
    EXPECT_FALSE(gm20b::reachesEveryTile(p));
    p.color[1] = {FakeAttachment::Clear, &transient};
    EXPECT_FALSE(gm20b::reachesEveryTile(p));
    p.color[0].load = FakeAttachment::Clear;
    EXPECT_FALSE(gm20b::loadsRotation4(p));
    EXPECT_TRUE(gm20b::reachedAtFrameResolution(0, 0));
    EXPECT_TRUE(gm20b::reachedAtFrameResolution(2, 0));
    EXPECT_TRUE(gm20b::reachedAtFrameResolution(3, 1));
    EXPECT_FALSE(gm20b::reachedAtFrameResolution(4, 0));
}

// A render pass descriptor's renderTargetWidth and renderTargetHeight are 0 until the client sets them (an M4 Max under macOS
// 27.2, with an attachment or without), and the range encode hands them on as such: the frame is then its attachments'
TEST(GM20BReach, TheFrameOfAPassNamingNoSizeIsItsSmallestAttachments)
{
    FakeTexture luma{0, false, 64, 48}, larger{0, false, 96, 64}, plane{1, false, 48, 80}, depth{0, false, 128, 40};
    FakePass p;
    p.color[0] = {FakeAttachment::Load, &luma};
    p.count = 1;
    EXPECT_EQ(gm20b::reachArea(p, 0, 0), (Rect{0, 0, 64, 48})) << "gpu.gm20b.ycbcr_load's pass";
    EXPECT_EQ(gm20b::reachArea(p, 40, 0), (Rect{0, 0, 40, 48})) << "a width it names bounds it";
    p.color[1] = {FakeAttachment::Clear, &larger};
    p.count = 2;
    EXPECT_EQ(gm20b::reachArea(p, 0, 0), (Rect{0, 0, 64, 48}));
    p.color[0].texture = &plane;
    EXPECT_EQ(gm20b::reachArea(p, 0, 0), (Rect{0, 0, 80, 48})) << "a quarter-turned plane 48 x 80 is a frame 80 x 48";
    p.depthStencil.texture = &depth;
    EXPECT_EQ(gm20b::reachArea(p, 0, 0), (Rect{0, 0, 80, 40}));
    EXPECT_EQ(gm20b::reachArea(FakePass{}, 32, 16), (Rect{0, 0, 32, 16}));
    EXPECT_EQ(gm20b::reachArea(FakePass{}, 0, 0), Rect{}) << "no attachment and no size";
}

}  // namespace
