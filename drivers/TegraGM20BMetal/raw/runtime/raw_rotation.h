// SPDX-License-Identifier: MIT
// A texture made with Metal's rotation SPI over an IOSurface plane (a CAMetalLayer drawable on a turned display). A pass draws
// its width x height frame unturned and Metal stores it turned (metal2vulkan-runtime rotated.rs, measured on an M4 and an M3
// Max): texel (x, y) lands in the plane at (H - 1 - y, x) for rotation 1 and 4, (W - 1 - x, H - 1 - y) for 2 and
// (y, W - 1 - x) for 3. The raw path renders straight into the plane: SET_VIEWPORT_COORDINATE_SWIZZLE exchanges clip x and y
// ahead of the viewport transform for a quarter turn, the viewport scales carry the signs, and every window-space rectangle
// (scissor, viewport clip, clears, render area, copies) goes through the same map. The 180-degree turn needs no swizzle.
// Rotation 4 loads the plane transposed where it stores it as rotation 1 does, so a pass that keeps its texels moves them.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace raw {

constexpr uint32_t kSwizzleMethod = 0x0a18;    // SET_VIEWPORT_COORDINATE_SWIZZLE(0); viewport j at + 32 j
constexpr uint32_t kSwizzleIdentity = 0x6420;  // X POS_X, Y POS_Y, Z POS_Z, W POS_W
constexpr uint32_t kSwizzleExchange = 0x6402;  // X POS_Y, Y POS_X
constexpr uint32_t kViewports = 16;

constexpr bool swizzleMethod(uint32_t m)
{
    return m >= kSwizzleMethod && m < kSwizzleMethod + 32 * kViewports && !((m - kSwizzleMethod) & 31);
}

struct Rect {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool operator==(const Rect &o) const
    {
        return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1;
    }
};

struct Rotation {
    uint8_t turn = 0;
    uint32_t width = 0, height = 0;

    bool turned() const
    {
        return turn != 0;
    }

    bool quarter() const
    {
        return turn == 1 || turn == 3 || turn == 4;
    }

    uint32_t planeWidth() const
    {
        return quarter() ? height : width;
    }

    uint32_t planeHeight() const
    {
        return quarter() ? width : height;
    }

    uint32_t swizzle() const
    {
        return quarter() ? kSwizzleExchange : kSwizzleIdentity;
    }

    Rect rect(const Rect &r) const
    {
        const auto back = [](uint32_t extent, uint32_t a) {
            return extent > a ? extent - a : 0u;
        };
        switch (turn) {
        case 1:
        case 4:
            return {back(height, r.y1), r.x0, back(height, r.y0), r.x1};
        case 2:
            return {back(width, r.x1), back(height, r.y1), back(width, r.x0), back(height, r.y0)};
        case 3:
            return {r.y0, back(width, r.x1), r.y1, back(width, r.x0)};
        default:
            return r;
        }
    }

    // The plane's viewport x and y scale and offset for the frame's (`scale`, `offset`: x then y, as the viewport transform
    // takes them, y's scale negative for Metal's upward clip y)
    void viewport(const float scale[2], const float offset[2], float outScale[2], float outOffset[2]) const
    {
        const float w = float(width), h = float(height);
        switch (turn) {
        case 1:
        case 4:
            outScale[0] = -scale[1], outOffset[0] = h - offset[1];
            outScale[1] = scale[0], outOffset[1] = offset[0];
            return;
        case 2:
            outScale[0] = -scale[0], outOffset[0] = w - offset[0];
            outScale[1] = -scale[1], outOffset[1] = h - offset[1];
            return;
        case 3:
            outScale[0] = scale[1], outOffset[0] = offset[1];
            outScale[1] = -scale[0], outOffset[1] = w - offset[0];
            return;
        default:
            outScale[0] = scale[0], outOffset[0] = offset[0];
            outScale[1] = scale[1], outOffset[1] = offset[1];
        }
    }

    // A sample position (x | y << 4, sixteenths of a pixel from its top left) as the plane's pixel holds it; a position on the
    // pixel's top or left edge turns onto its far edge, which the hardware's four bits cannot name, so it stops one short
    uint8_t samplePosition(uint8_t xy) const
    {
        const uint32_t x = xy & 15, y = xy >> 4;
        const auto back = [](uint32_t a) {
            return std::min(16u - a, 15u);
        };
        uint32_t px = x, py = y;
        switch (turn) {
        case 1:
        case 4:
            px = back(y), py = x;
            break;
        case 2:
            px = back(x), py = back(y);
            break;
        case 3:
            px = y, py = back(x);
            break;
        default:
            break;
        }
        return uint8_t(px | py << 4);
    }
};

// The root table's turn words (NVK's nvk_root_descriptor_table::switch_turn, nvk_switch_turn.h): a fragment program's turned
// variant reads the frame through them, f.x = sx (swap ? p.y : p.x) + tx and f.y = sy (swap ? p.x : p.y) + ty of the plane's p,
// a derivative the same without the offsets, a point coordinate with ox, oy (1 where an axis's sign is negative)
constexpr uint32_t kRootTurn = 0x268, kTurnWords = 8;

inline void turnWords(const Rotation &r, uint32_t out[kTurnWords])
{
    const auto bits = [](float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        return u;
    };
    const float w = float(r.width), h = float(r.height);
    float sx = 1, sy = 1, tx = 0, ty = 0;
    uint32_t swap = 0;
    switch (r.turn) {
    case 1:
    case 4:
        swap = 1, sy = -1, ty = h;
        break;
    case 2:
        sx = -1, tx = w, sy = -1, ty = h;
        break;
    case 3:
        swap = 1, sx = -1, tx = w;
        break;
    default:
        break;
    }
    out[0] = swap;
    out[1] = bits(sx);
    out[2] = bits(sy);
    out[3] = bits(tx);
    out[4] = bits(ty);
    out[5] = bits(sx < 0 ? 1.0f : 0.0f);
    out[6] = bits(sy < 0 ? 1.0f : 0.0f);
    out[7] = 0;
}

// A pixel program a turned plane changes the result of: it reads the fragment's position x or y (SPH IMAP_POSITION_X/Y, bits
// 188-189), a point sprite's coordinate (IMAP_POINT_SPRITE_S/T, bits 472-473), or takes a derivative along one axis (FSWZADD,
// NAK's ddx/ddy on SM50, opcode 0x50f8 in bits 63:48 of an instruction; every fourth 64-bit word is a scheduling word). Those
// see the plane's axes, not the frame's. A program that reads its position only to address its own pixel's attachment
// (framebuffer fetch through the interlock or an input attachment) reads the same pixel either way: `fetches` exempts its
// position reads. Implicit-LOD sampling and fwidth do not depend on the axes' order or signs, which this code scan cannot tell
// from a lone derivative: it only picks the programs whose turned variant is worth compiling (raw_direct.cpp), and the
// variant's code (NVK's nvk_switch_turn.c over the program's NIR) decides.
inline bool turnSensitive(const uint32_t sph[20], const uint32_t *code, uint32_t words, bool fetches)
{
    const bool position = (sph[5] >> 28) & 3, sprite = (sph[14] >> 24) & 3;
    if ((position && !fetches) || sprite)
        return true;
    for (uint32_t i = 0; i + 1 < words; i += 2) {
        if ((i / 2) % 4 == 0)
            continue;
        if ((code[i + 1] >> 16) == 0x50f8)
            return true;
    }
    return false;
}

}  // namespace raw
