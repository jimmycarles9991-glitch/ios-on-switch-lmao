// SPDX-License-Identifier: MIT
// raw/runtime/raw_kernel_texture.h: the textures a raw kernel samples in place. metal.float32_filtering's three-level
// R32F, RG32F and RGBA32F textures went to m2v (compute.image) because a mipmapped texture was taken for one sampled
// through an image of its own; its header is its chain's, every level in place.
#include <gtest/gtest.h>

#include "../raw/runtime/raw_kernel_texture.h"

namespace {

struct Texture {
    unsigned tic = 7;
    bool planar = false, ycbcr = false, surfaceView = false, memoryless = false;
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
    EXPECT_TRUE(raw::kernelSamplesInPlace(t));
}

TEST(KernelTexture, ATextureSampledThroughAnImageOfItsOwnIsNot)
{
    const Texture other;
    Texture t;
    EXPECT_TRUE(raw::kernelSamplesInPlace(t));
    t.tic = 0;
    EXPECT_FALSE(raw::kernelSamplesInPlace(t));
    t = Texture();
    t.planar = true;
    EXPECT_FALSE(raw::kernelSamplesInPlace(t));
    t = Texture();
    t.standIn = &other;
    EXPECT_FALSE(raw::kernelSamplesInPlace(t));
    t = Texture();
    t.sampled = &other;
    EXPECT_FALSE(raw::kernelSamplesInPlace(t));
    t.surfaceView = true;
    EXPECT_TRUE(raw::kernelSamplesInPlace(t));
    t = Texture();
    t.memoryless = true;
    EXPECT_FALSE(raw::kernelSamplesInPlace(t));
}

}  // namespace
