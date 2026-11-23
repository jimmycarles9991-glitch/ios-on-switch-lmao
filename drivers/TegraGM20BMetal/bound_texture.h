// SPDX-License-Identifier: MIT
// A texture bound as a texture argument (resources.h Texture, or any type of the same fields): whether a draw reads it
// from a header of the driver's.
#pragma once

#include "srgb_pair.h"
#include "xr.h"

namespace gm20b {

// An extended-range or RG8Unorm_sRGB texture through its stand-in (xr.h, srgb_pair.h); one without (no
// ShaderRead usage) has no header a draw reads. A turned plane (the rotation SPI's: framebuffer-only, MTLTexture.h
// framebufferOnly, no texture argument) is read through its plane's header, as its memory lies, as an M4 Max reads one
// bound anyway.
template <typename Texture> constexpr bool drawsBound(const Texture &t)
{
    return t.tic && (t.standIn || (!xrSampled(t.format) && !srgbPair(t.format)));
}

}  // namespace gm20b
