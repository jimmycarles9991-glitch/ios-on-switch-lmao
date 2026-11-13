// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_store.h: Metal's store of a pass into a two-plane YCbCr code attachment, against bytes an M4 Max's
// Metal stored (the measured texels below; every byte of its passes: raw/test/ycbcrstoreoracle.mm), and the raw path's route to
// it: the pack's store states (pack/ycbcr_store.metal, step for step ycbcrStoreDrawn) over the pass's stand-in. Also which
// YCbCr textures planYcbcr takes as render targets. The converted and sRGB targets: raw_ycbcr_converted_store_test.cpp.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "../raw/runtime/raw_ycbcr.h"
#include "../raw/runtime/raw_ycbcr_store.h"

namespace {

float asFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// One chroma block as Metal stored it: each texel's red, green and blue (float bits), its luma codes and the block's Cb, Cr
struct Block {
    uint32_t rgb[4][3];
    uint16_t luma[4];
    uint16_t cb, cr;
};

std::vector<float> texels(const Block &b, uint32_t n)
{
    std::vector<float> v(4 * n, 1.0f);
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++)
            v[4 * i + c] = asFloat(b.rgb[i][c]);
    return v;
}

constexpr Block k540[] = {
    {{{1060967144, 1064460635, 1057066791},
      {1041835799, 1051208286, 1060668499},
      {1027859266, 1053797362, 1047887546},
      {1060189453, 3165423067, 1051114670}},
     {241, 84, 103, 0},
     114,
     104},
    {{{3161701666, 1050864265, 1060320806},
      {1058213608, 1050108506, 1062745480},
      {1059293905, 1040927776, 1064748326},
      {1045103019, 1063515761, 1025273010}},
     {81, 75, 35, 227},
     163,
     90},
    {{{1061615641, 1062514497, 1032997430},
      {1060987518, 1049617573, 3166012900},
      {1032437385, 1035057288, 1058300985},
      {1041109397, 1058764876, 1060035990}},
     {212, 72, 22, 155},
     85,
     110},
    {{{1065393163, 1049617638, 1054481234},
      {1062843328, 1057972844, 1062338915},
      {3146959981, 1058977655, 1058364916},
      {1055508850, 1056676757, 3171775620}},
     {72, 143, 158, 125},
     117,
     147},
};

constexpr uint16_t k543Luma[][4] = {
    {969, 336, 415, 0}, {325, 302, 139, 911}, {850, 287, 89, 621}, {287, 573, 634, 503}};
constexpr uint16_t k543Chroma[][2] = {{458, 416}, {651, 361}, {341, 441}, {468, 590}};

TEST(RawYcbcrStore, EightBitFourTwoZeroIsTheBlocksRoundedMeanOfItsTexelsCodes)
{
    const raw::YcbcrCodeTarget t = {255, true};
    for (const Block &b : k540) {
        const std::vector<float> v = texels(b, 4);
        uint16_t luma[4], chroma[2];
        ASSERT_TRUE(raw::ycbcrStore(t, v.data(), 2, 2, raw::YcbcrTexelSource::DrawnFloat, luma, chroma));
        for (int i = 0; i < 4; i++)
            EXPECT_EQ(luma[i], b.luma[i]);
        EXPECT_EQ(chroma[0], b.cb);
        EXPECT_EQ(chroma[1], b.cr);
    }
}

TEST(RawYcbcrStore, TenBitCodesTakeTheirDrawnFloatsTowardZero)
{
    const raw::YcbcrCodeTarget t = {1023, true};
    for (size_t k = 0; k < 4; k++) {
        const std::vector<float> v = texels(k540[k], 4);
        uint16_t luma[4], chroma[2];
        ASSERT_TRUE(raw::ycbcrStore(t, v.data(), 2, 2, raw::YcbcrTexelSource::DrawnFloat, luma, chroma));
        for (int i = 0; i < 4; i++)
            EXPECT_EQ(luma[i], k543Luma[k][i]);
        EXPECT_EQ(chroma[0], k543Chroma[k][0]);
        EXPECT_EQ(chroma[1], k543Chroma[k][1]);
    }
}

TEST(RawYcbcrStore, FourTwoTwoIsThePairsMeanRoundedUp)
{
    const Block b[] = {
        {{{1060967144, 1064460635, 1057066791}, {1041835799, 1051208286, 1060668499}}, {241, 84}, 157, 113},
        {{{1058019451, 1050525071, 1063281338}, {1042085783, 1040321536, 3173461943}}, {79, 32}, 112, 92},
        {{{1065518380, 1064249310, 1058403101}, {998706520, 1052469293, 1064682394}}, {238, 93}, 197, 128},
        {{{1045955112, 1058851685, 1063224899}, {1048743012, 1063164290, 1060127544}}, {156, 222}, 200, 60},
    };
    for (const Block &x : b) {
        const std::vector<float> v = texels(x, 2);
        uint16_t luma[2], chroma[2];
        ASSERT_TRUE(raw::ycbcrStore({255, false}, v.data(), 2, 1, raw::YcbcrTexelSource::DrawnFloat, luma, chroma));
        EXPECT_EQ(luma[0], x.luma[0]);
        EXPECT_EQ(luma[1], x.luma[1]);
        EXPECT_EQ(chroma[0], x.cb);
        EXPECT_EQ(chroma[1], x.cr);
    }
}

TEST(RawYcbcrStore, ALumaCodeIsItsHeldHalfsNotItsFloats)
{
    // (float bits, Metal's code): floats beside the 1/510 and 1/2046 ties, where the half and the float round apart
    const std::pair<uint32_t, uint16_t> eight[] = {{0x7f800000, 255}, {0x7fc00000, 0}, {989888641, 0},
                                                   {1002488002, 1},   {1008771233, 2}, {1012981986, 3},
                                                   {1016107151, 5},   {1018212530, 6}};
    for (auto [bits, code] : eight)
        EXPECT_EQ(raw::ycbcrTexelCode(asFloat(bits), raw::YcbcrTexelSource::DrawnFloat, 255), code) << bits;
    const std::pair<uint32_t, uint16_t> ten[] = {{1042983595, 170}, {1043246764, 174}, {1043246770, 174},
                                                 {1043509941, 178}, {1043773114, 182}, {1044036281, 186}};
    for (auto [bits, code] : ten)
        EXPECT_EQ(raw::ycbcrTexelCode(asFloat(bits), raw::YcbcrTexelSource::DrawnFloat, 1023), code) << bits;
    // a 10-bit clear's colour rounds to the nearest half (145 of the probe's 903 clear channels tell the two apart)
    EXPECT_EQ(raw::ycbcrTexelCode(asFloat(1043509941), raw::YcbcrTexelSource::Clear, 1023), 179u);
}

TEST(RawYcbcrStore, ConvertedFormatsAreMtwovsRule)
{
    // YCBCR8_420_2P (500) over '420v' and '420f', no matrix (BT.601)
    const Block b = k540[0];
    const std::vector<float> v = texels(b, 4);

    const struct {
        bool full;
        uint16_t luma[4], cb, cr;
    } want[] = {{false, {199, 86, 77, 69}, 132, 126}, {true, {213, 81, 71, 62}, 132, 126}};

    for (const auto &w : want) {
        uint16_t luma[4], chroma[2];
        ASSERT_TRUE(raw::ycbcrStoreConverted({8, false, w.full, raw::YcbcrMatrix::Bt601, true, true}, nullptr, v.data(),
                                             2, 2, raw::YcbcrTexelSource::DrawnFloat, luma, chroma));
        for (int i = 0; i < 4; i++)
            EXPECT_EQ(luma[i], w.luma[i]);
        EXPECT_EQ(chroma[0], w.cb);
        EXPECT_EQ(chroma[1], w.cr);
    }
}

// A pass's floats: uniform ones beside [0, 1], the ties of every code width, and specials
std::vector<float> passTexels(uint32_t w, uint32_t h, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<float> rgba(4 * w * h);
    const float specials[] = {
        std::numeric_limits<float>::quiet_NaN(), INFINITY, -INFINITY, -0.0f, 0.0f, 1.0f, 0.5f, 1.5f, -0.25f};
    for (float &x : rgba) {
        const uint32_t pick = rng() % 8;
        if (pick == 0)
            x = specials[rng() % 9];
        else if (pick == 1)
            x = (float(rng() % 255) + 0.5f) / 255.0f;
        else if (pick == 2)
            x = (float(rng() % 1023) + 0.5f) / 1023.0f;
        else
            x = std::uniform_real_distribution<float>(-0.05f, 1.05f)(rng);
    }
    return rgba;
}

TEST(RawYcbcrStore, TheStoreStatesStoreMetalsCodes)
{
    constexpr uint32_t w = 96, h = 40;
    for (uint32_t bits : {8u, 10u})
        for (bool down : {true, false}) {
            const std::vector<float> rgba = passTexels(w, h, bits * 2 + down);
            std::vector<uint16_t> luma(w * h), chroma(w * h), wantLuma(w * h), wantChroma(w * h);
            ASSERT_TRUE(raw::ycbcrStoreDrawn(raw::ycbcrCodesShape(bits, true, down), nullptr, rgba.data(), w, h,
                                             luma.data(), chroma.data()));
            ASSERT_TRUE(raw::ycbcrStore({(1u << bits) - 1, down}, rgba.data(), w, h, raw::YcbcrTexelSource::DrawnFloat,
                                        wantLuma.data(), wantChroma.data()));
            EXPECT_EQ(luma, wantLuma) << bits << (down ? " 4:2:0" : " 4:2:2");
            EXPECT_EQ(chroma, wantChroma) << bits << (down ? " 4:2:0" : " 4:2:2");
        }
}

TEST(RawYcbcrStore, AClearsColourRoundedToItsHalfFirstStoresMetalsClearCodes)
{
    constexpr uint32_t w = 32, h = 16;
    const std::vector<float> rgba = passTexels(w, h, 7);
    std::vector<float> rounded(rgba.size());
    for (size_t i = 0; i < rgba.size(); i++)
        rounded[i] = float(_Float16(rgba[i]));
    std::vector<uint16_t> luma(w * h), chroma(w * h), wantLuma(w * h), wantChroma(w * h);
    ASSERT_TRUE(raw::ycbcrStoreDrawn(raw::ycbcrCodesShape(10, true, true), nullptr, rounded.data(), w, h, luma.data(),
                                     chroma.data()));
    ASSERT_TRUE(raw::ycbcrStore({1023, true}, rgba.data(), w, h, raw::YcbcrTexelSource::Clear, wantLuma.data(),
                                wantChroma.data()));
    EXPECT_EQ(luma, wantLuma);
    EXPECT_EQ(chroma, wantChroma);
}

TEST(RawYcbcrStore, FourFourFourChromaIsEachTexelsOwnCodes)
{
    constexpr uint32_t w = 8, h = 4;
    const std::vector<float> rgba = passTexels(w, h, 444);
    std::vector<uint16_t> luma(w * h), chroma(2 * w * h);
    ASSERT_TRUE(raw::ycbcrStoreDrawn(raw::ycbcrCodesShape(10, false, false), nullptr, rgba.data(), w, h, luma.data(),
                                     chroma.data()));
    for (uint32_t i = 0; i < w * h; i++) {
        EXPECT_EQ(chroma[2 * i], raw::ycbcrTexelCode(rgba[4 * i + 2], raw::YcbcrTexelSource::DrawnFloat, 1023));
        EXPECT_EQ(chroma[2 * i + 1], raw::ycbcrTexelCode(rgba[4 * i], raw::YcbcrTexelSource::DrawnFloat, 1023));
    }
}

raw::YcbcrFacts target(uint32_t format, uint32_t fourcc, uint32_t w, uint32_t h, uint32_t chromaRows)
{
    raw::YcbcrFacts f;
    f.pixelFormat = format;
    f.fourcc = fourcc;
    f.planes = 2;
    f.width = f.surfaceWidth = w;
    f.height = f.surfaceHeight = h;
    f.pitch[0] = f.pitch[1] = (w + 127) & ~127u;
    f.offset[1] = uint64_t(f.pitch[0]) * h;
    f.allocBytes = f.offset[1] + uint64_t(f.pitch[1]) * chromaRows;
    f.simple = true;
    f.target = true;
    return f;
}

constexpr uint32_t k420v = 0x34323076, k422v = 0x34323276, k444v = 0x34343476, kX420 = 0x78343230, kXf20 = 0x78663230,
                   kP420 = 0x70343230;

TEST(RawYcbcrStore, TargetsAreTakenOverTheirOwnChroma)
{
    raw::YcbcrPlan p;
    EXPECT_EQ(raw::planYcbcr(target(540, k420v, 1920, 1080, 540), p), nullptr);
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.chromaWidth, 960u);
    EXPECT_EQ(p.chromaHeight, 540u);
    EXPECT_EQ(raw::planYcbcr(target(541, k422v, 1280, 720, 720), p), nullptr);
    EXPECT_EQ(p.chromaHeight, 720u);
    EXPECT_NE(raw::planYcbcr(target(540, k420v, 1919, 1080, 540), p), nullptr);
    EXPECT_NE(raw::planYcbcr(target(540, k420v, 1920, 1079, 540), p), nullptr);
    EXPECT_NE(raw::planYcbcr(target(540, k422v, 1920, 1080, 1080), p), nullptr);
    raw::YcbcrFacts ten = target(543, kX420, 1920, 1080, 540);
    ten.pitch[0] = ten.pitch[1] = 3840;
    ten.offset[1] = uint64_t(ten.pitch[0]) * 1080;
    ten.allocBytes = ten.offset[1] + uint64_t(ten.pitch[1]) * 540;
    EXPECT_EQ(raw::planYcbcr(ten, p), nullptr);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.shift, 6u);
    EXPECT_EQ(p.store.towardZero, 1u);
    EXPECT_EQ(p.lumaFormat, 20u);
    EXPECT_EQ(p.chromaFormat, 60u);
    EXPECT_EQ(raw::planYcbcr(target(500, k420v, 1920, 1080, 540), p), nullptr);
    EXPECT_EQ(p.store.converted, 1u);
    EXPECT_EQ(p.store.offset[0], 16);
    EXPECT_EQ(p.store.srgb, 0u);
    raw::YcbcrFacts four = target(503, k444v, 1920, 1080, 1080);
    four.pitch[1] = 3840;
    four.allocBytes = four.offset[1] + uint64_t(four.pitch[1]) * 1080;
    EXPECT_EQ(raw::planYcbcr(four, p), nullptr);
    EXPECT_EQ(p.store.converted, 1u);
    EXPECT_EQ(p.store.across, 0u);
    raw::YcbcrFacts srgb = ten;
    srgb.pixelFormat = 525;
    srgb.fourcc = kXf20;
    EXPECT_EQ(raw::planYcbcr(srgb, p), nullptr);
    EXPECT_EQ(p.store.srgb, 1u);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.termBits, 4u);
    EXPECT_EQ(p.store.offset[0], 0);
    EXPECT_EQ(p.store.offset[1], 512);
    EXPECT_EQ(p.store.mul[0], 1023u);
    EXPECT_NE(raw::planYcbcr(target(546, kX420, 1920, 1080, 540), p), nullptr);
    raw::YcbcrFacts packed = target(546, kP420, 1920, 1080, 540);
    packed.pitch[0] = packed.pitch[1] = 2560;
    packed.offset[1] = uint64_t(packed.pitch[0]) * 1080;
    packed.allocBytes = packed.offset[1] + uint64_t(packed.pitch[1]) * 540;
    EXPECT_EQ(raw::planYcbcr(packed, p), nullptr);
    EXPECT_EQ(p.store.packed, 1u);
    EXPECT_EQ(p.store.shift, 0u);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.converted, 0u);
    EXPECT_EQ(p.lumaWidth, 640u);
    EXPECT_EQ(p.chromaWidth, 640u);
    raw::YcbcrFacts sampled = target(500, k420v, 1920, 1080, 540);
    sampled.target = false;
    EXPECT_EQ(raw::planYcbcr(sampled, p), nullptr);
    EXPECT_FALSE(p.target);
}

}  // namespace
