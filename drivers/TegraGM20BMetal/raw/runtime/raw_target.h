// SPDX-License-Identifier: MIT
// MAXWELL_B SET_COLOR_TARGET_A .. SET_COLOR_TARGET_LAYER for one target, the nine words NVK's nvk_CmdBeginRendering writes
// (nvk_cmd_draw.c; kits/SwitchGM20BEmit's switch_gm20b_color_target), in the two layouts the raw path renders:
//  - pitch (pool and surface textures): WIDTH the row's bytes, MEMORY LAYOUT_PITCH, one layer;
//  - block-linear (a later level of a mipmapped texture's chain, raw/fx): the level where NIL puts it (fx::mipChainLayout: its
//    offset, its block height clamped to the level as the hardware does, its rows aligned to one GOB's 64 bytes). WIDTH is
//    the level's row stride in texels (row_stride_B / bpp, as NVK writes it), HEIGHT the level's, MEMORY BLOCK_WIDTH
//    ONE_GOB, BLOCK_HEIGHT the level's, BLOCK_DEPTH 0, LAYOUT_BLOCKLINEAR, THIRD_DIMENSION_DEFINES_ARRAY_SIZE; one layer,
//    ARRAY_PITCH the image's layer size / 4, LAYER 0. The 2D engine's level writes (fx::emitCopyToLevel,
//    emitGenerate) address the same level by the same base and block height.
// A block-linear target of a layered pass binds its layers from the attachment's slice on (THIRD_DIMENSION, ARRAY_PITCH
// the image's array stride); a 3D image's level binds its planes (THIRD_DIMENSION_DEFINES_DEPTH_SIZE, BLOCK_DEPTH the
// level's), as nvk_CmdBeginRendering binds a view of either. NVIDIA renders a pitch target as one 2D layer only.
#pragma once
#include <algorithm>
#include <cstdint>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmit3D.h"
#include "../fx/fx_mip.h"

namespace raw {

struct ColorTargetDesc {
    uint64_t va = 0;
    uint32_t pitch = 0;  // row bytes (pitch: the target's rows; block-linear: the level's row stride)
    uint32_t height = 0, bpp = 0, rtFormat = 0;
    bool blockLinear = false;
    uint8_t blockHeight = 0;  // log2 GOBs (block-linear)
    uint64_t layerBytes = 0;  // the image's layer size (block-linear: ARRAY_PITCH)
    uint32_t layers = 1;      // block-linear: THIRD_DIMENSION
    bool depth = false;       // a 3D image's level: layers are its planes
    uint8_t blockDepth = 0;   // log2 planes a block (a 3D image's level)
};

// Texture `t` (raw_gpu.h, or any type of its fields) of render format `rtFormat` and `rows` rows as a target
template <typename Texture> ColorTargetDesc colorTargetDesc(const Texture &t, uint32_t rtFormat, uint32_t rows)
{
    ColorTargetDesc d;
    d.va = t.va;
    d.pitch = t.pitch;
    d.height = rows;
    d.bpp = t.bpp;
    d.rtFormat = rtFormat;
    d.blockLinear = t.blockLinear;
    d.blockHeight = t.blockHeight;
    d.layerBytes = t.layerBytes;
    return d;
}

inline switch_gm20b_target kitTarget(const ColorTargetDesc &d)
{
    switch_gm20b_target k = {};
    k.address = d.va;
    k.width = d.blockLinear ? (d.bpp ? d.pitch / d.bpp : 0) : d.pitch;
    k.height = d.height;
    k.format = uint8_t(d.rtFormat);
    k.pitch = !d.blockLinear;
    k.block_height_log2 = uint8_t(d.blockHeight & 0xf);
    k.layers = d.blockLinear ? std::max(d.layers, 1u) : 1;
    k.array_pitch = d.blockLinear ? uint32_t(d.layerBytes >> 2) : 0;
    k.depth = d.blockLinear && d.depth;
    k.block_depth_log2 = d.blockLinear ? uint8_t(d.blockDepth & 0xf) : 0;
    return k;
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
