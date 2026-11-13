// SPDX-License-Identifier: MIT
#pragma once
#include "TegraGM20BLayout.h"

namespace raw {

struct SurfaceView {
    uint32_t x = 0, y = 0;
    bool direct = false;
};

inline bool surfaceBackingAliases(uint64_t idA, uint64_t baseA, uint64_t bytesA, uint64_t idB, uint64_t baseB,
                                  uint64_t bytesB)
{
    if (idA && idA == idB)
        return true;
    return bytesA && bytesB && (baseA <= baseB ? baseB - baseA < bytesA : baseA - baseB < bytesB);
}

inline bool surfaceView(const tgm_layout &layout, uint64_t offset, uint32_t width, uint32_t height, uint32_t bpp,
                        uint32_t rowBytes, SurfaceView &out)
{
    if (!width || !height || bpp != layout.bpp || !bpp || !rowBytes || rowBytes < uint64_t(layout.width) * bpp ||
        offset % rowBytes % bpp)
        return false;
    const uint64_t x = offset % rowBytes / bpp, y = offset / rowBytes;
    if (x >= layout.width || y >= layout.height || width > layout.width - x || height > layout.height - y)
        return false;
    out.x = uint32_t(x);
    out.y = uint32_t(y);
    // The texture unit derives its stride from WIDTH and clamps block height at each LOD (NIL Tiling::clamp).
    const uint32_t stride = (width * bpp + 63) & ~63u;
    const uint32_t clamp = tgm_layout_log2_ceil((height + 7) / 8);
    out.direct = !offset && stride == layout.row_bytes && clamp >= layout.y_log2;
    return true;
}

}  // namespace raw
