// SPDX-License-Identifier: MIT
// raw_view.h: which header a kernel's texture slot binds, and the header of m2v's view of a texture in another format.
// Word 0 values are NVK's for the views m2v makes (raw/test/viewframe's decode of m2v's dispatches on the mock GPU); the
// multisampled views' headers are NIL's.
#include <cstdint>
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/raw/fx/fx_image.h"
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

// NIL's headers of a multisampled image at 0x100000 (raw/fx/test/nilimage.c ms), as a texture and as a storage image: the
// storage header's WIDTH and HEIGHT are the sample grid's
struct NilStorageView {
    uint32_t width, height, bpp, samples;
    uint32_t texture[8], storage[8];
};

// clang-format off
const NilStorageView kNilStorageViews[] = {
    {64, 64, 4, 4,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070018, 0xe080003f, 0x8000003f, 0x03000000, 0x00000200},
     {0x58d24908, 0x00100000, 0x00600000, 0x00070018, 0xe080007f, 0x8000007f, 0x03000000, 0x00000200}},
    {64, 64, 4, 2,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070018, 0xe080003f, 0x8000003f, 0x03000000, 0x00000500},
     {0x58d24908, 0x00100000, 0x00600000, 0x00070018, 0xe080007f, 0x8000003f, 0x03000000, 0x00000500}},
    {32, 32, 4, 2,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070010, 0xe080001f, 0x8000001f, 0x03000000, 0x00000500},
     {0x58d24908, 0x00100000, 0x00600000, 0x00070010, 0xe080003f, 0x8000001f, 0x03000000, 0x00000500}},
    {32, 32, 4, 4,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070010, 0xe080001f, 0x8000001f, 0x03000000, 0x00000200},
     {0x58d24908, 0x00100000, 0x00600000, 0x00070010, 0xe080003f, 0x8000003f, 0x03000000, 0x00000200}},
    {720, 1280, 4, 4,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe08002cf, 0x800004ff, 0x03000000, 0x00000200},
     {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe080059f, 0x800009ff, 0x03000000, 0x00000200}},
    {33, 9, 4, 4,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070000, 0xe0800020, 0x80000008, 0x03000000, 0x00000200},
     {0x58d24908, 0x00100000, 0x00600000, 0x00070000, 0xe0800041, 0x80000011, 0x03000000, 0x00000200}},
    {100, 50, 16, 8,
     {0x58d7ff81, 0x00100000, 0x00600000, 0x00070018, 0xe0800063, 0x80000031, 0x03000000, 0x00000400},
     {0x58d7ff81, 0x00100000, 0x00600000, 0x00070018, 0xe080018f, 0x80000063, 0x03000000, 0x00000400}},
    {720, 1280, 4, 1,
     {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe08002cf, 0x800004ff, 0x03000000, 0x00000000},
     {0x58d24908, 0x00100000, 0x00600000, 0x00070020, 0xe08002cf, 0x800004ff, 0x03000000, 0x00000000}},
};
// clang-format on

// The raw path's own header of a multisampled texture (Device::writeTic) is NIL's texture view; its storage view
// (Device::viewTic, which the storage form's fetched attachments and shader depth/stencil tests bind) is NIL's storage view
TEST(ViewHeader, AMultisampledStorageViewSpansTheSampleGridAsNilsDoes)
{
    for (const NilStorageView &n : kNilStorageViews) {
        fx::MipChain c;
        ASSERT_TRUE(fx::msLayout(n.width, n.height, n.samples, n.bpp, c)) << n.width << "x" << n.height;
        uint32_t own[8], texture[8], storage[8];
        fx::mipChainTic(c, 0x100000, n.texture[0], false, 0, 1, own);
        viewHeader(own, n.texture[0], texture);
        viewHeader(own, n.storage[0], storage, c.sxLog2, c.syLog2);
        for (int w = 0; w < 8; w++) {
            EXPECT_EQ(own[w], n.texture[w]) << n.width << "x" << n.height << " x" << n.samples << " word " << w;
            EXPECT_EQ(texture[w], n.texture[w]) << n.width << "x" << n.height << " x" << n.samples << " word " << w;
            EXPECT_EQ(storage[w], n.storage[w]) << n.width << "x" << n.height << " x" << n.samples << " word " << w;
        }
    }
}

}  // namespace
