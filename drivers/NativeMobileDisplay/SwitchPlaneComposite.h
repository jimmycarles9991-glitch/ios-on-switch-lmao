// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window B's composite: the overlays DC A's memory admission refuses, drawn by VIC into one BGRA plane the size of
// their destinations' union on the screen, so the window fetches at most a screen of 32-bit pixels instead of each
// overlay's source (one 4:2:0 window at 1/6 on both axes is 5.6 GB/s; two with the UI pass the 1600 MHz EMC rate's
// isochronous ceiling, SwitchWindowCaps.h). The geometry here is plain logic; SwitchPlaneCompositeDisplay.h runs it.
#pragma once
#include "NativePlanes.h"
#include "SwitchDisplayComposition.h"
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../TegraVIC/SwitchVICScanoutCompose.h"

namespace switch_display {
struct CompositeRect {
    uint32_t x, y, w, h;
};

// The part of a layer on the screen, and the source rectangle (16.16, in its surface) that lands on it.
struct CompositePart {
    CompositeRect screen;
    uint64_t srcX, srcY, srcW, srcH;
};

inline bool compositeAxis(int32_t s, int32_t sn, int32_t d, int32_t dn, uint32_t screen, uint32_t &pos, uint32_t &size,
                          uint64_t &src, uint64_t &length)
{
    if (s < 0 || sn <= 0 || dn <= 0)
        return false;
    const int64_t d0 = d < 0 ? 0 : d;
    const int64_t end = int64_t(d) + dn, d1 = end > int64_t(screen) ? int64_t(screen) : end;
    if (d1 <= d0)
        return false;
    pos = uint32_t(d0);
    size = uint32_t(d1 - d0);
    const uint64_t first = uint64_t(s) << 16, whole = uint64_t(sn) << 16;
    src = first + uint64_t(d0 - d) * whole / uint64_t(dn);
    length = uint64_t(size) * whole / uint64_t(dn);
    if (length < (1u << 16))
        length = 1u << 16;
    if (src + length > first + whole)
        src = first + whole - length;
    return true;
}

inline bool compositePart(const native_planes::Layer &l, uint32_t width, uint32_t height, CompositePart &part)
{
    part = {};
    return compositeAxis(l.src.x, l.src.w, l.dst.x, l.dst.w, width, part.screen.x, part.screen.w, part.srcX,
                         part.srcW) &&
           compositeAxis(l.src.y, l.src.h, l.dst.y, l.dst.h, height, part.screen.y, part.screen.h, part.srcY,
                         part.srcH);
}

inline CompositeRect compositeUnion(const CompositePart *parts, unsigned count)
{
    uint32_t left = UINT32_MAX, top = UINT32_MAX, right = 0, bottom = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto &r = parts[i].screen;
        left = r.x < left ? r.x : left;
        top = r.y < top ? r.y : top;
        right = r.x + r.w > right ? r.x + r.w : right;
        bottom = r.y + r.h > bottom ? r.y + r.h : bottom;
    }
    return count ? CompositeRect{left, top, right - left, bottom - top} : CompositeRect{};
}

// What DC A does with a window of this layer's kind: Y'CbCr scans opaque, RGB blends premultiplied.
inline uint32_t compositeBlend(const native_planes::Layer &l)
{
    return native_planes::isYuv(l.kind) ? TEGRA_VIC_BLEND_OPAQUE : TEGRA_VIC_BLEND_PREMULTIPLIED;
}

// GENERIC_16Bx2 in blocks of 16 GOBs, DC A's BLOCK_HEIGHT(4) (window A's own scanout and the GPU's render targets).
constexpr uint8_t kCompositeBlockLog2 = 4;

// The composite's BGRA surface for a union: rows of whole GOBs, and whole blocks when block linear.
struct CompositeSurface {
    uint32_t width, height, pitch, rows;
    uint64_t bytes;
    bool block;
};

inline CompositeSurface compositeSurface(const CompositeRect &u, bool block)
{
    CompositeSurface s = {u.w, u.h, (u.w * 4 + 63) & ~63u, u.h, 0, block};
    const uint32_t blockRows = 8u << kCompositeBlockLog2;
    if (block)
        s.rows = (u.h + blockRows - 1) / blockRows * blockRows;
    s.bytes = (uint64_t(s.pitch) * s.rows + 4095) & ~4095ULL;
    return s;
}

inline TegraVICImage compositeImage(const CompositeSurface &s)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    im.width = s.width;
    im.height = s.height;
    im.pitch[0] = s.pitch;
    im.rows[0] = s.rows;
    im.span[0] = uint64_t(s.pitch) * s.rows;
    im.blockLinear = s.block;
    im.blockHeightLog2 = s.block ? kCompositeBlockLog2 : 0;
    return im;
}

inline tgm_layout compositeLayout(const CompositeSurface &s)
{
    tgm_layout l = {};
    l.width = s.width;
    l.height = s.height;
    l.bpp = 4;
    l.row_bytes = s.pitch;
    l.rows = s.rows;
    l.y_log2 = kCompositeBlockLog2;
    l.align = 512ULL << kCompositeBlockLog2;
    l.bytes = s.bytes;
    return l;
}

// The composite as a layer window B shows 1:1 over the union.
inline native_planes::Layer compositeLayer(const CompositeSurface &s, const CompositeRect &u)
{
    native_planes::Layer l;
    l.kind = native_planes::kKindBgra;
    l.surface.format = native_planes::fourcc('B', 'G', 'R', 'A');
    l.surface.width = s.width;
    l.surface.height = s.height;
    l.surface.stride[0] = s.pitch;
    l.surface.planeHeight[0] = s.rows;
    l.surface.allocation = l.surface.descriptorBytes = s.bytes;
    l.src = {0, 0, int32_t(s.width), int32_t(s.height)};
    l.dst = {int32_t(u.x), int32_t(u.y), int32_t(u.w), int32_t(u.h)};
    return l;
}

// What a composite was made from: each source frame and where it lies. Equal keys are the same picture.
struct CompositeSource {
    uint64_t allocation, generation, content;
    uint32_t cropX, cropY, format, matrix, chromaTop, chromaBottom;
    native_planes::Rect src, dst;
};

struct CompositeKey {
    unsigned count;
    CompositeSource source[2];
    CompositeRect target;
};

inline bool sameRect(const native_planes::Rect &a, const native_planes::Rect &b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

inline bool sameComposite(const CompositeKey &a, const CompositeKey &b)
{
    if (a.count != b.count || a.target.x != b.target.x || a.target.y != b.target.y || a.target.w != b.target.w ||
        a.target.h != b.target.h)
        return false;
    for (unsigned i = 0; i < a.count; ++i) {
        const auto &x = a.source[i], &y = b.source[i];
        if (x.allocation != y.allocation || x.generation != y.generation || x.content != y.content ||
            x.cropX != y.cropX || x.cropY != y.cropY || x.format != y.format || x.matrix != y.matrix ||
            x.chromaTop != y.chromaTop || x.chromaBottom != y.chromaBottom || !sameRect(x.src, y.src) ||
            !sameRect(x.dst, y.dst))
            return false;
    }
    return true;
}

// The overlays' fetch as a set admission refused it: equal demands are refused again without asking the EMC.
struct CompositeDemand {
    bool on[2];
    uint32_t position[2], size[2], prescaled[2], colorDepth[2];
};

inline CompositeDemand compositeDemand(const Overlay wanted[2])
{
    CompositeDemand d = {};
    for (unsigned i = 0; i < 2; ++i) {
        d.on[i] = wanted[i].on;
        if (!d.on[i])
            continue;
        d.position[i] = wanted[i].image.position;
        d.size[i] = wanted[i].image.size;
        d.prescaled[i] = wanted[i].image.prescaled;
        d.colorDepth[i] = wanted[i].image.colorDepth;
    }
    return d;
}

inline bool sameDemand(const CompositeDemand &a, const CompositeDemand &b)
{
    for (unsigned i = 0; i < 2; ++i)
        if (a.on[i] != b.on[i] || (a.on[i] && (a.position[i] != b.position[i] || a.size[i] != b.size[i] ||
                                               a.prescaled[i] != b.prescaled[i] || a.colorDepth[i] != b.colorDepth[i])))
            return false;
    return true;
}
}  // namespace switch_display
