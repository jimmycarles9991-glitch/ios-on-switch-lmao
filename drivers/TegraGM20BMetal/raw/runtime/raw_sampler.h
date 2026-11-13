// SPDX-License-Identifier: MIT
// The sampler a Metal sampler descriptor is on the raw path: what m2v samples with (metal2vulkan-runtime sampler.rs
// taken_descriptor, then metal2vulkan metal/sampler.rs vk_sampler on NVK's profile, whose device samples natively), in the
// fields the texture sampler header takes (raw_gpu.cpp samplerHeader).
#pragma once
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace raw {

struct SamplerDesc {                                      // the MTLSamplerDescriptor fields the TSC takes
    uint8_t minFilter = 0, magFilter = 0, mipFilter = 0;  // MTLSamplerMinMagFilter / MTLSamplerMipFilter
    uint8_t sAddress = 0, tAddress = 0, rAddress = 0;     // MTLSamplerAddressMode (a border axis: transparent black)
    uint8_t maxAnisotropy = 1;                            // Metal's degree (metalAnisotropy), 1 to 32
    float lodMin = 0, lodMax = 3.402823466e+38f;
    bool pixel = false;  // normalizedCoordinates NO
    uint8_t compare =
        0;  // MTLCompareFunction (NV9097 TEXSAMP0 DEPTH_COMPARE_FUNC's own numbering): its compare header's
    // a min or mag filter Cubic (MTLSamplerMinMagFilterCubic): the header filters nearest, a placeholder no filtered read
    // takes, and a state samples through it on its cubic variant (raw_cubic.h), which emulates Metal's bicubic filter
    bool cubic = false;
};

// The MTLSamplerDescriptor's properties, as Metal answers them
struct MetalSamplerFields {
    uint64_t minFilter = 0, magFilter = 0, mipFilter = 0;
    uint64_t sAddress = 0, tAddress = 0, rAddress = 0;
    uint64_t maxAnisotropy = 1, compareFunction = 0, borderColor = 0;
    float lodMin = 0, lodMax = 3.402823466e+38f;
    bool normalizedCoordinates = true;
};

constexpr uint64_t kFilterCubic = 2;

enum : uint64_t {
    kAddressClampToEdge = 0,
    kAddressMirrorClampToEdge = 1,
    kAddressRepeat = 2,
    kAddressMirrorRepeat = 3,
    kAddressClampToZero = 4,
    kAddressClampToBorderColor = 5,
};

// TSC word 1's FLOAT_COORD_NORMALIZATION FORCE_UNNORMALIZED_COORDS (clb097tex.h TEXSAMP1 bit 25), which NVK sets for an
// unnormalizedCoordinates sampler: the texture unit reads texel coordinates whatever the texture header's NORMALIZED_COORDS
// says, so a texture keeps one header for both kinds of sampler
constexpr uint32_t kTscForceUnnormalized = 1u << 25;

inline uint32_t samplerCoordinateBits(const SamplerDesc &s)
{
    return s.pixel ? kTscForceUnnormalized : 0;
}

// Metal's degree for a maxAnisotropy (metal_anisotropy): the low five bits of the value taken through a 32-bit float, 0 as 1,
// 1 to 16 rounded up to a power of two, 17 to 31 as 32
inline uint8_t metalAnisotropy(uint64_t value)
{
    const float f = float(value);
    const uint32_t low = (f >= 4294967296.0f ? UINT32_MAX : uint32_t(f)) & 31;
    if (low == 0)
        return 1;
    if (low > 16)
        return 32;
    uint32_t p = 1;
    while (p < low)
        p <<= 1;
    return uint8_t(p);
}

// False where m2v's sampler is one the header does not express: a comparison, NaN clamps (m2v refuses where they are used), a
// border axis whose border is not transparent black (Metal's ClampToZero reads the descriptor's border colour too, and an
// opaque border depends on the view's swizzle), or pixel coordinates mirrored once on s or t (which Metal mirrors by texel
// index; the texture unit's unnormalized addressing takes only clamps, as Vulkan's unnormalized samplers do).
// Pixel coordinates are the texture unit's (samplerCoordinateBits): the sampler Metal samples them with is plain Metal's
// (metal2vulkan-runtime sampler.rs taken_descriptor): its wrapping axes clamped to the edge, level 0 alone, no anisotropy.
// lodAverage changes nothing m2v hands NVK.
// A Cubic min or mag filter is m2v's (metal2vulkan metal/bicubic.rs runtime_cubic_measured): without a mip filter,
// isotropic, mirrored once on no axis, a border colour Metal names; the header holds nearest filters.
// A sampler header with depth comparison on under `function` (TEXSAMP0 DEPTH_COMPARE, bit 9, and DEPTH_COMPARE_FUNC, bits
// 12:10, whose ZC_ values are MTLCompareFunction's), as NVK writes a compare-enabled VkSampler's (nvk_sampler.c)
inline void compareHeader(uint32_t w[8], uint8_t function)
{
    w[0] = (w[0] & ~0x1E00u) | 1u << 9 | uint32_t(function & 7) << 10;
}

inline bool takeSampler(const MetalSamplerFields &m, SamplerDesc &s)
{
    if (m.compareFunction > 7 || std::isnan(m.lodMin) || std::isnan(m.lodMax))
        return false;
    const bool cubic = m.minFilter == kFilterCubic || m.magFilter == kFilterCubic;
    if (cubic && (m.mipFilter != 0 || metalAnisotropy(m.maxAnisotropy) != 1 || (m.borderColor & 3) == 3 ||
                  m.sAddress == kAddressMirrorClampToEdge || m.tAddress == kAddressMirrorClampToEdge ||
                  m.rAddress == kAddressMirrorClampToEdge))
        return false;
    if (!m.normalizedCoordinates &&
        (m.sAddress == kAddressMirrorClampToEdge || m.tAddress == kAddressMirrorClampToEdge))
        return false;
    const bool border =
        m.sAddress >= kAddressClampToZero || m.tAddress >= kAddressClampToZero || m.rAddress >= kAddressClampToZero;
    const uint64_t borderColor = m.borderColor & 3;
    if (border && borderColor != 0 && borderColor != 3)
        return false;
    if (m.minFilter > kFilterCubic || m.magFilter > kFilterCubic || m.mipFilter > 2 ||
        m.sAddress > kAddressClampToBorderColor || m.tAddress > kAddressClampToBorderColor ||
        m.rAddress > kAddressClampToBorderColor)
        return false;
    s.cubic = cubic;
    s.minFilter = cubic ? 0 : uint8_t(m.minFilter);
    s.magFilter = cubic ? 0 : uint8_t(m.magFilter);
    s.mipFilter = uint8_t(m.mipFilter);
    s.sAddress = uint8_t(m.sAddress);
    s.tAddress = uint8_t(m.tAddress);
    s.rAddress = uint8_t(m.rAddress);
    s.maxAnisotropy = metalAnisotropy(m.maxAnisotropy);
    s.lodMax = m.lodMax;
    s.lodMin = m.lodMin > m.lodMax ? m.lodMax : m.lodMin;
    s.pixel = !m.normalizedCoordinates;
    s.compare = uint8_t(m.compareFunction);
    if (s.pixel) {
        for (uint8_t *axis : {&s.sAddress, &s.tAddress, &s.rAddress})
            if (*axis == kAddressRepeat || *axis == kAddressMirrorRepeat ||
                (axis == &s.rAddress && *axis == kAddressMirrorClampToEdge))
                *axis = kAddressClampToEdge;
        s.mipFilter = 0;
        s.maxAnisotropy = 1;
    }
    return true;
}

}  // namespace raw
