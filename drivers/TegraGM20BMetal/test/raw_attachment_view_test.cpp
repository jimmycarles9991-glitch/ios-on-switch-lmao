// SPDX-License-Identifier: MIT
// raw_attachment_view.h: the headers of a pass's views of its attachments. Each is read back as the texture unit reads it:
// the address of its first layer, the extent of the level the program addresses, its layers, and the layer stride the unit
// derives from the header's extent and levels (fx::arrayLayout of the decoded fields), which must be the stride the pass
// renders its layers at.
#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/raw/fx/fx_image.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_attachment_view.h"

namespace {
using namespace raw;

constexpr uint32_t kRgba8Word0 = 0x58d24908, kR32UintWord0 = 0x6014920f, kBpp = 4;
constexpr uint64_t kBase = 0x12'3450'0000;

uint32_t field(const uint32_t h[8], unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    for (unsigned b = lo; b <= hi; b++)
        v |= uint64_t(h[b / 32] >> (b % 32) & 1) << (b - lo);
    return uint32_t(v);
}

struct Decoded {
    uint64_t va;
    uint32_t type, width, height, layers, maxMip, minView, maxView, blockHeight, blockDepth;
    bool srgb;
};

Decoded decode(const uint32_t h[8])
{
    Decoded d;
    d.va = uint64_t(field(h, 63, 41)) << 9 | uint64_t(field(h, 79, 64)) << 32;
    d.type = field(h, 154, 151);
    d.width = field(h, 143, 128) + 1;
    d.height = field(h, 175, 160) + 1;
    d.layers = field(h, 189, 176) + 1;
    d.maxMip = field(h, 127, 124);
    d.minView = field(h, 227, 224);
    d.maxView = field(h, 231, 228);
    d.blockHeight = field(h, 101, 99);
    d.blockDepth = field(h, 104, 102);
    d.srgb = field(h, 150, 150);
    return d;
}

// The layer stride the texture unit steps by under header `d`
uint64_t unitStride(const Decoded &d)
{
    fx::MipChain c;
    EXPECT_TRUE(fx::arrayLayout(d.width, d.height, d.layers, d.maxMip + 1, kBpp, c));
    return c.arrayStride;
}

AttachmentView view(uint32_t level, uint32_t slice, uint32_t layers, bool arrayed)
{
    AttachmentView v;
    v.word0 = kR32UintWord0;
    v.level = uint8_t(level);
    v.slice = slice;
    v.layers = layers;
    v.arrayed = arrayed;
    v.part = true;
    return v;
}

TEST(AttachmentView, AOneLevelArraysLayersFromItsSlice)
{
    fx::MipChain c;
    ASSERT_TRUE(fx::arrayLayout(48, 20, 6, 1, kBpp, c));
    uint32_t h[8];
    ASSERT_TRUE(attachmentViewHeader(c, kBase, false, view(0, 1, 4, true), h));
    const Decoded d = decode(h);
    EXPECT_EQ(h[0], kR32UintWord0);
    EXPECT_FALSE(d.srgb);
    EXPECT_EQ(d.va, kBase + c.arrayStride);
    EXPECT_EQ(d.type, fx::kTexTwoDArray);
    EXPECT_EQ(d.layers, 4u);
    EXPECT_EQ(d.width, 48u);
    EXPECT_EQ(d.height, 20u);
    EXPECT_EQ(unitStride(d), c.arrayStride);
}

TEST(AttachmentView, ALevelOfAMipmappedArrayIsTheChainsHeaderAtTheSlice)
{
    fx::MipChain c;
    ASSERT_TRUE(fx::arrayLayout(64, 40, 6, 4, kBpp, c));
    uint32_t h[8];
    ASSERT_TRUE(attachmentViewHeader(c, kBase, false, view(2, 3, 2, true), h));
    const Decoded d = decode(h);
    EXPECT_EQ(d.va, kBase + 3 * c.arrayStride) << "a header's address is level 0 of its first layer";
    EXPECT_EQ(d.minView, 2u);
    EXPECT_EQ(d.maxView, 2u);
    EXPECT_EQ(d.maxMip, 3u);
    EXPECT_EQ(std::max(1u, d.width >> d.minView), c.level[2].width) << "the grid the program addresses";
    EXPECT_EQ(std::max(1u, d.height >> d.minView), c.level[2].height);
    EXPECT_EQ(d.layers, 2u);
    EXPECT_EQ(unitStride(d), c.arrayStride);
}

TEST(AttachmentView, AOneLayerViewIsTwoDOrAnArrayOfOne)
{
    fx::MipChain c;
    ASSERT_TRUE(fx::arrayLayout(32, 32, 6, 3, kBpp, c));
    uint32_t flat[8], one[8];
    ASSERT_TRUE(attachmentViewHeader(c, kBase, false, view(1, 5, 1, false), flat));
    ASSERT_TRUE(attachmentViewHeader(c, kBase, false, view(1, 5, 1, true), one));
    EXPECT_EQ(decode(flat).type, fx::kTexTwoD);
    EXPECT_EQ(decode(one).type, fx::kTexTwoDArray);
    for (const uint32_t *h : {flat, one}) {
        EXPECT_EQ(decode(h).va, kBase + 5 * c.arrayStride);
        EXPECT_EQ(decode(h).layers, 1u);
        EXPECT_EQ(decode(h).minView, 1u);
    }
}

TEST(AttachmentView, A3DLevelIsATwoDArrayOfItsPlanes)
{
    fx::MipChain c;
    ASSERT_TRUE(fx::volumeLayout(32, 16, 8, 2, kBpp, true, c));
    const fx::MipLevel &l = c.level[1];
    ASSERT_EQ(l.zLog2, 0u);
    uint32_t h[8];
    ASSERT_TRUE(attachmentViewHeader(c, kBase, true, view(1, 1, 3, true), h));
    const Decoded d = decode(h);
    const uint64_t plane = uint64_t(l.rowBytes) * l.rows;
    EXPECT_EQ(d.va, kBase + l.offset + plane);
    EXPECT_EQ(d.type, fx::kTexTwoDArray);
    EXPECT_EQ(d.width, l.width);
    EXPECT_EQ(d.height, l.height);
    EXPECT_EQ(d.layers, 3u);
    EXPECT_EQ(d.maxMip, 0u);
    EXPECT_EQ(d.minView, 0u);
    EXPECT_EQ(d.blockHeight, l.yLog2);
    EXPECT_EQ(d.blockDepth, 0u);
    EXPECT_EQ(unitStride(d), plane);
}

TEST(AttachmentView, NoneOutsideTheImage)
{
    fx::MipChain c, deep;
    ASSERT_TRUE(fx::arrayLayout(32, 32, 4, 2, kBpp, c));
    ASSERT_TRUE(fx::volumeLayout(16, 16, 8, 1, kBpp, false, deep));
    uint32_t h[8];
    EXPECT_FALSE(attachmentViewHeader(c, kBase, false, view(0, 2, 3, true), h));
    EXPECT_FALSE(attachmentViewHeader(c, kBase, false, view(2, 0, 1, true), h));
    EXPECT_FALSE(attachmentViewHeader(c, kBase, false, view(0, 0, 0, true), h));
    EXPECT_FALSE(attachmentViewHeader(deep, kBase, true, view(0, 1, 2, true), h)) << "planes inside a block of several";
}

// A layered pass's backing (Device::backing) is one level's tiles a layer: its own one-layer header, made an array of the
// pass's layers, steps them at the backing's stride
TEST(AttachmentView, AnOwnHeaderOfOneLevelAsTheLayersOfTheBacking)
{
    fx::MipChain one, layered;
    ASSERT_TRUE(fx::msLayout(80, 24, 1, kBpp, one));
    ASSERT_TRUE(fx::arrayLayout(80, 24, 5, 1, kBpp, layered));
    uint32_t own[8], h[8];
    fx::mipChainTic(one, kBase, kRgba8Word0, true, 0, 1, own);
    AttachmentView v;
    v.word0 = kRgba8Word0;
    v.srgb = true;
    v.layers = 5;
    v.arrayed = true;
    ASSERT_TRUE(ownViewHeader(own, v, h));
    const Decoded d = decode(h);
    EXPECT_EQ(d.va, kBase);
    EXPECT_TRUE(d.srgb);
    EXPECT_EQ(d.type, fx::kTexTwoDArray);
    EXPECT_EQ(d.layers, 5u);
    EXPECT_EQ(d.width, 80u);
    EXPECT_EQ(unitStride(d), uint64_t(one.level[0].rowBytes) * one.level[0].rows);
    EXPECT_EQ(unitStride(d), layered.arrayStride);
    v.word0 = kR32UintWord0;
    v.srgb = false;
    v.arrayed = false;
    v.layers = 1;
    ASSERT_TRUE(ownViewHeader(own, v, h));
    EXPECT_EQ(decode(h).type, fx::kTexTwoD);
    EXPECT_FALSE(decode(h).srgb);
    EXPECT_EQ(h[0], kR32UintWord0);
}

TEST(AttachmentView, APitchHeaderHasNoArrayForm)
{
    uint32_t own[8] = {kRgba8Word0, 0, 2u << 21, 0, fx::kTexTwoD << 23, 0, 0, 0}, h[8];
    AttachmentView v;
    v.word0 = kR32UintWord0;
    v.layers = 1;
    v.arrayed = true;
    EXPECT_FALSE(ownViewHeader(own, v, h));
}

}  // namespace
