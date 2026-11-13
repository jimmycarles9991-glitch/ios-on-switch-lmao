// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace raw {

inline bool nativeSamples(uint32_t samples)
{
    return samples == 1 || samples == 2 || samples == 4 || samples == 8;
}

inline bool sampleShadingMasks(uint32_t samples, uint32_t passes, uint32_t words[4])
{
    if (!nativeSamples(samples) || (passes != 1 && passes != samples))
        return false;
    for (uint32_t pair = 0; pair < 4; pair++)
        words[pair] = passes == samples ? (1u << (2 * pair)) | (1u << (2 * pair + 17)) : ~0u;
    return true;
}

inline uint32_t sampleShadingControl(uint32_t passes)
{
    return passes | (passes > 1 ? 0x10 : 0);
}

}  // namespace raw
