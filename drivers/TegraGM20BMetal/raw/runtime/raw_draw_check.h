// SPDX-License-Identifier: MIT
// Which binds make a render encoder's next draw check again (RMRenderEncoder checkDraw), which otherwise keeps its last
// answer: what the check and the variant it picks (drawnRecord) read of the bound textures and samplers beyond which they
// are. setBytes never does: a bound buffer's residual can only refuse, and setBytes leaves none.
#pragma once

namespace raw {

// What checkDraw reads of one bound texture: a planar address map (refused under a constexpr sampler) and two XR planes
// (a variant, else a stand-in)
struct CheckedTexture {
    bool addressMap = false, xrPlanes = false;

    constexpr bool read() const
    {
        return addressMap || xrPlanes;
    }
};

// A texture bound over `old`: also every texture where a cubic sampler is bound (cubicBinds reads them) or the state
// renders into textures it binds (reboundTextures reads their formats)
constexpr bool textureBindChecks(CheckedTexture old, CheckedTexture next, bool cubicSamplers, bool writesTextures)
{
    return old.read() || next.read() || cubicSamplers || writesTextures;
}

// A sampler bound over `old`: cubicBinds reads which are cubic
constexpr bool samplerBindChecks(bool oldCubic, bool nextCubic)
{
    return oldCubic || nextCubic;
}

}  // namespace raw
