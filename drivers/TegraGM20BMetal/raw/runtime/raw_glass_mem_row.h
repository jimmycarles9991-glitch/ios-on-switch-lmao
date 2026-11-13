// SPDX-License-Identifier: MIT
// DEVELOPMENT: the blur-texture probe's items (raw_glass_mem.h), where their rows are read and what it makes of a row.
#pragma once
#include <algorithm>
#include <cstdint>
#include "raw_nonfinite.h"

namespace raw {

// One row's read: every texel's colour (alpha left out) zero, every alpha zero (true for a format without one), any float
// component NaN or infinite, and its bytes' hash
struct GlassMemRow {
    bool black = false, alphaZero = true, nonFinite = false;
    uint64_t hash = 0;

    const char *verdict() const
    {
        return nonFinite ? "nf" : !black ? "ok" : alphaZero ? "zero.a0" : "zero.a";
    }
};

// The byte range of a texel's alpha in Metal format `format` ([begin, end): none for a format without one)
inline void glassMemAlpha(uint32_t format, uint32_t &begin, uint32_t &end)
{
    switch (format) {
    case 70:
    case 71:
    case 80:
    case 81:
        begin = 3, end = 4;
        return;
    case 115:
        begin = 6, end = 8;
        return;
    case 125:
        begin = 12, end = 16;
        return;
    default:
        begin = end = 0;
    }
}

inline GlassMemRow glassMemRow(const uint8_t *row, uint32_t texels, uint32_t bpp, uint32_t format)
{
    GlassMemRow r;
    r.black = texels > 0;
    r.hash = 0xcbf29ce484222325ull;
    uint32_t alphaBegin, alphaEnd;
    glassMemAlpha(format, alphaBegin, alphaEnd);
    const uint32_t component = floatComponentBytes(format);
    for (uint32_t t = 0; t < texels; t++) {
        const uint8_t *texel = row + uint64_t(t) * bpp;
        for (uint32_t b = 0; b < bpp; b++) {
            r.hash = (r.hash ^ texel[b]) * 0x100000001b3ull;
            const bool alpha = b >= alphaBegin && b < alphaEnd;
            // a float zero's sign bit: -0 is black too
            const bool sign = component && (b % component) == component - 1;
            if (texel[b] & (sign ? 0x7f : 0xff))
                (alpha ? r.alphaZero : r.black) = false;
        }
        for (uint32_t c = 0; component && c + component <= bpp; c += component)
            r.nonFinite |= nonFiniteComponent(texel + c, component);
    }
    return r;
}

// What a command buffer's passes into one image reached: the first pass's load ('c', 'l', 'd'; 0: none seen), the box
// their draws covered (the render area where their draws were not counted), the first pass's render area and the extent
// its clears reached from the origin (0 x 0: none)
struct GlassMemReach {
    char load = 0;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t areaW = 0, areaH = 0, clearW = 0, clearH = 0;
};

// One sampled texture's rows in a command buffer's record
struct GlassMemItem {
    uint64_t key = 0;    // its storage address (Texture::storageRange): a view's is its parent's
    uint64_t image = 0;  // the address of the image (level) sampled, which its passes name as their target
    uint32_t width = 0, height = 0, format = 0, levels = 1, level = 0, texels = 0, bpp = 0;
    uint8_t flags = 0;
    // why a kGlassMemUnsampled one's row is not read: c compressed, m memoryless, z depth, v converted from planes,
    // p read through a pool image, s a stand-in, o another layout the copy engine cannot read
    char unread = 0;
    char state[5] = "-";                                 // the sampling state's family (the "T..." term of its name)
    bool rebuilt = false, moved = false, fresh = false;  // fresh: its first write ever is this command buffer's
    bool glass = false;                                  // a glass draw (a "Tg" state) sampled it
    uint64_t age = 0, events = 0;                        // command buffers and memory events since its last write
    GlassMemReach reach;
    bool outside = false;   // a second row was read outside the reach
    bool quarters = false;  // two more rows were read, a quarter and three quarters down it
    uint32_t inX = 0, inY = 0, outX = 0, outY = 0;
};

// Where `it`'s rows are read: the first in the middle of what its passes reached (else of the image), the second outside
// that box, below or above it, else beside it; false when the box leaves no room for a row outside it
inline bool glassMemRowsAt(GlassMemItem &it)
{
    const GlassMemReach &r = it.reach;
    const bool known = r.load && r.x1 > r.x0 && r.y1 > r.y0;
    const uint32_t w = it.width, h = it.height, n = it.texels;
    const auto across = [&](uint32_t centre) {
        return std::min(centre > n / 2 ? centre - n / 2 : 0u, w - n);
    };
    it.inX = across(known ? (r.x0 + r.x1) / 2 : w / 2);
    it.inY = std::min(known ? (r.y0 + r.y1) / 2 : h / 2, h - 1);
    it.outside = known;
    if (known && r.y1 < h)
        it.outX = across(w / 2), it.outY = (r.y1 + h) / 2;
    else if (known && r.y0 > 0)
        it.outX = across(w / 2), it.outY = r.y0 / 2;
    else if (known && r.x1 + n <= w)
        it.outX = r.x1 + (w - r.x1 - n) / 2, it.outY = h / 2;
    else if (known && r.x0 >= n)
        it.outX = (r.x0 - n) / 2, it.outY = h / 2;
    else
        it.outside = false;
    return it.outside;
}

}  // namespace raw
