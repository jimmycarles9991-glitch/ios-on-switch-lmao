// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window probes for what windows B and C scan (SwitchWindowCaps.h), on DisplayWindowProbe.h's run; included at its end.
//  formats (L979, L955) NV12 at COLOR_DEPTH 43 (42 the control), NV24 at 49, '2vuy' at 16 and 'yuvs' at 16 with
//          BYTE_SWAP SWAP2 (without it the control), against BGRA. Window A shows the top half through a CSC that maps
//          luma alone onto black or white (YOF -128, KYRGB 3.99), window B the bottom half through one that maps Cb
//          alone onto red and Cr alone onto blue (KUR = KVB = 3.0, every other term 0). Samples are 16/235 and 0/255,
//          so every output channel saturates and the CRC is exact whatever the CSC's rounding or a video-range clamp;
//          chroma is one value over the bottom half for the subsampled formats, so no chroma filter changes it, and
//          per pixel for NV24, which has none.
//  wide    (L958) window B's V filter on lines 3840 and 4096 wide (pitch, and NVDEC's 2-GOB blocks), 2160 lines onto
//          405: the picture changes only across, so every V filter phase gives back its line and the scan must equal the
//          same source with the V filter off; a line buffer narrower than the source would show.
//  palette (L1004) windows A and B with CP_ENABLE and a per-channel palette (red inverted, green as is, blue squared)
//          over random BGRA, against the picture mapped in memory.
// Each step's CSC, BYTE_SWAP and palette words are Step::extra, put back as DisplayWindowProbeExtras.h says.
#pragma once

#include "NativePlanes.h"

namespace display_window_probe {

constexpr Probe kWindowFormats = static_cast<Probe>(4), kWindowWide = static_cast<Probe>(5),
                kWindowPalette = static_cast<Probe>(6);
constexpr uint32_t kHalf = kPanelHeight / 2;

enum class Picture : uint32_t {
    Nv12,
    Nv24,
    Uyvy,
    Yuyv,
    Reference,      // BGRA: the top half's luma as black or white, the bottom half red
    Reference444,   // the same with the bottom half's per-pixel Cb in red and Cr in blue
    Columns,        // NV12 that changes only across
    Palette,        // random BGRA
    PaletteMapped,  // Palette through kPalette's channels
};

inline bool lumaBit(uint32_t x, uint32_t y)
{
    return (mix(x, y, 11) >> 7 & 1) != 0;
}

inline bool cbBit(uint32_t x, uint32_t y)
{
    return (mix(x, y, 12) >> 9 & 1) != 0;
}

inline bool crBit(uint32_t x, uint32_t y)
{
    return (mix(x, y, 13) >> 11 & 1) != 0;
}

inline uint8_t lumaSample(uint32_t x, uint32_t y)
{
    return y < kHalf && lumaBit(x, y) ? 235 : y < kHalf ? 16 : 128;
}

// Cb | Cr << 8 at pixel (x, y): neutral in the top half, Cb high and Cr low below it, or per pixel for NV24.
inline uint16_t chromaSample(uint32_t x, uint32_t y, bool perPixel)
{
    if (y < kHalf)
        return 128 | 128 << 8;
    if (!perPixel)
        return 255;
    return static_cast<uint16_t>((cbBit(x, y) ? 255 : 0) | (crBit(x, y) ? 255 : 0) << 8);
}

inline uint32_t referencePixel(uint32_t x, uint32_t y, bool perPixel)
{
    if (y < kHalf)
        return lumaBit(x, y) ? 0xffffffffu : 0xff000000u;
    const uint16_t c = chromaSample(x, y, perPixel);
    return 0xff000000u | ((c & 0xff) == 255 ? 0xff0000u : 0) | ((c >> 8) == 255 ? 0xffu : 0);
}

// The palette's three channels: entry i is R | G << 8 | B << 16 (L4T t21x/lut.c tegra_dc_loop_lut).
constexpr uint32_t paletteWord(uint32_t i)
{
    return (255 - i) | i << 8 | (i * i / 255) << 16;
}

inline uint32_t paletteMapped(uint32_t bgra)
{
    const uint32_t b = bgra & 0xff, g = bgra >> 8 & 0xff, r = bgra >> 16 & 0xff;
    return (bgra & 0xff000000u) | (paletteWord(r) & 0xff) << 16 | (paletteWord(g) >> 8 & 0xff) << 8 |
           (paletteWord(b) >> 16 & 0xff);
}

inline uint8_t columnLuma(uint32_t x)
{
    return static_cast<uint8_t>(32 + (mix(x / 3, 0, 14) >> 24) % 192);
}

inline uint16_t columnChroma(uint32_t cx)
{
    return static_cast<uint16_t>(mix(cx, 0, 15) >> 8);
}

inline uint32_t lumaBytesPerPixel(const Surface &s)
{
    const auto p = static_cast<Picture>(s.tag);
    return p == Picture::Uyvy || p == Picture::Yuyv ? 2 : s.nv12 ? 1 : 4;
}

inline uint8_t pictureByte(const Surface &s, bool chromaPlane, uint32_t xB, uint32_t y)
{
    const auto picture = static_cast<Picture>(s.tag);
    switch (picture) {
    case Picture::Nv12:
        return chromaPlane ? static_cast<uint8_t>(chromaSample(xB / 2, 2 * y, false) >> (8 * (xB % 2)))
                           : lumaSample(xB, y);
    case Picture::Nv24:
        return chromaPlane ? static_cast<uint8_t>(chromaSample(xB / 2, y, true) >> (8 * (xB % 2))) : lumaSample(xB, y);
    case Picture::Uyvy:
    case Picture::Yuyv: {
        const uint32_t pair = xB / 4, at = xB % 4;
        const bool lumaAt = picture == Picture::Uyvy ? (at & 1) != 0 : (at & 1) == 0;
        if (lumaAt)
            return lumaSample(2 * pair + (at >= 2 ? 1 : 0), y);
        const bool cb = picture == Picture::Uyvy ? at == 0 : at == 1;
        return static_cast<uint8_t>(chromaSample(2 * pair, y, false) >> (cb ? 0 : 8));
    }
    case Picture::Reference:
    case Picture::Reference444:
        return static_cast<uint8_t>(referencePixel(xB / 4, y, picture == Picture::Reference444) >> (8 * (xB % 4)));
    case Picture::Columns:
        return chromaPlane ? static_cast<uint8_t>(columnChroma(xB / 2) >> (8 * (xB % 2))) : columnLuma(xB);
    case Picture::Palette:
    case Picture::PaletteMapped: {
        const uint32_t pixel = 0xff000000u | mix(xB / 4, y, 16) >> 8;
        const uint32_t shown = picture == Picture::PaletteMapped ? paletteMapped(pixel) : pixel;
        return static_cast<uint8_t>(shown >> (8 * (xB % 4)));
    }
    }
    return 0;
}

inline void fillPicture(uint8_t *buffer, const Surface &s)
{
    fillPlane(buffer + s.offset, s, s.lumaRows, [&](uint32_t xB, uint32_t y) -> uint8_t {
        return xB < s.width * lumaBytesPerPixel(s) && y < s.height ? pictureByte(s, false, xB, y) : 0;
    });
    if (s.nv12)
        fillPlane(buffer + s.chromaOffset, s, s.chromaRows, [&](uint32_t xB, uint32_t y) -> uint8_t {
            return pictureByte(s, true, xB, y);
        });
}

inline Surface picture(Picture p, uint32_t width, uint32_t height, uint8_t yLog2 = kPitch)
{
    Surface s;
    s.fillWith = fillPicture;
    s.tag = static_cast<uint32_t>(p);
    s.width = width;
    s.height = height;
    s.yLog2 = yLog2;
    s.nv12 = p == Picture::Nv12 || p == Picture::Nv24 || p == Picture::Columns;
    const uint32_t rowPixels = p == Picture::Nv24 ? 2 * width : width * lumaBytesPerPixel(s);
    s.rowBytes = static_cast<uint32_t>(roundUp(rowPixels, 64));
    s.lumaRows = static_cast<uint32_t>(roundUp(height, blockRows(yLog2)));
    const uint32_t chromaLines = p == Picture::Nv24 ? height : (height + 1) / 2;
    s.chromaRows = s.nv12 ? static_cast<uint32_t>(roundUp(chromaLines, blockRows(yLog2))) : 0;
    return s;
}

// Rows [firstRow, firstRow + kHalf) of `s` at 1:1 on panel lines from `line`, START on that row.
inline Window halfWindow(const Surface &s, uint32_t depth, uint32_t firstRow, uint32_t line)
{
    Window w;
    const bool yuv = depth != native_planes::kDepthB8G8R8A8;
    w.options = native_planes::kWinEnable | (yuv ? native_planes::kCscEnable : 0);
    w.depth = depth;
    w.position = line << 16;
    w.size = kHalf << 16 | kPanelWidth;
    w.prescaled = kHalf << 16 | kPanelWidth * lumaBytesPerPixel(s);
    w.dda = 0x10001000;
    w.lineStride = s.nv12 ? s.rowBytes << 16 | s.rowBytes : s.rowBytes;
    w.start = s.offset + static_cast<uint64_t>(firstRow) * s.rowBytes;
    const uint32_t chromaRow = s.tag == static_cast<uint32_t>(Picture::Nv24) ? firstRow : firstRow / 2;
    w.startU = s.nv12 ? s.chromaOffset + static_cast<uint64_t>(chromaRow) * s.rowBytes : 0;
    return w;
}

// Window B scaling surface `s`'s first `width` columns, all its lines, onto the letterbox a 16:9 video takes on the
// panel, as NativePlanes.h programs a detached layer; the V filter on or off.
inline Window letterbox(const Surface &s, uint32_t width, bool vFilter, int32_t top = 437)
{
    native_planes::Layer l;
    l.kind = native_planes::kKindNv12Full;
    l.surface.width = s.width;
    l.surface.height = s.height;
    l.src = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(s.height)};
    l.dst = {0, top, static_cast<int32_t>(kPanelWidth), 405};
    l.surface.planes = 2;
    l.surface.stride[0] = l.surface.stride[1] = s.rowBytes;
    l.surface.offset[1] = s.chromaOffset - s.offset;
    native_planes::Image m;
    Window w;
    if (!native_planes::imageOf(l, kPanelWidth, kPanelHeight, m) || !m.visible)
        return w;
    w.options = vFilter ? m.options : m.options & ~native_planes::kVFilterOn;
    w.depth = m.colorDepth;
    w.position = m.position;
    w.size = m.size;
    w.prescaled = m.prescaled;
    w.hInitial = m.hInitial;
    w.vInitial = m.vInitial;
    w.dda = m.dda;
    w.lineStride = m.lineStride;
    w.kind = surfaceKind(s);
    w.start = s.offset;
    w.startU = s.chromaOffset;
    return w;
}

template <unsigned N> struct Extras {
    Extra word[N] = {};
    unsigned count = 0;

    constexpr void add(uint32_t offset, uint32_t value)
    {
        word[count++] = {offset, value, 0xffffffffu};
    }

    constexpr void csc(uint32_t winc, const uint32_t (&k)[kCscWords])
    {
        for (uint32_t i = 0; i < kCscWords; ++i)
            add(winc + kCscYofWord + i, k[i]);
    }
};

// YOF, KYRGB, KUR, KVR, KUG, KVG, KUB, KVB (s.7.0, 2.8, s.2.8, s.2.8, s.1.8, s.1.8, s.2.8, s.2.8).
constexpr uint32_t kLumaCsc[kCscWords] = {0x80, 0x3ff, 0, 0, 0, 0, 0, 0};
constexpr uint32_t kChromaCsc[kCscWords] = {0, 0, 0x300, 0, 0, 0, 0, 0x300};

constexpr Extras<2 * kCscWords + 2> formatExtras(bool swap2)
{
    Extras<2 * kCscWords + 2> e;
    e.csc(kWincA, kLumaCsc);
    e.csc(kWincB, kChromaCsc);
    if (swap2) {
        e.add(kDcWinA + native_planes::kByteSwap, 1);
        e.add(kDcWinB + native_planes::kByteSwap, 1);
    }
    return e;
}

constexpr Extras<2 * kPaletteWords> paletteExtras()
{
    Extras<2 * kPaletteWords> e;
    for (uint32_t i = 0; i < kPaletteWords; ++i)
        e.add(kWincA + i, paletteWord(i));
    for (uint32_t i = 0; i < kPaletteWords; ++i)
        e.add(kWincB + i, paletteWord(i));
    return e;
}

inline constexpr auto kFormatExtras = formatExtras(false), kSwappedExtras = formatExtras(true);
inline constexpr auto kPaletteExtras = paletteExtras();
constexpr uint32_t kCpEnable = 1u << 16;

template <unsigned N> void withExtras(Plan &p, const Extras<N> &e)
{
    p.step[p.steps - 1].extra = e.word;
    p.step[p.steps - 1].extras = e.count;
}

inline void windowFormatsPlan(Plan &p)
{
    p = Plan();
    p.probe = kWindowFormats;
    const Surface ref = p.surface[addSurface(p, picture(Picture::Reference, kPanelWidth, kPanelHeight))];
    const Surface ref444 = p.surface[addSurface(p, picture(Picture::Reference444, kPanelWidth, kPanelHeight))];
    const Surface nv12 = p.surface[addSurface(p, picture(Picture::Nv12, kPanelWidth, kPanelHeight))];
    const Surface nv24 = p.surface[addSurface(p, picture(Picture::Nv24, kPanelWidth, kPanelHeight))];
    const Surface uyvy = p.surface[addSurface(p, picture(Picture::Uyvy, kPanelWidth, kPanelHeight))];
    const Surface yuyv = p.surface[addSurface(p, picture(Picture::Yuyv, kPanelWidth, kPanelHeight))];
    const auto both = [&](const char *name, const Surface &s, uint32_t depth, Check check, unsigned against,
                          const Extras<2 * kCscWords + 2> *extras) {
        addStep(p, name, halfWindow(s, depth, 0, 0), halfWindow(s, depth, kHalf, kHalf), check, against);
        if (extras)
            withExtras(p, *extras);
    };
    const uint32_t bgra = native_planes::kDepthB8G8R8A8;
    both("ref", ref, bgra, kShown, 0, nullptr);
    both("nv12_43", nv12, native_planes::kDepthYCbCr420SP, kEqual, 0, &kFormatExtras);
    both("nv21_42", nv12, native_planes::kDepthYCbCr420SP - 1, kDiffer, 0, &kFormatExtras);
    both("uyvy_16", uyvy, switch_window_caps::kDepthYCbCr422, kEqual, 0, &kFormatExtras);
    both("yuyv_swap2", yuyv, switch_window_caps::kDepthYCbCr422, kEqual, 0, &kSwappedExtras);
    both("yuyv_raw", yuyv, switch_window_caps::kDepthYCbCr422, kDiffer, 0, &kFormatExtras);
    both("ref444", ref444, bgra, kShown, 0, nullptr);
    both("nv24_49", nv24, switch_window_caps::kDepthYCbCr444SP, kEqual, 6, &kFormatExtras);
}

inline void windowWidePlan(Plan &p)
{
    p = Plan();
    p.probe = kWindowWide;
    const Window off;
    const Surface pitch = p.surface[addSurface(p, picture(Picture::Columns, 4096, 2160))];
    const Surface block = p.surface[addSurface(p, picture(Picture::Columns, 3840, 2160, 1))];
    addStep(p, "ref3840", off, letterbox(pitch, 3840, false));
    addStep(p, "v3840", off, letterbox(pitch, 3840, true), kEqual, 0);
    addStep(p, "block3840", off, letterbox(block, 3840, true), kEqual, 0);
    addStep(p, "ref4096", off, letterbox(pitch, 4096, false));
    addStep(p, "v4096", off, letterbox(pitch, 4096, true), kEqual, 3);
    addStep(p, "control", off, letterbox(pitch, 3840, false, 439), kDiffer, 0);
}

inline void windowPalettePlan(Plan &p)
{
    p = Plan();
    p.probe = kWindowPalette;
    const Surface plain = p.surface[addSurface(p, picture(Picture::Palette, kPanelWidth, kPanelHeight))];
    const Surface mapped = p.surface[addSurface(p, picture(Picture::PaletteMapped, kPanelWidth, kPanelHeight))];
    const uint32_t bgra = native_planes::kDepthB8G8R8A8;
    const auto paletted = [&](Window w) {
        w.options |= kCpEnable;
        return w;
    };
    addStep(p, "mapped", halfWindow(mapped, bgra, 0, 0), halfWindow(mapped, bgra, kHalf, kHalf));
    addStep(p, "palette", paletted(halfWindow(plain, bgra, 0, 0)), paletted(halfWindow(plain, bgra, kHalf, kHalf)),
            kEqual, 0);
    withExtras(p, kPaletteExtras);
    addStep(p, "cp_off", halfWindow(plain, bgra, 0, 0), halfWindow(plain, bgra, kHalf, kHalf), kDiffer, 0);
    withExtras(p, kPaletteExtras);
}

inline const char *extraProbeName(Probe p)
{
    return p == kWindowFormats ? "formats" : p == kWindowWide ? "wide" : p == kWindowPalette ? "palette" : "?";
}

inline bool extraPlanOf(Probe probe, Plan &plan)
{
    if (probe == kWindowFormats)
        windowFormatsPlan(plan);
    else if (probe == kWindowWide)
        windowWidePlan(plan);
    else if (probe == kWindowPalette)
        windowPalettePlan(plan);
    else
        return false;
    return true;
}

}  // namespace display_window_probe
