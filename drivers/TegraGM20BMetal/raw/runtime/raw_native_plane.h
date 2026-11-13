// SPDX-License-Identifier: MIT
#pragma once
#include "TegraGM20BABI.h"
#include "TegraGM20BLayout.h"

namespace raw {

inline bool nativePlaneFits(const TegraGM20BAbiNativeSurfaceViewReply &view, uint32_t plane, uint32_t width,
                            uint32_t height, uint32_t bpp)
{
    if (!view.handle || !bpp || (view.format != 0x34323076 && view.format != 0x34323066) || view.plane_count != 2 ||
        plane >= 2 || !width || !height || view.crop_x || view.crop_y ||
        (view.kind != 0 && view.kind != TGM_LAYOUT_KIND))
        return false;
    const auto &p = view.planes[plane];
    const uint32_t cropWidth = plane ? view.crop_width / 2 + (view.crop_width & 1) : view.crop_width;
    const uint32_t cropHeight = plane ? view.crop_height / 2 + (view.crop_height & 1) : view.crop_height;
    if (bpp != p.bpe || width > p.width || height > p.height || width > cropWidth || height > cropHeight ||
        p.offset > view.bytes || p.bytes > view.bytes - p.offset || (p.offset & 31) ||
        uint64_t(p.pitch) * p.height > p.bytes || uint64_t(width) * bpp > p.pitch)
        return false;
    if (!view.kind)
        return !(p.pitch & 31) && p.pitch / 32 <= 0xffff;
    // WIDTH determines GOB stride; hardware also clamps block height to the logical texture extent.
    return ((uint64_t(width) * bpp + 63) & ~uint64_t(63)) == p.pitch && p.block_height_log2 <= 5 &&
           tgm_layout_log2_ceil((height + 7) / 8) >= p.block_height_log2;
}

}  // namespace raw
