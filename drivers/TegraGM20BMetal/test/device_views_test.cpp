// SPDX-License-Identifier: MIT
// Texture views gm20b::Device makes (createView, createPartView): the header of each over the part of its texture's memory
// Metal names, as NIL's image of that part reads it (fx_mip.cpp mipChainTic, fx_image.h), and the image a copy or pass
// of the view addresses.
#include "device_views_harness.h"
#include "../fx/fx_image.h"
#include "../stencil_bits.h"
#include "../zeta.h"
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {

using namespace views;

// MetalResourceTests' view (boot 21's tex.view): level 1 of slice 1 of a mipmapped 2D array, as a 2D texture
TEST_F(DeviceViews, ALevelOfASliceOfAMipmappedArrayIsItsOwnHeader)
{
    gm20b::Texture array, view;
    ASSERT_TRUE(dev.createArray(16, 16, 2, kRGBA8, gm20b::fx::kTexTwoDArray, array, 5));
    ASSERT_NE(array.chain, nullptr);
    const gm20b::fx::MipChain &c = array.chain->layout;
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, gm20b::fx::kTexTwoD, 1, 1, 1, 1, view));
    const auto w = header(view);
    EXPECT_EQ(address(w), array.chain->va + c.arrayStride);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoD);
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
    gm20b::Texture array, cube;
    ASSERT_TRUE(dev.createArray(32, 32, 12, kRGBA8, gm20b::fx::kTexTwoDArray, array, 3));
    const gm20b::fx::MipChain &c = array.chain->layout;
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, gm20b::fx::kTexCube, 0, 3, 6, 6, cube));
    const auto w = header(cube);
    EXPECT_EQ(address(w), array.chain->va + 6 * c.arrayStride);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexCube);
    EXPECT_EQ(depthMinusOne(w), 0u) << "one cube";
    EXPECT_EQ(maxLevel(w), 2u);
    EXPECT_FALSE(cube.renderable);
    gm20b::Texture past;
    EXPECT_FALSE(dev.createPartView(array, kRGBA8, gm20b::fx::kTexCube, 0, 3, 7, 6, past)) << "past the last layer";
}

TEST_F(DeviceViews, ArrayViewTicReinterpretsCubeViewAsSixLayerTwoDArrayAndFreesWithIt)
{
    gm20b::Texture array, cube;
    ASSERT_TRUE(dev.createArray(32, 32, 12, kRGBA8, gm20b::fx::kTexTwoDArray, array, 3));
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, gm20b::fx::kTexCube, 1, 2, 6, 6, cube));
    const uint32_t tic = dev.arrayViewTic(cube);
    ASSERT_NE(tic, 0u);
    EXPECT_NE(tic, cube.tic);
    EXPECT_EQ(dev.arrayViewTic(cube), tic);
    const auto w = gm20b::DeviceHarness::header(dev, tic);
    const auto own = header(cube);
    EXPECT_EQ(address(w), address(own));
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 5u);
    EXPECT_EQ(minLevel(w), 1u);
    EXPECT_EQ(maxLevel(w), 2u);
    dev.destroyTexture(cube);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, tic), gm20b::DeviceHarness::header(dev, 0));
    dev.destroyTexture(array);
}

TEST_F(DeviceViews, ALevelRangeOfAVolumeIsAThreeDHeader)
{
    gm20b::Texture volume, view;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, volume, 3));
    ASSERT_TRUE(dev.createPartView(volume, kRGBA8, gm20b::fx::kTexThreeD, 1, 2, 0, 1, view));
    const auto w = header(view);
    EXPECT_EQ(address(w), volume.chain->va);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexThreeD);
    EXPECT_EQ(minLevel(w), 1u);
    EXPECT_EQ(maxLevel(w), 2u);
    EXPECT_EQ(depthMinusOne(w), 7u);
    EXPECT_EQ(view.depth, 4u) << "its image is level 1";
    gm20b::Texture flat;
    EXPECT_FALSE(dev.createPartView(volume, kRGBA8, gm20b::fx::kTexTwoD, 1, 1, 0, 1, flat))
        << "Metal views a 3D texture as 3D";
}

TEST_F(DeviceViews, ALineIsViewedAsAOneLayerArray)
{
    gm20b::Texture line, view;
    ASSERT_TRUE(dev.createLine(64, kRGBA8, line));
    ASSERT_TRUE(dev.createPartView(line, kRGBA8, gm20b::fx::kTexOneDArray, 0, 1, 0, 1, view));
    const auto w = header(view);
    EXPECT_EQ(address(w), line.va);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexOneDArray);
    EXPECT_EQ(field(w, 143, 128), 63u);
    EXPECT_EQ(field(w, 101, 99), uint32_t(line.blockHeight)) << "the line's own block height";
}

TEST_F(DeviceViews, ALayerOfADepthArrayIsItsDepthHeaderThere)
{
    gm20b::Texture depth, slice, array;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, false, depth, 4));
    ASSERT_TRUE(dev.createPartView(depth, kDepth32Float, gm20b::fx::kTexTwoD, 0, 1, 2, 1, slice));
    auto w = header(slice);
    EXPECT_EQ(address(w), depth.va + 2 * depth.layerBytes);
    EXPECT_EQ(w[0], gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(kDepth32Float)].tic0);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoD);
    EXPECT_TRUE(slice.zeta);
    ASSERT_TRUE(dev.createPartView(depth, kDepth32Float, gm20b::fx::kTexTwoDArray, 0, 1, 1, 3, array));
    w = header(array);
    EXPECT_EQ(address(w), depth.va + depth.layerBytes);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 2u);
    gm20b::Texture rgba;
    EXPECT_FALSE(dev.createPartView(depth, kRGBA8, gm20b::fx::kTexTwoD, 0, 1, 0, 1, rgba))
        << "a depth texture's own format";
}

TEST_F(DeviceViews, ADepthTexturesFormatViewAndItsStencilView)
{
    gm20b::Texture depth, same, stencil, other;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32FloatStencil8, 1, true, depth));
    ASSERT_NE(depth.tic, 0u);
    ASSERT_TRUE(dev.createView(depth, kDepth32FloatStencil8, same));
    EXPECT_NE(same.tic, depth.tic);
    EXPECT_EQ(header(same), header(depth));
    ASSERT_TRUE(dev.createView(depth, kX32Stencil8, stencil));
    auto w = header(stencil), d = header(depth);
    EXPECT_EQ(w[0], gm20b::stencilViewWord0(kDepth32FloatStencil8)) << "NVK's X32_S8X24_UINT";
    for (int i = 1; i < 8; i++)
        EXPECT_EQ(w[i], d[i]) << "word " << i;
    EXPECT_EQ(stencil.format, kDepth32FloatStencil8) << "the zeta tables' format";
    EXPECT_FALSE(stencil.renderable);
    EXPECT_FALSE(dev.createView(depth, kDepth32Float, other)) << "Metal views no depth aspect alone";
    gm20b::Texture plain;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, plain));
    EXPECT_FALSE(dev.createView(plain, kX32Stencil8, other)) << "no stencil to view";
}

// A block format's header extent is in texels (NIL's extent_px), a view's as its texture's
TEST_F(DeviceViews, AViewOfABlockChainKeepsItsExtentInTexels)
{
    gm20b::Texture astc, view, part;
    ASSERT_TRUE(dev.createBlockTexture(100, 60, 7, 1, kASTC4x4, gm20b::fx::kTexTwoD, astc));
    ASSERT_TRUE(dev.createView(astc, kASTC4x4, view));
    auto w = header(view);
    EXPECT_EQ(field(w, 143, 128), 99u);
    EXPECT_EQ(field(w, 175, 160), 59u);
    ASSERT_TRUE(dev.createPartView(astc, kASTC4x4, gm20b::fx::kTexTwoD, 2, 3, 0, 1, part));
    w = header(part);
    EXPECT_EQ(field(w, 143, 128), 99u);
    EXPECT_EQ(minLevel(w), 2u);
    EXPECT_EQ(maxLevel(w), 4u);
    EXPECT_EQ(header(view)[1], header(astc)[1]);
}

// A view frees its own header alone: its texture's memory, its owned depth and stencil temp stay its texture's
TEST_F(DeviceViews, DestroyingAViewLeavesItsTextureWhole)
{
    gm20b::Texture depth, slice, stencil, array, part;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32FloatStencil8, 1, true, depth));
    ASSERT_TRUE(dev.createView(depth, kX32Stencil8, stencil));
    ASSERT_TRUE(dev.createPartView(depth, kDepth32FloatStencil8, gm20b::fx::kTexTwoD, 0, 1, 0, 1, slice));
    ASSERT_TRUE(dev.createArray(16, 16, 2, kRGBA8, gm20b::fx::kTexTwoDArray, array, 5));
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, gm20b::fx::kTexTwoD, 1, 1, 1, 1, part));
    const auto depthHeader = header(depth), arrayHeader = header(array);
    const size_t freedBefore = kernel_model::model.freed.size();
    for (gm20b::Texture *v : {&stencil, &slice, &part})
        dev.destroyTexture(*v);
    EXPECT_EQ(kernel_model::model.freed.size(), freedBefore) << "no memory freed with a view";
    EXPECT_EQ(header(depth), depthHeader);
    EXPECT_EQ(header(array), arrayHeader);
    EXPECT_NE(depth.va, 0u);
    gm20b::Texture again;
    ASSERT_TRUE(dev.createView(depth, kX32Stencil8, again)) << "the depth texture is still whole";
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, again.tic)[1], depthHeader[1]);
}

// Metal views a buffer's or a multisampled texture as a one-layer 2D array: NIL's pitch header (TWO_D_NO_MIPMAP), and
// the multisampled image's header as an array
TEST_F(DeviceViews, APitchOrMultisampledTextureIsAOneLayerArrayWhole)
{
    gm20b::Texture pitch, pitchArray, ms, msArray, part;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, pitch, false, false));
    ASSERT_FALSE(pitch.blockLinear);
    ASSERT_TRUE(dev.createPartView(pitch, kRGBA8, gm20b::fx::kTexTwoDArray, 0, 1, 0, 1, pitchArray));
    EXPECT_EQ(header(pitchArray), header(pitch));
    ASSERT_TRUE(dev.createStoredMultisample(32, 32, kRGBA8, 4, ms));
    ASSERT_TRUE(dev.createPartView(ms, kRGBA8, gm20b::fx::kTexTwoDArray, 0, 1, 0, 1, msArray));
    auto w = header(msArray), own = header(ms);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 0u);
    EXPECT_EQ(field(w, 235, 232), field(own, 235, 232)) << "its sample layout";
    EXPECT_EQ(address(w), address(own));
    gm20b::Texture array;
    ASSERT_TRUE(dev.createArray(32, 32, 2, kRGBA8, gm20b::fx::kTexTwoDArray, array));
    EXPECT_TRUE(dev.createPartView(array, kRGBA8, gm20b::fx::kTexTwoD, 0, 1, 1, 1, part));
    EXPECT_FALSE(dev.createPartView(pitch, kRGBA8, gm20b::fx::kTexTwoDArray, 0, 1, 0, 2, part)) << "one layer";
}

// A sampled depth array or cube has a header of its own (enc.notic: a draw that sampled one had none to bind), and so
// has one of no ShaderRead usage, from its creation: nothing writes it later while recording threads read it unlocked
TEST_F(DeviceViews, ASampledDepthArrayOrCubeHasItsOwnHeader)
{
    gm20b::Texture array, cube, unsampled;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, array, 4, gm20b::fx::kTexTwoDArray));
    ASSERT_NE(array.tic, 0u);
    auto w = header(array);
    EXPECT_EQ(address(w), array.va);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 3u);
    EXPECT_EQ(w[0], gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(kDepth32Float)].tic0);
    ASSERT_TRUE(dev.createOwnedZeta(16, 16, kDepth32Float, 1, true, cube, 6, gm20b::fx::kTexCube));
    w = header(cube);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexCube);
    EXPECT_EQ(depthMinusOne(w), 0u);
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, false, unsampled, 4));
    EXPECT_NE(unsampled.tic, 0u) << "no ShaderRead usage: its header is made with it all the same";
}

// With every texture header held, a depth texture of no ShaderRead usage is not made either (no draw later finds it
// headerless), headersLeft names why, and one is made again once headers are given back
TEST_F(DeviceViews, NoTextureIsMadeWithNoHeaderLeft)
{
    std::vector<gm20b::Texture> held;
    gm20b::Texture t;
    while (dev.headersLeft() && dev.createTexture(4, 4, kRGBA8, t))
        held.push_back(t);
    ASSERT_EQ(dev.headersLeft(), 0u) << held.size() << " held";
    EXPECT_FALSE(dev.createOwnedZeta(8, 8, kDepth32Float, 1, false, t));
    for (gm20b::Texture &h : held)
        dev.destroyTexture(h);
    ASSERT_TRUE(dev.createOwnedZeta(8, 8, kDepth32Float, 1, false, t));
    EXPECT_NE(t.tic, 0u);
    dev.destroyTexture(t);
}

// The array header is writeTic's for an owned depth array
TEST_F(DeviceViews, AnOwnedDepthArraysHeaderIsAnArrays)
{
    gm20b::Texture array;
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, array, 4, gm20b::fx::kTexTwoDArray));
    ASSERT_NE(array.tic, 0u);
    const auto w = header(array);
    EXPECT_EQ(address(w), array.va);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoDArray);
    EXPECT_EQ(depthMinusOne(w), 3u);
}

TEST_F(DeviceViews, ADepthChainsSelectedLevelHeaderBelongsToTheChain)
{
    for (uint32_t format : {kDepth16Unorm, kDepth32Float, kDepth32FloatStencil8}) {
        SCOPED_TRACE(format);
        gm20b::Texture depth, part;
        ASSERT_TRUE(dev.createOwnedZeta(64, 32, format, 1, false, depth, 1, gm20b::fx::kTexTwoD, 4));
        const uint32_t own = depth.tic;
        ASSERT_TRUE(gm20b::chainLevelImage(depth, 2, part));
        const uint32_t tic = dev.attachmentTic(depth, part, 2, 0);
        ASSERT_NE(tic, 0u);
        const auto w = gm20b::DeviceHarness::header(dev, tic);
        EXPECT_EQ(w[0], gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(format)].tic0);
        EXPECT_EQ(address(w), depth.chain->va);
        EXPECT_EQ(minLevel(w), 2u);
        EXPECT_EQ(maxLevel(w), 2u);
        EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoD);
        EXPECT_EQ(field(w, 143, 128) + 1, 64u);
        EXPECT_EQ(field(w, 175, 160) + 1, 32u);
        for (int i = 0; i < 16; ++i)
            EXPECT_EQ(dev.attachmentTic(depth, part, 2, 0), tic);
        EXPECT_EQ(depth.tic, own);
        dev.destroyTexture(depth);
        EXPECT_EQ(gm20b::DeviceHarness::header(dev, tic), gm20b::DeviceHarness::header(dev, 0));
        gm20b::Texture replacement;
        ASSERT_TRUE(dev.createOwnedZeta(32, 16, format, 1, false, replacement, 1, gm20b::fx::kTexTwoD, 3));
        const uint32_t level = dev.levelTic(replacement, 1);
        ASSERT_NE(level, 0u);
        const auto next = gm20b::DeviceHarness::header(dev, level);
        EXPECT_EQ(minLevel(next), 1u);
        EXPECT_EQ(maxLevel(next), 1u);
        EXPECT_EQ(field(next, 143, 128) + 1, 32u);
        EXPECT_NE(gm20b::DeviceHarness::header(dev, tic), w);
        dev.destroyTexture(replacement);
    }
}

TEST_F(DeviceViews, ASelectedDepthHeaderIsFreedWithItsArrayViewAndNeverAnswersForItsReplacement)
{
    gm20b::Texture parent, view, part;
    ASSERT_TRUE(dev.createOwnedZeta(64, 32, kDepth16Unorm, 1, false, parent, 3, gm20b::fx::kTexTwoDArray, 4));
    const uint32_t parentTic = parent.tic;
    ASSERT_TRUE(dev.createPartView(parent, kDepth16Unorm, gm20b::fx::kTexTwoD, 1, 2, 2, 1, view));
    const uint32_t own = view.tic;
    const auto ownHeader = header(view);
    ASSERT_TRUE(gm20b::chainLevelImage(parent, 2, part));
    part = gm20b::layersFrom(part, 2);
    part.layers = 1;
    const uint32_t tic = dev.attachmentTic(view, part, 1, 0);
    ASSERT_NE(tic, 0u);
    EXPECT_NE(tic, own);
    auto w = gm20b::DeviceHarness::header(dev, tic);
    EXPECT_EQ(address(w),
              parent.chain->va + 2 * parent.chain->layout.arrayStride + parent.chain->layout.level[2].offset);
    EXPECT_EQ(w[0], gm20b::kZetaFormatInfo[gm20b::zetaFormatIndex(kDepth16Unorm)].tic0);
    EXPECT_EQ(minLevel(w), 0u);
    EXPECT_EQ(maxLevel(w), 0u);
    EXPECT_EQ(field(w, 143, 128) + 1, part.width);
    EXPECT_EQ(field(w, 175, 160) + 1, part.height);
    EXPECT_EQ(field(w, 101, 99), part.blockHeight);
    EXPECT_EQ(depthMinusOne(w), 0u);
    EXPECT_EQ(textureType(w), gm20b::fx::kTexTwoD);
    EXPECT_EQ(dev.attachmentTic(view, part, 1, 0), tic);
    EXPECT_EQ(header(view), ownHeader);
    EXPECT_EQ(parent.tic, parentTic);
    dev.destroyTexture(view);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, own), gm20b::DeviceHarness::header(dev, 0));
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, tic), gm20b::DeviceHarness::header(dev, 0));
    gm20b::Texture occupying, replacement;
    ASSERT_TRUE(dev.createPartView(parent, kDepth16Unorm, gm20b::fx::kTexTwoD, 0, 1, 0, 1, occupying));
    ASSERT_EQ(occupying.tic, tic);
    ASSERT_TRUE(dev.createPartView(parent, kDepth16Unorm, gm20b::fx::kTexTwoD, 1, 2, 1, 1, replacement));
    ASSERT_EQ(replacement.tic, own);
    ASSERT_TRUE(gm20b::chainLevelImage(parent, 2, part));
    part = gm20b::layersFrom(part, 1);
    part.layers = 1;
    const uint32_t next = dev.attachmentTic(replacement, part, 1, 0);
    ASSERT_NE(next, 0u);
    EXPECT_NE(next, tic);
    w = gm20b::DeviceHarness::header(dev, next);
    EXPECT_EQ(address(w), part.va);
    EXPECT_EQ(dev.attachmentTic(replacement, part, 1, 0), next);
    dev.destroyTexture(replacement);
    dev.destroyTexture(occupying);
    dev.destroyTexture(parent);
}

TEST_F(DeviceViews, ConcurrentDepthPartAdmissionPublishesOneResourceOwnedHeader)
{
    gm20b::Texture depth, part;
    ASSERT_TRUE(dev.createOwnedZeta(64, 32, kDepth16Unorm, 1, false, depth, 1, gm20b::fx::kTexTwoD, 4));
    part = gm20b::layersFrom(depth, 0);
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
    const auto w = gm20b::DeviceHarness::header(dev, tics[0]);
    EXPECT_EQ(address(w), part.va);
    EXPECT_EQ(minLevel(w), 0u);
    EXPECT_EQ(maxLevel(w), 0u);
    const uint32_t own = depth.tic;
    dev.destroyTexture(depth);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, own), gm20b::DeviceHarness::header(dev, 0));
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, tics[0]), gm20b::DeviceHarness::header(dev, 0));
}

}  // namespace
