// SPDX-License-Identifier: MIT
// raw_view.h: which header a kernel's texture slot binds, and the header of m2v's view of a texture in another format.
// Word 0 values are NVK's for the views m2v makes (raw/test/viewframe's decode of m2v's dispatches on the mock GPU).
#include <cstdint>
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_view.h"

namespace {
using namespace raw;

constexpr uint32_t kRgba16Float = 0x58d7ff83, kRgba16Uint = 0x58d49203;  // R16_G16_B16_A16: FLOAT, UINT
constexpr uint32_t kR16Float = 0x7017ff9b, kR16Uint = 0x6014921b;        // R16: FLOAT (R 0 0 1), UINT (R 0 0 1)
constexpr uint32_t kRgba8 = 0x58d24908, kBgra8 = 0x54e24908;             // A8B8G8R8 UNORM, red and blue exchanged
constexpr uint32_t kR32Uint = 0x6014920f, kDxt1 = 0x58d24924;

TEST(TexelBytes, ComponentLayoutsByTheirTexelSize)
{
    EXPECT_EQ(texelBytes(kRgba16Float), 8u);
    EXPECT_EQ(texelBytes(kRgba16Uint), 8u);
    EXPECT_EQ(texelBytes(kR16Uint), 2u);
    EXPECT_EQ(texelBytes(kRgba8), 4u);
    EXPECT_EQ(texelBytes(kR32Uint), 4u);
    EXPECT_EQ(texelBytes(kDxt1), 0u);
}

TEST(SlotView, TheTexturesOwnHeaderWhereM2vBindsItsFormat)
{
    EXPECT_EQ(slotView(0, kRgba16Float, kRgba16Float, 8, false), SlotView::Own);
    EXPECT_EQ(slotView(kRgba16Float, kRgba16Float, kRgba16Float, 8, false), SlotView::Own);
    // a swizzled texture: m2v's word 0 is the format's under the swizzle, which its own header already has
    EXPECT_EQ(slotView(kBgra8, kRgba8, kBgra8, 4, false), SlotView::Own);
}

TEST(SlotView, AViewWhereM2vWritesAHalfTextureThroughItsUnsignedTwin)
{
    EXPECT_EQ(slotView(kRgba16Uint, kRgba16Float, kRgba16Float, 8, false), SlotView::View);
    EXPECT_EQ(slotView(kR16Uint, kR16Float, kR16Float, 2, false), SlotView::View);
    EXPECT_EQ(slotView(kRgba8, kBgra8, kBgra8, 4, false), SlotView::View);
}

TEST(SlotView, NoneForTexelsOfAnotherSizeOrBlocks)
{
    EXPECT_EQ(slotView(kR32Uint, kRgba16Float, kRgba16Float, 8, false), SlotView::None);
    EXPECT_EQ(slotView(kRgba16Uint, kR16Float, kR16Float, 2, false), SlotView::None);
    EXPECT_EQ(slotView(kRgba16Uint, kDxt1, kDxt1, 8, true), SlotView::None);
}

TEST(ViewHeader, TheTexturesHeaderWithM2vsWordZeroAndNoSrgbConversion)
{
    // a 32x16 block-linear texture's header, its S_R_G_B_CONVERSION set
    const uint32_t own[8] = {kRgba8, 0, 0x00600000, 0x00070008, 0xe080001f | 1u << 22, 0x8000000f, 0x03000000, 0};
    uint32_t view[8];
    viewHeader(own, kR32Uint, view);
    EXPECT_EQ(view[0], kR32Uint);
    EXPECT_EQ(view[4], 0xe080001fu);
    for (int i : {1, 2, 3, 5, 6, 7})
        EXPECT_EQ(view[i], own[i]) << "word " << i;
}

}  // namespace
