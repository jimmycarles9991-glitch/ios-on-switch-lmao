// SPDX-License-Identifier: MIT
// Which 32x32 tiles of a pass's frame (from its origin) its draws reach: the tiles where Metal loads a rotation-4 plane
// transposed and stores it (metal2vulkan-runtime rotated.rs). Measured on an M4 Max under macOS 27.2.1 (a plane of unique
// texels loaded and stored by draws whose fragments are discarded):
//  - a primitive reaches each tile its area touches, its far edges included (a rectangle to x = 64 reaches the tile from 64),
//    after face culling and depth clipping; one that covers no pixel centre reaches nothing;
//  - the scissor and the viewport bound it in whole tiles, each rounded out to the tiles it touches (a primitive outside the
//    scissor in a tile the scissor touches reaches that tile); an empty scissor is tile (0, 0), and so is one wholly outside
//    the viewport;
//  - every tile when another colour attachment clears and stores, none in a pass without a draw call unless a turned colour
//    attachment clears and stores (raw_area_load.h).
// The raw path draws the pass's geometry again before the pass (CommandBuffer::reach) into an R8Uint mask of a texel a tile:
// each draw's own vertex program and bindings, the pack's turn_reach_frag writing 1, conservative raster (NVB197
// SET_CONSERVATIVE_RASTER), the viewport scaled to the mask, its clip rectangle and the scissor rounded out to tiles. A pass
// with point or line draws, whose size is in raster pixels, draws at the frame's resolution and is reduced to tiles. So that the
// target's own edge clips a primitive past the frame where a tile overhangs it, a frame of no whole tiles draws at the largest
// power-of-two step that divides it (reachStep), reduced to tiles likewise. The conservative rasteriser covers a primitive's
// touched pixels whether or not it covers a pixel centre: such a sliver reaches tiles Metal leaves alone. A draw whose vertex
// program writes memory (SPH DOES_GLOBAL_STORE) is not run again, which would write twice: it reaches every tile of its scissor
// within its viewport, said once a process.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include "raw_rotation.h"
#include "raw_turn_copy.h"

namespace raw {

struct Texture;
class RenderEncoder;

constexpr uint32_t kReachTile = 32;
constexpr uint32_t kReachFormat = 13;  // R8Uint
constexpr const char *kReachState = "raw.turn.turn_reach_frag";
constexpr const char *kReachReduceState = "raw.turn.turn_reach_reduce_frag";

inline uint32_t reachTiles(uint32_t extent)
{
    return (extent + kReachTile - 1) / kReachTile;
}

// A frame rectangle's tiles, rounded out, in the units of a reach target `step` frame pixels a texel (kReachTile: the mask; 1:
// the frame's resolution)
inline Rect reachTilesOf(const Rect &r, uint32_t step)
{
    const uint32_t k = kReachTile / step;
    const auto far = [&](uint32_t edge) {
        return std::min(reachTiles(edge) * k, 65535u);
    };
    return {r.x0 / kReachTile * k, r.y0 / kReachTile * k, far(r.x1), far(r.y1)};
}

// The frame pixels a texel of a `width` x `height` frame's reach target: the largest power of two up to a tile that divides both
inline uint32_t reachStep(uint32_t width, uint32_t height)
{
    uint32_t step = kReachTile;
    while (step > 1 && ((width | height) & (step - 1)))
        step >>= 1;
    return step;
}

// The scissor of a reach draw in its target: Metal's, clipped to the render area, rounded out to tiles; an empty one tile (0, 0)
inline Rect reachScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t areaW, uint32_t areaH, uint32_t step)
{
    const uint32_t x0 = std::min(x, areaW), y0 = std::min(y, areaH);
    const uint32_t x1 = uint32_t(std::min<uint64_t>(uint64_t(x) + w, areaW)),
                   y1 = uint32_t(std::min<uint64_t>(uint64_t(y) + h, areaH));
    return reachTilesOf(x1 > x0 && y1 > y0 ? Rect{x0, y0, x1, y1} : Rect{0, 0, 1, 1}, step);
}

// Whether a scissor lies wholly outside a viewport's pixels, where Metal's reach takes it as an empty one
inline bool scissorOutsideViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, double vx, double vy, double vw,
                                   double vh)
{
    const double x0 = std::floor(std::min(vx, vx + vw)), x1 = std::ceil(std::max(vx, vx + vw));
    const double y0 = std::floor(std::min(vy, vy + vh)), y1 = std::ceil(std::max(vy, vy + vh));
    return double(x) >= x1 || double(x) + w <= x0 || double(y) >= y1 || double(y) + h <= y0;
}

// The viewport's clip rectangle in a reach draw's target: Metal's viewport rounded out to tiles (empty where it is)
inline Rect reachViewportClip(double x, double y, double w, double h, uint32_t step)
{
    const auto edge = [](double v) {
        return uint32_t(std::clamp(v, 0.0, 65535.0));
    };
    const uint32_t x0 = edge(std::floor(std::min(x, x + w))), x1 = edge(std::ceil(std::max(x, x + w)));
    const uint32_t y0 = edge(std::floor(std::min(y, y + h))), y1 = edge(std::ceil(std::max(y, y + h)));
    return x1 > x0 && y1 > y0 ? reachTilesOf({x0, y0, x1, y1}, step) : Rect{};
}

// The frame tile of a masked turn copy's destination texel (x, y): (ax x + ay y + cx, bx x + by y + cy) / 32, read in the mask;
// a texel of no tile there, or of one no draw reaches, is kept (pack/turn.metal Tiles)
struct TileMap {
    int32_t ax = 1, ay = 0, cx = 0, bx = 0, by = 1, cy = 0;
    int32_t tilesX = 0, tilesY = 0;
};

struct MaskedTurnMap {  // pack/turn.metal Masked
    TurnMap map;
    TileMap tiles;
};

static_assert(sizeof(MaskedTurnMap) == 72, "pack/turn.metal Masked");

// `frameOfTexel`: the frame texel of each destination texel (frameToPlane for a plane of the pass's frame)
inline TileMap tileMap(const TurnMap &frameOfTexel, uint32_t tilesX, uint32_t tilesY)
{
    const TurnMap &f = frameOfTexel;
    return {f.ax, f.ay, f.cx, f.bx, f.by, f.cy, int32_t(tilesX), int32_t(tilesY)};
}

// Whether a masked copy writes destination texel (x, y): `mask` holds a byte a tile, tilesX a row
inline bool tileReached(const TileMap &t, const uint8_t *mask, int32_t x, int32_t y)
{
    const int32_t fx = t.ax * x + t.ay * y + t.cx, fy = t.bx * x + t.by * y + t.cy;
    if (fx < 0 || fy < 0)
        return false;
    const int32_t tx = fx / int32_t(kReachTile), ty = fy / int32_t(kReachTile);
    return tx < t.tilesX && ty < t.tilesY && mask[ty * t.tilesX + tx];
}

// A pass's reach (CommandBuffer::reach): null where no draw reaches anything
struct PassReach {
    Texture *mask = nullptr;
    uint32_t tilesX = 0, tilesY = 0;
};

// A masked turn copy's mask and map (CommandBuffer::turnCopy)
struct TurnMask {
    const Texture *mask = nullptr;
    TileMap tiles;
};

inline TurnMask turnMask(const PassReach &r, const TurnMap &frameOfTexel)
{
    return {r.mask, tileMap(frameOfTexel, r.tilesX, r.tilesY)};
}

// The pack's masked turn copy state of `bpp`-byte texels (pack/turn.metal); null for none
inline const char *turnMaskedState(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return "raw.turn.turn_masked8_frag";
    case 2:
        return "raw.turn.turn_masked16_frag";
    case 4:
        return "raw.turn.turn_masked32_frag";
    case 8:
        return "raw.turn.turn_masked64_frag";
    case 16:
        return "raw.turn.turn_masked128_frag";
    default:
        return nullptr;
    }
}

// MTLPrimitiveTypePoint, Line, LineStrip, or triangles drawn as lines (MTLTriangleFillModeLines)
constexpr bool reachedAtFrameResolution(uint32_t primitive, uint32_t fill)
{
    return primitive <= 2 || fill == 1;
}

// A pass's draws as a reach pass takes them again
class DrawReplay {
public:
    virtual bool any() const = 0;
    virtual bool atFrameResolution() const = 0;  // a draw of points or lines
    // each draw with its state into `e`, the state's pipeline taken as its reach twin (RenderEncoder::takeReachTwin), a quad
    // over its viewport where that covers it (reachCovers); false: *why
    virtual bool replay(RenderEncoder &e, const char **why) = 0;

protected:
    ~DrawReplay() = default;
};

// Metal's every tile: another colour attachment of the pass cleared and stored
template <typename Pass> bool reachesEveryTile(const Pass &pass)
{
    using Attachment = std::decay_t<decltype(pass.color[0])>;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const auto *t = pass.color[j].texture;
            t && !t->memoryless && pass.color[j].load == Attachment::Clear && pass.color[j].store)
            return true;
    return false;
}

template <typename Pass> bool loadsRotation4(const Pass &pass)
{
    using Attachment = std::decay_t<decltype(pass.color[0])>;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const auto *t = pass.color[j].texture; t && t->rotation == 4 && pass.color[j].load == Attachment::Load)
            return true;
    return false;
}

}  // namespace raw
