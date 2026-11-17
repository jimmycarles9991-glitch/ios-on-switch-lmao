// SPDX-License-Identifier: MIT
// A texture bound as a texture argument (raw_gpu.h Texture, or any type of the same fields): whether a draw reads it from a
// header of the raw path's.
#pragma once

#include "raw_srgb_pair.h"
#include "raw_xr.h"

namespace raw {

// An extended-range or RG8Unorm_sRGB texture through its stand-in (raw_xr.h, raw_srgb_pair.h); one without (no
// ShaderRead usage) has no header a draw reads. A turned plane (the rotation SPI's: framebuffer-only, MTLTexture.h
// framebufferOnly, no texture argument) is read through its plane's header, as its memory lies, as an M4 Max reads one
// bound anyway.
template <typename Texture> constexpr bool drawsBound(const Texture &t)
{
    return t.tic && (t.standIn || (xrForm(t.format) == XrForm::None && !srgbPair(t.format)));
}

}  // namespace raw
