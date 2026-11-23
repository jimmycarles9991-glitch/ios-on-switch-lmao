// SPDX-License-Identifier: MIT
// A state drawn with a cubic sampler state (MTLSamplerMinMagFilterCubic) bound where its vertex or fragment function
// samples: the texture unit has no bicubic filter (TEXSAMP1's filters are point, linear and anisotropic), so the
// state's record for those binds is m2v's cubic-sampler variant, which emulates Metal's filter in the program,
// specialized for the samplers' address modes, border and coordinates and for the format of each texture sampled
// through them. RenderBox samples icons so.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gm20b {

constexpr uint8_t kCubicVertex = 1, kCubicFragment = 2;

struct CubicBinds {
    struct Sampler {
        uint8_t slot, s, t, r, border;
        bool normalized;
        uint8_t stage = kCubicFragment;
    };

    struct Texture {
        uint8_t slot;
        uint32_t format;  // MTLPixelFormat
        uint8_t stage = kCubicFragment;
    };

    std::vector<Sampler> samplers;  // the fragment stage's by slot, then the vertex stage's
    std::vector<Texture> textures;  // alike

    bool empty() const
    {
        return samplers.empty();
    }

    // The record cache's and the state's name for the variant: " | cubic s<slot>:<s><t><r><border><n>...
    // t<slot>:<format>...", a vertex stage's binds as "vs" and "vt" (a fragment-only variant keeps the name its records
    // are packed under)
    std::string key() const
    {
        std::string k = " | cubic";
        for (const Sampler &s : samplers)
            k += std::string(s.stage == kCubicVertex ? " vs" : " s") + std::to_string(s.slot) + ":" +
                 std::to_string(s.s) + std::to_string(s.t) + std::to_string(s.r) + std::to_string(s.border) +
                 (s.normalized ? "n" : "p");
        for (const Texture &t : textures)
            k += std::string(t.stage == kCubicVertex ? " vt" : " t") + std::to_string(t.slot) + ":" +
                 std::to_string(t.format);
        return k;
    }
};

}  // namespace gm20b
