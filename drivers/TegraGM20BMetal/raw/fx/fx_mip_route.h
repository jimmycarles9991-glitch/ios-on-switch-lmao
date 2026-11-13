// SPDX-License-Identifier: MIT
// Which engine makes each level of a chain's generateMipmaps (rule mip.twod).
//
// Metal makes texel x of a level from the level above with one bilinear tap at x's centre mapped onto it: (x + 1/2) s / d - 1/2
// for an axis of s texels halved to d, weights at 1/256, sRGB filtered in linear space (an M4 Max's generateMipmaps, impulse
// responses of 4-9 texel rows). That is the sample emitGenerate asks the 2D engine for at every size. The engine is proven within
// one code of it only where every weight fraction is a multiple of 1/4, which is exactly where d divides 2s (an even s, or s of
// 1, 3 or 5): HW Test gpu.x1.mip_filter (2:1; probe G13/d4, boot 11 Nov 11:30), gpu.x1.twod_scale (1:1, 2:1, 3:2 bilinear,
// sRGB included; probe copy.twod, boot 6 stack-F-v1) and the 24x10 boot sweep (5 -> 2, 3 -> 1; raw/fx/test/mipcheck.cpp).
// Other steps (97 -> 48 of Core Animation's 776x24 chain) take fractions in 1/(2d) the device has not been asked about.
#pragma once
#include <cstddef>
#include <cstdint>
#include "fx_mip.h"

namespace raw {
namespace fx {

constexpr bool twodStepProven(uint32_t s, uint32_t d)
{
    return d && (2 * s) % d == 0;
}

enum class MipEngine : uint8_t {
    None,
    TwoD,
    ThreeD
};

// A step goes to the 2D engine where the format has a 2D surface format (twodFormat) and the step is proven, else to the
// pack's 3D mip state (raw_mip.h). A format with no 3D state keeps its unproven steps on the 2D engine: BGRA8, RGBA8,
// RGBA16Float, R8 and RG8 have none, and giving them one changes the pack.
struct MipStep {
    MipEngine engine = MipEngine::None;
    bool unproven = false;
};

inline MipStep mipStep(const MipChain &c, uint32_t level, bool twod, bool threeD)
{
    const MipLevel &s = c.level[level], &d = c.level[level + 1];
    const bool proven = twodStepProven(s.width, d.width) && twodStepProven(s.height, d.height);
    if (twod && (proven || !threeD))
        return {MipEngine::TwoD, !proven};
    if (threeD)
        return {MipEngine::ThreeD, false};
    return {};
}

// Levels first + 1 .. last, each made from the one above by `engine`
struct MipRun {
    MipEngine engine = MipEngine::None;
    uint32_t first = 0, last = 0;
    uint32_t unproven = 0;
};

// The chain's steps in order, consecutive steps of one engine as one run; false (after the runs before it) at a step no engine
// makes
template <class Run> bool eachMipRun(const MipChain &c, bool twod, bool threeD, Run &&run)
{
    MipRun r;
    for (uint32_t l = 0; l + 1 < c.levels; l++) {
        const MipStep s = mipStep(c, l, twod, threeD);
        if (s.engine == MipEngine::None)
            return false;
        if (s.engine != r.engine && r.engine != MipEngine::None) {
            run(r);
            r = MipRun();
        }
        if (r.engine == MipEngine::None)
            r = {s.engine, l, l, 0};
        r.last = l + 1;
        r.unproven += s.unproven;
    }
    if (r.engine != MipEngine::None)
        run(r);
    return true;
}

inline bool mipRoutable(const MipChain &c, bool twod, bool threeD)
{
    return eachMipRun(c, twod, threeD, [](const MipRun &) {});
}

inline size_t generateLayersWords(const MipChain &c, uint32_t first, uint32_t last)
{
    return generateWords(c, first, last) * c.layers;
}

// emitGenerate of levels first + 1 .. last in every layer of the chain at `va`
inline void emitGenerateLayers(Push &p, const MipChain &c, uint64_t va, uint32_t fmt2d, uint32_t first, uint32_t last)
{
    for (uint32_t layer = 0; layer < c.layers; layer++)
        emitGenerate(p, c, va + layer * c.arrayStride, nullptr, fmt2d, first, last);
}

}  // namespace fx
}  // namespace raw
