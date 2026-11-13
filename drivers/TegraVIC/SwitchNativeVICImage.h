// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeSurface.h"

static inline const char *switch_native_vic_image(const SwitchNativeSurfaceView &view, uint64_t address,
                                                  TegraVICImage *image)
{
    const auto &layout = view.layout;
    if (layout.plane_count != 2 ||
        (layout.format != TEGRA_VIC_FOURCC('4', '2', '0', 'v') &&
         layout.format != TEGRA_VIC_FOURCC('4', '2', '0', 'f')) ||
        layout.color_matrix >= SWITCH_VIC_MATRIX_UNSUPPORTED)
        return "native VIC source format";
    TegraVICImage result = {};
    result.format = tegra_vic_format(layout.format);
    result.width = layout.planes[0].width;
    result.height = layout.planes[0].height;
    result.standard = layout.color_matrix ? layout.color_matrix - 1 : TEGRA_VIC_YCC_601;
    result.transfer = layout.transfer;
    result.primaries = layout.primaries;
    if (!switch_surface_chroma_site(layout.chroma_top, layout.chroma_bottom, &result.chromaSite))
        return "native VIC chroma location";
    result.blockLinear = 1;
    result.blockHeightLog2 = layout.planes[0].block_height_log2;
    for (uint32_t plane = 0; plane < 2; ++plane) {
        const auto &source = layout.planes[plane];
        if (source.layout != SWITCH_GPU_BACKING_BLOCK_LINEAR || source.block_height_log2 != result.blockHeightLog2 ||
            source.offset > UINT64_MAX - address)
            return "native VIC plane layout";
        result.base[plane] = address + source.offset;
        result.pitch[plane] = source.pitch;
        result.rows[plane] = source.height;
        result.span[plane] = source.bytes;
    }
    *image = result;
    return nullptr;
}

static inline bool switch_native_vic_crop(const SwitchNativeSurfaceView &view, TegraVICOp *operation)
{
    const uint64_t x = uint64_t(view.crop.x) << 16, y = uint64_t(view.crop.y) << 16;
    if (operation->srcX > UINT64_MAX - x || operation->srcY > UINT64_MAX - y ||
        operation->srcX > (uint64_t(view.crop.width) << 16) ||
        operation->srcW > (uint64_t(view.crop.width) << 16) - operation->srcX ||
        operation->srcY > (uint64_t(view.crop.height) << 16) ||
        operation->srcH > (uint64_t(view.crop.height) << 16) - operation->srcY)
        return false;
    operation->srcX += x;
    operation->srcY += y;
    return true;
}
