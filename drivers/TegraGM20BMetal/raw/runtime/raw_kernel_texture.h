// SPDX-License-Identifier: MIT
// How a raw kernel reaches a texture it binds. A mipmapped texture's header is its chain's, every level in place
// (Texture::chain), so a kernel samples its levels as a draw does.
#pragma once
#include <cstdint>

namespace raw {

enum class KernelTextureForm : uint8_t {
    InPlace,  // its own header over its own texels
    Shadow,  // through its 32-aligned image (Texture::sampled), copied in and out around its encoder (raw_kernel_shadow.h)
    Converted,  // the image the command buffer converts from its planes before any work (planar, YCbCr, packed plane)
    NoHeader,   // no header and no texels a header could hold: memoryless, or a depth texture made without ShaderRead
    StandIn,    // extended range: the codes' header, sampled through a stand-in drawn from them
    TwoPlanes,  // RGB10A8_2P_XR10 in Metal's two planes: a header pair
};

template <typename Texture> constexpr KernelTextureForm kernelTextureForm(const Texture &x)
{
    if (x.xrPlanes)
        return KernelTextureForm::TwoPlanes;
    if (x.memoryless || !x.tic)
        return KernelTextureForm::NoHeader;
    if (x.converted())
        return KernelTextureForm::Converted;
    if (x.standIn)
        return KernelTextureForm::StandIn;
    if (x.sampled)
        return KernelTextureForm::Shadow;
    return KernelTextureForm::InPlace;
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

}  // namespace raw
