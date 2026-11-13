// SPDX-License-Identifier: MIT
// What the texture peer (texpeer.mm) writes into each shared texture and the app reads back
// (tests/NativeSharedTexturePeerTest.swift). A level the peer renders instead, and every sample of a multisampled
// texture, is a clear to texel (0, 0) of its level and slice 0.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Byte `c` of texel (x, y) of `level` of `slice` (a 3D texture's plane)
static inline uint8_t switch_texpeer_texel(uint32_t level, uint32_t slice, uint32_t x, uint32_t y, uint32_t c)
{
    return (uint8_t)(x * 5 + y * 3 + level * 29 + slice * 41 + c * 67 + 1);
}

// Byte `c` of block (x, y) of a block-compressed texture's `level` of `slice`
static inline uint8_t switch_texpeer_block(uint32_t level, uint32_t slice, uint32_t x, uint32_t y, uint32_t c)
{
    return (uint8_t)(x * 11 + y * 13 + level * 17 + slice * 19 + c * 23 + 5);
}

// Whether the peer renders `level` of a texture of MTLTextureType `type` and `levels` levels: a mipmapped 2D one's level 1
static inline bool switch_texpeer_rendered(uint32_t type, uint32_t levels, uint32_t level)
{
    return type == 2 && levels > 1 && level == 1;
}
