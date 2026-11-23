// SPDX-License-Identifier: MIT
// A plain format (no YCbCr, packed 10-bit or RGB10A8_2P_XR10 one) over a surface plane whose rows overlap or lie under 16
// bytes, as an M4 Max makes, reads and writes it (the plain-rows probe, macOS 27.2.1, every texel size from 1 to 16 bytes):
// a GPU read takes each row at the plane's bytesPerRow rounded down to 16 (rows under 16 bytes 1 MiB apart), a GPU store
// lands rows at that pitch (rows under 16 bytes 16 apart: Texture::storePitch), a CPU copy at the plane's own rows, last
// row last. Reads go through the pool image the copy engine gathers the rows into (Texture::sampled); a pass renders that
// pool, then the pack's overlap state draws the rows from it, each byte from the row whose store lands last in an M4 Max's
// pass (overlap_order.h); a whole copy into texels of 8 or 16 bytes goes into the pool and the overlap state draws its rows
// in an M4 Max's copy order (copyOrder); a kernel's pool and any other blit go back in row order (an M4 Max's kernel
// stores race: three runs of one shape differ).
#pragma once
#include <cstdint>

#include "gpu.h"
#include "overlap_rows.h"

namespace gm20b {

// The format the overlap state reads the pool and writes the rows as: a texel of `bpp` bytes moved whole
inline uint32_t plainRowsFormat(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return 10;  // R8Unorm
    case 2:
        return 20;  // R16Unorm
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

inline bool plainRowsPooled(const Texture &t)
{
    return t.storeView != nullptr;
}

inline void fillPlainRows(CommandBuffer &rc, const Texture &t)
{
    BlitEncoder *b = rc.blit();
    b->copyTexels(t, 0, 0, 0, *t.sampled, 0, 0, 0, t.width, t.height);
    b->end();
}

// The overlap state's order of `t`'s rows as its GPU stores land them (`rows`: storedRows)
inline OverlapRows plainRowsOrder(const Texture &t, const Texture &rows)
{
    OverlapRows s = overlapRowsOf(rows.pitch, t.width * t.bpp, t.height, t.bpp, t.bpp, 1, 1);
    s.tiles = frameTiles(t);
    return s;
}

// The rows from the pool after a pass rendered it, a loaded pass's only in the frame tiles its draws reach (`loaded`); 0,
// or 1 a target the driver cannot render or no overlap state in the pack, 2 a draw the encoder refused (*why)
inline int storePlainRows(Device &d, CommandBuffer &rc, const Texture &t, const TurnMask *loaded, const char **why)
{
    Texture scratch;
    const Texture &rows = storedRows(t, t, scratch);
    if (!overlapsRows(rows)) {
        BlitEncoder *b = rc.blit();
        b->copyTexels(*t.sampled, 0, 0, 0, rows, 0, 0, 0, t.width, t.height);
        b->end();
        return 0;
    }
    const uint32_t format = t.storeView->format;
    Pipeline *state = d.ownPipeline(overlapState(format));
    if (!state) {
        *why = "the pack has no overlap state storing a plain texture whose rows overlap";
        return 1;
    }
    const OverlapRows order = plainRowsOrder(t, rows);
    return storeOverlapRows(rc, state, format, rows.va, rows, *t.storeView, nullptr, order, order.tiles, loaded, why);
}

// The rows from the pool after a whole copy into a texture of 8- or 16-byte texels put its texels there, as an M4 Max's
// copies store them (overlap_order.h copyOrder); as storePlainRows answers
inline int storeCopiedPlainRows(Device &d, CommandBuffer &rc, const Texture &t, const char **why)
{
    Texture scratch;
    const Texture &rows = storedRows(t, t, scratch);
    const uint32_t format = t.storeView->format;
    Pipeline *state = d.ownPipeline(overlapState(format));
    if (!state) {
        *why = "the pack has no overlap state storing a plain texture whose rows overlap";
        return 1;
    }
    OverlapRows order = plainRowsOrder(t, rows);
    order.copied = 1;
    return storeOverlapRows(rc, state, format, rows.va, rows, *t.storeView, nullptr, order, order.tiles, nullptr, why);
}

// Whether a copy of `w` x `h` texels at (x, y) into `t` stores its rows through the pool by copyOrder: the whole of a
// pooled texture of 8- or 16-byte texels whose stored rows overlap (smaller texels' copies race on an M4 Max and keep
// the copy engine's row order)
inline bool copiedThroughPool(const Texture &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    Texture scratch;
    return plainRowsPooled(t) && (t.bpp == 8 || t.bpp == 16) && !x && !y && w == t.width && h == t.height &&
           overlapsRows(storedRows(t, t, scratch));
}

}  // namespace gm20b
