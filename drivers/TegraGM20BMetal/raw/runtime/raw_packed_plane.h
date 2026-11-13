// SPDX-License-Identifier: MIT
// R10Unorm_PACKED (588) and RG10Unorm_PACKED (589): Metal's views of a packed 10-bit two-plane surface's planes ('p420',
// 'pf20', 'p422', 'pf22', 'p444', 'pf44'), plane 0 a code a texel, plane 1 a pair. A row holds three codes to a
// little-endian 32-bit word, at bits 0, 10 and 20: code n of a row is channel n % k of texel n / k (k codes a texel), and
// a texel reads (code / 1023, the second code / 1023 or 0, 0, 1), bits 30 and 31 unread (an M4 Max's reads of random
// words over each of those surfaces: raw/test/packedplaneoracle.mm). The raw path samples an RGB10A2Unorm image of the
// codes, which the pack's ycbcr_packed_plane_frag draws from the plane's words (R32Uint), and renders a pass into a
// memoryless R32Float or RG32Float stand-in (ycbcrStandInFormat), whose codes ycbcr_store_plane_packed_frag writes into
// the words.
//
// Metal's store into these formats is not measured: on an M4 Max (macOS 27.2.1) a pass into one faults
// (IOGPUCommandQueueErrorDomain 5), or completes having written nothing, or six codes to a 64-bit word against its own
// reads. The store writes the layout Metal reads, each code its channel held as a half as the 10-bit YCbCr code targets
// hold it (raw_ycbcr_store_rule.h: a drawn float toward zero), so YCBCR10_420_2P_PACKED's passes and these views' over one
// surface store one code. Bits 30-31 and a row's slots past its last code are 0, as ycbcrPackRow writes them.
#pragma once

#include <cstdint>

#include "raw_ycbcr.h"
#include "raw_ycbcr_store.h"

namespace raw {

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

// The surfaces Metal's reads were measured over, by fourcc: the chroma halved across, and down
struct PackedSurface {
    uint32_t fourcc;
    bool across, down;
};

constexpr PackedSurface kPackedSurfaces[] = {
    {0x70343230, true, true},   {0x70663230, true, true},    // 'p420', 'pf20'
    {0x70343232, true, false},  {0x70663232, true, false},   // 'p422', 'pf22'
    {0x70343434, false, false}, {0x70663434, false, false},  // 'p444', 'pf44'
};

// Null when the raw path takes a texture of R10Unorm_PACKED or RG10Unorm_PACKED over a surface plane (`out` its plan: the
// plane's words its luma, the image RGB10A2Unorm), else why not
inline const char *planPackedPlane(const YcbcrFacts &f, YcbcrPlan &out)
{
    const uint32_t channels = packedPlaneChannels(f.pixelFormat);
    const PackedSurface *surface = nullptr;
    for (const PackedSurface &s : kPackedSurfaces)
        if (s.fourcc == f.fourcc)
            surface = &s;
    if (!channels)
        return "not a packed 10-bit plane format";
    if (!surface || f.planes != 2)
        return "pixel format";
    if (f.plane != channels - 1)
        return "R10Unorm_PACKED over a plane but 0, or RG10Unorm_PACKED over one but 1";
    if (!f.simple)
        return "shape or usage";
    const bool chroma = f.plane == 1;
    const uint32_t width = chroma && surface->across ? (f.surfaceWidth + 1) / 2 : f.surfaceWidth;
    const uint32_t height = chroma && surface->down ? (f.surfaceHeight + 1) / 2 : f.surfaceHeight;
    if (f.width != width || f.height != height || !width || !height)
        return "texture size not its plane's";
    const uint64_t offset = f.offset[f.plane];
    const uint32_t pitch = f.pitch[f.plane], words = packedPlaneWords(width * channels);
    if ((offset & 15) || (pitch & 15))
        return "plane offset or row not 16-byte";
    if (pitch < uint64_t(words) * 4)
        return "row shorter than its codes";
    if (offset + uint64_t(pitch) * (height - 1) + uint64_t(words) * 4 > f.allocBytes)
        return "plane past the allocation";
    out = {};
    out.target = f.target;
    out.output = YcbcrPackedPlane;
    out.image = 90;       // RGB10A2Unorm
    out.lumaFormat = 53;  // R32Uint
    out.luma = offset;
    out.lumaPitch = pitch;
    out.lumaWidth = words;
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

}  // namespace raw
