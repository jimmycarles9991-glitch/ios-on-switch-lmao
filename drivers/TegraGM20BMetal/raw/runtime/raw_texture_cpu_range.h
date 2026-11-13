// SPDX-License-Identifier: MIT
#pragma once
#include "../fx/fx_image.h"

namespace raw {
struct TextureCpuShape {
    uint32_t width, height, depth, layers, pitch, surfaceX, surfaceY;
    uint8_t bpp, blockDepth;
    bool tiled, volume;
    const fx::MipChain *chain;
    const tgm_layout *importLayout;
    uint64_t layerBytes;  // an array's layer stride (a mipmapped one's view: its chain's, past level 0's image)
};

struct TextureCpuRange {
    uint64_t offset = 0, bytes = 0;
};
enum class TextureCpuRegion {
    Native,
    Empty,
    Unsupported,
    Invalid
};

inline TextureCpuRegion textureCpuRange(const TextureCpuShape &shape, const uint64_t origin[3], const uint64_t size[3],
                                        uint64_t level, uint64_t slice, TextureCpuRange &out)
{
    out = {};
    if (!size[0] || !size[1] || !size[2])
        return TextureCpuRegion::Empty;
    for (unsigned axis = 0; axis < 3; ++axis)
        if (origin[axis] > UINT32_MAX || size[axis] > UINT32_MAX)
            return TextureCpuRegion::Invalid;
    if (!shape.bpp)
        return TextureCpuRegion::Invalid;
    fx::MipChain made;
    const fx::MipChain *chain = nullptr;
    uint64_t layerOffset = 0;
    if (shape.chain && (shape.volume || size[2] <= 1)) {
        // a layer's levels arrayStride on; a 3D level's planes in its own blocks (volumeOffset)
        if (level >= shape.chain->levels || slice >= shape.chain->layers)
            return TextureCpuRegion::Invalid;
        chain = shape.chain;
        layerOffset = slice * shape.chain->arrayStride;
    } else if (shape.volume && !level && !slice) {
        if (!fx::volumeLayout(shape.width, shape.height, shape.depth, 1, shape.bpp, !shape.blockDepth, made))
            return TextureCpuRegion::Invalid;
        chain = &made;
    } else if (shape.layers > 1 && !level && slice < shape.layers && size[2] <= 1) {
        if (!fx::arrayLayout(shape.width, shape.height, shape.layers, 1, shape.bpp, made))
            return TextureCpuRegion::Invalid;
        chain = &made;
        layerOffset = slice * (shape.layerBytes ? shape.layerBytes : made.arrayStride);
    } else if (level || slice || size[2] > 1) {
        return TextureCpuRegion::Unsupported;
    }
    const uint64_t width = chain ? chain->level[level].width : shape.width;
    const uint64_t height = chain ? chain->level[level].height : shape.height;
    const uint64_t depth = chain ? chain->level[level].depth : 1;
    if (origin[0] > width || size[0] > width - origin[0] || origin[1] > height || size[1] > height - origin[1] ||
        origin[2] > depth || size[2] > depth - origin[2])
        return TextureCpuRegion::Invalid;
    const uint64_t x = origin[0] * shape.bpp, endX = (origin[0] + size[0]) * shape.bpp;
    if (endX > UINT32_MAX)
        return TextureCpuRegion::Invalid;
    uint64_t first = 0, last = 0;
    if (chain) {
        first = layerOffset +
                fx::volumeOffset(*chain, uint32_t(level), uint32_t(x), uint32_t(origin[1]), uint32_t(origin[2]));
        last = layerOffset + fx::volumeOffset(*chain, uint32_t(level), uint32_t(endX - 1),
                                              uint32_t(origin[1] + size[1] - 1), uint32_t(origin[2] + size[2] - 1));
    } else if (shape.tiled) {
        tgm_layout madeLayout;
        const tgm_layout *layout = shape.importLayout;
        if (!layout) {
            if (!tgm_layout_tiled(shape.width, shape.height, shape.bpp, &madeLayout))
                return TextureCpuRegion::Invalid;
            layout = &madeLayout;
        }
        const uint64_t cropX = origin[0] + shape.surfaceX, cropY = origin[1] + shape.surfaceY;
        if (layout->bpp != shape.bpp || cropX > layout->width || size[0] > layout->width - cropX ||
            cropY > layout->height || size[1] > layout->height - cropY ||
            (cropX + size[0]) * shape.bpp > layout->row_bytes || layout->y_log2 > 5)
            return TextureCpuRegion::Invalid;
        first = tgm_layout_offset(layout, uint32_t(cropX * shape.bpp), uint32_t(cropY));
        last = tgm_layout_offset(layout, uint32_t((cropX + size[0]) * shape.bpp - 1), uint32_t(cropY + size[1] - 1));
    } else {
        if (uint64_t(shape.width) * shape.bpp > shape.pitch)
            return TextureCpuRegion::Invalid;
        first = origin[1] * shape.pitch + x;
        last = (origin[1] + size[1] - 1) * shape.pitch + endX - 1;
    }
    // Tegra's GOB byte address is monotonic in each coordinate; the opposite corners bound the box.
    if (last < first || last == UINT64_MAX)
        return TextureCpuRegion::Invalid;
    out = {first, last - first + 1};
    return TextureCpuRegion::Native;
}
}  // namespace raw
