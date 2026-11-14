// SPDX-License-Identifier: MIT
// raw/runtime/raw_sampler.h: the samplers the raw path takes, as m2v samples them on NVK (metal2vulkan-runtime sampler.rs
// taken_descriptor and metal_anisotropy; metal2vulkan metal/sampler.rs vk_sampler), and pixel coordinates as NVK's
// unnormalized samplers take them. Their headers against NVK's are raw/test/samplercheck's.
#include "../raw/runtime/raw_sampler.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <utility>

namespace {

TEST(RawSampler, AnisotropyIsMetalsDegree)
{
    const struct {
        uint64_t value;
        uint8_t degree;
    } cases[] = {{0, 1},   {1, 1},   {2, 2},   {3, 4},  {5, 8},  {8, 8},   {9, 16},
                 {16, 16}, {17, 32}, {31, 32}, {32, 1}, {33, 1}, {48, 16}, {UINT64_MAX, 32}};

    for (const auto &c : cases)
        EXPECT_EQ(raw::metalAnisotropy(c.value), c.degree) << c.value;
}

TEST(RawSampler, WidgetLodAverageSamplerIsTaken)
{
    raw::MetalSamplerFields m;
    m.minFilter = m.magFilter = 1;
    raw::SamplerDesc s;
    ASSERT_TRUE(raw::takeSampler(m, s));
    EXPECT_EQ(s.minFilter, 1);
    EXPECT_EQ(s.magFilter, 1);
    EXPECT_EQ(s.maxAnisotropy, 1);
}

TEST(RawSampler, AnisotropicSamplerIsTaken)
{
    raw::MetalSamplerFields m;
    m.minFilter = m.magFilter = 1;
    m.mipFilter = 2;
    m.sAddress = m.tAddress = 3;
    m.maxAnisotropy = 2;
    raw::SamplerDesc s;
    ASSERT_TRUE(raw::takeSampler(m, s));
    EXPECT_EQ(s.maxAnisotropy, 2);
    EXPECT_EQ(s.sAddress, 3);
    EXPECT_EQ(s.mipFilter, 2);
}

TEST(RawSampler, InvertedClampsReadTheUpperOne)
{
    raw::MetalSamplerFields m;
    m.lodMin = 3.0f;
    m.lodMax = 1.5f;
    raw::SamplerDesc s;
    ASSERT_TRUE(raw::takeSampler(m, s));
    EXPECT_EQ(s.lodMin, 1.5f);
    EXPECT_EQ(s.lodMax, 1.5f);
}

// m2v enables comparison per use (metal/sampler.rs SamplerUse::compare): a sampler keeps a compare header beside its own,
// under its compare function, for the states that sample its slot by comparison
TEST(RawSampler, ACompareFunctionIsTakenForItsCompareHeader)
{
    raw::MetalSamplerFields m;
    m.compareFunction = 1;  // MTLCompareFunctionLess
    raw::SamplerDesc s;
    ASSERT_TRUE(raw::takeSampler(m, s));
    EXPECT_EQ(s.compare, 1);
    uint32_t w[8] = {0x26092, 0x291, 0xfff000, 0, 0, 0, 0, 0};
    raw::compareHeader(w, s.compare);
    EXPECT_EQ(w[0], 0x26092u | 1u << 9 | 1u << 10);
    raw::compareHeader(w, 6);  // GreaterEqual: ZC_GEQUAL
    EXPECT_EQ(w[0] >> 10 & 7, 6u);
    EXPECT_EQ(w[1], 0x291u);
    m.compareFunction = 8;
    EXPECT_FALSE(raw::takeSampler(m, s));
}

TEST(RawSampler, WhatTheHeaderDoesNotExpressIsRefused)
{
    raw::SamplerDesc s;
    raw::MetalSamplerFields nan;
    nan.lodMax = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(raw::takeSampler(nan, s));
}

// RenderBox's and TVWidgetExtension's (boot 13: normalized 0, clamp to edge, lodAverage): the texture unit takes texel
// coordinates from the sampler header alone, so the texture's own header serves normalized samplers too
TEST(RawSampler, PixelCoordinatesAreTheSamplerHeaders)
{
    raw::MetalSamplerFields m;
    m.normalizedCoordinates = false;
    m.minFilter = m.magFilter = 1;
    raw::SamplerDesc s;
    ASSERT_TRUE(raw::takeSampler(m, s));
    EXPECT_TRUE(s.pixel);
    EXPECT_EQ(raw::samplerCoordinateBits(s), 1u << 25);
    EXPECT_EQ(s.minFilter, 1);
    raw::SamplerDesc normalized;
    ASSERT_TRUE(raw::takeSampler(raw::MetalSamplerFields{}, normalized));
    EXPECT_FALSE(normalized.pixel);
    EXPECT_EQ(raw::samplerCoordinateBits(normalized), 0u);
}

// metal2vulkan-runtime sampler.rs taken_descriptor: wrapping s and t clamp to the edge, r's mirror-once too, level 0 alone and
// no anisotropy
TEST(RawSampler, PixelSamplerIsPlainMetals)
{
    raw::MetalSamplerFields m;
    m.normalizedCoordinates = false;
    m.sAddress = raw::kAddressRepeat;
    m.tAddress = raw::kAddressMirrorRepeat;
    m.rAddress = raw::kAddressMirrorClampToEdge;
    m.mipFilter = 2;
    m.maxAnisotropy = 8;
    raw::SamplerDesc s;
    ASSERT_TRUE(raw::takeSampler(m, s));
    EXPECT_EQ(s.sAddress, raw::kAddressClampToEdge);
    EXPECT_EQ(s.tAddress, raw::kAddressClampToEdge);
    EXPECT_EQ(s.rAddress, raw::kAddressClampToEdge);
    EXPECT_EQ(s.mipFilter, 0);
    EXPECT_EQ(s.maxAnisotropy, 1);
    m.sAddress = raw::kAddressClampToZero;
    m.tAddress = raw::kAddressClampToBorderColor;
    ASSERT_TRUE(raw::takeSampler(m, s));
    EXPECT_EQ(s.sAddress, raw::kAddressClampToZero);
    EXPECT_EQ(s.tAddress, raw::kAddressClampToBorderColor);
}

TEST(RawSampler, PixelCoordinatesMirroredOnceOnAPlaneAxisAreRefused)
{
    raw::SamplerDesc s;
    for (int axis = 0; axis < 2; axis++) {
        raw::MetalSamplerFields m;
        m.normalizedCoordinates = false;
        (axis ? m.tAddress : m.sAddress) = raw::kAddressMirrorClampToEdge;
        EXPECT_FALSE(raw::takeSampler(m, s)) << axis;
        m.normalizedCoordinates = true;
        EXPECT_TRUE(raw::takeSampler(m, s)) << axis;
    }
}

// Boot 21 gpu.heap.m2v_target: an opaque white border was a sampler of m2v's alone (call.sampler "border 2"), and the
// range that bound it failed. Every border colour is taken, Metal's low two bits of it, 3 as transparent black.
TEST(RawSampler, EveryBorderColourIsTaken)
{
    for (uint64_t mode : {raw::kAddressClampToZero, raw::kAddressClampToBorderColor}) {
        for (uint64_t color = 0; color < 8; color++) {
            raw::MetalSamplerFields m;
            m.tAddress = mode;
            m.borderColor = color;
            raw::SamplerDesc s;
            ASSERT_TRUE(raw::takeSampler(m, s)) << mode << " " << color;
            EXPECT_EQ(s.border, (color & 3) == 3 ? 0 : color & 3) << mode << " " << color;
        }
    }
    raw::MetalSamplerFields noBorder;
    noBorder.borderColor = 2;
    raw::SamplerDesc s;
    EXPECT_TRUE(raw::takeSampler(noBorder, s));
    EXPECT_EQ(s.border, 0) << "no axis reads it";
}

// Header word 0 of A8Unorm, R8Unorm, BGRA8Unorm and RGBA8Uint before their swizzle (raw_gpu.cpp kFormats)
constexpr uint32_t kA8 = 0x2002491d, kR8 = 0x7012491d, kBGRA8 = 0x54e24908, kRGBA8Uint = 0x58d49208;
constexpr uint8_t kIdentity[4] = {2, 3, 4, 5};

// m2v metal/sampler.rs border_texel, Metal's: the colour in the format's channels, a missing red, green or blue 0 and
// alpha 1, A8Unorm's alpha the colour's red, then the swizzle
TEST(RawSampler, AnOpaqueBorderReadsMetalsTexelOfTheTexture)
{
    using raw::kBorderOpaqueBlack, raw::kBorderOpaqueWhite;
    EXPECT_EQ(raw::borderTexel(kBGRA8, kIdentity, false, kBorderOpaqueWhite), 0xF);
    EXPECT_EQ(raw::borderTexel(kBGRA8, kIdentity, false, kBorderOpaqueBlack), 0x8);
    EXPECT_EQ(raw::borderTexel(kR8, kIdentity, false, kBorderOpaqueWhite), 0x9) << "red and the missing alpha";
    EXPECT_EQ(raw::borderTexel(kR8, kIdentity, false, kBorderOpaqueBlack), 0x8);
    EXPECT_EQ(raw::borderTexel(kA8, kIdentity, true, kBorderOpaqueWhite), 0x8);
    EXPECT_EQ(raw::borderTexel(kA8, kIdentity, true, kBorderOpaqueBlack), 0x0) << "A8Unorm's alpha is the colour's red";
    const uint8_t alphaFirst[4] = {5, 4, 3, 2};
    EXPECT_EQ(raw::borderTexel(kBGRA8, alphaFirst, false, kBorderOpaqueBlack), 0x1) << "alpha read as red";
    const uint8_t ones[4] = {1, 1, 0, 2};
    EXPECT_EQ(raw::borderTexel(kR8, ones, false, kBorderOpaqueBlack), 0x3);
    EXPECT_EQ(raw::borderTexel(kRGBA8Uint, kIdentity, false, kBorderOpaqueWhite), 0xF | raw::kBorderInteger);
    EXPECT_EQ(raw::borderTexel(kBGRA8, kIdentity, false, 0), 0) << "no opaque border";
    EXPECT_EQ(raw::ownBorderTexel(kBorderOpaqueWhite), raw::borderTexel(kBGRA8, kIdentity, false, kBorderOpaqueWhite));
    EXPECT_EQ(raw::ownBorderTexel(kBorderOpaqueBlack), raw::borderTexel(kBGRA8, kIdentity, false, kBorderOpaqueBlack));
}

// NVK's words for the colour it is given (nvk_sampler.c): BORDER_COLOR_R..A a float or integer one each, and the sRGB
// border's 8-bit red, green and blue, the header's other fields kept
TEST(RawSampler, ABorderTexelIsTheHeadersBorderWords)
{
    uint32_t w[8] = {1, 2, 0x00abcdefu, 0xf0000fffu, 9, 9, 9, 9};
    raw::borderHeader(w, 0x9);
    EXPECT_EQ(w[4], 0x3f800000u);
    EXPECT_EQ(w[5], 0u);
    EXPECT_EQ(w[6], 0u);
    EXPECT_EQ(w[7], 0x3f800000u);
    EXPECT_EQ(w[2], 0xffabcdefu);
    EXPECT_EQ(w[3], 0xf0000fffu);
    raw::borderHeader(w, 0xF | raw::kBorderInteger);
    for (uint32_t c = 4; c < 8; c++)
        EXPECT_EQ(w[c], 1u);
    EXPECT_EQ(w[2], 0x00abcdefu) << "no sRGB border for integer ones";
    raw::borderHeader(w, 0xF);
    EXPECT_EQ(w[3], 0xf0000fffu | 255u << 12 | 255u << 20);
}

// RenderBox's icon sampler (boot 16: normalized, Cubic min and mag, lodAverage)
TEST(RawSampler, CubicSamplerIsTakenWithANearestHeader)
{
    const std::pair<uint64_t, uint64_t> filters[] = {
        {raw::kFilterCubic, raw::kFilterCubic}, {raw::kFilterCubic, 1}, {1, raw::kFilterCubic}};
    for (const auto &[minFilter, magFilter] : filters) {
        raw::MetalSamplerFields m;
        m.minFilter = minFilter;
        m.magFilter = magFilter;
        m.sAddress = raw::kAddressRepeat;
        m.tAddress = raw::kAddressClampToBorderColor;
        raw::SamplerDesc s;
        ASSERT_TRUE(raw::takeSampler(m, s)) << minFilter << " " << magFilter;
        EXPECT_TRUE(s.cubic);
        EXPECT_EQ(s.minFilter, 0);
        EXPECT_EQ(s.magFilter, 0);
        EXPECT_EQ(s.sAddress, raw::kAddressRepeat);
    }
    raw::SamplerDesc linear;
    raw::MetalSamplerFields l;
    l.minFilter = l.magFilter = 1;
    ASSERT_TRUE(raw::takeSampler(l, linear));
    EXPECT_FALSE(linear.cubic);
}

TEST(RawSampler, CubicSamplerOutsideMetalsMeasuredFilterIsRefused)
{
    raw::SamplerDesc s;
    const auto cubic = [] {
        raw::MetalSamplerFields m;
        m.minFilter = m.magFilter = raw::kFilterCubic;
        return m;
    };
    raw::MetalSamplerFields mip = cubic();
    mip.mipFilter = 1;
    EXPECT_FALSE(raw::takeSampler(mip, s));
    raw::MetalSamplerFields anisotropic = cubic();
    anisotropic.maxAnisotropy = 4;
    EXPECT_FALSE(raw::takeSampler(anisotropic, s));
    raw::MetalSamplerFields unnamedBorder = cubic();
    unnamedBorder.borderColor = 3;
    EXPECT_FALSE(raw::takeSampler(unnamedBorder, s));
    for (int axis = 0; axis < 3; axis++) {
        raw::MetalSamplerFields mirrored = cubic();
        (axis == 0   ? mirrored.sAddress
         : axis == 1 ? mirrored.tAddress
                     : mirrored.rAddress) = raw::kAddressMirrorClampToEdge;
        EXPECT_FALSE(raw::takeSampler(mirrored, s)) << axis;
    }
    raw::MetalSamplerFields filter3;
    filter3.minFilter = 3;
    EXPECT_FALSE(raw::takeSampler(filter3, s));
}

}  // namespace
