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

}  // namespace
