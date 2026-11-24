// SPDX-License-Identifier: MIT
// How a raw kernel reaches a texture it binds. A mipmapped texture's header is its chain's, every level in place
// (Texture::chain), so a kernel samples its levels as a draw does.
#pragma once
#include <cstdint>
#include "xr.h"

namespace gm20b {

enum class KernelTextureForm : uint8_t {
    InPlace,  // its own header over its own texels
    // through an image of its own, copied in and out around its encoder (kernel_shadow.h): its 32-aligned image
    // (Texture::sampled), or a texel buffer's pool of a block-linear surface's rows (Texture::texelRows)
    Shadow,
    Converted,  // the image the command buffer converts from its planes before any work (planar, YCbCr, packed plane)
    NoHeader,   // no header and no texels a header could hold: memoryless, or a depth texture made without ShaderRead
    StandIn,    // extended range: the codes' header, sampled through a stand-in drawn from them
    TwoPlanes,  // RGB10A8_2P_XR10 in Metal's two planes: each plane's own header (Texture::luma, Texture::chroma)
};

// `readsStandIn`: the kernel's record reads the texture's stand-in (admission); one that reads its codes reaches them as
// a texture of no stand-in does
template <typename Texture> constexpr KernelTextureForm kernelTextureForm(const Texture &x, bool readsStandIn = true)
{
    if (x.memoryless || !x.tic)
        return KernelTextureForm::NoHeader;
    if (x.xrPlanes)
        return KernelTextureForm::TwoPlanes;
    if (x.converted())
        return KernelTextureForm::Converted;
    // ahead of its stand-in: the stand-in is drawn from the pool the encoder's opening fills
    if (x.texelRows)
        return KernelTextureForm::Shadow;
    if (x.standIn && readsStandIn)
        return KernelTextureForm::StandIn;
    if (x.sampled)
        return KernelTextureForm::Shadow;
    return KernelTextureForm::InPlace;
}

// RGB10A8_2P_XR10 or its sRGB twin held as BGR10_XR's (BGR10_XR_sRGB's) words alone under that format's header, over
// a plane no alpha plane follows (TegraGM20BMetalXrPlanes.h xrWordsTexture, xrWordsFormat); a texel buffer's are its own
template <typename Texture> bool xrWordsAlone(const Texture &x, uint32_t format)
{
    constexpr uint32_t kBgr10Xr = 554, kBgr10XrSrgb = 555;
    return x.type != kTexelBufferType &&
           ((format == 550 && x.format == kBgr10Xr) || (format == kRgb10A8XrSrgb && x.format == kBgr10XrSrgb));
}

// The MTLPixelFormat a kernel is compiled to read texture `x` of `format` in: words alone in their own format (alpha 0
// by kernelTextureSwizzle); a two-plane RGB10A8_2P_XR10 texture's stand-in (kXrStandInFormat), whose header admission
// binds where the record views the slot so. An extended-range texture the kernel does not read by its codes (`codes`
// false: kernelReadsCodes) is read through its stand-in too. Else its own.
template <typename Texture> uint32_t kernelTextureFormat(const Texture &x, uint32_t format, bool codes)
{
    if (xrWordsAlone(x, format))
        return x.format;
    if (!x.standIn)
        return format;
    const XrForm form = xrForm(format);
    // a two-plane texel buffer over a buffer keeps its alpha bytes past its words (xr.h texelBufferBytes), which its
    // stand-in alone reads; one over a surface's plane is BGR10_XR's words, which a kernel that may store it reads and
    // stores through its format's texel buffer of words alone, alpha 0, as an M4 Max does
    const bool planesBuffer = x.type == kTexelBufferType && twoPlaneXr(format) && (twoPlaneXr(x.format) || !codes);
    return x.xrPlanes || planesBuffer || (!codes && form != XrForm::None && form != XrForm::Planes) ? kXrStandInFormat
                                                                                                    : format;
}

// The swizzle a kernel reads texture `x` of `format` through, from the texture's own `channels` (MTLTextureSwizzle each):
// words alone read alpha 0, as an M4 Max reads them (m2v-textures3 probes/xrsurf.mm), where their format reads 1, so a
// channel of alpha's takes zero
template <typename Texture> void kernelTextureSwizzle(const Texture &x, uint32_t format, uint8_t channels[4])
{
    constexpr uint8_t kZero = 0, kAlpha = 5;  // MTLTextureSwizzleZero, MTLTextureSwizzleAlpha
    if (xrWordsAlone(x, format))
        for (int c = 0; c < 4; c++)
            channels[c] = channels[c] == kAlpha ? kZero : channels[c];
}

// Whether the translator samples an extended-range texture's codes through a runtime sampler of these
// MTLSamplerDescriptor fields: a cubic filter (MTLSamplerMinMagFilterCubic), or one filter for minification and
// magnification with no mip filter and no anisotropy (m2v bind/plan/resources.rs extended_sampler, which refuses the rest)
constexpr bool xrCodesSampler(uint64_t minFilter, uint64_t magFilter, uint64_t mipFilter, uint64_t maxAnisotropy)
{
    constexpr uint64_t kCubic = 2;
    return minFilter == kCubic || magFilter == kCubic || (minFilter == magFilter && !mipFilter && maxAnisotropy <= 1);
}

// Whether a kernel reads extended-range texture slot `slot` by its codes, not its stand-in: a slot it may store
// (`written`) of a texture made with ShaderWrite (`storable`), or one whose components it reads as floats (`floats`)
// beside samplers the codes' fetches all take (`fetched`: xrCodesSampler). The RGBA16Float stand-in holds each value
// to half precision: gpu.compute.exact_round read 6,628 of BGR10_XR's and BGRA10_XR's float channels off through it
// on boot 2, every one a half's rounding, and every half read right. A slot read as halves keeps the stand-in, which
// the GM20B samples under any filter.
constexpr bool kernelReadsCodes(uint32_t slot, bool storable, uint32_t written, uint32_t floats, bool fetched)
{
    return (storable && (written >> slot & 1)) || (fetched && (floats >> slot & 1));
}

// The slots of the texture bindings (an array's: each element's) of a kernel's reflection (reflection_blob.h
// ReflectionView) whose binding node `take` takes. A binding node's values: 0 its MTLBindingType, 1 its MTLBindingAccess,
// 2 its index, 6 a texture's MTLDataType, 7 its array length.
template <typename View, typename Take> uint32_t kernelTextureSlots(const View &v, Take take)
{
    constexpr uint64_t kTextureBinding = 2;  // MTLBindingTypeTexture
    uint32_t mask = 0;
    for (uint32_t e = 0; e < v.stages[0].count; e++) {
        const auto &n = v.nodes[v.edges[v.stages[0].first + e]];
        if (n.values[0] != kTextureBinding || !take(n.values))
            continue;
        const uint64_t count = n.values[7] ? n.values[7] : 1;
        for (uint64_t slot = n.values[2]; slot < n.values[2] + count && slot < 32; slot++)
            mask |= 1u << slot;
    }
    return mask;
}

// The texture slots a kernel may store: each binding whose access is not read-only
template <typename View> uint32_t kernelTexturesWritten(const View &v)
{
    return kernelTextureSlots(v, [](const uint64_t *values) {
        return values[1] != 0;  // MTLBindingAccessReadOnly
    });
}

// The texture slots a kernel reads as floats: each binding of MTLDataTypeFloat components
template <typename View> uint32_t kernelTexturesFloat(const View &v)
{
    return kernelTextureSlots(v, [](const uint64_t *values) {
        return values[6] == 3;  // MTLDataTypeFloat
    });
}

inline const char *kernelTextureFormName(KernelTextureForm f)
{
    switch (f) {
    case KernelTextureForm::InPlace:
        return "in place";
    case KernelTextureForm::Shadow:
        return "shadow";
    case KernelTextureForm::Converted:
        return "converted";
    case KernelTextureForm::NoHeader:
        return "no header";
    case KernelTextureForm::StandIn:
        return "stand-in";
    case KernelTextureForm::TwoPlanes:
        return "two planes";
    }
    return "?";
}

}  // namespace gm20b
