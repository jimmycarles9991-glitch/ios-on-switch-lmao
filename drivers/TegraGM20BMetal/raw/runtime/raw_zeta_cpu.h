// SPDX-License-Identifier: MIT
// Metal's CPU copies of a shared depth/stencil texture (replaceRegion, getBytes; an M4 Max, m2vf-shapes2
// probe/hwref/depthcpu.swift): Depth16Unorm, Depth32Float and Stencil8 move their texels as they are,
// Depth32Float_Stencil8 its depth alone, a float a texel, keeping the stencil. The raw path holds a stored zeta in zeta
// memory under its Z kind, which the CPU never addresses: the copy engine moves the texels between it and a pitch
// staging buffer (TextureTransfers), with NVK's remaps for a buffer copy of each aspect (nvk_cmd_copy.c
// nouveau_copy_remap_format; nvk_remap_insert_aspect and nvk_remap_extract_aspect for Z32_FLOAT_S8X24_UINT's depth).
#pragma once
#include <cstdint>
#include "raw_kit.h"
#include "raw_zeta.h"

namespace raw {

struct ZetaCpuCopy {
    uint32_t bytes = 0;                  // a texel in the CPU's rows
    uint32_t remapIn = 0, remapOut = 0;  // SET_REMAP_COMPONENTS into the zeta and out of it
};

// NV90B5_SET_REMAP_COMPONENTS of `size`-byte components: DST_X..W each SRC_X..W (0..3) or NO_WRITE (6), and the
// source's and destination's component counts
constexpr uint32_t zetaRemap(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t src, uint32_t dst)
{
    return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (src - 1) << 20 | (dst - 1) << 24;
}

// The CPU's copy of zeta format `format`; false: not a stored zeta format
inline bool zetaCpuCopy(uint32_t format, ZetaCpuCopy &out)
{
    const uint32_t i = zetaFormatIndex(format);
    if (i == kZetaFormats)
        return false;
    if (format != kZetaFormat) {
        out = {kZetaFormatInfo[i].bpp, copyTexelRemap(kZetaFormatInfo[i].bpp), copyTexelRemap(kZetaFormatInfo[i].bpp)};
        return true;
    }
    constexpr uint32_t kNoWrite = 6;
    out = {4, zetaRemap(0, kNoWrite, kNoWrite, kNoWrite, 4, 1, 2), zetaRemap(0, kNoWrite, kNoWrite, kNoWrite, 4, 2, 1)};
    return true;
}

}  // namespace raw
