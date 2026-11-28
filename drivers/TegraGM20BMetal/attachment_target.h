// SPDX-License-Identifier: MIT
// What a render pass binds for an attachment of a level, slice or depth plane of a block-linear texture, as Mesa NVK's
// nvk_CmdBeginRendering binds the image view (nvk_cmd_draw.c, MIT): the level's address and tiling, moved to the base layer
// by the image's array stride, or for a 3D image to its plane (nil_image_level_z_offset_B: one plane a block, the layout
// m2v makes a 3D texture it renders into, fx::volumeLayout view2d). A layered pass binds its layers from there.
#pragma once
#include <algorithm>
#include <cstdint>
#include "attachment_lifetime.h"
#include "fx/fx_image.h"
#include "format_standin.h"
#include "gpu.h"

namespace gm20b {

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
    if (!fx::arrayLayout(t.width, t.height, std::max(t.layers, 1u), 1, t.bpp, c) || c.level[0].rowBytes != t.pitch)
        return false;
    // a view of layers of a mipmapped array: each layer's level 0 laid out as one level's, the layers its parent's whole
    // chain apart (Texture::layerBytes)
    if (t.layers > 1 && c.arrayStride != t.layerBytes) {
        if (t.layerBytes < c.arrayStride || c.level[0].yLog2 != t.blockHeight)
            return false;
        c.arrayStride = t.layerBytes;
    }
    return true;
}

// GM20B's pitch colour target as it takes one as it is (NVK's linear-render rule): address and row stride multiples of 128;
// any other renders through a block-linear shadow the copy engine fills and drains (Texture::shadowTarget)
inline void pitchTargetForm(Texture &t)
{
    t.renderable = !((t.va | t.pitch) & 127);
    t.shadowTarget = !t.renderable;
}

// A slice of a linear 2D array (its layers layerBytes apart under a pitch header) as a pitch target of one layer, at most
// 16384 wide (SET_SURFACE_CLIP_HORIZONTAL): NVIDIA renders pitch memory as one 2D layer
inline bool pitchSliceTarget(const Texture &t, uint32_t level, uint32_t slice, Texture &out)
{
    const FormatInfo *f = formatInfo(t.format);
    if (!f || !f->rt || t.blockLinear || t.layers <= 1 || level || slice >= t.layers || t.width > 16384 || t.sampled ||
        t.converted() || t.memoryless || t.zeta || t.samples > 1)
        return false;
    out = Texture();
    out.storage = t.storageRange();
    out.va = t.va + slice * t.layerBytes;
    out.width = out.texelWidth = t.width;
    out.height = out.texelHeight = t.height;
    out.format = t.format;
    out.pitch = t.pitch;
    out.bpp = t.bpp;
    out.layerBytes = t.layerBytes;
    pitchTargetForm(out);
    return true;
}

// A texture buffer (TIC ONE_D_BUFFER) as rows of 16384 texels back to back, a pitch target of `t`'s pitch: a YCbCr image's,
// which its conversion draws (ycbcr.h kYcbcrBufferRow), one row up to 16384 texels
inline bool texelBufferRows(const Texture &t, Texture &out)
{
    constexpr uint32_t kRow = 16384;
    const FormatInfo *f = formatInfo(t.format);
    if (t.type != fx::kTexOneDBuffer || !f || !f->rt)
        return false;
    out = t;
    out.type = fx::kTexTwoD;
    out.width = out.texelWidth = std::min(t.width, kRow);
    out.height = out.texelHeight = (t.width + kRow - 1) / kRow;
    out.tic = 0;
    pitchTargetForm(out);
    return true;
}

// A texture buffer as its one row of texels, a pitch target. An M4 Max renders into one made with render-target usage,
// and fails a pass into one past 16384 texels Internal (harness/passformsoracle.mm texture_buffer, unspecialized_views)
inline bool texelBufferTarget(const Texture &t, Texture &out)
{
    return t.width <= 16384 && texelBufferRows(t, out);
}

// The command buffer's scratch (CommandBuffer::turnScratch) of colour j's plane image (planeInBlocks): 24 + j, between the
// device's backing tags and the turn copies' (gpu.h BackingTag, turned_pass.cpp kTurnTag)
constexpr uint32_t kPlaneImageTag = 24;
static_assert(kPlaneImageTag + 8 <= kDriverPassColorTag, "the plane images' tags below a driver pass's colour tags");

// Plane `plane` of level 0 of a one-level 3D texture laid out several planes a block (texture_shape.cpp volumeView2d:
// one made without render-target usage), which no colour target addresses alone: the pass renders a one-plane-a-block image
// of the planes it names, which the copy engine fills from them and drains into them (MAXWELL_DMA_COPY_A takes a plane of a
// block of several: gm20b_gpu_internal.h sideOf)
inline bool planeInBlocks(const Texture &t, uint32_t level, uint32_t plane)
{
    const FormatInfo *f = formatInfo(t.format);
    return f && (f->rt || formatStandIn(t.format)) && t.type == fx::kTexThreeD && t.blockLinear && t.blockDepth &&
           !t.chain && !level && plane < t.depth && t.samples <= 1 && !t.xrPlanes;
}

// The target a pass binds for level `level` of slice `slice` of `t` (a 3D texture's plane `slice`): its address, the level's
// extent and tiling, ARRAY_PITCH the image's array stride, a 3D level's planes as its third dimension; `layers` the layers
// (planes) from it on, which a layered pass may render. No memory or header of its own (tic 0). A slice of a linear array is
// one pitch layer (pitchSliceTarget). False: no such part of a texture the driver renders. A part of a format the GM20B
// renders none of is not renderable: a stand-in renders it (format_standin.h).
inline bool attachmentTarget(const Texture &t, uint32_t level, uint32_t slice, Texture &out)
{
    const FormatInfo *f = formatInfo(t.format);
    if (!t.blockLinear && f && f->rt)
        return pitchSliceTarget(t, level, slice, out);
    if (!f || !(f->rt || formatStandIn(t.format)) || !t.blockLinear || t.zeta || t.memoryless || t.converted() ||
        t.sampled || t.planarStandIn || t.blockW > 1 || t.blockH > 1 || t.samples > 1)
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
    out.renderable = f->rt != 0;
    return true;
}

// The 2D texture header of one layer of target `part` (attachmentTarget) in format words `tic0`, `srgb`: what a fetch from
// a one-layer pass into that part reads. False: a plane inside blocks of several planes, which no 2D header addresses, and
// a pitch slice, whose block-linear header this is not.
inline bool attachmentHeader(const Texture &part, uint32_t tic0, bool srgb, uint32_t w[8])
{
    fx::MipChain one;
    if (part.blockDepth || !part.blockLinear || !fx::mipChainLayout(part.width, part.height, 1, part.bpp, one))
        return false;
    one.yLog2 = part.blockHeight;
    fx::mipChainTic(one, part.va, tic0, srgb, 0, 1, w, fx::kTexTwoD);
    return true;
}

// Whether an attachment of format `attachment` resolves into a texture of format `into`: its own, or across the 8-bit
// RGBA and BGRA formats and their sRGB twins, which Metal's validation stops and an M4 Max resolves all the same, the
// attachment's values in its own encoding in the destination's channel order (m2v resolve.rs, measured)
constexpr bool resolvesInto(uint32_t attachment, uint32_t into)
{
    const auto eightBit = [](uint32_t f) {
        return f == 70 || f == 71 || f == 80 || f == 81;
    };
    return attachment == into || (eightBit(attachment) && eightBit(into));
}

// A multisampled attachment whose resolve an M4 Max makes the mean of its samples' codes, rounded half up, in every channel
// (harness/resolvecodesoracle.mm: the signed normalized formats, 2x and 4x), and the 2D engine's resolve of its codes
// (resolve_twod.cpp resolveFormat) makes the same: the 16-bit ones (HW Test gpu.gm20b.resolve_codes, boot 2: 0 off).
// The engine's box of 8-bit signed codes rounds a tie toward zero, a positive one a code under Metal's (that rule gives
// every count of resolve_codes' six 8-bit cases and resolve_codes_parts' four on boot 2), and no 902d method sets its
// rounding (cl902d.h): those resolve by the pack's state of their codes (samples.h resolveState). BGR10_XR's is its
// stand-in's (formatResolveState)
constexpr bool codesResolvedByTwod(uint32_t format)
{
    switch (format) {
    case 22:   // R16Snorm
    case 62:   // RG16Snorm
    case 112:  // RGBA16Snorm
        return true;
    default:
        return false;
    }
}

// Whether a pass renders an attachment of stand-in format `format` (format_standin.h) of `samples` samples: one sample,
// or 2 or 4 where the pack names its per-sample load and store states (the copy engine moves the copied formats over the
// sample grid; a memoryless one is neither filled nor stored)
constexpr bool formatStandInSamples(uint32_t format, uint32_t samples, bool memoryless)
{
    if (samples <= 1)
        return true;
    if (samples != 2 && samples != 4)
        return false;
    return memoryless || formatStandInCopied(format) ||
           (formatLoadState(format, samples) != nullptr && formatStoreState(format, samples) != nullptr);
}

// The pack's state resolving a multisampled stand-in of a texture of `format` into a one-sample stand-in, null where the
// stand-in format's own resolve does (resolveState of formatStandIn). An M4 Max resolves such a texture within one code of
// its stand-in format's resolve stored by the store state (harness/standinresolveoracle.mm, 4096 texels at 2x and 4x:
// R8Unorm_sRGB exact, A1BGR5Unorm, ABGR4Unorm and BGRA4Unorm 98.8% to 99.3% exact), as the tile holds their colours before
// its store. RGB9E5Float's tile holds its words after each draw: its state takes the samples' words, their decoded mean
// rounded to the nearest word (98.3% exact at 2x, 88.9% at 4x, every texel within one mantissa code). BGRA10_XR and
// BGR10_XR resolve to the mean of their samples' stored codes (harness/resolvecodesoracle.mm): their states take each
// sample's store code and that mean (pack/xr_target.metal). Their sRGB twins resolve as their stand-in does, the mean of
// the samples' values encoded once: within one code of an M4 Max at every texel of gpu.gm20b.standin_ms_resolve's
// scene at 2x and 4x, where the mean of their sRGB codes lands up to 258 codes off (the twins' own resolve states, 11-24)
constexpr const char *formatResolveState(uint32_t format)
{
    if (format == 552 || format == 554)  // BGRA10_XR, BGR10_XR
        return xrTargetState(format, XrStep::Resolve);
    return format == kRgb9e5Float ? "raw.fmt.fmt_resolve_rgb9e5_frag" : nullptr;
}

// A one-sample attachment's resolve, which an M4 Max runs as a copy of its codes (harness/passoracle.mm
// resolve_one_sample): one whose bytes are the destination's, its own format or its sRGB twin; across the RGBA and BGRA
// orders (resolvesInto) the copy swaps red and blue
constexpr bool copiesInto(uint32_t attachment, uint32_t into)
{
    return attachment == into || ((attachment | 1) == (into | 1) && ((attachment | 1) == 71 || (attachment | 1) == 81));
}

// Whether an attachment of `t` at (level, slice) is the texture itself as a target (level 0 of a one-layer 2D or 1D image).
// A memoryless texture's every slice is: its contents last one pass, which backs it from its attachment's slice on
inline bool wholeAttachment(const Texture &t, uint32_t level, uint32_t slice)
{
    if (t.memoryless)
        return !level && slice < std::max(t.layers, 1u);
    return !level && !slice && (t.type == fx::kTexTwoD || t.type == fx::kTexOneD);
}

}  // namespace gm20b
