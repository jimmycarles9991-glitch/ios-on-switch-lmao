// SPDX-License-Identifier: MIT
// Texel copies a turned pass needs that no copy engine can make (it moves rows, never transposes or mirrors): the pack's turn
// copy states (pack/turn.metal), each destination texel (x, y) the source's at (ax x + ay y + cx, bx x + by y + cy), kept
// where that falls outside the map's source rectangle, both colour images viewed as unsigned integers of their texel size, a
// depth image's depth copied as the float it holds, its stencil by the pack's stencil bits (raw_stencil_bits.h), or a stencil
// plane's whole texel in one draw where raw_zeta_colour.h's route takes it.
//  - an attachment not of the pass's turn and frame beside a turned one whose contents outside the pass matter
//    (raw_rotated_pass.h): its texels into a plane-shaped stand-in the pass renders turned where it is loaded (the pass's
//    frame read through the attachment's own load map), and back where it is stored, over the render area alone;
//  - rotation 4 loaded (raw_rotation.h): Metal loads the plane transposed and stores it as rotation 1 does, which keeps a
//    texel only where the plane is first mirrored across its width (planeMirror); the pass then draws as rotation 1.
#pragma once
#include <cstdint>
#include <climits>
#include "raw_rotation.h"

namespace raw {

struct TurnMap {
    int32_t ax = 1, ay = 0, cx = 0, bx = 0, by = 1, cy = 0;
    int32_t x0 = 0, y0 = 0, x1 = INT32_MAX,
            y1 = INT32_MAX;  // the source texels it reads; the destination keeps the rest
};

// `outer` applied to what `inner` gives, `inner`'s source rectangle dropped
inline TurnMap after(const TurnMap &outer, const TurnMap &inner)
{
    TurnMap m;
    m.ax = outer.ax * inner.ax + outer.ay * inner.bx;
    m.ay = outer.ax * inner.ay + outer.ay * inner.by;
    m.cx = outer.ax * inner.cx + outer.ay * inner.cy + outer.cx;
    m.bx = outer.bx * inner.ax + outer.by * inner.bx;
    m.by = outer.bx * inner.ay + outer.by * inner.by;
    m.cy = outer.bx * inner.cx + outer.by * inner.cy + outer.cy;
    m.x0 = outer.x0, m.y0 = outer.y0, m.x1 = outer.x1, m.y1 = outer.y1;
    return m;
}

inline TurnMap within(TurnMap m, const Rect &source)
{
    m.x0 = int32_t(source.x0), m.y0 = int32_t(source.y0), m.x1 = int32_t(source.x1), m.y1 = int32_t(source.y1);
    return m;
}

// The frame texel of each plane texel: Metal's store map read backwards
inline TurnMap frameToPlane(const Rotation &r)
{
    const int32_t w = int32_t(r.width), h = int32_t(r.height);
    switch (r.turn) {
    case 1:
    case 4:
        return {0, 1, 0, -1, 0, h - 1};
    case 2:
        return {-1, 0, w - 1, 0, -1, h - 1};
    case 3:
        return {0, -1, w - 1, 1, 0, 0};
    default:
        return {};
    }
}

// The plane texel of each frame texel: Metal's store map (raw_rotation.h)
inline TurnMap planeToFrame(const Rotation &r)
{
    const int32_t w = int32_t(r.width), h = int32_t(r.height);
    switch (r.turn) {
    case 1:
    case 4:
        return {0, -1, h - 1, 1, 0, 0};
    case 2:
        return {-1, 0, w - 1, 0, -1, h - 1};
    case 3:
        return {0, 1, 0, -1, 0, w - 1};
    default:
        return {};
    }
}

// A plane `width` texels wide mirrored across it
inline TurnMap planeMirror(uint32_t width)
{
    return {-1, 0, int32_t(width) - 1, 0, 1, 0};
}

// The plane texel a pass loads each frame texel from: the store map's inverse, or rotation 4's transpose in a pass that draws
// (where nothing draws Metal keeps every texel in place, as the inverse does)
inline TurnMap planeLoad(const Rotation &r, bool draws)
{
    if (r.turn == 4 && draws)
        return {0, 1, 0, 1, 0, 0};
    return planeToFrame(r);
}

// The pack's turn copy state of `bpp`-byte texels and the unsigned format both images are viewed as; null, 0 for none
inline const char *turnCopyState(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return "raw.turn.turn_copy8_frag";
    case 2:
        return "raw.turn.turn_copy16_frag";
    case 4:
        return "raw.turn.turn_copy32_frag";
    case 8:
        return "raw.turn.turn_copy64_frag";
    case 16:
        return "raw.turn.turn_copy128_frag";
    default:
        return nullptr;
    }
}

// The pack's turn copy state of a depth format's depth (Depth16Unorm, Depth32Float, Depth32Float_Stencil8); null for none
inline const char *turnDepthState(uint32_t format)
{
    switch (format) {
    case 250:
        return "raw.turn.turn_depth16_frag";
    case 252:
        return "raw.turn.turn_depth32f_frag";
    case 260:
        return "raw.turn.turn_depth32fs8_frag";
    default:
        return nullptr;
    }
}

inline uint32_t turnCopyFormat(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return 13;  // R8Uint
    case 2:
        return 23;  // R16Uint
    case 4:
        return 53;  // R32Uint
    case 8:
        return 103;  // RG32Uint
    case 16:
        return 123;  // RGBA32Uint
    default:
        return 0;
    }
}

}  // namespace raw
