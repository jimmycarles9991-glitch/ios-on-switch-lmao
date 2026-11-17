// SPDX-License-Identifier: MIT
// Metal's CPU copies of an RGB10A8_2P_XR10 texture of its own memory (replaceRegion, getBytes) move each texel's colour
// word alone, 4 bytes in the caller's rows, as its blits with a buffer do (raw_xr.h bufferTexelBytes): getBytes leaves
// the alpha plane out, replaceRegion keeps it. The raw path holds a texel as its colour word then its alpha byte, 8
// bytes (raw_xr.h), so the CPU's copy goes through texels of 8 bytes: the colour words in or out of their first 4.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace raw {

constexpr uint32_t kXrWordBytes = 4, kXrTexelBytes = 8;

// `width` x `height` x `depth` colour words of `words` (rows `wordRow`, images `wordImage` apart) into the first 4
// bytes of the texels of `texels` (rows `texelRow`, images `texelImage` apart), each texel's alpha kept
inline void xrWordsIntoTexels(const uint8_t *words, size_t wordRow, size_t wordImage, uint8_t *texels, size_t texelRow,
                              size_t texelImage, uint32_t width, uint32_t height, uint32_t depth)
{
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
                memcpy(texels + z * texelImage + y * texelRow + x * kXrTexelBytes,
                       words + z * wordImage + y * wordRow + x * kXrWordBytes, kXrWordBytes);
}

// The colour words of those texels out into `words`
inline void xrWordsFromTexels(const uint8_t *texels, size_t texelRow, size_t texelImage, uint8_t *words, size_t wordRow,
                              size_t wordImage, uint32_t width, uint32_t height, uint32_t depth)
{
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
                memcpy(words + z * wordImage + y * wordRow + x * kXrWordBytes,
                       texels + z * texelImage + y * texelRow + x * kXrTexelBytes, kXrWordBytes);
}

}  // namespace raw
