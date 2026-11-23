// SPDX-License-Identifier: MIT
// What Metal's blits move of a texture held through a surface's planes (m2v copy_plan.rs, blit_plan.rs, two_planes.rs;
// EVs XRR2-ycc2-m4, TXN2-ycr2-m4 and XRR2-xrs1-m4 on an Apple M4 under macOS 27.0.1; an M4 Max under macOS 27.2.1,
// validation off): plane 0's bytes, planeUnit of them a texel, which a blit with a buffer moves, and a blit with a
// texture of another format counts in the destination's texel (into the planes: only when the two match). A blit
// between two such textures moves plane 0 so and plane 1 a byte a texel of the region at plane 0's rows, through plane
// 1's end into the surface's bytes after it (planeOneRows). Counted in a texel wider than plane 0's unit, a region runs
// along plane 0 as one run of bytes, texel x of row y at y * pitch + x * texel, through each row's padding into the
// next rows and past plane 0 into the surface's bytes after it, up to the end of its last page, past which an M4 Max
// reads 0 (planeReach; m2v-blits-fix probes/bound.mm, macOS 27.2.1).
#pragma once
#include <algorithm>
#include <cstdint>
#include "gpu.h"

namespace gm20b {

// A one-plane 4:4:4 YCbCr texture (504, 524): luma and chroma both read its one plane of words (ycbcr.h
// chromaGreenBlue), as RGB10A2Unorm
inline bool planeOfWords(const Texture &t)
{
    constexpr uint32_t kRGB10A2Unorm = 90;
    return t.ycbcr && !t.onePlane && t.luma && t.luma->format == kRGB10A2Unorm;
}

// The bytes of a texel of plane 0 that Metal's blits with a buffer, and with a texture of plane 0's texel size, count:
// a code's (1 or 2) for two-plane YCbCr and 4:4:4 codes, the word's (4) for RGB10A8_2P_XR10 and one-plane 4:4:4, one
// for packed 10-bit planes, two for one-plane 4:2:2; 0: not held through planes
inline uint32_t planeUnit(const Texture &t)
{
    constexpr uint32_t kR16Unorm = 20;
    if (t.xrPlanes || planeOfWords(t))
        return 4;
    if (t.onePlane)
        return 2;
    if (t.planar)
        return 1;
    if (t.ycbcr && t.luma)
        return t.luma->format == kR16Unorm ? 2 : 1;
    return 0;
}

// The bytes from `plane`'s first a job may name of `bytes`: a sparse window's bound pages alone (Texture::window), as the
// kernel refuses a use no one binding holds
inline uint64_t windowBound(const Texture &plane, uint64_t bytes)
{
    return plane.window.bytes ? std::min(bytes, plane.window.va + plane.window.pages - plane.va) : bytes;
}

// The format of a texel of 1, 2, 4, 8 or 16 bytes, by its log2: R8Unorm, R16Uint, R32Uint, RG32Uint, RGBA32Uint
constexpr uint32_t kUnitFormats[5] = {10, 23, 53, 103, 123};

// Plane 0 of `t`'s rows as texels of `unit` bytes (1, 2, 4, 8 or 16), pitch memory, or the texels of RGB10A8_2P_XR10's
// words over a surface the kernel laid out block-linear, in that layout: what Metal's blits of a texture held through
// planes move. `store`: its rows as a blit's writes land them (Texture::storePitch: an M4 Max reads rows under 16 bytes 1
// MiB apart and writes them 16 apart, m2v-textures12 probes/f2/copies-full.txt). False: no such plane
inline bool planeBytes(const Texture &t, uint32_t unit, Texture &out, bool store = false)
{
    const Texture *luma = t.xrPlanes || (t.ycbcr && !t.onePlane) ? t.luma : nullptr;
    const uint64_t va = luma ? luma->va : t.planes.luma;
    const uint32_t pitch = luma ? (store && luma->storePitch ? luma->storePitch : luma->pitch) : t.planes.lumaPitch,
                   rows = luma ? luma->height : t.height;
    Import *import = luma ? luma->import : t.import;
    const uint32_t log2 = unit ? uint32_t(__builtin_ctz(unit)) : 5;
    if (!va || !pitch || !rows || !import || log2 > 4 || unit != 1u << log2 || pitch < unit)
        return false;
    if (luma && luma->blockLinear) {
        if (!luma->width || (luma->width * luma->bpp) % unit || (luma->surfaceX * luma->bpp) % unit)
            return false;
        out = Texture();
        out.import = import;
        out.va = va;
        out.pitch = pitch;
        out.bpp = unit;
        out.format = kUnitFormats[log2];
        out.width = out.texelWidth = luma->width * luma->bpp / unit;
        out.height = out.texelHeight = rows;
        out.blockLinear = true;
        out.blockHeight = luma->blockHeight;
        out.layerBytes = luma->layerBytes;
        out.surfaceX = luma->surfaceX * luma->bpp / unit;
        out.surfaceY = luma->surfaceY;
        out.storage = luma->storage;
        return true;
    }
    out = Texture();
    out.import = import;
    out.va = va;
    out.pitch = pitch;
    out.bpp = unit;
    out.format = kUnitFormats[log2];
    // rows that overlap (a pitch below the plane's row) run each row to its own end, through the next rows' bytes
    out.width = out.texelWidth = std::max(pitch, luma ? luma->width * luma->bpp : 0u) / unit;
    out.height = out.texelHeight = rows;
    out.window = t.window;
    out.storage = {va, windowBound(out, uint64_t(pitch) * (rows - 1) + uint64_t(out.width) * unit), !import->id};
    return true;
}

// The bytes from plane 0's first that a blit of `w` x `h` of `plane`'s texels from (x, y) reaches (planeBytes' plane)
inline uint64_t planeReach(const Texture &plane, uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    return w && h ? (y + h - 1) * plane.pitch + (x + w) * plane.bpp : 0;
}

// The bytes from plane 0's first to the end of its surface's last page, as its import binds them
inline uint64_t planeReadable(const Texture &plane)
{
    if (plane.window.bytes)
        return plane.window.va + plane.window.pages - plane.va;
    const Bo &bo = plane.import->bo;
    return (bo.vaBase ? bo.vaBase : bo.va) + bo.bytes - plane.va;
}

// Whether a blit of `w` x `h` of `plane`'s texels from (x, y) reads its surface's bytes alone: a block-linear plane's
// region inside its texels, a pitch plane's run up to the end of its surface's last page
inline bool planeRegionReadable(const Texture &plane, uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    if (plane.blockLinear)
        return x + w <= plane.width && y + h <= plane.height;
    return planeReach(plane, x, y, w, h) <= planeReadable(plane);
}

// planeBytes, its storage what a blit of `w` x `h` of its texels from (x, y) reads of the surface (planeReach)
inline bool planeBytesRead(const Texture &t, uint32_t unit, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           Texture &out)
{
    if (!planeBytes(t, unit, out))
        return false;
    if (!out.blockLinear)
        out.storage.bytes = windowBound(out, std::max(out.storage.bytes, planeReach(out, x, y, w, h)));
    return true;
}

// Plane 1 of `t` (its chroma, its alpha bytes, a planar texture's second plane) as a blit between two textures held
// through planes moves it (an M4 Max under macOS 27.2.1, validation off, formats 550, 551, 500, 502, 503, 505, 508 and
// 542: m2v-textures12 probes/f1.txt): its bytes from its first, a byte a texel of the region, rows `pitch` apart (plane
// 0's, as planeBytes has them), running on through the plane's end into the surface's bytes after it, up to the end of
// its last page; false: no plane 1 (a one-plane format's, a plane alone's alpha): such a blit from it leaves the
// destination's plane 1 as it is, and into it writes nothing (an M4 Max: hwtests8 mac/e504/OUTPUT.txt)
inline bool planeOneRows(const Texture &t, uint32_t pitch, Texture &out)
{
    const Texture *c = (t.xrPlanes || (t.ycbcr && !t.onePlane && t.luma && !planeOfWords(t))) ? t.chroma : nullptr;
    const uint64_t va = c ? c->va : t.planes.chroma;
    Import *import = c ? c->import : t.import;
    if ((!c && !t.planar) || !va || !pitch || !import || (c && (c->memoryless || c->blockLinear)))
        return false;
    out = Texture();
    out.import = import;
    out.va = va;
    out.pitch = pitch;
    out.bpp = 1;
    out.format = 10;  // R8Unorm
    out.window = t.window;
    const uint64_t readable = planeReadable(out);
    out.width = out.texelWidth = pitch;
    out.height = out.texelHeight = uint32_t(std::min<uint64_t>(readable / pitch, UINT32_MAX));
    out.storage = {va, readable, !import->id};
    return true;
}

// Rows [row, row + rows) of a plane-1 copy (planeOneRuns): each moves `copied` bytes from the source and sets the
// `zeroed` bytes after them to 0
struct PlaneOneRun {
    uint32_t row, rows, copied, zeroed;
};

// The runs of a `w` x `h` plane-1 copy from (sx, sy) of rows `sPitch` apart, `readable` bytes from the source's first
// (0: a source with no plane 1), into (dx, dy) of rows `dPitch` apart, `writable` bytes from the destination's first:
// an M4 Max reads 0 past the source's last page and lands nothing past the destination's (probes/f1.txt A7). Rows
// wholly on one side of each end go in one run, a row an end cuts in a run of its own; emit(run) for each run that
// writes
template <typename Emit>
inline void planeOneRuns(uint64_t readable, uint64_t sPitch, uint32_t sx, uint32_t sy, uint64_t writable,
                         uint64_t dPitch, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, Emit &&emit)
{
    // of the region's rows from `y`, how many run wholly inside `limit`, and the first that starts past it
    const auto bounds = [w, h](uint64_t limit, uint64_t pitch, uint64_t x, uint64_t y, uint32_t &whole,
                               uint32_t &none) {
        const uint64_t wholeRows = limit >= x + w ? (limit - x - w) / pitch + 1 : 0;
        const uint64_t firstNone = limit > x ? (limit - x + pitch - 1) / pitch : 0;
        whole = uint32_t(std::min<uint64_t>(wholeRows > y ? wholeRows - y : 0, h));
        none = uint32_t(std::min<uint64_t>(firstNone > y ? firstNone - y : 0, h));
    };
    const auto part = [w](uint64_t limit, uint64_t pitch, uint64_t x, uint64_t row) {
        const uint64_t start = row * pitch + x;
        return start >= limit ? 0u : uint32_t(std::min<uint64_t>(limit - start, w));
    };
    uint32_t sWhole, sNone, dWhole, dNone;
    bounds(readable, sPitch, sx, sy, sWhole, sNone);
    bounds(writable, dPitch, dx, dy, dWhole, dNone);
    for (uint32_t k = 0; k < dNone;) {
        if (k < std::min(sWhole, dWhole)) {
            const uint32_t end = std::min(sWhole, dWhole);
            emit(PlaneOneRun{k, end - k, w, 0});
            k = end;
        } else if (k >= sNone && k < dWhole) {
            emit(PlaneOneRun{k, dWhole - k, 0, w});
            k = dWhole;
        } else {
            const uint32_t written = part(writable, dPitch, dx, uint64_t(dy) + k);
            const uint32_t copied = std::min(written, part(readable, sPitch, sx, uint64_t(sy) + k));
            emit(PlaneOneRun{k, 1, copied, written - copied});
            k++;
        }
    }
}

// A plain texture's level `image` read in texels of `unit` bytes, as Metal's blit into a texture held through planes of
// another unit reads it (an M4 Max with validation off reads a texture over a buffer's rows so, `unit` bytes a texel,
// and its own textures' bytes in their own layout: m2v-textures11 probes/h5/copies.txt): each row's bytes in its
// layout, pitch or block-linear; false: a row of no whole units, or a crop of the kernel's layout off them
inline bool imageInUnits(const Texture &image, uint32_t unit, Texture &out)
{
    const uint32_t log2 = unit ? uint32_t(__builtin_ctz(unit)) : 5;
    const uint32_t row = image.width * image.bpp;
    if (log2 > 4 || unit != 1u << log2 || !row ||
        (image.blockLinear && (row % unit || (image.surfaceX * image.bpp) % unit)))
        return false;
    out = image;
    out.bpp = unit;
    out.format = kUnitFormats[log2];
    out.width = out.texelWidth = (image.blockLinear ? row : std::max(image.pitch, row)) / unit;
    out.surfaceX = image.surfaceX * image.bpp / unit;
    return true;
}

// Whether a blit of `w` x `h` of `image`'s texels (imageInUnits) from (x, y) reads its own bytes alone: a block-linear
// image's region inside its texels, a pitch image's run of rows inside the `bytes` its memory holds from its first
inline bool unitRegionReadable(const Texture &image, uint64_t bytes, uint64_t x, uint64_t y, uint64_t w, uint64_t h)
{
    if (image.blockLinear)
        return x + w <= image.width && y + h <= image.height;
    return planeReach(image, x, y, w, h) <= bytes;
}

// Whether a blit moves `t`'s planes: one held through a surface's planes or through its two extended-range planes
inline bool heldInPlanes(const Texture &t)
{
    return t.converted() || t.xrPlanes;
}

}  // namespace gm20b
