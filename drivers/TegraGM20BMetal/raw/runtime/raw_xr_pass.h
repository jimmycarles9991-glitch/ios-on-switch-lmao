// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 (550) attachments drawn in Metal's two planes (m2v pipeline/two_plane.rs, VkProperty::TwoPlaneExtendedRange):
// colour j's word renders into plane 0 at target j (R32Uint) and its alpha byte into plane 1 at the lowest colour index the
// attachments leave free, taken by the 550 attachments in index order. The pass and its states share the rule: Metal makes a
// pipeline's colour formats the pass's, so both leave the same indices free. A state's record of this form is its variant
// " | xr2p" (PipelineTwoPlane). Plane 0 is raw_xr.h's BGR10 word with bits 30 and 31 clear, plane 1 the alpha byte.
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "raw_xr.h"

namespace raw {

constexpr uint32_t kXrWordFormat = 53, kXrAlphaFormat = 13;  // R32Uint, R8Uint
constexpr char kXrPlanesVariant[] = " | xr2p";

// The name of a state's variant whose fragment programs read the two-plane textures at fragment slots `slots` through their
// header pairs (nvk_switch_xr_planes.h, PackPipeline::xrSlots)
inline std::string xrSampledVariant(uint16_t slots)
{
    return " | xrs=" + std::to_string(slots);
}

// The slots of a key's " | xrs=<slots>" variant name, 0 for none
inline uint16_t xrSampledSlots(const std::string &key)
{
    const size_t at = key.find(" | xrs=");
    return at == std::string::npos ? 0 : uint16_t(strtoul(key.c_str() + at + 7, nullptr, 10));
}

// The alpha index of each colour of `formats` (MTLPixelFormat, 0 for none), -1 for one that is not 550 or has no free index
inline std::array<int8_t, 8> xrAlphaSlots(const uint32_t formats[8])
{
    std::array<int8_t, 8> alpha;
    alpha.fill(-1);
    int8_t next = 0;
    for (uint32_t j = 0; j < 8; j++) {
        if (xrForm(formats[j]) != XrForm::Planes)
            continue;
        while (next < 8 && formats[next])
            next++;
        if (next == 8)
            break;
        alpha[j] = next++;
    }
    return alpha;
}

// The alpha indices of `formats`'s 550 colours (xrAlphaSlots), bit j index j: in a state of the two-plane form, targets whose
// registers are the state's own as its colours' are (pack/mkpack.py and directlink.py hold its records to the same rule)
inline uint32_t xrAlphaTargets(const uint32_t formats[8])
{
    uint32_t targets = 0;
    for (const int8_t slot : xrAlphaSlots(formats))
        if (slot >= 0)
            targets |= 1u << slot;
    return targets;
}

// `formats` as a state of the two-plane form takes them: each 550 colour with a free index its word's R32Uint, the index
// its alpha byte's R8Uint
inline void xrPlaneFormats(uint32_t formats[8])
{
    const std::array<int8_t, 8> alpha = xrAlphaSlots(formats);
    for (uint32_t j = 0; j < 8; j++)
        if (alpha[j] >= 0) {
            formats[j] = kXrWordFormat;
            formats[alpha[j]] = kXrAlphaFormat;
        }
}

// The clear words of a 550 colour's planes for Metal's clear colour `color`: the word, then the alpha code (xrClearTexel)
inline std::array<uint32_t, 2> xrPlaneClears(const float color[4])
{
    uint32_t texel[4];
    xrClearTexel(XrForm::Planes, color, false, texel);
    return {texel[0], texel[1]};
}

}  // namespace raw
