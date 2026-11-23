// SPDX-License-Identifier: MIT
// A YCbCr target whose plane rows overlap (a pitch below its row): the store states draw the rows into a pool image of their
// own (the plane's Texture::sampled, or a one-plane target's luma and chroma images), then the pack's overlap state draws the
// plane's bytes from it in one pass, each byte from the row whose store lands last (overlap_order.h).
#pragma once

#include <algorithm>
#include <cstdint>

#include "gpu.h"
#include "overlap_order.h"
#include "reach.h"
#include "ycbcr_store.h"

namespace gm20b {

// The bytes from `va` on that a job of `t`'s planes may name: past its window's bound pages none (Texture::window; the GMMU
// reads 0 there and drops stores), as the kernel refuses a use no one binding holds
inline uint64_t boundFrom(const Texture &t, uint64_t va, uint64_t bytes)
{
    return t.window.pages ? std::min(bytes, t.window.va + t.window.pages - std::min(va, t.window.va + t.window.pages))
                          : bytes;
}

// A plane's rows as its GPU stores land them: at Texture::storePitch where it has one (rows under 16 bytes, which an M4
// Max stores 16 bytes apart and reads 1 MiB apart: m2v-textures11 probes/h1/rows550.txt, m2v-textures12
// probes/f2/targets-full.txt), else as it reads them; its storage the bytes those rows reach
inline Texture storedRows(const Texture &plane, const Texture &t)
{
    Texture rows = plane;
    if (plane.storePitch) {
        rows.pitch = plane.storePitch;
        rows.storage.va = plane.va;
        rows.storage.bytes =
            boundFrom(t, plane.va, uint64_t(rows.pitch) * (rows.height - 1) + uint64_t(rows.width) * rows.bpp);
        // a target as NVK's linear-render rule takes it (overlapTarget)
        rows.renderable = !((rows.va | rows.pitch) & 127);
        rows.shadowTarget = !rows.renderable;
    }
    return rows;
}

// storedRows, `scratch` holding the view where stores land at other rows than the plane's own, else the plane itself
inline const Texture &storedRows(const Texture &plane, const Texture &t, Texture &scratch)
{
    if (!plane.storePitch)
        return plane;
    scratch = storedRows(plane, t);
    return scratch;
}

// Each luma texel's frame texel of target `t`, a turned one's in its frame (overlap_order.h frameOrder)
inline TileMap frameTiles(const Texture &t)
{
    return tileMap(frameToPlane(frameOf(t)), 0, 0);
}

// Whether a plane's rows overlap: its pitch holds fewer bytes than a row
inline bool overlapsRows(const Texture &plane)
{
    return plane.pitch < uint32_t(plane.width) * plane.bpp;
}

// A two-plane target's plane (luma, or chroma blocks of the store's halving), a packed one's words of three codes, or a
// one-plane 4:4:4 target's RGB10A2Unorm words
inline OverlapRows overlapPlaneRows(const Texture &plane, bool chroma, const YcbcrStoreShape &store)
{
    uint32_t texelBytes = plane.bpp, across = chroma && store.across ? 2 : 1;
    if (store.packed) {
        texelBytes = chroma && !store.across ? 8 : 4;
        across = 3;
    }
    return overlapRowsOf(plane.pitch, uint32_t(plane.width) * plane.bpp, plane.height, plane.bpp, texelBytes, across,
                         chroma && store.down ? 2 : 1);
}

// A one-plane 4:2:2 target's pair bytes (Texture::onePlane), drawn a byte a texel from its luma and chroma images: a texel
// two bytes across, an odd width's last Y1 kept (an M4 Max writes it with no store: m2v-textures6 probes/onep.mm)
inline OverlapRows overlapPairsOf(const Texture &t)
{
    const uint32_t pairs = t.chroma->width;
    OverlapRows s = overlapRowsOf(t.planes.lumaPitch, 4 * pairs, t.height, 1, 2, 1, 1);
    s.pairs = t.pairsReversed ? 2 : 1;
    if (t.width & 1)
        s.kept = 4 * (pairs - 1) + (t.pairsReversed ? 3 : 2);
    return s;
}

// The plane bytes from `va`, `height` rows of `width` texels `pitch` apart, as a colour target of `format`: rendered in place
// where NVK's linear-render rule takes it, else through a backing the pass fills and drains (Texture::shadowTarget)
inline void overlapTarget(const Texture &plane, uint64_t va, uint32_t pitch, uint32_t width, uint32_t height,
                          uint32_t format, Texture &out)
{
    out = {};
    shape(width, height, format, out);
    out.va = va;
    out.pitch = pitch;
    out.import = plane.import;
    out.storage = {va, uint64_t(pitch) * (height - 1) + uint64_t(width) * out.bpp, !plane.import->id};
    const bool linear = !((va | pitch) & 127);
    out.shadowTarget = !linear;
    out.renderable = linear;
}

// The plane's bytes from `planeVa` drawn from `source` by `state`: the run of its whole pitches, then the bytes past it.
// `chroma`: a one-plane target's chroma image (s.pairs); `frame`: each luma texel's frame texel (frameTiles); `loaded`: a
// loaded pass's reach and each luma texel's frame tile. 0, or 1 a target the driver cannot render, 2 a draw the encoder
// refused (*why)
inline int storeOverlapRows(CommandBuffer &rc, Pipeline *state, uint32_t format, uint64_t planeVa, const Texture &plane,
                            const Texture &source, const Texture *chroma, OverlapRows s, const TileMap &frame,
                            const TurnMask *loaded, const char **why)
{
    const uint32_t span = s.pitch * (s.rows - 1) + s.row, whole = span / s.pitch, tail = span - whole * s.pitch;
    s.tiles = frame;
    if (loaded) {
        s.masked = 1;
        s.tiles = loaded->tiles;
    }
    // a byte no row stores keeps its value: the target is loaded (a shadow's backing filled first)
    const bool keeps = s.masked || s.kept != ~0u;
    for (const bool part : {false, true}) {
        const uint32_t width = (part ? tail : s.pitch) / s.element, height = part ? 1 : whole;
        if (!width)
            continue;
        s.base = part ? whole * s.pitch : 0;
        Texture target;
        overlapTarget(plane, planeVa + s.base, s.pitch, width, height, format, target);
        target.storage.bytes = boundFrom(plane, target.va, target.storage.bytes);
        PassDesc p;
        p.color[0].texture = &target;
        p.color[0].load = keeps ? ColorAttachment::Load : ColorAttachment::DontCare;
        p.color[0].store = true;
        p.count = 1;
        p.width = width;
        p.height = height;
        RenderEncoder *e = rc.render(p);
        if (!e) {
            *why = "a YCbCr plane whose rows overlap the driver cannot render";
            return 1;
        }
        e->setPipeline(state);
        e->setTexture(0, &source);
        e->setTexture(1, loaded ? loaded->mask : &source);
        e->setTexture(2, chroma ? chroma : &source);
        e->setBytes(2, 0, &s, sizeof s);
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return 2;
        e->end();
    }
    return 0;
}

// Two planes' span from `lo` to `hi` of target `t`'s planes, drawn a byte a texel (R8Uint) by `state` from the planes'
// pools `p0` and `p1` as `s` orders their stores (overlap_order.h OverlapPlanes; `reach`: a loaded pass's tile mask):
// rows `s.pitch` bytes long, then the bytes past them, a byte no store writes keeping its value. 0, or 1 a target the raw
// path cannot render (*why `cannot`), 2 a draw the encoder refused (*why)
inline int storePlanesSpan(CommandBuffer &rc, Pipeline *state, const Texture &t, OverlapPlanes s, uint64_t lo,
                           uint64_t hi, const Texture &p0, const Texture &p1, const Texture &reach, const char *cannot,
                           const char **why)
{
    constexpr uint32_t kR8Uint = 13;
    const uint32_t span = uint32_t(hi - lo), whole = span / s.pitch, tail = span - whole * s.pitch;
    for (const bool part : {false, true}) {
        const uint32_t width = part ? tail : s.pitch, height = part ? 1 : whole;
        if (!width || !height)
            continue;
        s.base = part ? whole * s.pitch : 0;
        Texture target;
        overlapTarget(*t.luma, lo + s.base, s.pitch, width, height, kR8Uint, target);
        target.storage.bytes = boundFrom(t, target.va, target.storage.bytes);
        PassDesc p;
        p.color[0].texture = &target;
        p.color[0].load = ColorAttachment::Load;
        p.color[0].store = true;
        p.count = 1;
        p.width = width;
        p.height = height;
        RenderEncoder *e = rc.render(p);
        if (!e) {
            *why = cannot;
            return 1;
        }
        e->setPipeline(state);
        e->setTexture(0, &p0);
        e->setTexture(1, &p1);
        e->setTexture(2, &reach);
        e->setBytes(2, 0, &s, sizeof s);
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return 2;
        e->end();
    }
    return 0;
}

}  // namespace gm20b
