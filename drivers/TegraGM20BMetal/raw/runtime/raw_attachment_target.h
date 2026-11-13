// SPDX-License-Identifier: MIT
// What a render pass binds for an attachment of a level, slice or depth plane of a block-linear texture, as Mesa NVK's
// nvk_CmdBeginRendering binds the image view (nvk_cmd_draw.c, MIT): the level's address and tiling, moved to the base layer
// by the image's array stride, or for a 3D image to its plane (nil_image_level_z_offset_B: one plane a block, the layout
// m2v makes a 3D texture it renders into, fx::volumeLayout view2d). A layered pass binds its layers from there.
#pragma once
#include <algorithm>
#include <cstdint>
#include "../fx/fx_image.h"
#include "raw_gpu.h"

namespace raw {

// The image `t` is a level 0 of: its chain's, else its one-level array's or volume's layout, from the base `base`
inline bool attachmentImage(const Texture &t, fx::MipChain &c, uint64_t &base)
{
    if (t.chain) {
        c = t.chain->layout;
        base = t.chain->va;
        return true;
    }
    base = t.va;
    if (t.type == fx::kTexThreeD)
        return fx::volumeLayout(t.width, t.height, t.depth, 1, t.bpp, t.blockDepth == 0, c) &&
               c.level[0].rowBytes == t.pitch;
    return fx::arrayLayout(t.width, t.height, std::max(t.layers, 1u), 1, t.bpp, c) && c.level[0].rowBytes == t.pitch &&
           (t.layers <= 1 || c.arrayStride == t.layerBytes);
}

// The target a pass binds for level `level` of slice `slice` of `t` (a 3D texture's plane `slice`): its address, the level's
// extent and tiling, ARRAY_PITCH the image's array stride, a 3D level's planes as its third dimension; `layers` the layers
// (planes) from it on, which a layered pass may render. No memory or header of its own (tic 0). False: no such part of a
// texture the raw path renders, a pitch one above all (NVIDIA renders pitch memory as one 2D layer).
inline bool attachmentTarget(const Texture &t, uint32_t level, uint32_t slice, Texture &out)
{
    const FormatInfo *f = formatInfo(t.format);
    if (!f || !f->rt || !t.blockLinear || t.zeta || t.memoryless || t.converted() || t.sampled || t.standIn ||
        t.planarStandIn || t.blockW > 1 || t.blockH > 1 || t.samples > 1)
        return false;
    fx::MipChain c;
    uint64_t base = 0;
    if (!attachmentImage(t, c, base) || level >= c.levels)
        return false;
    const fx::MipLevel &l = c.level[level];
    const bool volume = t.type == fx::kTexThreeD;
    if (volume ? (slice >= l.depth || l.zLog2) : slice >= c.layers)
        return false;
    const uint64_t plane = uint64_t(l.rowBytes) * l.rows;
    out = Texture();
    out.storage = t.storageRange();
    out.va = base + l.offset + (volume ? slice * plane : slice * c.arrayStride);
    out.width = l.width;
    out.height = l.height;
    out.texelWidth = l.width;
    out.texelHeight = l.height;
    out.format = t.format;
    out.pitch = l.rowBytes;
    out.bpp = t.bpp;
    out.blockLinear = true;
    out.blockHeight = l.yLog2;
    out.blockDepth = l.zLog2;
    out.layerBytes = c.layers > 1 || volume ? c.arrayStride : c.bytes;  // one layer: chainLevelDesc's ARRAY_PITCH
    out.type = volume ? fx::kTexThreeD : fx::kTexTwoD;
    out.layers = (volume ? l.depth : c.layers) - slice;
    out.renderable = true;
    return true;
}

// The 2D texture header of one layer of target `part` (attachmentTarget) in format words `tic0`, `srgb`: what a fetch from
// a one-layer pass into that part reads. False: a plane inside blocks of several planes, which no 2D header addresses.
inline bool attachmentHeader(const Texture &part, uint32_t tic0, bool srgb, uint32_t w[8])
{
    fx::MipChain one;
    if (part.blockDepth || !fx::mipChainLayout(part.width, part.height, 1, part.bpp, one))
        return false;
    one.yLog2 = part.blockHeight;
    fx::mipChainTic(one, part.va, tic0, srgb, 0, 1, w, fx::kTexTwoD);
    return true;
}

// Whether an attachment of `t` at (level, slice) is the texture itself as a target (level 0 of a one-layer 2D or 1D image).
// A memoryless texture's every slice is: its contents last one pass, which backs it from its attachment's slice on
inline bool wholeAttachment(const Texture &t, uint32_t level, uint32_t slice)
{
    if (t.memoryless)
        return !level && slice < std::max(t.layers, 1u);
    return !level && !slice && (t.type == fx::kTexTwoD || t.type == fx::kTexOneD);
}

}  // namespace raw
