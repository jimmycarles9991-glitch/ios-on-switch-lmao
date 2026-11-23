// SPDX-License-Identifier: MIT
// The texture slots a kernel's sampler entry samples, as the direct compile records them (pack.h kernelSamplerPairs).
#pragma once
#include <cstdint>

#include "pack.h"

namespace gm20b {

// The translator binds every (texture, sampler) a kernel samples through one combined descriptor (metal2vulkan
// lower/sampler `declare`; it takes them apart only past the profile's max_per_stage_descriptor_samplers, 1,048,576 in
// both compile profiles), whose sampler samples its own image alone: the texture at `slot`, or the null image where it is
// not `slotted`. A sampler of its own descriptor samples set-1 heap images, at no slot: its pairs are unknown.
constexpr uint32_t directSamplerWindow(bool combined, bool slotted, uint32_t slot)
{
    if (!combined)
        return 0;
    if (!slotted)
        return kSamplerPairsKnown;
    return slot < 31 ? kSamplerPairsKnown | 1u << slot : 0;
}

}  // namespace gm20b
