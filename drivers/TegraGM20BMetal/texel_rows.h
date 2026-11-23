// SPDX-License-Identifier: MIT
// A texel buffer over a surface the kernel laid out block-linear (Texture::texelRows). An M4 Max reads and stores such a
// buffer's bytes in order: byte b is the surface's byte offset + b of its rows bytesPerRow apart, as a CPU lock sees them
// (m2v-textures9 probes/tbuf.txt). A ONE_D_BUFFER header addresses linear memory alone (clb097tex.h TEXHEAD_1D: ADDRESS and
// WIDTH, no GOB or block fields), so the texture is a pitch pool of those bytes: the one surface texture that is not
// block-linear, for that reason. The copy engine fills the pool from the rows before work that uses it and copies it back
// after work that writes it, at the points a pool image of Texture::sampled is copied. A row's bytes past its texels and
// before bytesPerRow, and bytes past the last row, have no storage in the layout: they read 0 and what is stored there
// lands nowhere.
#pragma once
#include <algorithm>
#include <cstdint>
#include "TegraGM20BLayout.h"

namespace gm20b {

struct Import;
struct Texture;
class BlitEncoder;

// Rows [row, row + rows) of the surface, bytes [column, column + bytes) of each, holding the range's bytes from `at` on,
// copied in elements of `unit` bytes (a power of two up to 16 dividing column, bytes and the rows' stride)
struct TexelRowsPart {
    uint32_t row = 0, rows = 0, column = 0, bytes = 0, unit = 16;
    uint64_t at = 0;
};

struct TexelRowsParts {
    TexelRowsPart part[3];
    uint32_t count = 0;
    uint64_t held = 0;  // the range's bytes the layout holds
};

// The parts of the rows of `l`, `bytesPerRow` apart, that hold `bytes` bytes from byte `offset`: a part of the first row,
// the rows held whole, a part of the last
inline TexelRowsParts texelRowsParts(const tgm_layout &l, uint64_t offset, uint32_t bytesPerRow, uint64_t bytes)
{
    TexelRowsParts out;
    if (!bytes || !bytesPerRow)
        return out;
    const uint64_t texels = uint64_t(l.width) * l.bpp, end = offset + bytes;
    const auto add = [&](uint64_t row, uint64_t rows, uint64_t from, uint64_t to) {
        to = std::min(to, texels);
        rows = row < l.height ? std::min<uint64_t>(rows, l.height - row) : 0;
        if (!rows || from >= to)
            return;
        const uint32_t mask = uint32_t(from | (to - from) | bytesPerRow | 16);
        out.part[out.count++] = {uint32_t(row),       uint32_t(rows),     uint32_t(from),
                                 uint32_t(to - from), mask & (0u - mask), row * bytesPerRow + from - offset};
        out.held += rows * (to - from);
    };
    const uint64_t first = offset / bytesPerRow, last = (end - 1) / bytesPerRow;
    const uint64_t lead = offset - first * bytesPerRow, tail = end - last * bytesPerRow;
    const bool partFirst = lead || (first == last && tail < texels), partLast = last > first && tail < texels;
    if (partFirst)
        add(first, 1, lead, first == last ? tail : bytesPerRow);
    const uint64_t whole = partFirst ? first + 1 : first, wholeEnd = partLast ? last : last + 1;
    if (wholeEnd > whole)
        add(whole, wholeEnd - whole, 0, texels);
    if (partLast)
        add(last, 1, 0, tail);
    return out;
}

// Whether the copy engine can address every part: SET_SRC_ORIGIN and SET_DST_ORIGIN take X in 16 bits (clb0b5.h)
inline bool texelRowsAddressable(const TexelRowsParts &parts)
{
    for (uint32_t k = 0; k < parts.count; k++)
        if (parts.part[k].column / parts.part[k].unit > 0xffff)
            return false;
    return true;
}

struct TexelRows {
    Import *import = nullptr;  // the surface's, kernelTiled
    uint64_t offset = 0;
    uint32_t bytesPerRow = 0;
    TexelRowsParts parts;  // of the whole buffer, which the pool's fills and copies back move
};

// The pool `t` of a texel buffer over a surface's rows filled from them by the copy engine (MAXWELL_DMA_COPY_A, which
// takes the block-linear side by the layout's GOBs), bytes with no storage there zeroed first; else copied back into them
void moveTexelRows(BlitEncoder &b, const Texture &t, bool fill);

}  // namespace gm20b
