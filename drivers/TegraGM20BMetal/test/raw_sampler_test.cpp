// SPDX-License-Identifier: MIT
// raw/runtime/raw_sampler.h: the samplers the raw path takes, as m2v samples them on NVK (metal2vulkan-runtime sampler.rs
// taken_descriptor and metal_anisotropy; metal2vulkan metal/sampler.rs vk_sampler), and pixel coordinates as NVK's
// unnormalized samplers take them. Their headers against NVK's are raw/test/samplercheck's.
#include "../raw/runtime/raw_sampler.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

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

TEST(RawSampler, WhatTheHeaderDoesNotExpressIsRefused)
{
    raw::SamplerDesc s;
    raw::MetalSamplerFields compare;
    compare.compareFunction = 1;
    EXPECT_FALSE(raw::takeSampler(compare, s));
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

TEST(RawSampler, OnlyATransparentBlackBorderIsTaken)
{
    raw::SamplerDesc s;
    for (uint64_t mode : {raw::kAddressClampToZero, raw::kAddressClampToBorderColor}) {
        for (uint64_t color = 0; color < 8; color++) {
            raw::MetalSamplerFields m;
            m.tAddress = mode;
            m.borderColor = color;
            const bool transparent = (color & 3) == 0 || (color & 3) == 3;
            EXPECT_EQ(raw::takeSampler(m, s), transparent) << mode << " " << color;
        }
    }
    raw::MetalSamplerFields noBorder;
    noBorder.borderColor = 2;
    EXPECT_TRUE(raw::takeSampler(noBorder, s));
}

}  // namespace
