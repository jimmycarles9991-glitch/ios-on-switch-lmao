// SPDX-License-Identifier: MIT
// A sampler under a converted texture's address map (Texture::addressMap), and whether a kernel's sampler entry whose
// pairs its record holds (raw_pack.h kernelSamplerPairs) samples its textures as Metal does. The translator reads a
// constexpr sampler's modes into the kernel's code as well as its header (nearest samples on a wrapping axis, gathers,
// emulated filters: metal2vulkan lower/sampler/position.rs), and maps them only where a texture's map changes one
// (bind/plan/slots.rs address_map_variant): a sampler whose modes its textures' maps leave is exact unmapped.
#pragma once
#include <cstdint>

namespace raw {

// An MTLSamplerAddressMode under address map `map` (m2v texture/ycbcr_image.rs `ycbcr_modes`, Metal's measured reads): bit
// 0 sends the wrapping modes (mirror clamp to edge, repeat, mirrored repeat) to clamp to edge, bit 1 the clamps to zero
// and to the border colour
constexpr uint8_t mappedAddressMode(uint8_t mode, uint8_t map)
{
    return ((map & 1) && mode >= 1 && mode <= 3) || ((map & 2) && (mode == 4 || mode == 5)) ? 0 : mode;
}

// The same on a header's TEXSAMP0 ADDRESS_U, _V and _P values, as samplerHeader and NVK write the modes (WRAP 0, MIRROR 1,
// CLAMP_TO_EDGE 2, BORDER 3, MIRROR_ONCE_CLAMP_TO_EDGE 5)
constexpr uint32_t mappedHeaderAddress(uint32_t field, uint8_t map)
{
    return ((map & 1) && (field == 0 || field == 1 || field == 5)) || ((map & 2) && field == 3) ? 2 : field;
}

// Whether `map` leaves the modes of the header whose word 0 is `word0`
constexpr bool mapLeavesHeader(uint32_t word0, uint8_t map)
{
    for (const uint32_t shift : {0u, 3u, 6u})
        if (mappedHeaderAddress(word0 >> shift & 7, map) != (word0 >> shift & 7))
            return false;
    return true;
}

// Whether `map` leaves a bound sampler's modes
constexpr bool mapLeavesModes(uint8_t s, uint8_t t, uint8_t r, uint8_t map)
{
    return mappedAddressMode(s, map) == s && mappedAddressMode(t, map) == t && mappedAddressMode(r, map) == r;
}

// Whether the maps of the textures at a sampler entry's paired slots (`mapOf`, 0 where none is bound) all leave its modes
// (`leaves(m)`)
template <typename Leaves, typename MapOf> bool pairedSamplerKept(uint32_t pairs, Leaves leaves, MapOf mapOf)
{
    for (uint32_t s = pairs; s; s &= s - 1)
        if (const uint8_t m = mapOf(uint32_t(__builtin_ctz(s))); m && !leaves(m))
            return false;
    return true;
}

}  // namespace raw
