// SPDX-License-Identifier: MIT
// A turned colour attachment loaded by a pass whose render area (from the origin, the smallest attachment and target size)
// ends short of its frame. Measured on an M4 Max under macOS 27.2 (planes of every rotation beside planes and textures of
// other extents, out/agent-scratch/x1-mixedext/mac): the area, default viewport and scissor are the intersection, and each
// attachment stores by its own map, but its Load reads the plane as if the frame were the area's size. A plane texel p the
// pass stores takes texel p - o, clamped to the plane's first row and column, o = (H - ah, 0) for rotation 1, (W - aw,
// H - ah) for 2 and (0, W - aw) for 3; frame texel (x, y) takes (min(x + W - aw, W - 1), min(y + H - ah, H - 1)) on the
// axes the turn reverses. Rotation 4 loads transposed whatever the area (raw_reach.h). Only the frame's 32x32 tiles over the
// area that the pass reaches are stored (raw_reach.h). Metal's tiles read texels that other tiles of the pass, and of the
// pass before, store, in no order: about 1 texel in 100 of a 1280x720 pass differed from run to run. The raw path moves the
// texels before the pass, which is the ordered result.
#pragma once
#include <cstdint>
#include <type_traits>
#include "raw_rotated_pass.h"
#include "raw_turn_copy.h"

namespace raw {

struct PlaneOffset {
    uint32_t x = 0, y = 0;
};

inline PlaneOffset areaLoadOffset(const Rotation &own, uint32_t areaW, uint32_t areaH)
{
    const uint32_t dx = own.width > areaW ? own.width - areaW : 0, dy = own.height > areaH ? own.height - areaH : 0;
    switch (own.turn) {
    case 1:
        return {dy, 0};
    case 2:
        return {dx, dy};
    case 3:
        return {0, dx};
    default:
        return {};
    }
}

// A turn copy over the destination's first width x height texels (0: all of an axis)
struct AreaLoadCopy {
    TurnMap map;
    uint32_t width = 0, height = 0;
};

// The turn copies that move a plane (from a copy of it) as such a load does: the offset read, then the first column and row
// read where it reaches past them, and their corner
inline uint32_t areaLoadCopies(const PlaneOffset &o, AreaLoadCopy out[4])
{
    const int32_t ox = int32_t(o.x), oy = int32_t(o.y);
    uint32_t n = 0;
    out[n++] = {TurnMap{1, 0, -ox, 0, 1, -oy}};
    if (ox)
        out[n++] = {TurnMap{0, 0, 0, 0, 1, -oy, 0, 0, 1, INT32_MAX}, o.x, 0};
    if (oy)
        out[n++] = {TurnMap{1, 0, -ox, 0, 0, 0, 0, 0, INT32_MAX, 1}, 0, o.y};
    if (ox && oy)
        out[n++] = {TurnMap{0, 0, 0, 0, 0, 0, 0, 0, 1, 1}, o.x, o.y};
    return n;
}

// A pass Metal runs with no draw call: a turned colour attachment cleared and stored
template <typename Pass> bool runsWithoutDraws(const Pass &pass)
{
    using Attachment = std::decay_t<decltype(pass.color[0])>;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const auto *t = pass.color[j].texture;
            t && t->rotation && !t->memoryless && pass.color[j].load == Attachment::Clear && pass.color[j].store)
            return true;
    return false;
}

// Whether the pass loads a turned colour attachment whose texels its Load moves
template <typename Pass> bool loadsPastArea(const Pass &pass, uint32_t areaW, uint32_t areaH)
{
    using Attachment = std::decay_t<decltype(pass.color[0])>;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const auto *t = pass.color[j].texture; t && pass.color[j].load == Attachment::Load) {
            const PlaneOffset o = areaLoadOffset(frameOf(*t), areaW, areaH);
            if (o.x || o.y)
                return true;
        }
    return false;
}

struct Texture;
struct PassReach;
class CommandBuffer;

// `t`'s texels moved as a pass of an areaW x areaH render area reaching `reach` loads and stores them; false: *why
bool loadPastArea(CommandBuffer &rc, Texture &t, uint32_t areaW, uint32_t areaH, const PassReach &reach, uint32_t tag,
                  const char **why);

}  // namespace raw
