// SPDX-License-Identifier: MIT
// Colour targets of the formats the GM20B renders none of (format_standin.h): the store state's words and a
// clear's against an M4 Max's (draws and clears of edge values into 64x64 render targets Metal compresses, 0 of 4096 off
// each; a texture it does not compress clears as a draw, 512 of 512), and the stand-in
// passes gm20b::Device submits: R8Unorm_sRGB's first byte moved by the copy engine into an RGBA8Unorm_sRGB stand-in and back;
// A1BGR5Unorm, ABGR4Unorm and RGB9E5Float filled by the load state into an RGBA32Float stand-in and stored by their store
// state into the texture's words viewed as R16Uint or R32Uint; a level of a chain rendered through its own stand-in; and
// generateMipmapsForTexture: of each, a pass of the format's mip state a level into the level's words; BGR10_XR_sRGB's
// passes through an RGBA16Float stand-in into its codes.
#include "format_standin_harness.h"
#include "../attachment_target.h"
#include <map>

namespace {

using namespace standin;

float asFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// A texel an M4 Max drew (or cleared to) and the word it stored
struct Stored {
    uint32_t format;
    uint32_t rgba[4];
    uint32_t word;
    bool clear;
};

const Stored kStored[] = {
    {41, {0x3edef7be, 0x477fe000, 0x80000000, 0x3f3bd98f}, 0x000077c1, false},
    {41, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x0000001a, false},
    {41, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x000063cc, false},
    {41, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fe48, false},
    {41, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x0000023f, false},
    {41, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000008b, false},
    {41, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x00000353, false},
    {41, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000fabe, false},
    {41, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, false},
    {41, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007ffe, false},
    {41, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x000007c0, false},
    {41, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x00001838, false},
    {41, {0x3e6fce22, 0x3f000000, 0x3d45c8b0, 0x44e46365}, 0x00003c03, true},
    {41, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x0000001a, true},
    {41, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x000063cd, true},
    {41, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fe49, true},
    {41, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x0000023f, true},
    {41, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000008b, true},
    {41, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x00000353, true},
    {41, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000fabf, true},
    {41, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, true},
    {41, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007fff, true},
    {41, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x000007c0, true},
    {41, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x00001839, true},
    {42, {0x3edef7be, 0x477fe000, 0x80000000, 0x3f3bd98f}, 0x00007f0b, false},
    {42, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x00000066, false},
    {42, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x00006738, false},
    {42, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fc28, false},
    {42, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x000004ff, false},
    {42, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000013c, false},
    {42, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x0000064f, false},
    {42, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000f5f8, false},
    {42, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000003, false},
    {42, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007ff8, false},
    {42, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x00000f06, false},
    {42, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x000010d8, false},
    {42, {0x3e6fce22, 0x3f000000, 0x3d45c8b0, 0x44e46365}, 0x0000481f, true},
    {42, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x00000066, true},
    {42, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x00006738, true},
    {42, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fc28, true},
    {42, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x000004ff, true},
    {42, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000013c, true},
    {42, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x0000064f, true},
    {42, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000f5f8, true},
    {42, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000003, true},
    {42, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007ff8, true},
    {42, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x00000f06, true},
    {42, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x000010d8, true},
    {93, {0x3edef7be, 0x477fe000, 0x80000000, 0x3f3bd98f}, 0xf803fe00, false},
    {93, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x76700000, false},
    {93, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x72d7bf7c, false},
    {93, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0xf80001fe, false},
    {93, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0xfffc0000, false},
    {93, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x6d84c600, false},
    {93, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x74633a00, false},
    {93, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0xc800011a, false},
    {93, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, false},
    {93, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0xf803fe00, false},
    {93, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0xc8027e00, false},
    {93, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x7f200029, false},
    {93, {0x3e6fce22, 0x3f000000, 0x3d45c8b0, 0x44e46365}, 0x78660078, true},
    {93, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x76740000, true},
    {93, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x72dbbf7c, true},
    {93, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0xf80001ff, true},
    {93, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0xfffc0000, true},
    {93, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x6d88c600, true},
    {93, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x74673a00, true},
    {93, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0xc800011b, true},
    {93, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, true},
    {93, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0xf803fe00, true},
    {93, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0xc8028000, true},
    {93, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x7f240029, true},
};

TEST(FormatStandIn, TheStoreStateStoresMetalsWords)
{
    uint32_t checked = 0;
    for (const Stored &s : kStored) {
        float v[4];
        for (int k = 0; k < 4; k++)
            v[k] = asFloat(s.rgba[k]);
        if (s.clear)
            gm20b::formatStandInClear(s.format, v, v, true);
        EXPECT_EQ(gm20b::formatStoredWord(s.format, v), s.word)
            << s.format << (s.clear ? " clear " : " draw ") << std::hex << s.rgba[0] << " " << s.rgba[1] << " "
            << s.rgba[2] << " " << s.rgba[3];
        checked++;
    }
    EXPECT_EQ(checked, 72u);
}

// Of a texture Metal does not compress (ShaderWrite, PixelFormatView or Unknown usage, a side under 16 texels), a clear
// stores as a draw of its colour does
TEST(FormatStandIn, AnUncompressedTexturesClearIsADrawOfItsColour)
{
    for (const Stored &s : kStored) {
        float v[4], held[4];
        for (int k = 0; k < 4; k++)
            v[k] = asFloat(s.rgba[k]);
        gm20b::formatStandInClear(s.format, v, held, false);
        EXPECT_EQ(gm20b::formatStoredWord(s.format, held), gm20b::formatStoredWord(s.format, v));
    }
}

TEST(FormatStandIn, EachFormatsStandIn)
{
    EXPECT_EQ(gm20b::formatStandIn(11), 71u);
    for (uint32_t format : {41u, 42u, 93u})
        EXPECT_EQ(gm20b::formatStandIn(format), 125u);
    for (uint32_t format : {10u, 70u, 71u, 80u, 125u, 540u})
        EXPECT_EQ(gm20b::formatStandIn(format), 0u);
    EXPECT_TRUE(gm20b::formatHoldsAlpha(41) && gm20b::formatHoldsAlpha(42));
    EXPECT_FALSE(gm20b::formatHoldsAlpha(11) || gm20b::formatHoldsAlpha(93));
}

TEST_F(DeviceFormatStandIn, AnSrgbR8TargetsFirstByteGoesIntoItsStandInAndBack)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexture(40, 12, gm20b::kR8UnormSrgb, t));
    EXPECT_FALSE(t.renderable || t.shadowTarget) << "no render target format of the GM20B's";
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 3, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->format, 71u) << "RGBA8Unorm_sRGB";
    EXPECT_TRUE(s->renderable && s->blockLinear);
    EXPECT_EQ(s->width, 40u);
    EXPECT_EQ(s->height, 12u);
    const uint64_t standIn = s->va;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    ASSERT_EQ(launches.size(), 2u);
    EXPECT_EQ(launches[0].from, t.va);
    EXPECT_EQ(launches[0].to, standIn);
    EXPECT_EQ(launches[0].remap, remap(1, 4)) << "the byte into red, green, blue and alpha left";
    EXPECT_EQ(launches[1].from, standIn);
    EXPECT_EQ(launches[1].to, t.va);
    EXPECT_EQ(launches[1].remap, remap(4, 1)) << "red back into the byte";
}

// RG8Unorm_sRGB alike, its two bytes (the GM20B has no two-component sRGB target): red and green of the stand-in, blue and
// alpha left, and back
TEST_F(DeviceFormatStandIn, AnSrgbRg8TargetsTwoBytesGoIntoItsStandInAndBack)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexture(40, 12, gm20b::kRg8UnormSrgb, t));
    EXPECT_FALSE(t.renderable || t.shadowTarget) << "no render target format of the GM20B's";
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 2, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->format, 71u) << "RGBA8Unorm_sRGB";
    const uint64_t standIn = s->va;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    ASSERT_EQ(launches.size(), 2u);
    constexpr uint32_t kTwoOfOne = 1u << 4 | 6u << 8 | 6u << 12;  // DST_X from 0, DST_Y from 1, Z and W not written
    EXPECT_EQ(launches[0].from, t.va);
    EXPECT_EQ(launches[0].to, standIn);
    EXPECT_EQ(launches[0].remap, kTwoOfOne | 1u << 20 | 3u << 24) << "two bytes into red and green";
    EXPECT_EQ(launches[1].from, standIn);
    EXPECT_EQ(launches[1].to, t.va);
    EXPECT_EQ(launches[1].remap, kTwoOfOne | 3u << 20 | 1u << 24) << "red and green back into the two bytes";
}

TEST_F(DeviceFormatStandIn, AClearedTargetsStandInIsNotFilled)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexture(16, 16, gm20b::kR8UnormSrgb, t));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    ASSERT_NE(gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, false, &why), nullptr) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    EXPECT_TRUE(launches.empty());
}

TEST_F(DeviceFormatStandIn, WithoutThePacksStatesAPackedFormatHasNoStandIn)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexture(16, 16, gm20b::kAbgr4Unorm, t));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    EXPECT_EQ(gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why), nullptr);
    ASSERT_NE(why, nullptr);
    EXPECT_NE(strstr(why, "no load or store state"), nullptr) << why;
    cb->abandon();
}

TEST_F(DeviceFormatStandIn, APackedTargetIsFilledByTheLoadStateAndStoredIntoItsWords)
{
    for (uint32_t format : {gm20b::kA1Bgr5Unorm, gm20b::kAbgr4Unorm, gm20b::kRgb9e5Float}) {
        addState(gm20b::formatLoadState(format), 125);
        addState(gm20b::formatStoreState(format), gm20b::formatStoreView(format));
    }
    const std::map<uint32_t, uint32_t> view = {
        {gm20b::kA1Bgr5Unorm, 0xf1}, {gm20b::kAbgr4Unorm, 0xf1}, {gm20b::kRgb9e5Float, 0xe4}};  // R16_UINT, R32_UINT
    for (const auto &[format, rt] : view) {
        gm20b::Texture t;
        ASSERT_TRUE(dev.createTexture(64, 16, format, t, false, true));
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        const char *why = nullptr;
        gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 1, true, &why);
        ASSERT_NE(s, nullptr) << why;
        EXPECT_EQ(s->format, 125u) << "RGBA32Float";
        const uint64_t standIn = s->va;
        ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
        uint32_t seq = 0;
        ASSERT_TRUE(cb->commit(seq));
        std::vector<Launch> launches;
        std::vector<Method> methods;
        decode(first, launches, methods);
        const auto passes = targets(methods);
        ASSERT_EQ(passes.size(), 2u) << format;
        EXPECT_EQ(passes[0].first, standIn) << "the load state's pass into the stand-in";
        EXPECT_EQ(passes[0].second, 0xc0u) << "RGBA32Float's target";
        EXPECT_EQ(passes[1].first, t.va) << "the store state's pass into the texture's words";
        EXPECT_EQ(passes[1].second, rt) << format;
    }
}

// BGR10_XR_sRGB: a texture of its codes, which the 3D engine renders none of; a pass renders into an RGBA16Float stand-in
// the texture's own sampling conversion fills from the codes, and the store state writes the stand-in back into them
TEST_F(DeviceFormatStandIn, AnSrgbExtendedRangeTargetRendersThroughAHalfStandInIntoItsCodes)
{
    addState(gm20b::formatLoadState(gm20b::kBgr10XrSrgb), 115);
    addState(gm20b::formatStoreState(gm20b::kBgr10XrSrgb), gm20b::formatStoreView(gm20b::kBgr10XrSrgb));
    gm20b::Texture t, words;
    ASSERT_TRUE(dev.createTexture(64, 32, gm20b::kBgr10XrSrgb, t, true, true));
    ASSERT_TRUE(dev.createTexture(64, 32, 53, words, true, true));
    EXPECT_FALSE(t.renderable || t.shadowTarget) << "no render target format of the GM20B's";
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, t.tic)[0], gm20b::DeviceHarness::header(dev, words.tic)[0])
        << "its codes, read as R32Uint's are";
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->format, 115u) << "RGBA16Float: Metal's tile holds the pass in halves";
    const uint64_t standIn = s->va;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    const auto passes = targets(methods);
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_EQ(passes[0].first, standIn) << "the load state's pass into the stand-in";
    EXPECT_EQ(passes[0].second, gm20b::formatInfo(115)->rt) << "RGBA16Float's target";
    EXPECT_EQ(passes[1].first, t.va) << "the store state's pass into the codes";
    EXPECT_EQ(passes[1].second, gm20b::formatInfo(53)->rt) << "R32Uint's target";
}

// BGRA10_XR_sRGB alike, over its fields: the load reads them through an RGBA16Uint header of its own (not the sampled
// stand-in a bind takes), and the store state writes the stand-in back into them viewed as RGBA16Uint
TEST_F(DeviceFormatStandIn, AnSrgbExtendedRangeTargetWithAlphaRendersThroughAHalfStandInIntoItsFields)
{
    addState(gm20b::formatLoadState(gm20b::kBgra10XrSrgb), 115);
    addState(gm20b::formatStoreState(gm20b::kBgra10XrSrgb), gm20b::formatStoreView(gm20b::kBgra10XrSrgb));
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 32, gm20b::kBgra10XrSrgb, t, true, true));
    EXPECT_FALSE(t.renderable || t.shadowTarget) << "no render target format of the GM20B's";
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->format, 115u) << "RGBA16Float: Metal's tile holds the pass in halves";
    const uint64_t standIn = s->va;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    const auto passes = targets(methods);
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_EQ(passes[0].first, standIn) << "the load state's pass into the stand-in";
    EXPECT_EQ(passes[1].first, t.va) << "the store state's pass into the fields";
    EXPECT_EQ(passes[1].second, gm20b::formatInfo(113)->rt) << "RGBA16Uint's target";
}

// RGB10A8_2P_XR10_sRGB of a descriptor keeps its texels, as an M4 Max keeps a one-level one's (m2v-textures3
// probes/keeps.mm): a pass fills its stand-in from its texels viewed as RG32Uint and stores it back into them
TEST_F(DeviceFormatStandIn, AnSrgbTwoPlaneFormatOfADescriptorKeepsItsTexels)
{
    addState(gm20b::formatLoadState(gm20b::kRgb10A8XrSrgb), 115);
    addState(gm20b::formatStoreState(gm20b::kRgb10A8XrSrgb), gm20b::formatStoreView(gm20b::kRgb10A8XrSrgb));
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 32, gm20b::kRgb10A8XrSrgb, t, true, true));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->format, 115u);
    const uint64_t standIn = s->va;
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    const auto passes = targets(methods);
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_EQ(passes[0].first, standIn) << "the load state's pass into the stand-in";
    EXPECT_EQ(passes[1].first, t.va) << "the store state's pass into its texels";
    EXPECT_EQ(passes[1].second, gm20b::formatInfo(103)->rt) << "RG32Uint's target";
}

// BGR10_XR, BGRA10_XR and RGB10A8_2P_XR10 of a descriptor: the GM20B renders their codes as integers, which it cannot
// blend, so a pass renders into an RGBA16Float stand-in, Metal's tile, which the load state fills with Metal's blend read
// of the codes and the store state writes back into them viewed as R32Uint, RGBA16Uint or RG32Uint
TEST_F(DeviceFormatStandIn, AnExtendedRangeTargetRendersThroughAHalfStandInIntoItsCodes)
{
    for (const uint32_t format : {554u, 552u, 550u}) {
        SCOPED_TRACE(format);
        EXPECT_EQ(gm20b::formatStandIn(format), 115u) << "RGBA16Float: Metal's tile holds the pass in halves";
        EXPECT_STREQ(gm20b::formatLoadState(format), gm20b::xrTargetState(format, gm20b::XrStep::Load));
        EXPECT_STREQ(gm20b::formatStoreState(format), gm20b::xrTargetState(format, gm20b::XrStep::Store));
        addState(gm20b::formatLoadState(format), 115);
        addState(gm20b::formatStoreState(format), gm20b::formatStoreView(format));
        gm20b::Texture t;
        ASSERT_TRUE(dev.createTexture(64, 32, format, t, true, true));
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        const char *why = nullptr;
        gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why);
        ASSERT_NE(s, nullptr) << why;
        const uint64_t standIn = s->va;
        ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why, true)) << why;
        uint32_t seq = 0;
        ASSERT_TRUE(cb->commit(seq));
        std::vector<Launch> launches;
        std::vector<Method> methods;
        decode(first, launches, methods);
        const auto passes = targets(methods);
        ASSERT_EQ(passes.size(), 2u);
        EXPECT_EQ(passes[0].first, standIn) << "the load state's pass into the stand-in";
        EXPECT_EQ(passes[0].second, gm20b::formatInfo(115)->rt) << "RGBA16Float's target";
        EXPECT_EQ(passes[1].first, t.va) << "the store state's pass into the codes";
        EXPECT_EQ(passes[1].second, gm20b::formatInfo(gm20b::formatStoreView(format))->rt);
    }
    EXPECT_TRUE(gm20b::formatHoldsAlpha(552) && gm20b::formatHoldsAlpha(550));
    EXPECT_FALSE(gm20b::formatHoldsAlpha(554)) << "a destination alpha of 1 (standin_blend.h)";
}

// A clear goes into such a stand-in as Metal's tile holds it: each channel's nearest half, BGR10_XR's alpha 1
TEST(FormatStandIn, AnExtendedRangeClearIsItsHalves)
{
    const float clear[4] = {0.1f, -0.3f, 1.3f, 0.6f};
    for (const uint32_t format : {550u, 552u, 554u}) {
        float held[4];
        gm20b::formatStandInClear(format, clear, held, true);
        for (int k = 0; k < 3; k++)
            EXPECT_EQ(held[k], float(_Float16(clear[k]))) << format << " " << k;
        EXPECT_EQ(held[3], format == 554 ? 1.0f : float(_Float16(clear[3]))) << format;
    }
}

// A turned target's stand-in is laid out as its plane (the pass draws into it turned: TurnedPass), so its fill and store are
// passes of the plane as it lies: no turned viewport, no coordinate swizzle
TEST_F(DeviceFormatStandIn, ATurnedTargetsStandInIsStoredAsThePlaneLies)
{
    addState(gm20b::formatLoadState(gm20b::kBgr10XrSrgb), 115);
    addState(gm20b::formatStoreState(gm20b::kBgr10XrSrgb), gm20b::formatStoreView(gm20b::kBgr10XrSrgb));
    gm20b::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 32, gm20b::kBgr10XrSrgb, t, true, true));
    t.rotation = 1;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, whole(t), 1, 0, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->width, 64u) << "the plane's";
    ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, whole(t), 1, *s, false, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    const auto passes = targets(methods);
    ASSERT_EQ(passes.size(), 2u);
    EXPECT_EQ(passes[1].first, t.va) << "the store state's pass into the codes";
    for (const Method &m : methods)
        if (m.method == NVB197_SET_VIEWPORT_COORDINATE_SWIZZLE(0))
            EXPECT_NE(m.value, gm20b::kSwizzleExchange) << "a store drawn turned";
}

TEST_F(DeviceFormatStandIn, ALevelOfAChainRendersThroughItsOwnStandIn)
{
    for (uint32_t format : {gm20b::kA1Bgr5Unorm, gm20b::kAbgr4Unorm, gm20b::kRgb9e5Float}) {
        addState(gm20b::formatLoadState(format), 125);
        addState(gm20b::formatStoreState(format), gm20b::formatStoreView(format));
    }
    for (uint32_t format : {gm20b::kR8UnormSrgb, gm20b::kA1Bgr5Unorm, gm20b::kAbgr4Unorm, gm20b::kRgb9e5Float}) {
        gm20b::Texture t, level;
        ASSERT_TRUE(dev.createMipTexture(64, 32, format, 4, t)) << format;
        ASSERT_TRUE(gm20b::attachmentTarget(t, 2, 0, level)) << format;
        EXPECT_FALSE(level.renderable) << "no render target format of the GM20B's";
        EXPECT_EQ(level.width, 16u);
        level.tic = dev.attachmentTic(t, level, 2, 0);
        gm20b::ColorAttachment part;
        part.texture = &level;
        part.image = &t;
        part.level = 2;
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        const char *why = nullptr;
        gm20b::Texture *s = gm20b::formatStandInFor(dev, *cb, part, 1, 0, true, &why);
        ASSERT_NE(s, nullptr) << why;
        EXPECT_EQ(s->width, 16u);
        EXPECT_EQ(s->height, 8u);
        ASSERT_TRUE(gm20b::storeFormatStandIn(dev, *cb, part, 1, *s, false, &why)) << why;
        uint32_t seq = 0;
        ASSERT_TRUE(cb->commit(seq));
        std::vector<Launch> launches;
        std::vector<Method> methods;
        decode(first, launches, methods);
        if (format == gm20b::kR8UnormSrgb) {
            ASSERT_EQ(launches.size(), 2u);
            EXPECT_EQ(launches[1].to, level.va) << "the level's bytes";
        } else {
            const auto passes = targets(methods);
            ASSERT_EQ(passes.size(), 2u);
            EXPECT_EQ(passes[1].first, level.va) << "the store state's pass into the level's words";
        }
    }
}

// generateMipmapsForTexture: of a chain of each format: one pass a level and layer, of the format's mip state reading the
// level above's words, into the level's words viewed as the integers of its texel size
TEST_F(DeviceFormatStandIn, EachFormatsLevelsAreItsMipStatesPasses)
{
    const std::map<uint32_t, uint32_t> view = {{gm20b::kR8UnormSrgb, 0xf6},
                                               {gm20b::kA1Bgr5Unorm, 0xf1},
                                               {gm20b::kAbgr4Unorm, 0xf1},
                                               {gm20b::kRgb9e5Float, 0xe4}};  // R8_UINT, R16_UINT, R32_UINT
    for (const auto &[format, rt] : view) {
        gm20b::Texture t;
        ASSERT_TRUE(dev.createMipTexture(64, 32, format, 4, t)) << format;
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const char *why = nullptr;
        EXPECT_FALSE(gm20b::formatMipPasses(dev, *cb, t, 0, 3, 0, 1, format, &why)) << "no mip state in the pack yet";
        cb->abandon();
        addState(gm20b::formatMipState(format), gm20b::formatMipView(format));
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        ASSERT_TRUE(gm20b::formatMipPasses(dev, *cb, t, 0, 3, 0, 1, format, &why)) << why;
        uint32_t seq = 0;
        ASSERT_TRUE(cb->commit(seq));
        std::vector<Launch> launches;
        std::vector<Method> methods;
        decode(first, launches, methods);
        const auto passes = targets(methods);
        ASSERT_EQ(passes.size(), 3u) << format;
        for (uint32_t l = 1; l <= 3; l++) {
            gm20b::Texture level;
            ASSERT_TRUE(gm20b::attachmentTarget(t, l, 0, level));
            EXPECT_EQ(passes[l - 1].first, level.va) << format << " level " << l;
            EXPECT_EQ(passes[l - 1].second, rt) << format;
        }
    }
}

}  // namespace
