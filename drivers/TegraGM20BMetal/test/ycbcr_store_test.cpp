// SPDX-License-Identifier: MIT
// ycbcr_store.h: Metal's store of a pass into a two-plane YCbCr code attachment, against bytes an M4 Max's
// Metal stored (the measured texels below; every byte of its passes: harness/ycbcrstoreoracle.mm), and the driver's route to
// it: the pack's store states (pack/ycbcr_store.metal, step for step ycbcrStoreDrawn) over the pass's stand-in. Also which
// YCbCr textures planYcbcr takes as render targets. The converted and sRGB targets: ycbcr_converted_store_test.cpp.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

#include "../overlap_order.h"
#include "../standin_blend.h"
#include "../ycbcr.h"
#include "../ycbcr_store.h"

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

TEST(GM20BYcbcrStore, EightBitFourTwoZeroIsTheBlocksRoundedMeanOfItsTexelsCodes)
{
    const gm20b::YcbcrCodeTarget t = {255, true};
    for (const Block &b : k540) {
        const std::vector<float> v = texels(b, 4);
        uint16_t luma[4], chroma[2];
        ASSERT_TRUE(gm20b::ycbcrStore(t, v.data(), 2, 2, gm20b::YcbcrTexelSource::DrawnFloat, luma, chroma));
        for (int i = 0; i < 4; i++)
            EXPECT_EQ(luma[i], b.luma[i]);
        EXPECT_EQ(chroma[0], b.cb);
        EXPECT_EQ(chroma[1], b.cr);
    }
}

TEST(GM20BYcbcrStore, TenBitCodesTakeTheirDrawnFloatsTowardZero)
{
    const gm20b::YcbcrCodeTarget t = {1023, true};
    for (size_t k = 0; k < 4; k++) {
        const std::vector<float> v = texels(k540[k], 4);
        uint16_t luma[4], chroma[2];
        ASSERT_TRUE(gm20b::ycbcrStore(t, v.data(), 2, 2, gm20b::YcbcrTexelSource::DrawnFloat, luma, chroma));
        for (int i = 0; i < 4; i++)
            EXPECT_EQ(luma[i], k543Luma[k][i]);
        EXPECT_EQ(chroma[0], k543Chroma[k][0]);
        EXPECT_EQ(chroma[1], k543Chroma[k][1]);
    }
}

TEST(GM20BYcbcrStore, FourTwoTwoIsThePairsMeanRoundedUp)
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
        ASSERT_TRUE(gm20b::ycbcrStore({255, false}, v.data(), 2, 1, gm20b::YcbcrTexelSource::DrawnFloat, luma, chroma));
        EXPECT_EQ(luma[0], x.luma[0]);
        EXPECT_EQ(luma[1], x.luma[1]);
        EXPECT_EQ(chroma[0], x.cb);
        EXPECT_EQ(chroma[1], x.cr);
    }
}

TEST(GM20BYcbcrStore, ALumaCodeIsItsHeldHalfsNotItsFloats)
{
    // (float bits, Metal's code): floats beside the 1/510 and 1/2046 ties, where the half and the float round apart
    const std::pair<uint32_t, uint16_t> eight[] = {{0x7f800000, 255}, {0x7fc00000, 0}, {989888641, 0},
                                                   {1002488002, 1},   {1008771233, 2}, {1012981986, 3},
                                                   {1016107151, 5},   {1018212530, 6}};
    for (auto [bits, code] : eight)
        EXPECT_EQ(gm20b::ycbcrTexelCode(asFloat(bits), gm20b::YcbcrTexelSource::DrawnFloat, 255), code) << bits;
    const std::pair<uint32_t, uint16_t> ten[] = {{1042983595, 170}, {1043246764, 174}, {1043246770, 174},
                                                 {1043509941, 178}, {1043773114, 182}, {1044036281, 186}};
    for (auto [bits, code] : ten)
        EXPECT_EQ(gm20b::ycbcrTexelCode(asFloat(bits), gm20b::YcbcrTexelSource::DrawnFloat, 1023), code) << bits;
    // a 10-bit clear's colour rounds to the nearest half (145 of the probe's 903 clear channels tell the two apart)
    EXPECT_EQ(gm20b::ycbcrTexelCode(asFloat(1043509941), gm20b::YcbcrTexelSource::Clear, 1023), 179u);
}

TEST(GM20BYcbcrStore, ConvertedFormatsAreMtwovsRule)
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
        ASSERT_TRUE(gm20b::ycbcrStoreConverted({8, false, w.full, gm20b::YcbcrMatrix::Bt601, true, true}, nullptr,
                                               v.data(), 2, 2, gm20b::YcbcrTexelSource::DrawnFloat, luma, chroma));
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

TEST(GM20BYcbcrStore, TheStoreStatesStoreMetalsCodes)
{
    constexpr uint32_t w = 96, h = 40;
    for (uint32_t bits : {8u, 10u})
        for (bool down : {true, false}) {
            const std::vector<float> rgba = passTexels(w, h, bits * 2 + down);
            std::vector<uint16_t> luma(w * h), chroma(w * h), wantLuma(w * h), wantChroma(w * h);
            ASSERT_TRUE(gm20b::ycbcrStoreDrawn(gm20b::ycbcrCodesShape(bits, true, down), nullptr, rgba.data(), w, h,
                                               luma.data(), chroma.data()));
            ASSERT_TRUE(gm20b::ycbcrStore({(1u << bits) - 1, down}, rgba.data(), w, h,
                                          gm20b::YcbcrTexelSource::DrawnFloat, wantLuma.data(), wantChroma.data()));
            EXPECT_EQ(luma, wantLuma) << bits << (down ? " 4:2:0" : " 4:2:2");
            EXPECT_EQ(chroma, wantChroma) << bits << (down ? " 4:2:0" : " 4:2:2");
        }
}

TEST(GM20BYcbcrStore, AClearsColourRoundedToItsHalfFirstStoresMetalsClearCodes)
{
    constexpr uint32_t w = 32, h = 16;
    const std::vector<float> rgba = passTexels(w, h, 7);
    std::vector<float> rounded(rgba.size());
    for (size_t i = 0; i < rgba.size(); i++)
        rounded[i] = float(_Float16(rgba[i]));
    std::vector<uint16_t> luma(w * h), chroma(w * h), wantLuma(w * h), wantChroma(w * h);
    ASSERT_TRUE(gm20b::ycbcrStoreDrawn(gm20b::ycbcrCodesShape(10, true, true), nullptr, rounded.data(), w, h,
                                       luma.data(), chroma.data()));
    ASSERT_TRUE(gm20b::ycbcrStore({1023, true}, rgba.data(), w, h, gm20b::YcbcrTexelSource::Clear, wantLuma.data(),
                                  wantChroma.data()));
    EXPECT_EQ(luma, wantLuma);
    EXPECT_EQ(chroma, wantChroma);
}

TEST(GM20BYcbcrStore, FourFourFourChromaIsEachTexelsOwnCodes)
{
    constexpr uint32_t w = 8, h = 4;
    const std::vector<float> rgba = passTexels(w, h, 444);
    std::vector<uint16_t> luma(w * h), chroma(2 * w * h);
    ASSERT_TRUE(gm20b::ycbcrStoreDrawn(gm20b::ycbcrCodesShape(10, false, false), nullptr, rgba.data(), w, h,
                                       luma.data(), chroma.data()));
    for (uint32_t i = 0; i < w * h; i++) {
        EXPECT_EQ(chroma[2 * i], gm20b::ycbcrTexelCode(rgba[4 * i + 2], gm20b::YcbcrTexelSource::DrawnFloat, 1023));
        EXPECT_EQ(chroma[2 * i + 1], gm20b::ycbcrTexelCode(rgba[4 * i], gm20b::YcbcrTexelSource::DrawnFloat, 1023));
    }
}

gm20b::YcbcrFacts target(uint32_t format, uint32_t fourcc, uint32_t w, uint32_t h, uint32_t chromaRows)
{
    gm20b::YcbcrFacts f;
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

TEST(GM20BYcbcrStore, TargetsAreTakenOverTheirOwnChroma)
{
    gm20b::YcbcrPlan p;
    EXPECT_EQ(gm20b::planYcbcr(target(540, k420v, 1920, 1080, 540), p), nullptr);
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.chromaWidth, 960u);
    EXPECT_EQ(p.chromaHeight, 540u);
    EXPECT_EQ(gm20b::planYcbcr(target(541, k422v, 1280, 720, 720), p), nullptr);
    EXPECT_EQ(p.chromaHeight, 720u);
    // an odd size (a texture made to be read alone, which an M4 Max renders all the same) and chroma coarser than the
    // surface's are targets too (m2v-textures5 probes/targets.mm, edges.mm)
    EXPECT_EQ(gm20b::planYcbcr(target(540, k420v, 1919, 1080, 540), p), nullptr);
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.chromaWidth, 960u);
    EXPECT_EQ(gm20b::planYcbcr(target(540, k420v, 1920, 1079, 540), p), nullptr);
    EXPECT_EQ(p.chromaHeight, 540u);
    EXPECT_EQ(gm20b::planYcbcr(target(540, k422v, 1920, 1080, 1080), p), nullptr);
    EXPECT_EQ(p.chromaHeight, 540u) << "4:2:0 codes stored into the first half of a 4:2:2 surface's chroma rows";
    gm20b::YcbcrFacts ten = target(543, kX420, 1920, 1080, 540);
    ten.pitch[0] = ten.pitch[1] = 3840;
    ten.offset[1] = uint64_t(ten.pitch[0]) * 1080;
    ten.allocBytes = ten.offset[1] + uint64_t(ten.pitch[1]) * 540;
    EXPECT_EQ(gm20b::planYcbcr(ten, p), nullptr);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.shift, 6u);
    EXPECT_EQ(p.store.towardZero, 1u);
    EXPECT_EQ(p.lumaFormat, 20u);
    EXPECT_EQ(p.chromaFormat, 60u);
    EXPECT_EQ(gm20b::planYcbcr(target(500, k420v, 1920, 1080, 540), p), nullptr);
    EXPECT_EQ(p.store.converted, 1u);
    EXPECT_EQ(p.store.offset[0], 16);
    EXPECT_EQ(p.store.srgb, 0u);
    gm20b::YcbcrFacts four = target(503, k444v, 1920, 1080, 1080);
    four.pitch[1] = 3840;
    four.allocBytes = four.offset[1] + uint64_t(four.pitch[1]) * 1080;
    EXPECT_EQ(gm20b::planYcbcr(four, p), nullptr);
    EXPECT_EQ(p.store.converted, 1u);
    EXPECT_EQ(p.store.across, 0u);
    gm20b::YcbcrFacts srgb = ten;
    srgb.pixelFormat = 525;
    srgb.fourcc = kXf20;
    EXPECT_EQ(gm20b::planYcbcr(srgb, p), nullptr);
    EXPECT_EQ(p.store.srgb, 1u);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.termBits, 4u);
    EXPECT_EQ(p.store.offset[0], 0);
    EXPECT_EQ(p.store.offset[1], 512);
    EXPECT_EQ(p.store.mul[0], 1023u);
    EXPECT_NE(gm20b::planYcbcr(target(546, kX420, 1920, 1080, 540), p), nullptr);
    gm20b::YcbcrFacts packed = target(546, kP420, 1920, 1080, 540);
    packed.pitch[0] = packed.pitch[1] = 2560;
    packed.offset[1] = uint64_t(packed.pitch[0]) * 1080;
    packed.allocBytes = packed.offset[1] + uint64_t(packed.pitch[1]) * 540;
    EXPECT_EQ(gm20b::planYcbcr(packed, p), nullptr);
    EXPECT_EQ(p.store.packed, 1u);
    EXPECT_EQ(p.store.shift, 0u);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.converted, 0u);
    EXPECT_EQ(p.lumaWidth, 640u);
    EXPECT_EQ(p.chromaWidth, 640u);
    gm20b::YcbcrFacts sampled = target(500, k420v, 1920, 1080, 540);
    sampled.target = false;
    EXPECT_EQ(gm20b::planYcbcr(sampled, p), nullptr);
    EXPECT_FALSE(p.target);
}

// A plane's bytes from the first a run holds on, and the row each of those bytes holds after Apple's store
struct Run {
    uint32_t first, row;
};

// Every byte of a plane whose rows overlap holds the row the overlap states draw it from (overlap_order.h), as an M4 Max
// stored it in every one of 8 runs (m2v-textures7 probes/overlap.mm: 540 over a '420v' of rows `pitch` apart; the shapes whose
// runs agree)
void expectStoredAsApple(const gm20b::OverlapRows &s, const std::vector<Run> &runs, const char *what)
{
    const uint32_t span = s.pitch * (s.rows - 1) + s.row;
    const auto every = [](uint32_t, uint32_t) {
        return true;
    };
    for (uint32_t a = 0, k = 0; a < span; a += s.element) {
        while (k + 1 < runs.size() && runs[k + 1].first <= a)
            k++;
        uint32_t row = ~0u, offset = 0;
        ASSERT_TRUE(gm20b::overlapWinner(s, a, every, row, offset)) << what << " byte " << a;
        EXPECT_EQ(row, runs[k].row) << what << " byte " << a;
        EXPECT_EQ(offset, a - row * s.pitch) << what << " byte " << a;
    }
}

TEST(GM20BYcbcrStore, RowsThatOverlapKeepTheRowAppleStoresLast)
{
    // 64x16, rows 16 apart: one tile across; by column, then row pair, the even row last
    expectStoredAsApple(gm20b::overlapRowsOf(16, 64, 16, 1, 1, 1, 1),
                        {{0, 0}, {64, 2}, {96, 4}, {128, 6}, {160, 8}, {192, 10}, {224, 12}, {256, 14}, {288, 15}},
                        "64x16 luma");
    expectStoredAsApple(gm20b::overlapRowsOf(16, 64, 8, 2, 2, 2, 2),
                        {{0, 0}, {16, 1}, {32, 0}, {48, 1}, {64, 2}, {80, 3}, {96, 4}, {112, 5}, {128, 6}, {144, 7}},
                        "64x16 chroma");
    // 64x64: the upper tile row's stores land after the lower one's
    const std::vector<::Run> luma = {{0, 0},    {64, 2},   {96, 4},   {128, 6},  {160, 8},   {192, 10}, {224, 12},
                                     {256, 14}, {288, 16}, {320, 18}, {352, 20}, {384, 22},  {416, 24}, {448, 26},
                                     {480, 28}, {512, 30}, {544, 31}, {560, 32}, {576, 34},  {608, 36}, {640, 38},
                                     {672, 40}, {704, 42}, {736, 44}, {768, 46}, {800, 48},  {832, 50}, {864, 52},
                                     {896, 54}, {928, 56}, {960, 58}, {992, 60}, {1024, 62}, {1056, 63}};
    expectStoredAsApple(gm20b::overlapRowsOf(16, 64, 64, 1, 1, 1, 1), luma, "64x64 luma");
    const std::vector<::Run> chroma = {{0, 0},    {16, 1},   {32, 0},   {48, 1},   {64, 2},   {80, 3},   {96, 4},
                                       {112, 5},  {128, 6},  {144, 7},  {160, 8},  {176, 9},  {192, 10}, {208, 11},
                                       {224, 12}, {240, 13}, {256, 14}, {272, 15}, {304, 17}, {320, 18}, {336, 19},
                                       {352, 20}, {368, 21}, {384, 22}, {400, 23}, {416, 24}, {432, 25}, {448, 26},
                                       {464, 27}, {480, 28}, {496, 29}, {512, 30}, {528, 31}};
    expectStoredAsApple(gm20b::overlapRowsOf(16, 64, 32, 2, 2, 2, 2), chroma, "64x64 chroma");
    // 128x17, rows 32 apart: the second tile's stores land before the first's
    expectStoredAsApple(gm20b::overlapRowsOf(32, 128, 17, 1, 1, 1, 1),
                        {{0, 0},
                         {64, 1},
                         {96, 2},
                         {128, 3},
                         {160, 4},
                         {192, 5},
                         {224, 6},
                         {256, 7},
                         {288, 8},
                         {320, 9},
                         {352, 10},
                         {384, 11},
                         {416, 12},
                         {448, 13},
                         {480, 14},
                         {512, 15},
                         {544, 16},
                         {576, 15},
                         {608, 16}},
                        "128x17 luma");
    expectStoredAsApple(
        gm20b::overlapRowsOf(32, 128, 9, 2, 2, 2, 2),
        {{0, 0}, {64, 1}, {96, 2}, {128, 3}, {160, 4}, {192, 5}, {224, 6}, {256, 7}, {288, 8}, {320, 7}, {352, 8}},
        "128x17 chroma");
}

// A loaded pass stores only the rows in tiles its draws reach: a byte keeps the last of those, or no row where none reaches it
TEST(GM20BYcbcrStore, RowsThatOverlapStoreOnlyFromReachedTiles)
{
    gm20b::OverlapRows s = gm20b::overlapRowsOf(16, 64, 64, 1, 1, 1, 1);
    s.masked = 1;
    const auto upperHalf = [](uint32_t, uint32_t y) {
        return y < 32;
    };
    const auto lowerHalf = [](uint32_t, uint32_t y) {
        return y >= 32;
    };
    uint32_t row = 0, offset = 0;
    ASSERT_TRUE(gm20b::overlapWinner(s, 528, lowerHalf, row, offset));
    EXPECT_EQ(row, 32u) << "rows 30 and 31, which land later, lie in an unreached tile";
    EXPECT_EQ(offset, 16u);
    EXPECT_FALSE(gm20b::overlapWinner(s, 560, upperHalf, row, offset)) << "rows 32 to 35 lie in an unreached tile";
    EXPECT_FALSE(gm20b::overlapWinner(s, 0, lowerHalf, row, offset)) << "row 0's first byte is its alone";
    s.masked = 0;
    s.kept = 0;
    EXPECT_FALSE(gm20b::overlapWinner(s, 0, upperHalf, row, offset)) << "a kept byte no row stores";
}

// RGB10A8_2P_XR10 words whose rows run on into its alpha plane: a byte both planes reach keeps the store of the later 2x2
// quad, taken across both planes, the OR of a quad's word and alpha (an M4 Max's clear pass of a 32x8 texture of words 128
// bytes a row at 0 and alpha 32 a row at 512, 3 runs: alpha rows 0 to 3 keep word row 4, rows 4, 6 and 7 their alpha,
// row 5 the words to its 10th byte; bytes 0-1 of row 4 and 10-11 of row 5 the OR, hwtests8 mac/gpu.gm20b.xr_into_alpha_tie)
TEST(GM20BYcbcrStore, WordsRunningIntoTheAlphaPlaneKeepTheStoreAppleLandsLast)
{
    gm20b::OverlapPlanes s;
    s.pitch = 128;
    s.plane[0] = gm20b::meetPlane(0, 128, 128, 8, 4, 4);
    s.plane[1] = gm20b::meetPlane(512, 32, 32, 8, 1, 1);
    for (uint32_t a = 0; a < 1024; a++) {
        gm20b::OverlapWriters w;
        ASSERT_TRUE(gm20b::overlapPlanesWriters(s, a, w)) << a;
        const uint32_t r = (a - 512) / 32, b = (a - 512) % 32;
        const bool inAlpha = a >= 512 && a < 768;
        const bool word = !inAlpha || r < 4 || (r == 4 && b < 2) || (r == 5 && b < 12);
        const bool alpha = inAlpha && r >= 4 && !(r == 5 && b < 10);
        ASSERT_EQ(w.count, (word ? 1u : 0u) + (alpha ? 1u : 0u)) << "byte " << a;
        if (word)
            EXPECT_EQ(w.y[0] * 128 + 4 * w.x[0] + w.byte[0], a) << "byte " << a;
        if (alpha)
            EXPECT_EQ(std::make_tuple(w.plane[w.count - 1], w.x[w.count - 1], w.y[w.count - 1], w.byte[w.count - 1]),
                      std::make_tuple(1u, b, r, 0u))
                << "byte " << a;
    }
}

}  // namespace

// A blended or masked state's stand-in reads a destination alpha of 1, as an M4 Max blends a YCbCr attachment
// (harness/ycbcrblendoracle.mm): DestinationAlpha is One, OneMinusDestinationAlpha Zero, SourceAlphaSaturated Zero for a
// colour and One for alpha, every other factor itself
TEST(GM20BYcbcrStore, ABlendReadsADestinationAlphaOfOne)
{
    EXPECT_EQ(gm20b::alphaOneFactor(gm20b::kBlendDestinationAlpha, true), gm20b::kBlendOne);
    EXPECT_EQ(gm20b::alphaOneFactor(gm20b::kBlendDestinationAlpha, false), gm20b::kBlendOne);
    EXPECT_EQ(gm20b::alphaOneFactor(gm20b::kBlendOneMinusDestinationAlpha, true), gm20b::kBlendZero);
    EXPECT_EQ(gm20b::alphaOneFactor(gm20b::kBlendOneMinusDestinationAlpha, false), gm20b::kBlendZero);
    EXPECT_EQ(gm20b::alphaOneFactor(gm20b::kBlendSourceAlphaSaturated, true), gm20b::kBlendZero);
    EXPECT_EQ(gm20b::alphaOneFactor(gm20b::kBlendSourceAlphaSaturated, false), gm20b::kBlendOne);
    for (uint32_t f = 0; f <= 18; f++)
        if (f < gm20b::kBlendDestinationAlpha || f > gm20b::kBlendSourceAlphaSaturated)
            EXPECT_EQ(gm20b::alphaOneFactor(f, true), f);
}
