// SPDX-License-Identifier: MIT
// Format stand-ins (format_standin.h) of layered and multisampled targets: a stand-in of the pass's layers
// and the target's samples, filled and stored a layer at a time, each layer read through a header of its own (an array's
// layers layerBytes apart, a 3D level's planes a plane apart), a multisampled one by the per-sample states, the copy engine's
// formats over the sample grid.
#include "format_standin_harness.h"
#include "../attachment_target.h"

namespace {

using namespace standin;

// The address header `tic` reads (ADDRESS_BITS31TO9, ADDRESS_BITS47TO32)
uint64_t headerAddress(gm20b::Device &dev, uint32_t tic)
{
    const uint32_t *w = dev.headerWords(tic);
    return uint64_t(w[2] & 0xffff) << 32 | (w[1] & 0xfffffe00u);
}

TEST_F(DeviceFormatStandIn, ALayeredPassRendersEachLayerThroughItsStandIn)
{
    addState(gm20b::formatLoadState(gm20b::kAbgr4Unorm), 125);
    addState(gm20b::formatStoreState(gm20b::kAbgr4Unorm), gm20b::formatStoreView(gm20b::kAbgr4Unorm));
    gm20b::Texture t;
    ASSERT_TRUE(dev.createArray(32, 16, 3, gm20b::kAbgr4Unorm, gm20b::fx::kTexTwoDArray, t));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 3, 2, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->layers, 3u);
    EXPECT_TRUE(s->blockLinear);
    const gm20b::Texture held = *s;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 3, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    const auto passes = targets(methods);
    ASSERT_EQ(passes.size(), 6u) << "a load and a store a layer";
    for (uint32_t l = 0; l < 3; l++) {
        EXPECT_EQ(passes[l].first, held.va + l * held.layerBytes) << "the load into the stand-in's layer " << l;
        EXPECT_EQ(passes[3 + l].first, t.va + l * t.layerBytes) << "the store into the texture's layer " << l;
        EXPECT_EQ(passes[3 + l].second, 0xf1u) << "R16_UINT";
        gm20b::Texture one;
        ASSERT_TRUE(gm20b::attachmentTarget(t, 0, l, one));
        const uint32_t tic = dev.attachmentTic(t, one, 0, l);
        ASSERT_NE(tic, 0u);
        EXPECT_EQ(headerAddress(dev, tic), t.va + l * t.layerBytes) << "the fill read layer " << l << " itself";
    }
}

// A pass into planes 1 and 2 of a 3D texture's level: its planes are the stand-in's layers, each a plane of whole tiles
// past the last
TEST_F(DeviceFormatStandIn, A3DLevelsPlanesAreTheStandInsLayers)
{
    addState(gm20b::formatLoadState(gm20b::kRgb9e5Float), 125);
    addState(gm20b::formatStoreState(gm20b::kRgb9e5Float), gm20b::formatStoreView(gm20b::kRgb9e5Float));
    gm20b::Texture t, part;
    ASSERT_TRUE(dev.createVolume(32, 12, 4, gm20b::kRgb9e5Float, true, t));
    ASSERT_TRUE(gm20b::attachmentTarget(t, 0, 1, part));
    gm20b::ColorAttachment a;
    a.texture = &part;
    a.image = &t;
    a.slice = 1;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, a, 2, 0, true, &why);
    ASSERT_NE(s, nullptr) << why;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, a, 2, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    const auto passes = targets(methods);
    ASSERT_EQ(passes.size(), 4u);
    const uint64_t plane = uint64_t(part.pitch) * 16;  // 12 rows in one GOB-high block row of 8: 16
    for (uint32_t l = 0; l < 2; l++) {
        EXPECT_EQ(passes[2 + l].first, part.va + l * plane) << "the store into plane " << 1 + l;
        gm20b::Texture one;
        ASSERT_TRUE(gm20b::attachmentTarget(t, 0, 1 + l, one));
        one.format = gm20b::formatStoreView(gm20b::kRgb9e5Float);
        const uint32_t tic = dev.attachmentTic(t, one, 0, 1 + l);
        ASSERT_NE(tic, 0u);
        EXPECT_EQ(headerAddress(dev, tic), part.va + l * plane) << "the fill read plane " << 1 + l << "'s words";
    }
}

TEST_F(DeviceFormatStandIn, ALayeredPassOfMoreLayersThanItsTargetHasNoStandIn)
{
    addState(gm20b::formatLoadState(gm20b::kAbgr4Unorm), 125);
    addState(gm20b::formatStoreState(gm20b::kAbgr4Unorm), gm20b::formatStoreView(gm20b::kAbgr4Unorm));
    gm20b::Texture t;
    ASSERT_TRUE(dev.createArray(16, 16, 2, gm20b::kAbgr4Unorm, gm20b::fx::kTexTwoDArray, t));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    EXPECT_EQ(gm20b::formatStandInFor(dev, *cb, whole(t), 3, 0, true, &why), nullptr);
    ASSERT_NE(why, nullptr);
    cb->abandon();
}

// A 4-sample target: a 4-sample stand-in the per-sample load state fills and the per-sample store state stores; with the
// one-sample states alone none (a one-sample state draws sample 0 of a 4-sample pass)
TEST_F(DeviceFormatStandIn, AMultisampledTargetIsFilledAndStoredPerSample)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createStoredMultisample(16, 8, gm20b::kAbgr4Unorm, 4, t));
    addState(gm20b::formatLoadState(gm20b::kAbgr4Unorm), 125);
    addState(gm20b::formatStoreState(gm20b::kAbgr4Unorm), gm20b::formatStoreView(gm20b::kAbgr4Unorm));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    EXPECT_EQ(gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why), nullptr);
    ASSERT_NE(why, nullptr);
    EXPECT_NE(strstr(why, "sample count"), nullptr) << why;
    cb->abandon();

    EXPECT_STREQ(gm20b::formatLoadState(gm20b::kAbgr4Unorm, 4), "raw.fmt.fmt_load_ms4_frag");
    EXPECT_STREQ(gm20b::formatStoreState(gm20b::kAbgr4Unorm, 2), "raw.fmt.fmt_store_abgr4_ms2_frag");
    EXPECT_EQ(gm20b::formatLoadState(gm20b::kAbgr4Unorm, 8), nullptr);
    addState(gm20b::formatLoadState(gm20b::kAbgr4Unorm, 4), 125, 4);
    addState(gm20b::formatStoreState(gm20b::kAbgr4Unorm, 4), gm20b::formatStoreView(gm20b::kAbgr4Unorm), 4);
    cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->samples, 4u);
    EXPECT_EQ(s->format, 125u);
    const uint64_t standIn = s->va;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    const auto passes = targets(methods);
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_EQ(passes[0].first, standIn);
    EXPECT_EQ(passes[1].first, t.va);
    EXPECT_EQ(passes[1].second, 0xf1u) << "R16_UINT";
}

// A memoryless target is neither filled nor stored: its stand-in needs no state, at any sample count
TEST_F(DeviceFormatStandIn, AMemorylessMultisampledTargetsStandInNeedsNoState)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createMemoryless(16, 8, gm20b::kRgb9e5Float, t, 4));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, false, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->samples, 4u);
    cb->abandon();
}

// The copy engine's formats: a layer's launch each way per layer, a multisampled one over its sample grid (4 samples: two
// columns and two rows of each pixel's)
TEST_F(DeviceFormatStandIn, TheCopiedFormatsMoveEachLayerAndEverySample)
{
    gm20b::Texture array, samples;
    ASSERT_TRUE(dev.createArray(16, 8, 2, gm20b::kR8UnormSrgb, gm20b::fx::kTexTwoDArray, array));
    ASSERT_TRUE(dev.createStoredMultisample(16, 8, gm20b::kArgb8Unorm, 4, samples));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(array), 2, 0, true, &why);
    ASSERT_NE(s, nullptr) << why;
    const gm20b::Texture layered = *s;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(array), 2, *s, false, &why)) << why;
    gm20b::Texture *m = gm20b::formatStandInFor(dev, *cb, whole(samples), 1, 1, true, &why);
    ASSERT_NE(m, nullptr) << why;
    EXPECT_EQ(m->samples, 4u);
    const uint64_t grid = m->va;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(samples), 1, *m, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    ASSERT_EQ(launches.size(), 6u);
    for (uint32_t l = 0; l < 2; l++) {
        EXPECT_EQ(launches[l].from, array.va + l * array.layerBytes);
        EXPECT_EQ(launches[l].to, layered.va + l * layered.layerBytes);
        EXPECT_EQ(launches[2 + l].from, layered.va + l * layered.layerBytes);
        EXPECT_EQ(launches[2 + l].to, array.va + l * array.layerBytes);
    }
    EXPECT_EQ(launches[4].from, samples.va);
    EXPECT_EQ(launches[4].to, grid);
    EXPECT_EQ(launches[4].width, 32u) << "two columns of samples a pixel";
    EXPECT_EQ(launches[4].lines, 16u) << "two rows of samples a pixel";
    EXPECT_EQ(launches[5].to, samples.va);
}

}  // namespace
