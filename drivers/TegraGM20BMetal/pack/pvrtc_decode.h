// SPDX-License-Identifier: MIT
// One texel of a PVRTC (version 1) level as Apple's GPUs sample it, measured on an M4 Max (harness/decodeoracle.mm),
// as 8-bit codes. A block is 64 bits: the modulation word, then the colour word (colour B in its top half, colour A
// below it, the modulation mode in bit 0). Colours A and B are each a low-resolution image upscaled bilinearly between
// block centres, wrapping at the level's edges; each texel blends them by its modulation. After Imagination's
// PVRTDecompress (PowerVR SDK, MIT).
//
// `Source` is the caller's: blockAt(source, x, y) answers the block at block coordinates already wrapped to the level
// (decode.metal's in rows, runtime/decoded.h pvrtcMortonBlock for replaceRegion's order).
#pragma once
#include "decode_bits.h"

namespace gm20b {
namespace decode {

struct Words2 {
    uint32_t modulation, colour;
};

// Colour A (`b` false) or B of a colour word as 5-bit red, green and blue and 4-bit alpha
static inline Int4 pvrtcColour(uint32_t word, bool b)
{
    if (b) {
        if (word & 0x80000000u)
            return int4Of(int((word >> 26) & 0x1f), int((word >> 21) & 0x1f), int((word >> 16) & 0x1f), 0xf);
        return int4Of(int(((word >> 23) & 0x1e) | ((word >> 27) & 1)), int(((word >> 19) & 0x1e) | ((word >> 23) & 1)),
                      int(((word >> 15) & 0x1e) | ((word >> 19) & 1)), int((word >> 27) & 0xe));
    }
    if (word & 0x8000u)
        return int4Of(int((word >> 10) & 0x1f), int((word >> 5) & 0x1f), int((word & 0x1e) | ((word >> 4) & 1)), 0xf);
    return int4Of(int(((word >> 7) & 0x1e) | ((word >> 11) & 1)), int(((word >> 3) & 0x1e) | ((word >> 7) & 1)),
                  int(((word << 1) & 0x1c) | ((word >> 2) & 3)), int((word >> 11) & 0xe));
}

// The modulation index (0..3, onto 0, 3, 5, 8 eighths) a 2-bit-per-pixel block in its interpolated mode stores for its
// texel (x, y), one whose x + y is even
static inline int pvrtc2StoredIndex(uint32_t modulation, int x, int y)
{
    uint32_t m = modulation;
    if (m & 1)
        m = (m & ~(1u << 20)) | ((m >> 1) & (1u << 20));
    m = (m & ~1u) | ((m >> 1) & 1);
    return int((m >> (2 * (y * 4 + x / 2))) & 3);
}

static inline int pvrtcEighths(int index)
{
    return index == 0 ? 0 : index == 1 ? 3 : index == 2 ? 5 : 8;
}

// A 2-bit-per-pixel texel's modulation in eighths as a neighbour reads it: a stored texel of its block (x + y even)
static inline int pvrtc2Neighbour(Words2 block, int x, int y)
{
    if (!(block.colour & 1))
        return (block.modulation >> (y * 8 + x)) & 1 ? 8 : 0;
    return pvrtcEighths(pvrtc2StoredIndex(block.modulation, x, y));
}

template <class Source> static inline Words2 pvrtcBlock(Source source, int bx, int by, int blocksX, int blocksY)
{
    return blockAt(source, (bx % blocksX + blocksX) % blocksX, (by % blocksY + blocksY) % blocksY);
}

// Texel (x, y) of a level of blocksX x blocksY blocks of 4x4 texels (4 bits a pixel) or 8x4 (`twoBpp`)
template <class Source>
static inline Texel pvrtcTexel(Source source, int x, int y, int blocksX, int blocksY, bool twoBpp)
{
    const int bw = twoBpp ? 8 : 4, bh = 4;
    const int px = x - bw / 2 + bw * blocksX, py = y - bh / 2 + bh * blocksY;
    const int bx0 = px / bw, by0 = py / bh, fx = px - bx0 * bw, fy = py - by0 * bh;
    const Words2 p = pvrtcBlock(source, bx0, by0, blocksX, blocksY),
                 q = pvrtcBlock(source, bx0 + 1, by0, blocksX, blocksY),
                 r = pvrtcBlock(source, bx0, by0 + 1, blocksX, blocksY),
                 s = pvrtcBlock(source, bx0 + 1, by0 + 1, blocksX, blocksY);
    const int wp = (bw - fx) * (bh - fy), wq = fx * (bh - fy), wr = (bw - fx) * fy, ws = fx * fy;
    Int4 c[2];
    for (int k = 0; k < 2; k++) {
        const Int4 cp = pvrtcColour(p.colour, k), cq = pvrtcColour(q.colour, k), cr = pvrtcColour(r.colour, k),
                   cs = pvrtcColour(s.colour, k);
        const int vr = cp.r * wp + cq.r * wq + cr.r * wr + cs.r * ws,
                  vg = cp.g * wp + cq.g * wq + cr.g * wr + cs.g * ws,
                  vb = cp.b * wp + cq.b * wq + cr.b * wr + cs.b * ws,
                  va = cp.a * wp + cq.a * wq + cr.a * wr + cs.a * ws;
        c[k] = twoBpp
                   ? int4Of((vr >> 7) + (vr >> 2), (vg >> 7) + (vg >> 2), (vb >> 7) + (vb >> 2), (va >> 5) + (va >> 1))
                   : int4Of((vr >> 6) + (vr >> 1), (vg >> 6) + (vg >> 1), (vb >> 6) + (vb >> 1), (va >> 4) + va);
    }
    const int ox = x / bw, oy = y / bh, lx = x - ox * bw, ly = y - oy * bh;
    const Words2 own = pvrtcBlock(source, ox, oy, blocksX, blocksY);
    int eighths;
    bool punch = false;
    if (!twoBpp) {
        const int index = int((own.modulation >> (2 * (ly * 4 + lx))) & 3);
        eighths = own.colour & 1 ? (index == 0 ? 0 : index == 3 ? 8 : 4) : pvrtcEighths(index);
        punch = (own.colour & 1) && index == 2;
    } else if (!(own.colour & 1)) {
        eighths = (own.modulation >> (ly * 8 + lx)) & 1 ? 8 : 0;
    } else if (!((lx ^ ly) & 1)) {
        eighths = pvrtcEighths(pvrtc2StoredIndex(own.modulation, lx, ly));
    } else {
        const int hv = own.modulation & 1 ? ((own.modulation >> 20) & 1 ? 2 : 1) : 0;
        const int left = pvrtc2Neighbour(pvrtcBlock(source, (x - 1 + bw * blocksX) / bw, oy, blocksX, blocksY),
                                         (lx + bw - 1) % bw, ly);
        const int right = pvrtc2Neighbour(pvrtcBlock(source, (x + 1) / bw, oy, blocksX, blocksY), (lx + 1) % bw, ly);
        const int up = pvrtc2Neighbour(pvrtcBlock(source, ox, (y - 1 + bh * blocksY) / bh, blocksX, blocksY), lx,
                                       (ly + bh - 1) % bh);
        const int down = pvrtc2Neighbour(pvrtcBlock(source, ox, (y + 1) / bh, blocksX, blocksY), lx, (ly + 1) % bh);
        eighths = hv == 0 ? (left + right + up + down + 2) / 4 : hv == 1 ? (left + right + 1) / 2 : (up + down + 1) / 2;
    }
    const Int4 a = c[0], b = c[1];
    return texelOf(uint32_t((a.r * (8 - eighths) + b.r * eighths) / 8),
                   uint32_t((a.g * (8 - eighths) + b.g * eighths) / 8),
                   uint32_t((a.b * (8 - eighths) + b.b * eighths) / 8),
                   punch ? 0u : uint32_t((a.a * (8 - eighths) + b.a * eighths) / 8));
}

}  // namespace decode
}  // namespace gm20b
