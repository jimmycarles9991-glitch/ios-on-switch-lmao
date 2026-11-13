// SPDX-License-Identifier: BSD-2-Clause-Patent
// A landscape video with no copy (V3, display.x1.column_scan_yuv): the decoder's NV12 picture turned a quarter by window
// A's column scan, the composited UI on window B in front of it. On an internal display Core Animation turns a landscape
// video layer through the IOSurfaceAccelerator (TegraVIC) into a portrait destination and detaches that; T210's window A
// can scan the decoder's picture turned instead (L4T dc_config.c: only window A rotates planar formats, to
// PLANAR_ROTATION_MAXIMUM_SIZE 4096 x 2304; windows B and C cannot), so the UI leaves window A for window B, block-linear
// BGRA premultiplied over A as A is over B today. Plain logic: test/switch_landscape_test.cpp.
//
// The turn is IOSurfaceAccelerator's transform t (TEGRA_VIC_FLIP_H 1, FLIP_V 2, ROT90 4: the flips, then a quarter turn
// clockwise, TegraVIC/test/TegraVICProbe.h tegra_vic_transform_point, which VIC's own jobs are held to): destination
// pixel (dx, dy) of a W x H source rectangle shows source pixel
//   t 4: (dy, H - 1 - dx)   t 5: (W - 1 - dy, H - 1 - dx)   t 6: (dy, dx)   t 7: (W - 1 - dy, dx).
// Under SCAN_COLUMN a panel line is a source column and a panel pixel a source row (NativeColumnScan.h), so the column
// walks back from ADDR_H_OFFSET under H_DIRECTION (t 5, 7) and the row from ADDR_V_OFFSET under V_DIRECTION (t 4, 5),
// each starting on the rectangle's last byte or line (L4T window.c: (x + w) * Bpp - 1, y + h - 1).
#pragma once
#include <stdint.h>

#include "NativeColumnScan.h"
#include "SwitchNativePlane.h"

namespace switch_landscape {

constexpr uint32_t kPlanarRotationMaxWidth = 4096, kPlanarRotationMaxHeight = 2304;
constexpr uint32_t kRot90 = 4;

struct Turn {
    bool hDirection = false, vDirection = false;
};

inline bool turnOf(uint32_t transform, Turn &turn)
{
    if (transform > 7 || !(transform & kRot90))
        return false;
    turn.hDirection = transform == 5 || transform == 7;
    turn.vDirection = transform == 4 || transform == 5;
    return true;
}

// The visible picture's source rectangle `src` (pixels, its corner even) turned by `transform` onto panel rectangle
// `dst` (the destination wholly on the panel) as window A scans it: START on the plane bases, the origin in the offsets,
// the DDAs along the source's rows (H) and columns (V). False when window A cannot.
inline bool videoImage(const native_planes::Layer &picture, const switch_gpu_backing_layout &layout, uint32_t cropX,
                       uint32_t cropY, native_planes::Rect src, uint32_t transform, native_planes::Rect dst,
                       uint32_t width, uint32_t height, native_planes::Image &image)
{
    using namespace native_planes;
    Turn turn;
    if (!turnOf(transform, turn) || !isNv12(picture.kind) || src.x < 0 || src.y < 0 || src.w < 2 || src.h < 2 ||
        (src.x & 1) || (src.y & 1) || uint64_t(src.x) + src.w > picture.surface.width ||
        uint64_t(src.y) + src.h > picture.surface.height || dst.x < 0 || dst.y < 0 || dst.w <= 0 || dst.h <= 0 ||
        uint64_t(dst.x) + dst.w > width || uint64_t(dst.y) + dst.h > height ||
        uint32_t(src.w) > kPlanarRotationMaxWidth || uint32_t(src.h) > kPlanarRotationMaxHeight ||
        uint64_t(cropX) + picture.surface.width > layout.planes[0].width ||
        uint64_t(cropY) + picture.surface.height > layout.planes[0].height)
        return false;
    const uint32_t hInc = ddaIncrement(uint32_t(src.h), uint32_t(dst.w), 8);
    const uint32_t vInc = ddaIncrement(uint32_t(src.w), uint32_t(dst.h), 15);
    if (!hInc || !vInc)
        return false;
    image = Image();
    image.visible = true;
    image.yuv = true;
    image.convert = !switch_surface_dc_siting(layout.chroma_top, layout.chroma_bottom);
    image.colorDepth = kDepthYCbCr420SP;
    image.position = uint32_t(dst.y) << 16 | uint32_t(dst.x);
    image.size = uint32_t(dst.h) << 16 | uint32_t(dst.w);
    image.prescaled = uint32_t(src.w) << 16 | uint32_t(src.h);
    image.dda = vInc << 16 | hInc;
    image.hInitial = hInc > 4096 ? (hInc - 4096) / 2 : 0;
    image.vInitial = vInc > 4096 ? (vInc - 4096) / 2 : 0;
    const uint32_t x0 = cropX + uint32_t(src.x), y0 = cropY + uint32_t(src.y);
    image.hOffset = turn.hDirection ? x0 + uint32_t(src.w) - 1 : x0;
    const uint32_t v = turn.vDirection ? y0 + uint32_t(src.h) - 1 : y0;
    if (v > 0xffff)
        return false;
    image.vOffset = uint16_t(v);
    image.options = kWinEnable | kCscEnable | native_column_scan::kScanColumn |
                    (turn.hDirection ? native_column_scan::kHDirection : 0) |
                    (turn.vDirection ? native_column_scan::kVDirection : 0) |
                    (hInc != 4096 || image.hInitial ? kHFilterOn : 0) |
                    (vInc != 4096 || image.vInitial ? kVFilterOn : 0);
    image.matrix = layout.color_matrix == 2 ? kBt709 : layout.color_matrix == 3 ? kBt2020 : kBt601;
    image.fullRange = layout.color_range == 2;
    return switch_native_plane_planes(picture, layout, image);
}

// A detached layer showing the whole of a destination TegraVIC recorded as the decoder's picture (`width` x `height`
// visible at `cropX`, `cropY` in its buffer) turned by `transform` from source rectangle `turned`: window A scans the
// picture itself onto the layer's destination. False when the layer shows part of the destination (or turns it again),
// the picture is not 8-bit 4:2:0, or its chroma is sited where the window does not sample it (VIC resamples it).
inline bool turnedVideoImage(const native_planes::Layer &layer, const switch_gpu_backing_layout &layout, uint32_t cropX,
                             uint32_t cropY, uint32_t width, uint32_t height, uint32_t transform,
                             native_planes::Rect turned, uint32_t panelWidth, uint32_t panelHeight,
                             native_planes::Image &image)
{
    using namespace native_planes;
    if (layer.transform || layer.src.x || layer.src.y || uint32_t(layer.src.w) != layer.surface.width ||
        uint32_t(layer.src.h) != layer.surface.height || layout.plane_count != 2 || layout.color_matrix > 3)
        return false;
    Layer picture;
    picture.kind = layout.format == fourcc('4', '2', '0', 'v')   ? kKindNv12Video
                   : layout.format == fourcc('4', '2', '0', 'f') ? kKindNv12Full
                                                                 : kKindNone;
    picture.surface.format = layout.format;
    picture.surface.width = width;
    picture.surface.height = height;
    return videoImage(picture, layout, cropX, cropY, turned, transform, layer.dst, panelWidth, panelHeight, image) &&
           !image.convert;
}

// Window A's and B's blend while B carries the UI over the video on A: B in front, premultiplied; A opaque behind it.
struct Blend {
    uint32_t layer, match, nomatch;
};

constexpr Blend kUiOnB = {native_planes::kBlendPremultiplied, native_planes::kBlendOver, native_planes::kBlendOver};
constexpr Blend kVideoOnA = {native_planes::kBlendBypass | 1, 0, 0};

}  // namespace switch_landscape
