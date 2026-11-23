// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 held in a surface's two planes (xr_pass.h) whose plane rows or offset are 16 bytes but not 32
// apart: such a plane is sampled through its pool image (Texture::sampled), so the texture's header pair reads the
// pools. Every write lands in the rows (passes through their shadow backings, blits, CPU copies) except a kernel's,
// which stores through the pool header: the copy engine fills each pool from its rows before work reads the pair, and
// drains a pool a kernel wrote back into its rows as the kernel's encoder ends (kernel_shadow.h).
// Planes whose rows overlap one another (a pitch below their row's bytes) are read as Metal reads them, each row at its
// pitch (an M4 Max over words of rows 64 and 16 for 32 texels reads every texel there), through pools the copy engine
// gathers. A pass renders into both pools, then each plane goes into its rows, each byte from the row whose store lands
// last in an M4 Max's pass (overlap_order.h, which its 550 passes follow too), a kernel's in row order (an M4 Max's
// kernel stores race: five runs of one shape differ).
#pragma once
#include <algorithm>

#include "gpu.h"
#include "overlap_rows.h"
#include "xr_pass.h"

namespace gm20b {

inline bool xrPlanesPooled(const Texture &t)
{
    return t.xrPlanes && ((t.luma && t.luma->sampled) || (t.chroma && t.chroma->sampled));
}

// Whether the rows of a texture's two planes, as its stores land them (storedRows), reach one another's bytes (words that
// run on into the alpha plane): an M4 Max's pass gives such a byte to whichever store lands later, taken across both planes
// (overlap_order.h overlapPlanesWriters). Both planes lie in one import, a surface's by its ID (Device::importSurface),
// or in the window both were moved into
inline bool xrPlanesCross(const Texture &w, const Texture &a)
{
    const uint64_t wordsEnd = w.va + uint64_t(w.pitch) * (w.height - 1) + uint64_t(w.width) * w.bpp;
    const uint64_t alphaEnd = a.va + uint64_t(a.pitch) * (a.height - 1) + uint64_t(a.width) * a.bpp;
    return !a.memoryless && w.va < alphaEnd && a.va < wordsEnd;
}

// Held in planes stored through both pools: stored at other rows than they are read (Texture::storePitch), or of rows that
// overlap, their own or one another's
inline bool xrPlanesOverlap(const Texture &t)
{
    if (!t.xrPlanes || !t.luma || !t.chroma || !t.luma->sampled)
        return false;
    return t.luma->storePitch || t.chroma->storePitch || overlapsRows(*t.luma) ||
           (!t.chroma->memoryless && (overlapsRows(*t.chroma) || xrPlanesCross(*t.luma, *t.chroma)));
}

// The overlap planes state's buffer for planes whose stored rows reach one another's bytes, their span from `lo` to `hi`;
// `cleared`: the pass drew nothing
inline OverlapPlanes crossingPlanesOrder(const Texture &t, bool cleared, uint64_t &lo, uint64_t &hi)
{
    const Texture w = storedRows(*t.luma, t), a = storedRows(*t.chroma, t);
    lo = std::min(w.va, a.va);
    hi = std::max(w.va + uint64_t(w.pitch) * (w.height - 1) + uint64_t(w.width) * w.bpp,
                  a.va + uint64_t(a.pitch) * (a.height - 1) + uint64_t(a.width) * a.bpp);
    OverlapPlanes s;
    s.pitch = w.pitch;
    s.plane[0] = meetPlane(uint32_t(w.va - lo), w.pitch, w.width * w.bpp, w.height, 4, 4);
    s.plane[1] = meetPlane(uint32_t(a.va - lo), a.pitch, a.width * a.bpp, a.height, 1, 1);
    s.tiles = frameTiles(t);
    s.cleared = cleared ? 1 : 0;
    return s;
}

// Planes whose rows reach one another's bytes, from their pools after work rendered them there: their span drawn a byte a
// texel by the pack's overlap planes state, rows `pitch` bytes long, then the bytes past them, in a clear's order where the
// pass drew nothing (`cleared`); 0, or 1 a target the driver cannot render, 2 a draw the encoder refused (*why)
inline int storeXrCrossingPlanes(Device &d, CommandBuffer &rc, const Texture &t, bool cleared, const char **why)
{
    Pipeline *state = d.ownPipeline(kOverlapPlanesState);
    if (!state) {
        *why = "the pack has no state storing RGB10A8_2P_XR10 planes whose rows reach one another's";
        return 1;
    }
    uint64_t lo = 0, hi = 0;
    const OverlapPlanes s = crossingPlanesOrder(t, cleared, lo, hi);
    return storePlanesSpan(rc, state, t, s, lo, hi, *t.luma->sampled, *t.chroma->sampled, *t.luma->sampled,
                           "RGB10A8_2P_XR10 planes whose rows reach one another's the driver cannot render", why);
}

// Planes that overlap from their pools after work rendered them there, each byte from the row whose store an M4 Max's
// pass lands last (pack overlap states), a plane whose rows do not overlap by the copy engine; 0, or 1 a target the raw
// path cannot render, 2 a draw the encoder refused (*why); `cleared`: the pass drew nothing
inline int storeXrPlaneRows(Device &d, CommandBuffer &rc, const Texture &t, bool cleared, const char **why)
{
    constexpr uint32_t kR8Unorm = 10;
    Texture words, alpha;
    if (xrPlanesCross(storedRows(*t.luma, t, words), storedRows(*t.chroma, t, alpha)))
        return storeXrCrossingPlanes(d, rc, t, cleared, why);
    for (const Texture *plane : {t.chroma, t.luma}) {
        if (!plane->sampled)
            continue;
        Texture scratch;
        const Texture &rows = storedRows(*plane, t, scratch);
        if (!overlapsRows(rows)) {
            BlitEncoder *b = rc.blit();
            b->copyTexels(*plane->sampled, 0, 0, 0, rows, 0, 0, 0, plane->width, plane->height);
            b->end();
            continue;
        }
        const bool alpha = plane == t.chroma;
        const uint32_t format = alpha ? kR8Unorm : kXrWordFormat;
        Pipeline *state = d.ownPipeline(overlapState(format));
        if (!state) {
            *why = "the pack has no overlap state storing an RGB10A8_2P_XR10 plane whose rows overlap";
            return 1;
        }
        // the r8 state draws unorm bytes: the pair's alpha header reads the pool as R8Unorm
        Texture source = *plane->sampled;
        if (alpha) {
            source.format = kR8Unorm;
            source.tic = t.tic + 1;
        }
        const OverlapRows order =
            overlapRowsOf(rows.pitch, plane->width * plane->bpp, plane->height, plane->bpp, plane->bpp, 1, 1);
        if (const int failed =
                storeOverlapRows(rc, state, format, rows.va, rows, source, nullptr, order, frameTiles(t), nullptr, why))
            return failed;
    }
    return 0;
}

// `fill`: each pool from its rows as they are read; else each pool back into its rows as stores land them (storedRows),
// the words last: where they reach the alpha plane an M4 Max's kernel stores leave the word (probes/h1/rows550.txt a1)
inline void moveXrPools(BlitEncoder &b, const Texture &t, bool fill)
{
    for (const Texture *plane : {t.chroma, t.luma})
        if (plane && plane->sampled) {
            Texture scratch;
            const Texture &rows = fill ? *plane : storedRows(*plane, t, scratch), &pool = *plane->sampled;
            b.copyTexels(fill ? rows : pool, 0, 0, 0, fill ? pool : rows, 0, 0, 0, plane->width, plane->height);
        }
}

inline void fillXrPools(CommandBuffer &rc, const Texture &t)
{
    if (!xrPlanesPooled(t))
        return;
    BlitEncoder *b = rc.blit();
    moveXrPools(*b, t, true);
    b->end();
}

}  // namespace gm20b
