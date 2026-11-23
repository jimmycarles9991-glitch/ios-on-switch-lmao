// SPDX-License-Identifier: MIT
// renderTargetArrayLength past an attachment's layers on gm20b::Device: each shorter attachment renders through a backing of
// the pass's layers, its own copied in and back, the rest held as an M4 Max tests them.
#include "device_forms.h"
#include "../mme.h"

namespace {

// renderTargetArrayLength 4 over a colour of 4 layers and a loaded one of 2: an M4 Max draws all four layers of the first and
// the two the second has (m2v-pass-forms bitsprobe layers). The second renders through a 4-layer backing, its two layers
// copied in and back and nothing past them
TEST_F(DevicePassForms, AColourOfFewerLayersThanThePassRendersThroughABackingAndKeepsItsOwnLayersAlone)
{
    gm20b::Texture four, two;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, gm20b::fx::kTexTwoDArray, four));
    ASSERT_TRUE(dev.createArray(32, 32, 2, kRGBA8, gm20b::fx::kTexTwoDArray, two));
    gm20b::PassDesc p;
    p.color[0] = kept(four, gm20b::ColorAttachment::Clear);
    p.color[1] = kept(two, gm20b::ColorAttachment::Load);
    p.count = 2;
    p.layers = 4;
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    EXPECT_EQ(addressOf(ms, NVB197_SET_COLOR_TARGET_A(0)), four.va);
    EXPECT_EQ(lastOf(ms, NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(0)), 4u);
    const uint64_t backing = addressOf(ms, NVB197_SET_COLOR_TARGET_A(1));
    EXPECT_NE(backing, two.va);
    EXPECT_EQ(lastOf(ms, NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(1)), 4u);
    const auto cs = copies(ms);
    for (uint32_t l = 0; l < 2; l++) {
        EXPECT_EQ(std::count_if(cs.begin(), cs.end(),
                                [&](const Copy &c) {
                                    return c.from == two.va + l * two.layerBytes;
                                }),
                  1)
            << "layer " << l << " copied in once";
        EXPECT_EQ(std::count_if(cs.begin(), cs.end(),
                                [&](const Copy &c) {
                                    return c.to == two.va + l * two.layerBytes;
                                }),
                  1)
            << "layer " << l << " copied back once";
    }
    EXPECT_EQ(touching(cs, two.va, 2 * two.layerBytes), 4u) << "nothing past its two layers";
    EXPECT_EQ(touching(cs, four.va, 4 * four.layerBytes), 0u) << "the four-layer colour renders in place";
}

// The same with a depth of 2 layers: the zeta renders through a backing of the pass's 4 (ZT_SIZE_C), its own layers in and out
TEST_F(DevicePassForms, ADepthOfFewerLayersThanThePassRendersThroughAZetaBackingOfThePasss)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, gm20b::fx::kTexTwoDArray, colour));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, false, zeta, 2));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.layers = 4;
    p.depthStencil.texture = &zeta;
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    const uint64_t backing = addressOf(ms, NVB197_SET_ZT_A);
    EXPECT_NE(backing, zeta.va);
    EXPECT_EQ(lastOf(ms, NVB197_SET_ZT_SIZE_C), 4u);
    EXPECT_EQ(lastOf(ms, NVB197_SET_ZT_FORMAT), gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(kDepth32Float)].zt);
    const auto cs = copies(ms);
    EXPECT_EQ(copiesBetween(cs, zeta.va, backing), 1u) << "layer 0 in";
    EXPECT_EQ(copiesBetween(cs, backing, zeta.va), 1u) << "layer 0 out";
    EXPECT_EQ(touching(cs, zeta.va, 2 * zeta.layerBytes), 4u) << "its two layers in and out, nothing past them";
}

// The backing's layers past a shorter depth's own, which no copy fills, hold what an M4 Max tests there
// (m2v-pass-forms/probe/layerprobe.mm): loaded, a float depth above 1.0 (2.0), Depth16Unorm's 1.0, stencil 0xff; DontCare,
// the pass's clear values. One clear over those layers alone, from the first past the zeta's
TEST_F(DevicePassForms, ADepthsLayersPastItsOwnHoldWhatAppleTestsThere)
{
    struct Case {
        uint32_t format;
        bool loaded;
        uint32_t depthBits, stencil, aspects;
    };

    const Case cases[] = {{gm20b::kZetaFormat, true, 0x40000000, 0xff, 3},
                          {kDepth16, true, 0x3f800000, ~0u, 1},
                          {kDepth32Float, false, 0x3f400000, ~0u, 1}};
    for (const Case &k : cases) {
        gm20b::Texture colour, zeta;
        ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, gm20b::fx::kTexTwoDArray, colour));
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, k.format, 1, false, zeta, 2));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.count = 1;
        p.layers = 4;
        p.depthStencil = {&zeta, false, false, 0.75f, 9};
        p.depthStencil.loaded = k.loaded;
        std::string why;
        const auto ms = pass(p, &why);
        ASSERT_FALSE(ms.empty()) << why;
        const uint32_t call = NVB197_CALL_MME_MACRO(gm20b::mme::kClearLayers);
        uint32_t depth = ~0u, stencil = ~0u;
        bool past = false;
        for (size_t i = 0; i < ms.size(); i++) {
            const Method &x = ms[i];
            if (x.subc != gm20b::SUBC_3D)
                continue;
            if (x.method == NVB197_SET_Z_CLEAR_VALUE)
                depth = x.value;
            else if (x.method == NVB197_SET_STENCIL_CLEAR_VALUE)
                stencil = x.value;
            else if (x.method == call && (x.value >> 10) == 2) {
                past = true;
                EXPECT_EQ(x.value & 3, k.aspects) << k.format;
                ASSERT_LT(i + 1, ms.size());
                EXPECT_EQ(ms[i + 1].value, 2u) << "layers 2 and 3";
                EXPECT_EQ(depth, k.depthBits) << k.format;
                if (k.aspects & 2)
                    EXPECT_EQ(stencil, k.stencil);
            }
        }
        EXPECT_TRUE(past) << k.format << (k.loaded ? " loaded" : " DontCare");
    }
}

}  // namespace
