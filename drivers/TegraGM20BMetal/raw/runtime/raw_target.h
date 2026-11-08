// SPDX-License-Identifier: MIT
// MAXWELL_B SET_COLOR_TARGET_A .. SET_COLOR_TARGET_LAYER for one target, the nine words NVK's nvk_CmdBeginRendering writes
// (nvk_cmd_draw.c), in the two layouts the raw path renders:
//  - pitch (pool and surface textures): WIDTH the row's bytes, MEMORY LAYOUT_PITCH, one layer;
//  - block-linear (a later level of a mipmapped texture's chain, raw/fx): the level where NIL puts it (fx::mipChainLayout: its
//    offset, its block height clamped to the level as the hardware does, its rows aligned to one GOB's 64 bytes). WIDTH is
//    the level's row stride in texels (row_stride_B / bpp, as NVK writes it), HEIGHT the level's, MEMORY BLOCK_WIDTH
//    ONE_GOB, BLOCK_HEIGHT the level's, BLOCK_DEPTH 0, LAYOUT_BLOCKLINEAR, THIRD_DIMENSION_DEFINES_ARRAY_SIZE; one layer,
//    ARRAY_PITCH the image's layer size / 4, LAYER 0. The 2D engine's level writes (fx::emitCopyToLevel,
//    emitGenerate) address the same level by the same base and block height.
#pragma once
#include <cstdint>
#include "../fx/fx_mip.h"

namespace raw {

struct ColorTargetDesc {
    uint64_t va = 0;
    uint32_t pitch = 0;  // row bytes (pitch: the target's rows; block-linear: the level's row stride)
    uint32_t height = 0, bpp = 0, rtFormat = 0;
    bool blockLinear = false;
    uint8_t blockHeight = 0;  // log2 GOBs (block-linear)
    uint64_t layerBytes = 0;  // the image's layer size (block-linear: ARRAY_PITCH)
};

constexpr uint32_t kColorTargetWords = 9;
constexpr uint32_t kTargetLayoutPitch = 0x1000;  // SET_COLOR_TARGET_MEMORY LAYOUT_PITCH (bit 12); 0 is BLOCKLINEAR

inline void colorTargetWords(const ColorTargetDesc &d, uint32_t w[kColorTargetWords])
{
    w[0] = uint32_t(d.va >> 32);
    w[1] = uint32_t(d.va);
    w[3] = d.height;
    w[4] = d.rtFormat;
    w[6] = 1;  // THIRD_DIMENSION: one layer
    w[8] = 0;  // LAYER
    if (!d.blockLinear) {
        w[2] = d.pitch;
        w[5] = kTargetLayoutPitch;
        w[7] = 0;
        return;
    }
    w[2] = d.bpp ? d.pitch / d.bpp : 0;
    w[5] = uint32_t(d.blockHeight & 0xf) << 4;  // BLOCK_WIDTH ONE_GOB, BLOCK_DEPTH ONE_GOB, BLOCKLINEAR, ARRAY_SIZE
    w[7] = uint32_t(d.layerBytes >> 2);
}

// A slot with no target below the pass's last one, as NVK writes it (nvk_CmdBeginRendering): FORMAT DISABLED, VA 0. Left as it
// was, the slot would still name the target of an earlier pass, which CT_SELECT's count makes the hardware render to.
inline void disabledTargetWords(uint32_t w[kColorTargetWords])
{
    for (uint32_t i = 0; i < kColorTargetWords; i++)
        w[i] = 0;
    w[2] = 64;  // WIDTH
    w[6] = 1;   // THIRD_DIMENSION
}

// Level `level` (1 .. levels - 1) of the chain at `chainVa` as a block-linear target; false: not a level of it
inline bool chainLevelDesc(const fx::MipChain &c, uint64_t chainVa, uint32_t level, uint32_t rtFormat,
                           ColorTargetDesc &d)
{
    if (!level || level >= c.levels || !c.bpp || !rtFormat)
        return false;
    const fx::MipLevel &l = c.level[level];
    d = ColorTargetDesc();
    d.va = chainVa + l.offset;
    d.pitch = l.rowBytes;
    d.height = l.height;
    d.bpp = c.bpp;
    d.rtFormat = rtFormat;
    d.blockLinear = true;
    d.blockHeight = l.yLog2;
    d.layerBytes = c.bytes;
    return true;
}

}  // namespace raw
