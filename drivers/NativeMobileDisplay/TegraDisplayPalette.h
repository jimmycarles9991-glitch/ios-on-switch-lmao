// SPDX-License-Identifier: BSD-2-Clause-Patent
// A gamma table whose channels differ (L1004). IOMobileFramebuffer's table has a curve per channel; the CMU has one LUT2
// for all three. Each window's palette (DC_WIN_COLOR_PALETTE, WINC words 0x000-0x0ff, entry i = R | G << 8 | B << 16)
// is a per-channel table a true-colour window looks its pixels up in while WIN_OPTIONS CP_ENABLE is set (L4T
// t21x/lut.c tegra_dc_set_lut; dc_reg.h CP_ENABLE bit 16): such a table goes into windows A, B and C's palettes, and
// LUT2 carries no gamma. The palette comes before blending and the CMU, Apple's gamma stage after them, so the result is
// Apple's for an opaque window with no matrix, contrast or inversion asked for, and differs from it where the UI blends
// over a video or another CMU stage is active. The palettes are volatile and not shadowed: written in the vertical blank
// as the CMU tables are, and again after SC7.
#pragma once
#include <stdint.h>
#include <string.h>

#include "TegraDisplayColorMath.h"

namespace switch_color {

constexpr uint32_t kPaletteWords = 256, kCpEnable = 1u << 16;
constexpr uint32_t kPaletteBase[3] = {0xa00, 0xc00, 0xe00};  // WINC A, B, C (mainline dc.c tegra_plane_offset)

inline uint32_t gammaPoint(const void *table, unsigned channel, unsigned point)
{
    uint32_t word = 0;
    memcpy(&word, static_cast<const uint8_t *>(table) + (channel * kGammaPoints + point) * sizeof(word), sizeof(word));
    return word;
}

// The 8-bit output of a 10-bit gamma point, rounded.
inline uint8_t gammaByte(uint32_t word)
{
    const uint32_t value = (word > kGammaMax ? kGammaMax : word) + 2;
    return static_cast<uint8_t>(value >> 2 > 255 ? 255 : value >> 2);
}

inline bool channelsDiffer(const void *table)
{
    for (unsigned point = 0; point < kGammaPoints; ++point) {
        const uint32_t red = gammaPoint(table, 0, point);
        if (red != gammaPoint(table, 1, point) || red != gammaPoint(table, 2, point))
            return true;
    }
    return false;
}

inline void paletteOf(const void *table, uint32_t (&palette)[kPaletteWords])
{
    for (unsigned i = 0; i < kPaletteWords; ++i)
        palette[i] = gammaByte(gammaPoint(table, 0, i)) | uint32_t(gammaByte(gammaPoint(table, 1, i))) << 8 |
                     uint32_t(gammaByte(gammaPoint(table, 2, i))) << 16;
}

}  // namespace switch_color
