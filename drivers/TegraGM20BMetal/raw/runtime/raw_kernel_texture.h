// SPDX-License-Identifier: MIT
// A texture a raw kernel samples as it is: no image of its own the raw path would refresh first (planes converted, an
// extended-range stand-in, a shadow of rows the texture header cannot take, a memoryless backing). A mipmapped texture's
// header is its chain's, every level in place (Texture::chain), so a kernel samples its levels as a draw does.
#pragma once

namespace raw {

template <typename Texture> constexpr bool kernelSamplesInPlace(const Texture &x)
{
    return x.tic && !x.converted() && !x.standIn && (!x.sampled || x.surfaceView) && !x.memoryless;
}

}  // namespace raw
