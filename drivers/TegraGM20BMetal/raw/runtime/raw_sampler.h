// SPDX-License-Identifier: MIT
// The sampler a Metal sampler descriptor is on the raw path: what m2v samples with (metal2vulkan-runtime sampler.rs
// taken_descriptor, then metal2vulkan metal/sampler.rs vk_sampler on NVK's profile, whose device samples natively), in the
// fields the texture sampler header takes (raw_gpu.cpp samplerHeader).
#pragma once
#include <cmath>
#include <cstdint>

namespace raw {

struct SamplerDesc {                                      // the MTLSamplerDescriptor fields the TSC takes
    uint8_t minFilter = 0, magFilter = 0, mipFilter = 0;  // MTLSamplerMinMagFilter / MTLSamplerMipFilter
    uint8_t sAddress = 0, tAddress = 0, rAddress = 0;     // MTLSamplerAddressMode (a border axis: transparent black)
    uint8_t maxAnisotropy = 1;                            // Metal's degree (metalAnisotropy), 1 to 32
    float lodMin = 0, lodMax = 3.402823466e+38f;
};

// The MTLSamplerDescriptor's properties, as Metal answers them
struct MetalSamplerFields {
    uint64_t minFilter = 0, magFilter = 0, mipFilter = 0;
    uint64_t sAddress = 0, tAddress = 0, rAddress = 0;
    uint64_t maxAnisotropy = 1, compareFunction = 0, borderColor = 0;
    float lodMin = 0, lodMax = 3.402823466e+38f;
    bool normalizedCoordinates = true;
};

enum : uint64_t {
    kAddressClampToZero = 4,
    kAddressClampToBorderColor = 5,
};

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

// False where m2v's sampler is one the header does not express: pixel coordinates (m2v's function reads them through the
// sampler's word), a comparison, NaN clamps (m2v refuses where they are used), or a border axis whose border is not transparent
// black (Metal's ClampToZero reads the descriptor's border colour too, and an opaque border depends on the view's swizzle).
// lodAverage changes nothing m2v hands NVK.
inline bool takeSampler(const MetalSamplerFields &m, SamplerDesc &s)
{
    if (!m.normalizedCoordinates || m.compareFunction != 0 || std::isnan(m.lodMin) || std::isnan(m.lodMax))
        return false;
    const bool border =
        m.sAddress >= kAddressClampToZero || m.tAddress >= kAddressClampToZero || m.rAddress >= kAddressClampToZero;
    const uint64_t borderColor = m.borderColor & 3;
    if (border && borderColor != 0 && borderColor != 3)
        return false;
    if (m.minFilter > 1 || m.magFilter > 1 || m.mipFilter > 2 || m.sAddress > kAddressClampToBorderColor ||
        m.tAddress > kAddressClampToBorderColor || m.rAddress > kAddressClampToBorderColor)
        return false;
    s.minFilter = uint8_t(m.minFilter);
    s.magFilter = uint8_t(m.magFilter);
    s.mipFilter = uint8_t(m.mipFilter);
    s.sAddress = uint8_t(m.sAddress);
    s.tAddress = uint8_t(m.tAddress);
    s.rAddress = uint8_t(m.rAddress);
    s.maxAnisotropy = metalAnisotropy(m.maxAnisotropy);
    s.lodMax = m.lodMax;
    s.lodMin = m.lodMin > m.lodMax ? m.lodMax : m.lodMin;
    return true;
}

}  // namespace raw
