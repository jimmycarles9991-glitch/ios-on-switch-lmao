// SPDX-License-Identifier: MIT
// Same-bits attachments on gm20b::Device: a depth texture as a colour attachment and a colour texture as the depth
// attachment, each through a backing of the other class the copy engine fills and drains.
#include "device_forms.h"

namespace {

// A Depth32Float texture as colour 0: an R32Float pool image the copy engine fills from it and drains back into it
TEST_F(DevicePassForms, ADepthTextureAsAColourAttachmentRendersAsItsColourTwinThroughAPoolImage)
{
    gm20b::Texture z;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, false, z));
    gm20b::PassDesc p;
    p.color[0] = kept(z, gm20b::ColorAttachment::Load);
    p.count = 1;
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    const uint64_t target = addressOf(ms, NVB197_SET_COLOR_TARGET_A(0));
    EXPECT_NE(target, z.va);
    EXPECT_EQ(lastOf(ms, NVB197_SET_COLOR_TARGET_FORMAT(0)), gm20b::formatInfo(kR32Float)->rt);
    const auto cs = copies(ms);
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].from, z.va);
    EXPECT_EQ(cs[0].to, target);
    EXPECT_EQ(cs[1].from, target);
    EXPECT_EQ(cs[1].to, z.va);
}

TEST_F(DevicePassForms, AMultisampledDepthTextureAsAColourAttachmentIsRefused)
{
    gm20b::Texture z;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, z));
    gm20b::PassDesc p;
    p.color[0] = kept(z, gm20b::ColorAttachment::Clear);
    p.count = 1;
    std::string why;
    EXPECT_TRUE(pass(p, &why).empty());
    EXPECT_NE(why.find("keeps no bits of"), std::string::npos) << why;
}

// A one-sample RGBA8 texture, block-linear and pitch, as the depth attachment: a Depth32Float backing (ZF32) the copy engine
// fills from the texture's texels and drains back into them
TEST_F(DevicePassForms, AColourTextureOfFourByteTexelsAsTheDepthAttachmentRendersThroughADepth32FloatBacking)
{
    for (const bool tiled : {true, false}) {
        gm20b::Texture colour, asDepth;
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, asDepth, false, tiled));
        ASSERT_EQ(asDepth.blockLinear, tiled);
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.count = 1;
        p.depthStencil.texture = &asDepth;
        std::string why;
        const auto ms = pass(p, &why);
        ASSERT_FALSE(ms.empty()) << why;
        EXPECT_EQ(lastOf(ms, NVB197_SET_ZT_FORMAT), kZF32) << tiled;
        const uint64_t backing = addressOf(ms, NVB197_SET_ZT_A);
        EXPECT_NE(backing, asDepth.va) << tiled;
        const auto cs = copies(ms);
        EXPECT_EQ(copiesBetween(cs, asDepth.va, backing), 1u) << tiled;
        EXPECT_EQ(copiesBetween(cs, backing, asDepth.va), 1u) << tiled;
    }
}

TEST_F(DevicePassForms, AColourTextureOfTwoByteTexelsAsTheDepthAttachmentIsRefused)
{
    gm20b::Texture colour, asDepth;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createTexture(32, 32, 20 /* R16Unorm */, asDepth, false, true));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil.texture = &asDepth;
    std::string why;
    EXPECT_TRUE(pass(p, &why).empty());
    EXPECT_NE(why.find("a depth/stencil attachment of a colour pixel format"), std::string::npos) << why;
}

}  // namespace
