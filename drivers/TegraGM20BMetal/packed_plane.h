// SPDX-License-Identifier: MIT
// R10Unorm_PACKED (588) and RG10Unorm_PACKED (589): Metal's views of a packed 10-bit two-plane surface's planes ('p420',
// 'pf20', 'p422', 'pf22', 'p444', 'pf44'), plane 0 a code a texel, plane 1 a pair; Metal reads any plane so, by the
// format. A row holds three codes to a
// little-endian 32-bit word, at bits 0, 10 and 20: code n of a row is channel n % k of texel n / k (k codes a texel), and
// a texel reads (code / 1023, the second code / 1023 or 0, 0, 1), bits 30 and 31 unread (an M4 Max's reads of random
// words over each of those surfaces: harness/packedplaneoracle.mm). The driver samples an RGB10A2Unorm image of the
// codes, which the pack's ycbcr_packed_plane_frag draws from the plane's words (R32Uint), and renders a pass into a
// memoryless R32Float or RG32Float stand-in (ycbcrStandInFormat), whose codes ycbcr_store_plane_packed_frag writes into
// the words.
//
// Metal's store into these formats is not measured: on an M4 Max (macOS 27.2.1) a pass into one faults
// (IOGPUCommandQueueErrorDomain 5), or completes having written nothing, or six codes to a 64-bit word against its own
// reads. The store writes the layout Metal reads, each code its channel held as a half as the 10-bit YCbCr code targets
// hold it (ycbcr_store_rule.h: a drawn float toward zero), so YCBCR10_420_2P_PACKED's passes and these views' over one
// surface store one code. Bits 30-31 and a row's slots past its last code are 0, as ycbcrPackRow writes them.
#pragma once

#include <algorithm>
#include <cstdint>

#include "ycbcr.h"
#include "ycbcr_store.h"

namespace gm20b {

// The command buffer's backing tag of a loaded packed plane's stand-in at colour j: kPackedLoadTag + j
constexpr uint32_t kPackedLoadTag = 80;

// The words of a row of `codes` codes
inline uint32_t packedPlaneWords(uint32_t codes)
{
    return (codes + 2) / 3;
}

// The store state's shape of a plane of `channels` codes a texel: 10-bit codes held toward zero, three to a word
inline YcbcrStoreShape packedPlaneShape(uint32_t channels)
{
    YcbcrStoreShape s = ycbcrPackedShape(ycbcrCodesShape(10, false, false));
    s.plane = channels;
    return s;
}

// Null when the driver takes a texture of R10Unorm_PACKED or RG10Unorm_PACKED over a surface plane (`out` its plan: the
// plane's words its luma, the image RGB10A2Unorm), else why not. Metal reads one over any surface's plane of any size so (an
// M4 Max, validation on and off, over 'p420', '420v', 'x420', 'v210', one-plane and planeless 'BGRA', a made-up code, either
// plane and plane 2 of three, sizes smaller and larger than the plane): its codes a texel by the format alone, the plane's
// offset and rows rounded down to 16, rows of fewer bytes than their codes overlapping, the bytes past the allocation in its
// last page read, 0 past the mapping. Of several levels, every lod reads level 0: a read at lod L level-0 texel (x, y), a
// sample level 0 at the same coordinate, while the shader sees each level's size (an M4 Max, validation on and off:
// m2v-textures11 probes/h4/mips.txt), which an image of as many levels holds (Texture::levelZero)
inline const char *planPackedPlane(const YcbcrFacts &f, YcbcrPlan &out)
{
    const uint32_t channels = packedPlaneChannels(f.pixelFormat);
    if (!channels)
        return "not a packed 10-bit plane format";
    if (!f.simple)
        return "shape";
    const uint32_t width = f.width, height = f.height;
    if (!width || !height)
        return "an empty texture";
    const uint64_t offset = f.offset[f.plane] & ~uint64_t(15);
    const uint32_t pitch = ycbcrPlaneStride(f.pitch[f.plane]), words = packedPlaneWords(width * channels);
    const uint64_t rowsEnd = offset + uint64_t(pitch) * (height - 1) + uint64_t(words) * 4;
    const uint64_t mapped = std::max(f.allocBytes, f.mappedBytes);
    const bool window = rowsEnd > mapped;
    if (window && !f.sparseTail)
        return kRowsPastWithoutSparse;
    out = {};
    out.readable = window ? rowsEnd : mapped;
    out.window = window;
    out.target = f.target;
    out.output = YcbcrPackedPlane;
    out.image = 90;       // RGB10A2Unorm
    out.lumaFormat = 53;  // R32Uint
    out.luma = offset;
    out.lumaPitch = pitch;
    out.lumaWidth = words;
    out.levels = f.levels;
    out.buffer = f.buffer;
    out.shape[0] = width;
    out.shape[1] = height;
    out.shape[2] = channels;
    if (f.target)
        out.store = packedPlaneShape(channels);
    return nullptr;
}

// Texel x's codes of a plane row's words, as Metal reads them and pack/ycbcr.metal ycbcr_packed_plane_frag draws them (the
// second 0 of a one-code texel)
inline void packedPlaneTexel(const uint32_t *row, uint32_t x, uint32_t channels, uint32_t codes[2])
{
    for (uint32_t c = 0; c < 2; c++) {
        const uint32_t n = channels * x + c;
        codes[c] = c < channels ? (row[n / 3] >> (10 * (n % 3))) & 1023 : 0;
    }
}

// A loaded pass's stand-in texel x of a plane row (pack/ycbcr.metal ycbcr_load_plane_packed_*_frag): each code over 1023, the
// stand-in's float. The store gives each back (packedPlaneDrawn): it holds the value as the half toward zero, at most one half
// step below it, and rounds h x 1023 to the nearest code, which a half step (2^-11 below 1) x 1023 keeps within 0.4995.
inline void packedPlaneLoad(const uint32_t *row, uint32_t x, uint32_t channels, float value[2])
{
    uint32_t codes[2];
    packedPlaneTexel(row, x, channels, codes);
    for (uint32_t c = 0; c < 2; c++)
        value[c] = float(codes[c]) / 1023.0f;
}

// The store state's words of a pass's stand-in (`rgba`, width x height texels), as pack/ycbcr_store.metal
// ycbcr_store_plane_packed_frag writes them: code n of a row the code of channel n % k of texel n / k, held as
// ycbcrStoreCode holds it; the slots past the row's last code and each word's top two bits 0
inline void packedPlaneDrawn(const YcbcrStoreShape &s, const float *rgba, uint32_t width, uint32_t height,
                             uint32_t *words)
{
    const uint32_t codes = width * s.plane, perRow = packedPlaneWords(codes);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t w = 0; w < perRow; w++) {
            uint32_t word = 0;
            for (uint32_t i = 0; i < 3; i++) {
                const uint32_t n = 3 * w + i;
                if (n < codes)
                    word |= ycbcrStoreCode(s, rgba[4 * (y * width + n / s.plane) + n % s.plane]) << (10 * i);
            }
            words[y * perRow + w] = word;
        }
}

}  // namespace gm20b
