// SPDX-License-Identifier: MIT
// A sampler's word in the device's sampler table (raw_heap_tables.h nameSampler), the flags a translated function's heap
// sample reads to choose its form: m2v's SamplerWord::of (third_party/m2v metal/sampler.rs), whose layout the translator
// compiles against, so it is that rule bit for bit; its reduction field is weighted average's 0. 0 for a descriptor with a
// value no Metal enumerator has.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace raw {

// MTLSamplerDescriptor's fields, as Metal's enumerators' values
struct SamplerFacts {
    uint64_t minFilter, magFilter, mipFilter, sAddress, tAddress, rAddress, borderColor, normalized, maxAnisotropy,
        compare, lodAverage;
    float lodMin, lodMax;
};

namespace sampler_word {

constexpr uint32_t kPixelCoordinates = 1u << 0, kMinLinear = 1u << 1, kMagLinear = 1u << 2, kMipFilterShift = 3,
                   kBorderShift = 14, kCompareShift = 16, kAnisotropic = 1u << 21, kLevelMinShift = 22,
                   kLevelMaxShift = 26, kCubic = 1u << 30, kLastLevel = 15;
constexpr uint32_t kAddressShift[3] = {5, 8, 11};
constexpr uint64_t kLinear = 1, kCubicFilter = 2;

// A level clamp on Metal's grid of sixty-fourths, at or above 0; 64 or more stays as it is. NaN reads as 0 (fmax).
inline float levelOnGrid(float c)
{
    c = std::fmax(c, 0.0f);
    if (c >= 64.0f)
        return c;
    return std::floor(c * 64.0f) / 64.0f;
}

// The whole level nearest a clamp, past one half up, at most the word's last
inline uint32_t nearestLevel(float c)
{
    c = levelOnGrid(c);
    if (c >= float(kLastLevel))
        return kLastLevel;
    const float whole = std::floor(c);
    return std::min(uint32_t(whole) + uint32_t(c - whole > 0.5f), kLastLevel);
}

}  // namespace sampler_word

inline uint32_t samplerWord(const SamplerFacts &d)
{
    using namespace sampler_word;
    const uint64_t addresses[3] = {d.sAddress, d.tAddress, d.rAddress};
    if (d.minFilter > kCubicFilter || d.magFilter > kCubicFilter || d.mipFilter > 2 || d.borderColor > 2 ||
        d.compare > 7 || std::any_of(addresses, addresses + 3, [](uint64_t a) {
            return a > 5;
        }))
        return 0;
    uint32_t w = d.normalized ? 0 : kPixelCoordinates;
    w |= d.minFilter == kLinear ? kMinLinear : 0;
    w |= d.magFilter == kLinear ? kMagLinear : 0;
    w |= d.minFilter == kCubicFilter || d.magFilter == kCubicFilter ? kCubic : 0;
    w |= uint32_t(d.mipFilter) << kMipFilterShift;
    for (int i = 0; i < 3; i++)
        w |= uint32_t(addresses[i]) << kAddressShift[i];
    w |= uint32_t(d.borderColor) << kBorderShift;
    w |= uint32_t(d.compare) << kCompareShift;
    w |= uint32_t(std::max<uint64_t>(d.maxAnisotropy, 1)) > 1 ? kAnisotropic : 0;
    w |= nearestLevel(d.lodMin) << kLevelMinShift;
    w |= (kLastLevel - nearestLevel(d.lodMax)) << kLevelMaxShift;
    return w;
}

}  // namespace raw
