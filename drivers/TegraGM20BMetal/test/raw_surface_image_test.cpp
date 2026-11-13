// SPDX-License-Identifier: MIT
// raw::Device::createSurfaceImage over the kernel model (raw_device_harness.h): a shared texture over its IOSurface's pages
// is the texture the raw path makes of the same descriptor in memory of its own, field for field and header word for
// header word but its address, which is the import's, bound 0xFE and never a kernel-tiled surface's.
#include "raw_device_harness.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdlib>
#include <functional>
#include <unistd.h>

namespace {

constexpr uint32_t kRGBA8 = 70, kASTC4x4 = 204, kSurfaceId = 7;

raw::SurfaceImageFacts facts(uint32_t type, uint32_t width, uint32_t height, uint32_t format)
{
    const raw::FormatInfo *f = raw::formatInfo(format);
    raw::SurfaceImageFacts d;
    d.type = type;
    d.width = width;
    d.height = height;
    d.bpp = f->bpp;
    d.blockW = f->blockW;
    d.blockH = f->blockH;
    d.target = f->rt;
    return d;
}

uint64_t headerAddress(const uint32_t *w)
{
    return (uint64_t(w[1] >> 9) | uint64_t(w[2] & 0xffff) << 23) << 9;
}

class SurfaceImage : public harness::DeviceTest {
protected:
    void *pages = nullptr;
    CFTypeRef owner = nullptr;
    raw::Texture made;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_EQ(posix_memalign(&pages, size_t(getpagesize()), 4 << 20), 0);
        owner = CFDataCreate(nullptr, nullptr, 0);
    }

    void TearDown() override
    {
        dev.destroyTexture(made);
        CFRelease(owner);
        free(pages);
    }

    // The shared texture of `d` (format `format`) beside `privately`, the raw path's own texture of the same descriptor
    void same(const raw::SurfaceImageFacts &d, uint32_t format, raw::SurfaceImage::Kind kind,
              const std::function<bool(raw::Texture &)> &privately)
    {
        raw::SurfaceImage image;
        ASSERT_EQ(raw::surfaceImage(d, image), nullptr);
        ASSERT_EQ(image.kind, kind);
        ASSERT_LE(image.layout.bytes, uint64_t(4 << 20));
        raw::Texture own;
        ASSERT_TRUE(privately(own));
        ASSERT_TRUE(dev.createSurfaceImage(kSurfaceId, pages, image.layout.bytes, owner, image, d.width, d.height,
                                           format, made));
        ASSERT_NE(made.import, nullptr);
        EXPECT_EQ(made.import->kind, TGM_LAYOUT_KIND);
        EXPECT_FALSE(made.import->kernelTiled());
        EXPECT_EQ(made.import->id, kSurfaceId);
        EXPECT_EQ(made.va, made.import->bo.va);
        EXPECT_EQ(made.cpu, static_cast<uint8_t *>(pages));
        EXPECT_EQ(made.storageRange().bytes, image.layout.bytes);
        EXPECT_EQ(made.storageRange().hostVisible, false);
        EXPECT_EQ(made.width, own.width);
        EXPECT_EQ(made.height, own.height);
        EXPECT_EQ(made.pitch, own.pitch);
        EXPECT_EQ(made.bpp, own.bpp);
        EXPECT_EQ(made.blockLinear, own.blockLinear);
        EXPECT_EQ(made.blockHeight, own.blockHeight);
        EXPECT_EQ(made.layerBytes, own.layerBytes);
        EXPECT_EQ(made.type, own.type);
        EXPECT_EQ(made.depth, own.depth);
        EXPECT_EQ(made.blockDepth, own.blockDepth);
        EXPECT_EQ(made.layers, own.layers);
        EXPECT_EQ(made.samples, own.samples);
        EXPECT_EQ(made.renderable, own.renderable);
        EXPECT_EQ(made.blockW, own.blockW);
        EXPECT_EQ(made.texelWidth, own.texelWidth);
        EXPECT_EQ(made.chain != nullptr, own.chain != nullptr);
        if (made.chain && own.chain) {
            EXPECT_EQ(made.chain->layout.bytes, own.chain->layout.bytes);
            EXPECT_EQ(made.chain->layout.levels, own.chain->layout.levels);
            EXPECT_EQ(made.chain->va, made.va);
            EXPECT_EQ(made.chain->fmt2d, own.chain->fmt2d);
        }
        ASSERT_NE(made.tic, 0u);
        uint32_t mine[8], theirs[8];
        memcpy(mine, dev.headerWords(made.tic), 32);
        memcpy(theirs, dev.headerWords(own.tic), 32);
        EXPECT_EQ(headerAddress(mine), made.va);
        EXPECT_EQ(headerAddress(theirs), own.va);
        for (uint32_t *w : {mine, theirs}) {
            w[1] &= 0x1ff;
            w[2] &= 0xffff0000;
        }
        for (int i = 0; i < 8; i++)
            EXPECT_EQ(mine[i], theirs[i]) << "header word " << i;
        dev.destroyTexture(own);
    }
};

TEST_F(SurfaceImage, AMipChainIsCreateMipTexturesChain)
{
    raw::SurfaceImageFacts d = facts(raw::kType2D, 64, 48, kRGBA8);
    d.levels = 7;
    same(d, kRGBA8, raw::SurfaceImage::Chain, [&](raw::Texture &t) {
        return dev.createMipTexture(64, 48, kRGBA8, 7, t);
    });
}

TEST_F(SurfaceImage, ArraysAndCubesAreCreateArraysLayers)
{
    raw::SurfaceImageFacts d = facts(raw::kType2DArray, 40, 24, kRGBA8);
    d.arrayLength = 3;
    same(d, kRGBA8, raw::SurfaceImage::Layers, [&](raw::Texture &t) {
        return dev.createArray(40, 24, 3, kRGBA8, raw::fx::kTexTwoDArray, t);
    });
    dev.destroyTexture(made);
    d = facts(raw::kTypeCube, 32, 32, kRGBA8);
    same(d, kRGBA8, raw::SurfaceImage::Layers, [&](raw::Texture &t) {
        return dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, t);
    });
    EXPECT_EQ(made.layers, 6u);
    dev.destroyTexture(made);
    d = facts(raw::kTypeCubeArray, 16, 16, kRGBA8);
    d.arrayLength = 2;
    same(d, kRGBA8, raw::SurfaceImage::Layers, [&](raw::Texture &t) {
        return dev.createArray(16, 16, 12, kRGBA8, raw::fx::kTexCubeArray, t);
    });
    dev.destroyTexture(made);
    d = facts(raw::kType1DArray, 100, 1, kRGBA8);
    d.arrayLength = 4;
    same(d, kRGBA8, raw::SurfaceImage::Layers, [&](raw::Texture &t) {
        return dev.createArray(100, 1, 4, kRGBA8, raw::fx::kTexOneDArray, t);
    });
    EXPECT_EQ(made.layers, 4u);
}

TEST_F(SurfaceImage, MipmappedArraysCubesAndVolumesAreTheirChains)
{
    raw::SurfaceImageFacts d = facts(raw::kTypeCubeArray, 32, 32, kRGBA8);
    d.arrayLength = 2;
    d.levels = 6;
    same(d, kRGBA8, raw::SurfaceImage::Layers, [&](raw::Texture &t) {
        return dev.createArray(32, 32, 12, kRGBA8, raw::fx::kTexCubeArray, t, 6);
    });
    ASSERT_NE(made.chain, nullptr);
    EXPECT_EQ(made.chain->layout.layers, 12u);
    dev.destroyTexture(made);
    d = facts(raw::kType2DArray, 40, 24, kRGBA8);
    d.arrayLength = 3;
    d.levels = 4;
    same(d, kRGBA8, raw::SurfaceImage::Layers, [&](raw::Texture &t) {
        return dev.createArray(40, 24, 3, kRGBA8, raw::fx::kTexTwoDArray, t, 4);
    });
    dev.destroyTexture(made);
    for (const bool rendered : {false, true}) {
        d = facts(raw::kType3D, 20, 12, kRGBA8);
        d.depth = 9;
        d.levels = 3;
        d.rendered = rendered;
        same(d, kRGBA8, raw::SurfaceImage::Volume, [&](raw::Texture &t) {
            return dev.createVolume(20, 12, 9, kRGBA8, rendered, t, 3);
        });
        ASSERT_NE(made.chain, nullptr);
        EXPECT_EQ(made.chain->layout.depth, 9u);
        dev.destroyTexture(made);
    }
}

TEST_F(SurfaceImage, AVolumeIsCreateVolumesImageEitherWay)
{
    for (const bool rendered : {false, true}) {
        raw::SurfaceImageFacts d = facts(raw::kType3D, 20, 12, kRGBA8);
        d.depth = 9;
        d.rendered = rendered;
        same(d, kRGBA8, raw::SurfaceImage::Volume, [&](raw::Texture &t) {
            return dev.createVolume(20, 12, 9, kRGBA8, rendered, t);
        });
        dev.destroyTexture(made);
    }
}

TEST_F(SurfaceImage, SamplesAreCreateStoredMultisamplesGrid)
{
    for (const uint32_t samples : {2u, 4u}) {
        raw::SurfaceImageFacts d = facts(raw::kType2DMultisample, 30, 18, kRGBA8);
        d.samples = samples;
        same(d, kRGBA8, raw::SurfaceImage::Samples, [&](raw::Texture &t) {
            return dev.createStoredMultisample(30, 18, kRGBA8, samples, t);
        });
        EXPECT_EQ(made.samples, samples);
        dev.destroyTexture(made);
    }
}

TEST_F(SurfaceImage, ALineIsCreateLinesRow)
{
    same(facts(raw::kType1D, 300, 1, kRGBA8), kRGBA8, raw::SurfaceImage::Line, [&](raw::Texture &t) {
        return dev.createLine(300, kRGBA8, t);
    });
}

TEST_F(SurfaceImage, BlocksAreTheNoCopyTexturesImage)
{
    void *bytes = nullptr;
    ASSERT_EQ(posix_memalign(&bytes, size_t(getpagesize()), 4 << 20), 0);
    for (const uint32_t layers : {1u, 3u}) {
        raw::SurfaceImageFacts d = facts(layers > 1 ? raw::kType2DArray : raw::kType2D, 100, 60, kASTC4x4);
        d.levels = 7;
        d.arrayLength = layers;
        raw::fx::MipChain c;
        ASSERT_TRUE(raw::fx::blockChainLayout(100, 60, 7, layers, 4, 4, 16, c));
        same(d, kASTC4x4, raw::SurfaceImage::Blocks, [&](raw::Texture &t) {
            return dev.createBlockTextureOver(bytes, 4 << 20, 100, 60, kASTC4x4, c, t);
        });
        EXPECT_EQ(made.chain->layout.levels, 7u);
        dev.destroyTexture(made);
    }
    free(bytes);
}

TEST_F(SurfaceImage, NoTextureOverAPlainImageOrTooLittleMemory)
{
    raw::SurfaceImage image;
    ASSERT_EQ(raw::surfaceImage(facts(raw::kType2D, 64, 64, kRGBA8), image), nullptr);
    EXPECT_EQ(image.kind, raw::SurfaceImage::Plain);
    EXPECT_FALSE(dev.createSurfaceImage(kSurfaceId, pages, 1 << 20, owner, image, 64, 64, kRGBA8, made));
    raw::SurfaceImageFacts d = facts(raw::kType2DArray, 64, 64, kRGBA8);
    d.arrayLength = 2;
    ASSERT_EQ(raw::surfaceImage(d, image), nullptr);
    EXPECT_FALSE(dev.createSurfaceImage(kSurfaceId, pages, image.layout.bytes - 1, owner, image, 64, 64, kRGBA8, made));
    EXPECT_FALSE(dev.createSurfaceImage(0, pages, image.layout.bytes, owner, image, 64, 64, kRGBA8, made));
    EXPECT_FALSE(dev.createSurfaceImage(kSurfaceId, static_cast<uint8_t *>(pages) + 512, image.layout.bytes, owner,
                                        image, 64, 64, kRGBA8, made));
}

}  // namespace
