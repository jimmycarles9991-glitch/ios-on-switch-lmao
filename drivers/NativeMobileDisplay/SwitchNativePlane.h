// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativePlanes.h"
#include "SwitchDisplayTraits.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

// A decoder's own NV12 picture, both planes block-linear in the same block height: START on each plane's base, its
// strides and block height, and the window's reads (both whole planes).
inline bool switch_native_plane_planes(const native_planes::Layer &layer, const switch_gpu_backing_layout &layout,
                                       native_planes::Image &image)
{
    using namespace native_planes;
    const auto &luma = layout.planes[0], &chroma = layout.planes[1];
    if (luma.layout != SWITCH_GPU_BACKING_BLOCK_LINEAR || chroma.layout != SWITCH_GPU_BACKING_BLOCK_LINEAR ||
        luma.block_height_log2 > 5 || luma.block_height_log2 != chroma.block_height_log2 ||
        luma.bytes_per_element != 1 || chroma.bytes_per_element != 2 || luma.width < layer.surface.width ||
        luma.height < layer.surface.height || uint64_t(chroma.width) * 2 < layer.surface.width ||
        uint64_t(chroma.height) * 2 < layer.surface.height)
        return false;
    uint64_t low = UINT64_MAX, high = 0;
    for (unsigned plane = 0; plane < 2; ++plane) {
        const auto &p = layout.planes[plane];
        const uint64_t rows = 8ULL << p.block_height_log2;
        if (!p.pitch || p.pitch > UINT16_MAX || (p.pitch & 63) || (p.offset & 255) ||
            uint64_t(p.width) * p.bytes_per_element > p.pitch || p.height % rows ||
            p.bytes < uint64_t(p.pitch) * p.height || p.bytes > UINT64_MAX - 4095 ||
            p.offset > UINT64_MAX - p.bytes - 4095)
            return false;
        image.start[plane] = p.offset;
        if (p.offset < low)
            low = p.offset;
        if (p.offset + p.bytes > high)
            high = p.offset + p.bytes;
    }
    if (luma.offset + luma.bytes > chroma.offset && chroma.offset + chroma.bytes > luma.offset)
        return false;
    image.readLow = low & ~4095ULL;
    image.readHigh = (high + 4095) & ~4095ULL;
    image.lineStride = (chroma.pitch << 16) | luma.pitch;
    image.surfaceKind = uint8_t(kSurfaceKindBlock | (luma.block_height_log2 << kSurfaceKindBlockHeightShift));
    return true;
}

// The picture from source pixel (cropX + src.x, cropY + src.y) on: the destination clipped to the screen as imageOf clips
// it, the source origin in ADDR_H_OFFSET (bytes: a luma pixel is one) and ADDR_V_OFFSET (luma lines), as L4T window.c
// programs a buffer's source rectangle; DC A takes the chroma plane's origin from the same two registers (probe V5c).
inline bool switch_native_plane_offset_image(const native_planes::Layer &l, uint32_t width, uint32_t height,
                                             const switch_gpu_backing_layout &layout, uint32_t cropX, uint32_t cropY,
                                             native_planes::Image &image)
{
    using namespace native_planes;
    if (uint64_t(cropX) + l.surface.width > layout.planes[0].width ||
        uint64_t(cropY) + l.surface.height > layout.planes[0].height || cropX > 8191 || cropY > 8191)
        return false;
    image = Image();
    image.yuv = true;
    image.convert = !switch_surface_dc_siting(l.surface.chromaTop, l.surface.chromaBottom);
    Axis x;
    Axis y;
    if (!axisOf(int32_t(cropX) + l.src.x, l.src.w, l.dst.x, l.dst.w, width, cropX + l.surface.width, 8, true, x) ||
        !axisOf(int32_t(cropY) + l.src.y, l.src.h, l.dst.y, l.dst.h, height, cropY + l.surface.height, 15, true, y) ||
        !x.visible || !y.visible || y.first > 0xffff)
        return false;
    image.visible = true;
    image.colorDepth = kDepthYCbCr420SP;
    image.position = y.pos << 16 | x.pos;
    image.size = y.size << 16 | x.size;
    image.prescaled = y.count << 16 | x.count;
    image.dda = y.inc << 16 | x.inc;
    image.hInitial = x.initial;
    image.vInitial = y.initial;
    image.hOffset = x.first;
    image.vOffset = uint16_t(y.first);
    const bool scaledX = x.inc != 4096 || x.initial;
    const bool scaledY = y.inc != 4096 || y.initial;
    image.options = kWinEnable | kCscEnable | (scaledX ? kHFilterOn : 0) | (scaledY ? kVFilterOn : 0);
    image.matrix = matrixOf(l.surface);
    image.fullRange = l.kind == kKindNv12Full;
    return true;
}

// The decoder's picture as window B or C scans it, or false for its pitch representation (a VIC copy). The whole
// picture, unclipped, as imageOf places it; a crop (the decoder's or the layer's) or a clipped destination through the
// offsets once V5a and V5c have passed.
inline bool switch_native_plane_image(const native_planes::Layer &layer, uint32_t width, uint32_t height,
                                      const switch_gpu_backing_layout &layout, uint32_t cropX, uint32_t cropY,
                                      native_planes::Image &image,
                                      const SwitchDisplayTraits &traits = kSwitchDisplayTraits)
{
    using namespace native_planes;
    if (!isNv12(layer.kind) || layout.format != layer.surface.format || layout.plane_count != 2 ||
        layout.color_matrix > 3 || layout.color_range != (layer.kind == kKindNv12Full ? 2u : 1u))
        return false;
    Layer native = layer;
    native.surface.colorMatrix = layout.color_matrix;
    native.surface.transfer = layout.transfer;
    native.surface.primaries = layout.primaries;
    native.surface.chromaTop = layout.chroma_top;
    native.surface.chromaBottom = layout.chroma_bottom;
    const bool whole = !cropX && !cropY && !layer.src.x && !layer.src.y &&
                       layer.src.w == int32_t(layer.surface.width) && layer.src.h == int32_t(layer.surface.height) &&
                       layer.dst.x >= 0 && layer.dst.y >= 0 && int64_t(layer.dst.x) + layer.dst.w <= width &&
                       int64_t(layer.dst.y) + layer.dst.h <= height;
    if (whole) {
        if (!imageOf(native, width, height, image) || !image.visible || image.hOffset ||
            image.start[0] != layer.surface.offset[0] || image.start[1] != layer.surface.offset[1])
            return false;
    } else if (!traits.overlayBlockYuv || !traits.offsetCrop ||
               !switch_native_plane_offset_image(native, width, height, layout, cropX, cropY, image)) {
        return false;
    }
    return switch_native_plane_planes(layer, layout, image);
}
