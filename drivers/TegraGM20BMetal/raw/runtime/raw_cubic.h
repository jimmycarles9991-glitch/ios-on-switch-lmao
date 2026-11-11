// SPDX-License-Identifier: MIT
// A state drawn with a cubic sampler state (MTLSamplerMinMagFilterCubic) bound where its fragment function samples: the
// texture unit has no bicubic filter (TEXSAMP1's filters are point, linear and anisotropic), so the state's record for those
// binds is m2v's cubic-sampler variant, which emulates Metal's filter in the program, specialized for the samplers' address
// modes, border and coordinates and for the format of each texture sampled through them. RenderBox samples icons so.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace raw {

struct CubicBinds {
    struct Sampler {
        uint8_t slot, s, t, r, border;
        bool normalized;
    };

    struct Texture {
        uint8_t slot;
        uint32_t format;  // MTLPixelFormat
    };

    std::vector<Sampler> samplers;  // by slot
    std::vector<Texture> textures;  // by slot

    bool empty() const
    {
        return samplers.empty();
    }

    // The record cache's and the state's name for the variant: " | cubic s<slot>:<s><t><r><border><n>... t<slot>:<format>..."
    std::string key() const
    {
        std::string k = " | cubic";
        for (const Sampler &s : samplers)
            k += " s" + std::to_string(s.slot) + ":" + std::to_string(s.s) + std::to_string(s.t) + std::to_string(s.r) +
                 std::to_string(s.border) + (s.normalized ? "n" : "p");
        for (const Texture &t : textures)
            k += " t" + std::to_string(t.slot) + ":" + std::to_string(t.format);
        return k;
    }
};

}  // namespace raw
