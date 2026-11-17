// SPDX-License-Identifier: MIT
// raw::Device::createPartView of a mipmapped array (HW Test metal.texture_view: level 1 of slice 1 of a 16x16 RGBA8 2D
// array of five levels, viewed as a 2D texture): the layer's chain under a header of its level, as NIL lays it out.
#include "raw_device_harness.h"
#include "../raw/fx/fx_image.h"

namespace {

constexpr uint32_t kRGBA8 = 70;

class DevicePartView : public harness::DeviceTest {};

TEST_F(DevicePartView, ALevelOfALayerOfAMipmappedArrayIsThatLayersChainUnderItsLevel)
{
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(16, 16, 2, kRGBA8, raw::fx::kTexTwoDArray, array, 5));
    ASSERT_NE(array.chain, nullptr);
    ASSERT_TRUE(dev.createPartView(array, kRGBA8, raw::fx::kTexTwoD, 1, 1, 1, 1, view));
    raw::fx::MipChain layer = array.chain->layout;
    const uint64_t at = array.chain->va + layer.arrayStride;
    layer.layers = 1;
    uint32_t want[8];
    raw::fx::mipChainTic(layer, at, raw::formatInfo(kRGBA8)->tic0, false, 1, 1, want, raw::fx::kTexTwoD);
    const std::array<uint32_t, 8> got = raw::DeviceHarness::header(dev, view.tic);
    for (uint32_t w = 0; w < 8; w++)
        EXPECT_EQ(got[w], want[w]) << "header word " << w;
    EXPECT_EQ(view.va, at + layer.level[1].offset) << "the view's image is level 1 of layer 1";
    EXPECT_EQ(view.width, 8u);
    EXPECT_EQ(view.height, 8u);
    EXPECT_EQ(view.layers, 1u);
    EXPECT_TRUE(view.renderable);
}

TEST_F(DevicePartView, AViewPastTheArraysLayersOrLevelsIsRefused)
{
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(16, 16, 2, kRGBA8, raw::fx::kTexTwoDArray, array, 5));
    EXPECT_FALSE(dev.createPartView(array, kRGBA8, raw::fx::kTexTwoD, 1, 1, 2, 1, view));
    EXPECT_FALSE(dev.createPartView(array, kRGBA8, raw::fx::kTexTwoDArray, 0, 1, 1, 2, view));
    EXPECT_FALSE(dev.createPartView(array, kRGBA8, raw::fx::kTexTwoD, 4, 2, 0, 1, view));
}

}  // namespace
