// SPDX-License-Identifier: MIT
// raw/runtime/raw_kernel_texture.h: how a raw kernel reaches each texture it binds. metal.float32_filtering's
// three-level R32F, RG32F and RGBA32F textures went to m2v (compute.image) because a mipmapped texture was taken for
// one sampled through an image of its own; its header is its chain's, every level in place. mediaanalysisd's Core Image
// kernel bound an RGBA8 texture whose rows are 16 bytes off 32 (boot 21: 326 command buffers on m2v): it is reached
// through its shadow image, not refused.
#include <gtest/gtest.h>

#include "../raw/runtime/raw_kernel_texture.h"

namespace {

using raw::KernelTextureForm;

struct Texture {
    unsigned tic = 7;
    bool planar = false, ycbcr = false, surfaceView = false, memoryless = false, xrPlanes = false;
    const Texture *standIn = nullptr, *sampled = nullptr;
    const void *chain = nullptr;

    bool converted() const
    {
        return planar || ycbcr;
    }
};

// gpu.ca.glass_split's compute read of a 'b3a8' surface's RGB10A8_2P_XR10 texture: m2v compiles the kernel for the format
// it is asked at, and at 550 reads one R32G32_UINT image of the word and the alpha that no header of the two planes is,
// so admission refused it (compute.image b 2). Asked at its stand-in's RGBA16Float, the kernel's view is the stand-in's.
TEST(KernelTexture, ATwoPlaneTextureIsCompiledForAsItsStandIn)
{
    const Texture standIn;
    Texture t;
    t.xrPlanes = true;
    t.standIn = &standIn;
    EXPECT_EQ(raw::kernelTextureFormat(t, 550), raw::kXrStandInFormat);
    t.standIn = nullptr;
    EXPECT_EQ(raw::kernelTextureFormat(t, 550), 550u) << "no stand-in: refused as it was";
    Texture codes;
    codes.standIn = &standIn;
    EXPECT_EQ(raw::kernelTextureFormat(codes, 554), 554u) << "BGR10_XR's codes are an image of their own";
    EXPECT_EQ(raw::kernelTextureFormat(Texture(), 80), 80u);
}

TEST(KernelTexture, AMipmappedTextureIsSampledInPlace)
{
    Texture t;
    const int chain = 0;
    t.chain = &chain;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::InPlace);
}

TEST(KernelTexture, RowsTheHeaderCannotAddressGoThroughTheShadow)
{
    const Texture image;
    Texture t;
    t.sampled = &image;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Shadow);
    t.surfaceView = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Shadow);
}

TEST(KernelTexture, EveryOtherTermHasItsForm)
{
    const Texture other;
    Texture t;
    t.planar = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Converted);
    t = Texture();
    t.ycbcr = true;
    t.sampled = &other;  // a plane's pool image is the conversion's, not the kernel's
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::Converted);
    t = Texture();
    t.standIn = &other;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::StandIn);
    t = Texture();
    t.tic = 0;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::NoHeader);
    t = Texture();
    t.memoryless = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::NoHeader);
    t = Texture();
    t.xrPlanes = true;
    t.standIn = &other;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::TwoPlanes);
}

// A memoryless RGB10A8_2P_XR10 texture's planes have no memory a kernel could reach (raw_xr_texture.cpp
// createXrMemoryless): a load reads header 0, a store fails its command buffer, as any memoryless texture's
TEST(KernelTexture, AMemorylessTwoPlaneTextureHasNoHeader)
{
    Texture t;
    t.xrPlanes = true;
    t.memoryless = true;
    EXPECT_EQ(raw::kernelTextureForm(t), KernelTextureForm::NoHeader);
}

}  // namespace
