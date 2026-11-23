// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 (550) held in Metal's two planes: plane 0 xr.h's BGR10 word with bits 30 and 31 clear (R32Uint), plane 1
// the alpha byte (R8Uint). A draw samples such a texture through its planes' header pair at the fragment slots a variant
// reads them at (" | xrs=<slots>"), through its stand-in elsewhere.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>

#include "slot_mask.h"
#include "xr.h"

namespace gm20b {

constexpr uint32_t kXrWordFormat = 53, kXrAlphaFormat = 13;  // R32Uint, R8Uint

// The fragment slots whose two-plane textures a variant's programs read through header pairs (PackPipeline::xrSlots, 16
// bits); a two-plane texture a draw samples at any other slot of either stage is sampled through its stand-in
constexpr uint32_t kXrPairSlots = 16;

inline bool xrPairSlot(uint16_t xrSlots, uint32_t slot)
{
    return slot < kXrPairSlots && (xrSlots >> slot & 1);
}

// Of the fragment slots a draw samples two-plane textures at, those a variant reads through header pairs
template <typename Mask> uint16_t xrPairSlots(const Mask &fragment)
{
    return uint16_t(SlotMaskWords<Mask>::word(fragment, 0));
}

// ... and the others, sampled through their stand-ins
template <typename Mask> Mask pastXrPairSlots(Mask fragment)
{
    SlotMaskWords<Mask>::word(fragment, 0) &= ~((1u << kXrPairSlots) - 1);
    return fragment;
}

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

}  // namespace gm20b
