// SPDX-License-Identifier: MIT
// Texture views raw::Device makes (createView, createPartView): the header of each over the part of its texture's memory
// Metal names, as NIL's image of that part reads it (fx_mip.cpp mipChainTic, fx_image.h), and the image a copy or pass
// of the view addresses.
#include "raw_device_harness.h"
#include "../raw/fx/fx_image.h"
#include "../raw/runtime/raw_stencil_bits.h"
#include "../raw/runtime/raw_zeta.h"
#include <thread>

namespace {

constexpr uint32_t kRGBA8 = 70, kDepth16Unorm = 250, kDepth32Float = 252, kDepth32FloatStencil8 = 260,
                   kX32Stencil8 = 261, kASTC4x4 = 204;

uint32_t field(const std::array<uint32_t, 8> &w, unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    for (unsigned b = lo; b <= hi; b++)
        v |= uint64_t(w[b / 32] >> (b % 32) & 1) << (b - lo);
    return uint32_t(v);
}

uint64_t address(const std::array<uint32_t, 8> &w)
{
    return uint64_t(field(w, 63, 41)) << 9 | uint64_t(field(w, 79, 64)) << 32;
}

uint32_t textureType(const std::array<uint32_t, 8> &w)
{
    return field(w, 154, 151);
}

uint32_t minLevel(const std::array<uint32_t, 8> &w)
{
    return field(w, 227, 224);
}

uint32_t maxLevel(const std::array<uint32_t, 8> &w)
{
    return field(w, 231, 228);
}

uint32_t depthMinusOne(const std::array<uint32_t, 8> &w)
{
    return field(w, 189, 176);
}

class DeviceViews : public harness::DeviceTest {
protected:
    std::array<uint32_t, 8> header(const raw::Texture &t) const
    {
        return raw::DeviceHarness::header(dev, t.tic);
    }
};

// MetalResourceTests' view (boot 21's tex.view): level 1 of slice 1 of a mipmapped 2D array, as a 2D texture
TEST_F(DeviceViews, ALevelOfASliceOfAMipmappedArrayIsItsOwnHeader)
{
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(16, 16, 2, kRGBA8, raw::fx::kTexTwoDArray, array, 5));
    ASSERT_NE(array.chain, nullptr);
    const raw::fx::MipChain &c = array.chain->layout;
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, raw::fx::kTexTwoD, 1, 1, 1, 1, view));
    const auto w = header(view);
    EXPECT_EQ(address(w), array.chain->va + c.arrayStride);
    EXPECT_EQ(textureType(w), raw::fx::kTexTwoD);
    EXPECT_EQ(minLevel(w), 1u);
    EXPECT_EQ(maxLevel(w), 1u);
    EXPECT_EQ(depthMinusOne(w), 0u);
    EXPECT_EQ(field(w, 143, 128), 15u) << "the header's extent is level 0's";
    EXPECT_EQ(view.va, array.chain->va + c.arrayStride + c.level[1].offset) << "its image: level 1 of layer 1";
    EXPECT_EQ(view.width, 8u);
    EXPECT_EQ(view.layers, 1u);
    EXPECT_TRUE(view.renderable);
}

TEST_F(DeviceViews, ACubeOfAMipmappedArraysLayersStartsAtItsFirstFace)
{
    raw::Texture array, cube;
    ASSERT_TRUE(dev.createArray(32, 32, 12, kRGBA8, raw::fx::kTexTwoDArray, array, 3));
    const raw::fx::MipChain &c = array.chain->layout;
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, raw::fx::kTexCube, 0, 3, 6, 6, cube));
    const auto w = header(cube);
    EXPECT_EQ(address(w), array.chain->va + 6 * c.arrayStride);
    EXPECT_EQ(textureType(w), raw::fx::kTexCube);
    EXPECT_EQ(depthMinusOne(w), 0u) << "one cube";
    EXPECT_EQ(maxLevel(w), 2u);
    EXPECT_FALSE(cube.renderable);
    raw::Texture past;
    EXPECT_FALSE(dev.createPartView(array, kRGBA8, raw::fx::kTexCube, 0, 3, 7, 6, past)) << "past the last layer";
}

TEST_F(DeviceViews, ALevelRangeOfAVolumeIsAThreeDHeader)
{
    raw::Texture volume, view;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, volume, 3));
    ASSERT_TRUE(dev.createPartView(volume, kRGBA8, raw::fx::kTexThreeD, 1, 2, 0, 1, view));
    const auto w = header(view);
    EXPECT_EQ(address(w), volume.chain->va);
    EXPECT_EQ(textureType(w), raw::fx::kTexThreeD);
    EXPECT_EQ(minLevel(w), 1u);
    EXPECT_EQ(maxLevel(w), 2u);
    EXPECT_EQ(depthMinusOne(w), 7u);
    EXPECT_EQ(view.depth, 4u) << "its image is level 1";
    raw::Texture flat;
    EXPECT_FALSE(dev.createPartView(volume, kRGBA8, raw::fx::kTexTwoD, 1, 1, 0, 1, flat))
        << "Metal views a 3D texture as 3D";
}

TEST_F(DeviceViews, ALineIsViewedAsAOneLayerArray)
{
    raw::Texture line, view;
    ASSERT_TRUE(dev.createLine(64, kRGBA8, line));
    ASSERT_TRUE(dev.createPartView(line, kRGBA8, raw::fx::kTexOneDArray, 0, 1, 0, 1, view));
    const auto w = header(view);
    EXPECT_EQ(address(w), line.va);
    EXPECT_EQ(textureType(w), raw::fx::kTexOneDArray);
    EXPECT_EQ(field(w, 143, 128), 63u);
    EXPECT_EQ(field(w, 101, 99), uint32_t(line.blockHeight)) << "the line's own block height";
}

TEST_F(DeviceViews, ALayerOfADepthArrayIsItsDepthHeaderThere)
{
    raw::Texture depth, slice, array;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, false, depth, 4));
    ASSERT_TRUE(dev.createPartView(depth, kDepth32Float, raw::fx::kTexTwoD, 0, 1, 2, 1, slice));
    auto w = header(slice);
    EXPECT_EQ(address(w), depth.va + 2 * depth.layerBytes);
    EXPECT_EQ(w[0], raw::kZetaFormatInfo[raw::zetaFormatIndex(kDepth32Float)].tic0);
    EXPECT_EQ(textureType(w), raw::fx::kTexTwoD);
    EXPECT_TRUE(slice.zeta);
    ASSERT_TRUE(dev.createPartView(depth, kDepth32Float, raw::fx::kTexTwoDArray, 0, 1, 1, 3, array));
    w = header(array);
    EXPECT_EQ(address(w), depth.va + depth.layerBytes);
    EXPECT_EQ(textureType(w), raw::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 2u);
    raw::Texture rgba;
    EXPECT_FALSE(dev.createPartView(depth, kRGBA8, raw::fx::kTexTwoD, 0, 1, 0, 1, rgba))
        << "a depth texture's own format";
}

TEST_F(DeviceViews, ADepthTexturesFormatViewAndItsStencilView)
{
    raw::Texture depth, same, stencil, other;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32FloatStencil8, 1, true, depth));
    ASSERT_NE(depth.tic, 0u);
    ASSERT_TRUE(dev.createView(depth, kDepth32FloatStencil8, same));
    EXPECT_NE(same.tic, depth.tic);
    EXPECT_EQ(header(same), header(depth));
    ASSERT_TRUE(dev.createView(depth, kX32Stencil8, stencil));
    auto w = header(stencil), d = header(depth);
    EXPECT_EQ(w[0], raw::stencilViewWord0(kDepth32FloatStencil8)) << "NVK's X32_S8X24_UINT";
    for (int i = 1; i < 8; i++)
        EXPECT_EQ(w[i], d[i]) << "word " << i;
    EXPECT_EQ(stencil.format, kDepth32FloatStencil8) << "the zeta tables' format";
    EXPECT_FALSE(stencil.renderable);
    EXPECT_FALSE(dev.createView(depth, kDepth32Float, other)) << "Metal views no depth aspect alone";
    raw::Texture plain;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, plain));
    EXPECT_FALSE(dev.createView(plain, kX32Stencil8, other)) << "no stencil to view";
}

// A block format's header extent is in texels (NIL's extent_px), a view's as its texture's
TEST_F(DeviceViews, AViewOfABlockChainKeepsItsExtentInTexels)
{
    raw::Texture astc, view, part;
    ASSERT_TRUE(dev.createBlockTexture(100, 60, 7, 1, kASTC4x4, raw::fx::kTexTwoD, astc));
    ASSERT_TRUE(dev.createView(astc, kASTC4x4, view));
    auto w = header(view);
    EXPECT_EQ(field(w, 143, 128), 99u);
    EXPECT_EQ(field(w, 175, 160), 59u);
    ASSERT_TRUE(dev.createPartView(astc, kASTC4x4, raw::fx::kTexTwoD, 2, 3, 0, 1, part));
    w = header(part);
    EXPECT_EQ(field(w, 143, 128), 99u);
    EXPECT_EQ(minLevel(w), 2u);
    EXPECT_EQ(maxLevel(w), 4u);
    EXPECT_EQ(header(view)[1], header(astc)[1]);
}

// A view frees its own header alone: its texture's memory, its owned depth and stencil temp stay its texture's
TEST_F(DeviceViews, DestroyingAViewLeavesItsTextureWhole)
{
    raw::Texture depth, slice, stencil, array, part;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32FloatStencil8, 1, true, depth));
    ASSERT_TRUE(dev.createView(depth, kX32Stencil8, stencil));
    ASSERT_TRUE(dev.createPartView(depth, kDepth32FloatStencil8, raw::fx::kTexTwoD, 0, 1, 0, 1, slice));
    ASSERT_TRUE(dev.createArray(16, 16, 2, kRGBA8, raw::fx::kTexTwoDArray, array, 5));
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, raw::fx::kTexTwoD, 1, 1, 1, 1, part));
    const auto depthHeader = header(depth), arrayHeader = header(array);
    const size_t freedBefore = kernel_model::model.freed.size();
    for (raw::Texture *v : {&stencil, &slice, &part})
        dev.destroyTexture(*v);
    EXPECT_EQ(kernel_model::model.freed.size(), freedBefore) << "no memory freed with a view";
    EXPECT_EQ(header(depth), depthHeader);
    EXPECT_EQ(header(array), arrayHeader);
    EXPECT_NE(depth.va, 0u);
    raw::Texture again;
    ASSERT_TRUE(dev.createView(depth, kX32Stencil8, again)) << "the depth texture is still whole";
    EXPECT_EQ(raw::DeviceHarness::header(dev, again.tic)[1], depthHeader[1]);
}

// Metal views a buffer's or a multisampled texture as a one-layer 2D array: NIL's pitch header (TWO_D_NO_MIPMAP), and
// the multisampled image's header as an array
TEST_F(DeviceViews, APitchOrMultisampledTextureIsAOneLayerArrayWhole)
{
    raw::Texture pitch, pitchArray, ms, msArray, part;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, pitch, false, false));
    ASSERT_FALSE(pitch.blockLinear);
    ASSERT_TRUE(dev.createPartView(pitch, kRGBA8, raw::fx::kTexTwoDArray, 0, 1, 0, 1, pitchArray));
    EXPECT_EQ(header(pitchArray), header(pitch));
    ASSERT_TRUE(dev.createStoredMultisample(32, 32, kRGBA8, 4, ms));
    ASSERT_TRUE(dev.createPartView(ms, kRGBA8, raw::fx::kTexTwoDArray, 0, 1, 0, 1, msArray));
    auto w = header(msArray), own = header(ms);
    EXPECT_EQ(textureType(w), raw::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 0u);
    EXPECT_EQ(field(w, 235, 232), field(own, 235, 232)) << "its sample layout";
    EXPECT_EQ(address(w), address(own));
    raw::Texture array;
    ASSERT_TRUE(dev.createArray(32, 32, 2, kRGBA8, raw::fx::kTexTwoDArray, array));
    EXPECT_TRUE(dev.createPartView(array, kRGBA8, raw::fx::kTexTwoD, 0, 1, 1, 1, part));
    EXPECT_FALSE(dev.createPartView(pitch, kRGBA8, raw::fx::kTexTwoDArray, 0, 1, 0, 2, part)) << "one layer";
}

// A sampled depth array or cube has a header of its own (enc.notic: a draw that sampled one had none to bind)
TEST_F(DeviceViews, ASampledDepthArrayOrCubeHasItsOwnHeader)
{
    raw::Texture array, cube, unsampled;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, array, 4, raw::fx::kTexTwoDArray));
    ASSERT_NE(array.tic, 0u);
    auto w = header(array);
    EXPECT_EQ(address(w), array.va);
    EXPECT_EQ(textureType(w), raw::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 3u);
    EXPECT_EQ(w[0], raw::kZetaFormatInfo[raw::zetaFormatIndex(kDepth32Float)].tic0);
    ASSERT_TRUE(dev.createOwnedZeta(16, 16, kDepth32Float, 1, true, cube, 6, raw::fx::kTexCube));
    w = header(cube);
    EXPECT_EQ(textureType(w), raw::fx::kTexCube);
    EXPECT_EQ(depthMinusOne(w), 0u);
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, false, unsampled, 4));
    EXPECT_EQ(unsampled.tic, 0u) << "no ShaderRead usage: nothing samples it";
}

// The array header is writeTic's for an owned depth array: one over NVK's image the texels moved into (moveOwnedZeta)
// is the array's too
TEST_F(DeviceViews, ADepthArraysHeaderSurvivesItsMove)
{
    raw::Texture array;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, array, 4, raw::fx::kTexTwoDArray));
    EXPECT_EQ(textureType(header(array)), raw::fx::kTexTwoDArray);
    raw::ZetaMemory m;
    m.va = 0x7000000000;
    m.bytes = array.layerBytes * 4;
    m.layerBytes = array.layerBytes;
    m.pitch = array.pitch;
    m.bpp = array.bpp;
    m.blockHeight = array.blockHeight;
    m.tiled = true;
    const char *why = nullptr;
    ASSERT_TRUE(dev.moveOwnedZeta(array, m, &why)) << why;
    ASSERT_NE(array.tic, 0u);
    const auto w = header(array);
    EXPECT_EQ(address(w), m.va);
    EXPECT_EQ(textureType(w), raw::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 3u);
}

TEST_F(DeviceViews, ADepthChainsSelectedLevelHeaderBelongsToTheChain)
{
    for (uint32_t format : {kDepth16Unorm, kDepth32Float, kDepth32FloatStencil8}) {
        SCOPED_TRACE(format);
        raw::Texture depth, part;
        ASSERT_TRUE(dev.createOwnedZeta(64, 32, format, 1, false, depth, 1, raw::fx::kTexTwoD, 4));
        ASSERT_EQ(depth.tic, 0u);
        ASSERT_TRUE(raw::chainLevelImage(depth, 2, part));
        const uint32_t tic = dev.attachmentTic(depth, part, 2, 0);
        ASSERT_NE(tic, 0u);
        const auto w = raw::DeviceHarness::header(dev, tic);
        EXPECT_EQ(w[0], raw::kZetaFormatInfo[raw::zetaFormatIndex(format)].tic0);
        EXPECT_EQ(address(w), depth.chain->va);
        EXPECT_EQ(minLevel(w), 2u);
        EXPECT_EQ(maxLevel(w), 2u);
        EXPECT_EQ(textureType(w), raw::fx::kTexTwoD);
        EXPECT_EQ(field(w, 143, 128) + 1, 64u);
        EXPECT_EQ(field(w, 175, 160) + 1, 32u);
        for (int i = 0; i < 16; ++i)
            EXPECT_EQ(dev.attachmentTic(depth, part, 2, 0), tic);
        EXPECT_EQ(depth.tic, 0u);
        dev.destroyTexture(depth);
        EXPECT_EQ(raw::DeviceHarness::header(dev, tic), raw::DeviceHarness::header(dev, 0));
        raw::Texture replacement;
        ASSERT_TRUE(dev.createOwnedZeta(32, 16, format, 1, false, replacement, 1, raw::fx::kTexTwoD, 3));
        EXPECT_EQ(dev.levelTic(replacement, 1), tic);
        const auto next = raw::DeviceHarness::header(dev, tic);
        EXPECT_EQ(minLevel(next), 1u);
        EXPECT_EQ(maxLevel(next), 1u);
        EXPECT_EQ(field(next, 143, 128) + 1, 32u);
        EXPECT_NE(next, w);
        dev.destroyTexture(replacement);
    }
}

TEST_F(DeviceViews, ASelectedDepthHeaderIsFreedWithItsArrayViewAndNeverAnswersForItsReplacement)
{
    raw::Texture parent, view, part;
    ASSERT_TRUE(dev.createOwnedZeta(64, 32, kDepth16Unorm, 1, false, parent, 3, raw::fx::kTexTwoDArray, 4));
    ASSERT_TRUE(dev.createPartView(parent, kDepth16Unorm, raw::fx::kTexTwoD, 1, 2, 2, 1, view));
    const uint32_t own = view.tic;
    const auto ownHeader = header(view);
    ASSERT_TRUE(raw::chainLevelImage(parent, 2, part));
    part = raw::layersFrom(part, 2);
    part.layers = 1;
    const uint32_t tic = dev.attachmentTic(view, part, 1, 0);
    ASSERT_NE(tic, 0u);
    EXPECT_NE(tic, own);
    auto w = raw::DeviceHarness::header(dev, tic);
    EXPECT_EQ(address(w),
              parent.chain->va + 2 * parent.chain->layout.arrayStride + parent.chain->layout.level[2].offset);
    EXPECT_EQ(w[0], raw::kZetaFormatInfo[raw::zetaFormatIndex(kDepth16Unorm)].tic0);
    EXPECT_EQ(minLevel(w), 0u);
    EXPECT_EQ(maxLevel(w), 0u);
    EXPECT_EQ(field(w, 143, 128) + 1, part.width);
    EXPECT_EQ(field(w, 175, 160) + 1, part.height);
    EXPECT_EQ(field(w, 101, 99), part.blockHeight);
    EXPECT_EQ(depthMinusOne(w), 0u);
    EXPECT_EQ(textureType(w), raw::fx::kTexTwoD);
    EXPECT_EQ(dev.attachmentTic(view, part, 1, 0), tic);
    EXPECT_EQ(header(view), ownHeader);
    EXPECT_EQ(parent.tic, 0u);
    dev.destroyTexture(view);
    EXPECT_EQ(raw::DeviceHarness::header(dev, own), raw::DeviceHarness::header(dev, 0));
    EXPECT_EQ(raw::DeviceHarness::header(dev, tic), raw::DeviceHarness::header(dev, 0));
    raw::Texture occupying, replacement;
    ASSERT_TRUE(dev.createPartView(parent, kDepth16Unorm, raw::fx::kTexTwoD, 0, 1, 0, 1, occupying));
    ASSERT_EQ(occupying.tic, tic);
    ASSERT_TRUE(dev.createPartView(parent, kDepth16Unorm, raw::fx::kTexTwoD, 1, 2, 1, 1, replacement));
    ASSERT_EQ(replacement.tic, own);
    ASSERT_TRUE(raw::chainLevelImage(parent, 2, part));
    part = raw::layersFrom(part, 1);
    part.layers = 1;
    const uint32_t next = dev.attachmentTic(replacement, part, 1, 0);
    ASSERT_NE(next, 0u);
    EXPECT_NE(next, tic);
    w = raw::DeviceHarness::header(dev, next);
    EXPECT_EQ(address(w), part.va);
    EXPECT_EQ(dev.attachmentTic(replacement, part, 1, 0), next);
    dev.destroyTexture(replacement);
    dev.destroyTexture(occupying);
    dev.destroyTexture(parent);
}

TEST_F(DeviceViews, ConcurrentDepthPartAdmissionPublishesOneResourceOwnedHeader)
{
    raw::Texture depth, part;
    ASSERT_TRUE(dev.createOwnedZeta(64, 32, kDepth16Unorm, 1, false, depth, 1, raw::fx::kTexTwoD, 4));
    ASSERT_EQ(depth.tic, 0u);
    part = raw::layersFrom(depth, 0);
    std::array<uint32_t, 8> tics = {};
    std::array<std::thread, 8> threads;
    for (size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([&, i] {
            tics[i] = dev.attachmentTic(depth, part, 0, 0);
        });
    for (auto &thread : threads)
        thread.join();
    ASSERT_NE(depth.tic, 0u);
    ASSERT_NE(tics[0], 0u);
    EXPECT_NE(tics[0], depth.tic);
    for (uint32_t tic : tics)
        EXPECT_EQ(tic, tics[0]);
    const auto w = raw::DeviceHarness::header(dev, tics[0]);
    EXPECT_EQ(address(w), part.va);
    EXPECT_EQ(minLevel(w), 0u);
    EXPECT_EQ(maxLevel(w), 0u);
    const uint32_t own = depth.tic;
    dev.destroyTexture(depth);
    EXPECT_EQ(raw::DeviceHarness::header(dev, own), raw::DeviceHarness::header(dev, 0));
    EXPECT_EQ(raw::DeviceHarness::header(dev, tics[0]), raw::DeviceHarness::header(dev, 0));
}

}  // namespace
