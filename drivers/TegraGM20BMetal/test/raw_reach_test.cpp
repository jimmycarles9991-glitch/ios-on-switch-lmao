// SPDX-License-Identifier: MIT
// raw/runtime/raw_reach.h on the host. The reach pass's scissor and viewport clip, against the tiles Metal reached on an M4 Max
// (macOS 27.2.1, a 128x96 frame); and the masked turn copies, which must leave a rotation-4 plane, or an attachment of another
// frame through its stand-in, as Metal's load and store do: transposed in the reached tiles, every other texel in place.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "../raw/runtime/raw_reach.h"

namespace {

using raw::Rect;
using raw::Rotation;
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
    const uint32_t step = raw::reachStep(w, h);
    if (raw::scissorOutsideViewport(s.x, s.y, s.w, s.h, v.x, v.y, v.w, v.h))
        s.w = s.h = 0;
    const Rect sc = raw::reachScissor(s.x, s.y, s.w, s.h, w, h, step);
    const Rect vc = raw::reachViewportClip(v.x, v.y, v.w, v.h, step);
    const float k = float(step);
    Tiles out;
    for (uint32_t j = 0; j < h / step; j++)
        for (uint32_t i = 0; i < w / step; i++) {
            const bool touched = i <= x1 / k && i + 1 > x0 / k && j <= y1 / k && j + 1 > y0 / k;
            const bool inside = i >= sc.x0 && i < sc.x1 && j >= sc.y0 && j < sc.y1 && i >= vc.x0 && i < vc.x1 &&
                                j >= vc.y0 && j < vc.y1;
            if (touched && inside)
                out.insert({i * step / raw::kReachTile, j * step / raw::kReachTile});
        }
    return out;
}

TEST(RawReach, ScissorsAndViewportsBoundTheReachInWholeTilesAsMetalDoes)
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

TEST(RawReach, AFrameOfNoWholeTilesDrawsAtTheLargestStepThatDividesIt)
{
    EXPECT_EQ(raw::reachStep(96, 64), 32u);
    EXPECT_EQ(raw::reachStep(1280, 720), 16u);
    EXPECT_EQ(raw::reachStep(90, 60), 2u);
    EXPECT_EQ(raw::reachStep(1279, 720), 1u);
}

TEST(RawReach, TheFramesResolutionTakesTheSameTiles)
{
    for (const auto &[x, y, w, h] :
         {std::array<uint32_t, 4>{0, 0, 128, 96}, {40, 4, 20, 20}, {33, 31, 0, 5}, {90, 60, 400, 9}}) {
        const Rect tiles = raw::reachScissor(x, y, w, h, 120, 90, raw::kReachTile);
        const Rect pixels = raw::reachScissor(x, y, w, h, 120, 90, 1);
        EXPECT_EQ(pixels, (Rect{tiles.x0 * 32, tiles.y0 * 32, tiles.x1 * 32, tiles.y1 * 32})) << x << " " << y;
    }
    const Rect a = raw::reachViewportClip(-8.5, 3.25, 50, 40, raw::kReachTile),
               b = raw::reachViewportClip(-8.5, 3.25, 50, 40, 1);
    EXPECT_EQ(a, (Rect{0, 0, 2, 2}));
    EXPECT_EQ(b, (Rect{0, 0, 64, 64}));
    EXPECT_EQ(raw::reachViewportClip(5, 5, 0, 10, 1), Rect{});
}

// pack/turn.metal's masked copy over whole images: each destination texel the source's at `map` where its frame tile is reached
void maskedCopy(std::vector<uint32_t> &dst, uint32_t dw, uint32_t dh, const std::vector<uint32_t> &src, uint32_t sw,
                uint32_t sh, const raw::TurnMap &map, const raw::TileMap &tiles, const std::vector<uint8_t> *mask)
{
    for (int32_t y = 0; y < int32_t(dh); y++)
        for (int32_t x = 0; x < int32_t(dw); x++) {
            if (mask && !raw::tileReached(tiles, mask->data(), x, y))
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
            if (mask[(y / 32) * raw::reachTiles(area.x1) + x / 32])
                out[size_t(x) * pw + (h - 1 - y)] = plane[size_t(x) * pw + y];
    return out;
}

TEST(RawReach, TheMaskedMirrorTurnsTheReachedTilesAlone)
{
    for (const auto &[w, h] : {std::pair<uint32_t, uint32_t>{96, 64}, {90, 60}, {128, 96}})
        for (uint32_t seed = 0; seed < 4; seed++) {
            const Rotation r = turned(1, w, h);
            const uint32_t tx = raw::reachTiles(w), ty = raw::reachTiles(h);
            std::vector<uint8_t> mask = seed == 3 ? std::vector<uint8_t>(size_t(tx) * ty, 1) : someTiles(tx, ty, seed);
            const std::vector<uint32_t> plane = unique(r.planeWidth(), r.planeHeight());
            std::vector<uint32_t> got = plane;
            const raw::TurnMask m = raw::turnMask({nullptr, tx, ty}, raw::frameToPlane(r));
            maskedCopy(got, r.planeWidth(), r.planeHeight(), plane, r.planeWidth(), r.planeHeight(),
                       raw::planeMirror(r.planeWidth()), m.tiles, &mask);
            EXPECT_EQ(got, metalRotation4(plane, h, {0, 0, w, h}, mask)) << w << "x" << h << " " << seed;
        }
}

// A rotation-4 attachment of another frame beside a turned one: its stand-in takes every texel by the store map's inverse, then
// the reached tiles transposed; the pass stores the stand-in back over the render area. Its plane must end as Metal leaves it.
TEST(RawReach, AStandInOfAnotherFrameLoadsTransposedInTheReachedTilesAlone)
{
    const auto run = [](uint8_t passTurn, uint32_t pw, uint32_t ph, uint32_t w4, uint32_t h4, uint32_t seed) {
        const Rotation pass = turned(passTurn, pw, ph), own = turned(4, w4, h4);
        const Rect area{0, 0, std::min(pw, w4), std::min(ph, h4)};
        const uint32_t tx = raw::reachTiles(area.x1), ty = raw::reachTiles(area.y1);
        const std::vector<uint8_t> mask = someTiles(tx, ty, seed);
        const std::vector<uint32_t> t = unique(own.planeWidth(), own.planeHeight());
        const raw::TurnMap intoPlane = raw::frameToPlane(pass);
        const raw::TurnMap keep = raw::after(raw::planeLoad(own, false), intoPlane),
                           transposed = raw::after(raw::planeLoad(own, true), intoPlane),
                           back = raw::within(raw::after(raw::planeToFrame(pass), raw::frameToPlane(own)),
                                              pass.rect(area));
        std::vector<uint32_t> standIn(size_t(pass.planeWidth()) * pass.planeHeight(), 0);
        maskedCopy(standIn, pass.planeWidth(), pass.planeHeight(), t, own.planeWidth(), own.planeHeight(), keep, {},
                   nullptr);
        const raw::TurnMask m = raw::turnMask({nullptr, tx, ty}, intoPlane);
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

TEST(RawReach, WhichPassesReachEveryTileAndWhichLoadTransposed)
{
    FakeTexture plane{4}, other{0}, transient{0, true};
    FakePass p;
    p.color[0] = {FakeAttachment::Load, &plane};
    p.count = 1;
    EXPECT_TRUE(raw::loadsRotation4(p));
    EXPECT_FALSE(raw::reachesEveryTile(p));
    p.color[1] = {FakeAttachment::Clear, &other};
    p.count = 2;
    EXPECT_TRUE(raw::reachesEveryTile(p));
    p.color[1].store = false;
    EXPECT_FALSE(raw::reachesEveryTile(p));
    p.color[1] = {FakeAttachment::Clear, &transient};
    EXPECT_FALSE(raw::reachesEveryTile(p));
    p.color[0].load = FakeAttachment::Clear;
    EXPECT_FALSE(raw::loadsRotation4(p));
    EXPECT_TRUE(raw::reachedAtFrameResolution(0, 0));
    EXPECT_TRUE(raw::reachedAtFrameResolution(2, 0));
    EXPECT_TRUE(raw::reachedAtFrameResolution(3, 1));
    EXPECT_FALSE(raw::reachedAtFrameResolution(4, 0));
}

}  // namespace
