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
    uint8_t border = 0;  // a border axis's opaque colour: kBorderOpaqueBlack or kBorderOpaqueWhite, 0 transparent black
};

constexpr uint8_t kBorderOpaqueBlack = 1, kBorderOpaqueWhite = 2;  // MTLSamplerBorderColor

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

// False where m2v's sampler is one the header does not express: a comparison, NaN clamps (m2v refuses where they are used), or
// pixel coordinates mirrored once on s or t (which Metal mirrors by texel index; the texture unit's unnormalized addressing
// takes only clamps, as Vulkan's unnormalized samplers do). A border axis (Metal's ClampToZero reads the descriptor's border
// colour too) reads the colour's texel for the texture it samples (borderTexel), which its header holds.
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

// The texel Metal reads at a border of colour `border` (SamplerDesc::border) of a texture whose header word 0 before its
// swizzle is `tic0` and whose MTLTextureSwizzle is `swizzle` (m2v metal/sampler.rs border_texel, measured on an M4): the
// colour in each channel the format has, a missing red, green or blue 0 and alpha 1, A8Unorm's alpha the colour's red,
// then the swizzle. Bit c: lane c reads one; kBorderInteger: an integer format's ones.
constexpr uint8_t kBorderInteger = 0x10;

inline uint8_t borderTexel(uint32_t tic0, const uint8_t swizzle[4], bool a8, uint8_t border)
{
    if (!border)
        return 0;
    const bool white = border == kBorderOpaqueWhite;
    bool v[4];
    for (uint32_t c = 0; c < 4; c++) {
        const uint32_t source = tic0 >> (19 + 3 * c) & 7;
        const bool present = source >= 2 && source <= 5;
        v[c] = c < 3 ? present && white : !present || !a8 || white;
    }
    uint8_t texel = 0;
    for (uint32_t lane = 0; lane < 4; lane++) {
        const uint8_t s = swizzle[lane];
        if (s == 1 || (s >= 2 && s <= 5 && v[s - 2]))
            texel |= uint8_t(1u << lane);
    }
    const uint32_t type = tic0 >> 7 & 7;
    return texel | (type == 3 || type == 4 ? kBorderInteger : 0);
}

// The texel an identity view of a float format of four channels reads, which a sampler's own header holds
inline uint8_t ownBorderTexel(uint8_t border)
{
    return border == kBorderOpaqueWhite ? 0xF : border == kBorderOpaqueBlack ? 0x8 : 0;
}

// Header `w`'s border words reading `texel`: TEXSAMP4..7 BORDER_COLOR_R..A (a float or integer one, by kBorderInteger)
// and the sRGB border TEXSAMP2 S_R_G_B_BORDER_COLOR_R (31:24) and TEXSAMP3 _G (19:12) and _B (27:20), as NVK writes
// them for the colour it is given
inline void borderHeader(uint32_t w[8], uint8_t texel)
{
    const uint32_t one = texel & kBorderInteger ? 1u : 0x3f800000u;
    for (uint32_t c = 0; c < 4; c++)
        w[4 + c] = texel >> c & 1 ? one : 0;
    const auto srgb = [&](uint32_t c) {
        return !(texel & kBorderInteger) && (texel >> c & 1) ? 255u : 0u;
    };
    w[2] = (w[2] & 0x00ffffffu) | srgb(0) << 24;
    w[3] = (w[3] & ~0x0ffff000u) | srgb(1) << 12 | srgb(2) << 20;
}

// TEXSAMP0 ADDRESS_U, ADDRESS_V and ADDRESS_P values (bits 2:0, 5:3, 8:6) samplerHeader and NVK write
constexpr uint32_t kTscWrap = 0, kTscMirror = 1, kTscClampToEdge = 2, kTscBorder = 3, kTscMirrorOnceClampToEdge = 5;

// Sampler header `w` under a planar texture's address map `map` (Texture::addressMap): 1 sends the wrapping modes to the
// edge, 2 the clamps to zero and to the border, 3 both. On the header's own fields, so a constexpr sampler's header,
// which has no descriptor, takes the map as a sampler state's does (Device::samplerVariant).
inline void mapSamplerAddresses(uint32_t w[8], uint8_t map)
{
    const bool wrapped = map == 1 || map == 3, clamps = map == 2 || map == 3;
    for (uint32_t shift : {0u, 3u, 6u}) {
        const uint32_t mode = w[0] >> shift & 7;
        const bool wraps = mode == kTscWrap || mode == kTscMirror || mode == kTscMirrorOnceClampToEdge;
        if ((wrapped && wraps) || (clamps && mode == kTscBorder))
            w[0] = (w[0] & ~(7u << shift)) | kTscClampToEdge << shift;
    }
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
    s.border = border && borderColor != 3 ? uint8_t(borderColor) : 0;
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
