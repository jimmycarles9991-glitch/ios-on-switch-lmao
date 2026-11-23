// SPDX-License-Identifier: MIT
// What Metal's blits move between RGB10A8_2P_XR10 (550) or RGB10A8_2P_XR10_sRGB (551) textures at least one of which a
// descriptor made (an M4 Max under macOS 27.2.1, validation off and on, 287 cases with 0 misses: m2v-textures13
// probes/c1copies/RESULT.txt). Each side's words and alpha bytes are two planes addressed as a surface's are
// (blit_planes.h): byte b of region row j's texel x at row * Pb + unit * x + b, `unit` 4 for words and 1 for alpha.
// A surface's Pb is its plane 0's rows rounded down to 16, from each plane's offset rounded down to 16. A descriptor's Pb
// is its width's words rounded down to 16 (16 under four texels) for both planes, over a layout of its own: rows of
// words and rows of alpha each 16-byte aligned, slices 128-byte aligned, so a texel moves to its own place only where
// the two pitches meet. Bytes past a side's texels (row padding, past its last slice, past a surface's last page) read 0
// and take no write; a descriptor under four texels wide gives its row 0 alone as a source.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace gm20b {

// One plane of one side of such a copy, its bytes from the plane's first: a surface's `bound` of them (to its last page's
// end), or a descriptor's rows of `rowBytes` stored `stored` apart, `rows` a slice, slices `sliceBytes` apart, `slices`
// of them from the copy's
struct XrCopySide {
    bool surface = false;
    uint64_t bound = 0;
    uint32_t pitch = 0;  // Pb
    uint32_t unit = 1;
    uint32_t rowBytes = 0, stored = 0, rows = 0, slices = 0;
    uint64_t sliceBytes = 0;
    bool rowZeroOnly = false;
};

inline XrCopySide xrSurfaceSide(uint64_t bound, uint32_t pitch, bool words)
{
    XrCopySide s;
    s.surface = true;
    s.bound = bound;
    s.pitch = pitch;
    s.unit = words ? 4 : 1;
    return s;
}

inline XrCopySide xrDescriptorSide(uint32_t width, uint32_t height, uint32_t slices, bool words, bool source)
{
    XrCopySide s;
    const uint32_t wordRow = 4 * width;
    s.pitch = wordRow < 16 ? 16 : wordRow & ~15u;
    s.unit = words ? 4 : 1;
    s.rowBytes = s.unit * width;
    s.stored = (s.rowBytes + 15) & ~15u;
    s.rows = height;
    s.sliceBytes = (uint64_t(s.stored) * height + 127) & ~uint64_t(127);
    s.slices = slices;
    s.rowZeroOnly = source && wordRow < 16;
    return s;
}

// Byte `at` of a side: a surface's byte `at`, or a descriptor's texel (x, y) of slice z; `run` bytes on to where that
// stops being so (a texel row's end, the surface's bound; unmapped: to where bytes may be mapped again, ~0 never)
struct XrPlace {
    bool mapped = false;
    uint64_t at = 0;
    uint32_t x = 0, y = 0, z = 0;
    uint64_t run = 0;
};

inline XrPlace xrPlace(const XrCopySide &s, uint64_t at)
{
    constexpr uint64_t kNever = ~uint64_t(0);
    XrPlace p;
    p.at = at;
    if (s.surface) {
        p.mapped = at < s.bound;
        p.run = p.mapped ? s.bound - at : kNever;
        return p;
    }
    if (at >= uint64_t(s.slices) * s.sliceBytes) {
        p.run = kNever;
        return p;
    }
    const uint64_t in = at % s.sliceBytes, column = in % s.stored;
    p.z = uint32_t(at / s.sliceBytes);
    p.y = uint32_t(in / s.stored);
    if (p.y >= s.rows) {
        p.run = s.sliceBytes - in;
        return p;
    }
    if (column >= s.rowBytes) {
        p.run = s.stored - column;
        return p;
    }
    p.x = uint32_t(column / s.unit);
    p.run = s.rowBytes - column;
    p.mapped = !s.rowZeroOnly || (!p.y && !p.z);
    return p;
}

// One run of a copy row: `bytes` from `from` to `to`, each mapped or not
struct XrPiece {
    XrPlace from, to;
    uint32_t bytes = 0;
};

// The pieces of `bytes` bytes of one plane from byte `fromAt` of side `s` to byte `toAt` of side `d`, each a run both
// sides hold in one stretch: emit(piece) for each
template <typename Emit>
inline void xrCopyRow(const XrCopySide &s, uint64_t fromAt, const XrCopySide &d, uint64_t toAt, uint32_t bytes,
                      Emit &&emit)
{
    for (uint32_t k = 0; k < bytes;) {
        XrPiece piece{xrPlace(s, fromAt + k), xrPlace(d, toAt + k), 0};
        piece.bytes = uint32_t(std::min<uint64_t>({piece.from.run, piece.to.run, uint64_t(bytes - k)}));
        emit(piece);
        k += piece.bytes;
    }
}

// `rows` copy rows each moving `piece`'s shape, each next row on each side a row further: a surface's Pb on, a
// descriptor's next texel row
struct XrBlock {
    XrPiece piece;
    uint32_t rows = 0;
};

inline bool xrContinues(const XrBlock &b, const XrPiece &p, const XrCopySide &from, const XrCopySide &to)
{
    const auto next = [&](const XrPlace &was, const XrPlace &is, const XrCopySide &side) {
        if (was.mapped != is.mapped)
            return false;
        if (!is.mapped)
            return true;
        if (side.surface)
            return is.at == was.at + uint64_t(b.rows) * side.pitch;
        return is.x == was.x && is.z == was.z && is.y == was.y + b.rows;
    };
    return b.rows && p.bytes == b.piece.bytes && next(b.piece.from, p.from, from) && next(b.piece.to, p.to, to);
}

// One plane of a copy of `w` x `h` texels from (sx, sy) of side `s` to (dx, dy) of side `d`: emit(block) for each run of
// rows that move alike, one row's pieces each a block of their own where a row holds several
template <typename Emit>
inline void xrCopyBlocks(const XrCopySide &s, uint32_t sx, uint32_t sy, const XrCopySide &d, uint32_t dx, uint32_t dy,
                         uint32_t w, uint32_t h, Emit &&emit)
{
    std::vector<XrBlock> open, next;
    for (uint32_t j = 0; j < h; j++) {
        next.clear();
        xrCopyRow(s, uint64_t(sy + j) * s.pitch + uint64_t(s.unit) * sx, d,
                  uint64_t(dy + j) * d.pitch + uint64_t(d.unit) * dx, w * s.unit, [&](const XrPiece &p) {
                      next.push_back({p, 1});
                  });
        if (open.size() == 1 && next.size() == 1 && xrContinues(open[0], next[0].piece, s, d)) {
            open[0].rows++;
            continue;
        }
        for (const XrBlock &b : open)
            emit(b);
        open.swap(next);
    }
    for (const XrBlock &b : open)
        emit(b);
}

}  // namespace gm20b
