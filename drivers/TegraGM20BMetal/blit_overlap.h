// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include "resources.h"
#include "transfer.h"

namespace gm20b {

// Past kBackingTags and kXrPlanesOverflowTag, right below kCopyClampTag (0xffffffffu).
constexpr uint32_t kCopyOverlapTag = 0xfffffffeu;

enum class CopyOverlap : uint8_t {
    None,
    Identity,
    Overlap,
};

enum class VolumeOverlap : uint8_t {
    None,
    Reverse,
    Forward,
    StageAll,
};

// z: a 3D texture's plane, or an array's layer (its own 2D image, layerBytes on: NVK's array_stride step)
inline CopySurface copySideOf(const Texture &t, uint32_t x, uint32_t y, uint32_t z)
{
    if (!t.blockLinear)
        return {t.va + (t.layers > 1 ? uint64_t(z) * t.layerBytes : 0) + uint64_t(y) * t.pitch + uint64_t(x) * t.bpp,
                t.pitch};
    if (t.import && t.import->kernelTiled())
        return {t.va, t.pitch, x + t.surfaceX, y + t.surfaceY, t.import->layout.height, true, t.blockHeight};
    if (t.layers > 1)
        return {t.va + z * t.layerBytes, t.pitch, x, y, t.height, true, t.blockHeight};
    return {t.va, t.pitch, x, y, t.height, true, t.blockHeight, t.depth, z, t.blockDepth};
}

inline uint64_t copySpanBytes(uint32_t pitch, uint32_t width, uint32_t height, uint32_t element)
{
    if (!width || !height || !element)
        return 0;
    const uint64_t rows = uint64_t(pitch) * (height - 1), last = uint64_t(width) * element;
    return last <= UINT64_MAX - rows ? rows + last : UINT64_MAX;
}

inline uint64_t blockLinearSliceBytes(const CopySurface &s)
{
    if (!s.pitch)
        return 0;
    const uint64_t rowBytes = (uint64_t(s.pitch) + 63ull) & ~63ull;
    const uint64_t blockRows = 8ull << s.blockHeight;
    const uint64_t rows = ((uint64_t(std::max(s.height, 1u)) + blockRows - 1) / blockRows) * blockRows;
    const uint64_t blockPlanes = 1ull << s.blockDepth;
    const uint64_t planes = ((uint64_t(std::max(s.depth, 1u)) + blockPlanes - 1) / blockPlanes) * blockPlanes;
    if (rows > UINT64_MAX / rowBytes)
        return UINT64_MAX;
    const uint64_t slice = rowBytes * rows;
    return planes <= UINT64_MAX / slice ? slice * planes : UINT64_MAX;
}

inline bool copyRangesOverlap(uint64_t a, uint64_t aBytes, uint64_t b, uint64_t bBytes)
{
    if (!aBytes || !bBytes)
        return false;
    const uint64_t aEnd = a <= UINT64_MAX - aBytes ? a + aBytes : UINT64_MAX;
    const uint64_t bEnd = b <= UINT64_MAX - bBytes ? b + bBytes : UINT64_MAX;
    return a < bEnd && b < aEnd;
}

// Different PTE kinds in one placement heap have separate VA views over the same physical range; same-kind placements
// share a single VA space and are already decided accurately by VA comparison.
inline bool arenaCrossKindOverlap(const ArenaStorage *a, const ArenaStorage *b)
{
    return a && b && a->arenaId && a->arenaId == b->arenaId && a->kind != b->kind &&
           copyRangesOverlap(a->offset, a->bytes(), b->offset, b->bytes());
}

inline bool rectsOverlap(uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by, uint32_t w, uint32_t h)
{
    const uint64_t aw = uint64_t(ax) + w, ah = uint64_t(ay) + h;
    const uint64_t bw = uint64_t(bx) + w, bh = uint64_t(by) + h;
    return ax < bw && bx < aw && ay < bh && by < ah;
}

inline bool sameBlockLinearLayout(const CopySurface &s, const CopySurface &d, uint32_t sElement, uint32_t dElement)
{
    return s.va == d.va && s.pitch == d.pitch && s.height == d.height && s.blockHeight == d.blockHeight &&
           s.depth == d.depth && s.blockDepth == d.blockDepth && sElement == dElement;
}

inline CopyOverlap copySideOverlap(const CopySurface &s, const CopySurface &d, uint32_t w, uint32_t h,
                                   uint32_t sElement, uint32_t dElement, bool sameComponents = true,
                                   const ArenaStorage *sArena = nullptr, const ArenaStorage *dArena = nullptr)
{
    if (!w || !h || !sElement || !dElement)
        return CopyOverlap::Identity;
    if (!s.blockLinear && !d.blockLinear) {
        if (sameComponents && sElement == dElement && s.va == d.va && (h <= 1 || s.pitch == d.pitch))
            return CopyOverlap::Identity;
        const uint64_t sSpan = copySpanBytes(s.pitch, w, h, sElement);
        const uint64_t dSpan = copySpanBytes(d.pitch, w, h, dElement);
        if (!copyRangesOverlap(s.va, sSpan, d.va, dSpan))
            return arenaCrossKindOverlap(sArena, dArena) ? CopyOverlap::Overlap : CopyOverlap::None;
        const uint64_t sRow = uint64_t(w) * sElement, dRow = uint64_t(w) * dElement;
        if (s.pitch == d.pitch && s.pitch && s.pitch >= sRow && d.pitch >= dRow) {
            const bool srcFirst = s.va <= d.va;
            const uint64_t diff = srcFirst ? d.va - s.va : s.va - d.va;
            const uint64_t dy = diff / s.pitch, dx = diff % s.pitch;
            const uint64_t loRow = srcFirst ? sRow : dRow, hiRow = srcFirst ? dRow : sRow;
            return (dy < h && dx < loRow) || (dy + 1 < h && dx + hiRow > s.pitch) ? CopyOverlap::Overlap
                                                                                  : CopyOverlap::None;
        }
        return CopyOverlap::Overlap;
    }
    if (s.blockLinear && d.blockLinear) {
        if (sameBlockLinearLayout(s, d, sElement, dElement)) {
            if (s.layer != d.layer)
                return CopyOverlap::None;
            if (sameComponents && s.x == d.x && s.y == d.y)
                return CopyOverlap::Identity;
            return rectsOverlap(s.x, s.y, d.x, d.y, w, h) ? CopyOverlap::Overlap : CopyOverlap::None;
        }
        return copyRangesOverlap(s.va, blockLinearSliceBytes(s), d.va, blockLinearSliceBytes(d)) ||
                       arenaCrossKindOverlap(sArena, dArena)
                   ? CopyOverlap::Overlap
                   : CopyOverlap::None;
    }
    const uint64_t sBytes = s.blockLinear ? blockLinearSliceBytes(s) : copySpanBytes(s.pitch, w, h, sElement);
    const uint64_t dBytes = d.blockLinear ? blockLinearSliceBytes(d) : copySpanBytes(d.pitch, w, h, dElement);
    return copyRangesOverlap(s.va, sBytes, d.va, dBytes) || arenaCrossKindOverlap(sArena, dArena) ? CopyOverlap::Overlap
                                                                                                  : CopyOverlap::None;
}

inline VolumeOverlap volumePlanesOverlap(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst,
                                         uint32_t dx, uint32_t dy, uint32_t dz, uint32_t w, uint32_t h, uint32_t planes,
                                         const ArenaStorage *sArena = nullptr, const ArenaStorage *dArena = nullptr)
{
    if (planes <= 1 || !w || !h)
        return VolumeOverlap::None;
    const CopySurface s0 = copySideOf(src, sx, sy, sz), d0 = copySideOf(dst, dx, dy, dz);
    if (s0.blockLinear && d0.blockLinear) {
        if (sameBlockLinearLayout(s0, d0, src.bpp, dst.bpp)) {
            if (sz == dz || !(uint64_t(sz) < uint64_t(dz) + planes && uint64_t(dz) < uint64_t(sz) + planes) ||
                !rectsOverlap(s0.x, s0.y, d0.x, d0.y, w, h))
                return VolumeOverlap::None;
            return dz > sz ? VolumeOverlap::Reverse : VolumeOverlap::Forward;
        }
        return copyRangesOverlap(s0.va, blockLinearSliceBytes(s0), d0.va, blockLinearSliceBytes(d0)) ||
                       arenaCrossKindOverlap(sArena, dArena)
                   ? VolumeOverlap::StageAll
                   : VolumeOverlap::None;
    }
    const uint64_t sBytes = s0.blockLinear ? blockLinearSliceBytes(s0) : copySpanBytes(s0.pitch, w, h, src.bpp);
    const uint64_t dBytes = d0.blockLinear ? blockLinearSliceBytes(d0) : copySpanBytes(d0.pitch, w, h, dst.bpp);
    return copyRangesOverlap(s0.va, sBytes, d0.va, dBytes) || arenaCrossKindOverlap(sArena, dArena)
               ? VolumeOverlap::StageAll
               : VolumeOverlap::None;
}

inline bool overlapStageLayout(uint32_t w, uint32_t h, uint32_t bpp, uint32_t planes, uint32_t &pitch,
                               uint64_t &planeBytes, uint64_t &totalBytes)
{
    if (!w || !h || !bpp || !planes)
        return false;
    const uint64_t rawRow = uint64_t(w) * bpp;
    if (rawRow > UINT32_MAX - 255ull)
        return false;
    const uint64_t aligned = (rawRow + 255ull) & ~255ull;
    if (aligned > UINT32_MAX || h > UINT64_MAX / aligned)
        return false;
    const uint64_t perPlane = aligned * h;
    if (planes > UINT64_MAX / perPlane)
        return false;
    pitch = uint32_t(aligned);
    planeBytes = perPlane;
    totalBytes = perPlane * planes;
    return true;
}

inline Texture makeOverlapStage(const Texture &backing, uint64_t offset, uint32_t w, uint32_t h, uint32_t bpp,
                                uint32_t pitch)
{
    Texture t;
    t.va = backing.va + offset;
    t.storage = backing.storage;
    t.width = t.texelWidth = w;
    t.height = t.texelHeight = h;
    t.pitch = pitch;
    t.bpp = uint8_t(bpp);
    t.layers = 1;
    t.depth = 1;
    t.samples = 1;
    return t;
}

}  // namespace gm20b
