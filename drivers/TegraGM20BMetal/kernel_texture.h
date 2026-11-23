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

template <typename Texture> constexpr KernelTextureForm kernelTextureForm(const Texture &x)
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
    if (x.standIn)
        return KernelTextureForm::StandIn;
    if (x.sampled)
        return KernelTextureForm::Shadow;
    return KernelTextureForm::InPlace;
}

// The MTLPixelFormat a kernel is compiled to read texture `x` of `format` in: a two-plane RGB10A8_2P_XR10 texture's
// stand-in (kXrStandInFormat), whose header admission binds where the record views the slot so. An extended-range
// texture the kernel does not store (`storable` false: made without MTLTextureUsageShaderWrite, or at a slot its
// state's reflection writes nothing to) is read through its stand-in too: the GM20B samples the RGBA16Float image with
// every filter, where the translator takes no mip filter, anisotropy or mixed min and mag filters over the codes. Else
// its own.
template <typename Texture> uint32_t kernelTextureFormat(const Texture &x, uint32_t format, bool storable)
{
    if (!x.standIn)
        return format;
    const XrForm form = xrForm(format);
    // a two-plane texel buffer over a buffer keeps its alpha bytes past its words (xr.h texelBufferBytes), which its
    // stand-in alone reads; one over a surface's plane is BGR10_XR's words, which a kernel that may store it reads and
    // stores through its format's texel buffer of words alone, alpha 0, as an M4 Max does
    const bool planesBuffer = x.type == kTexelBufferType && twoPlaneXr(format) && (twoPlaneXr(x.format) || !storable);
    return x.xrPlanes || planesBuffer || (!storable && form != XrForm::None && form != XrForm::Planes)
               ? kXrStandInFormat
               : format;
}

// The texture slots a kernel's reflection (reflection_blob.h ReflectionView) says it may store: each slot of a
// texture binding (an array: each element's) whose access is not read-only. A binding node's values: 0 its
// MTLBindingType, 1 its MTLBindingAccess, 2 its index, 7 its array length.
template <typename View> uint32_t kernelTexturesWritten(const View &v)
{
    constexpr uint64_t kTextureBinding = 2, kReadOnly = 0;  // MTLBindingTypeTexture, MTLBindingAccessReadOnly
    uint32_t mask = 0;
    for (uint32_t e = 0; e < v.stages[0].count; e++) {
        const auto &n = v.nodes[v.edges[v.stages[0].first + e]];
        if (n.values[0] != kTextureBinding || n.values[1] == kReadOnly)
            continue;
        const uint64_t count = n.values[7] ? n.values[7] : 1;
        for (uint64_t slot = n.values[2]; slot < n.values[2] + count && slot < 32; slot++)
            mask |= 1u << slot;
    }
    return mask;
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
