// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_store_rule.h's converted and sRGB targets (YCBCR*_2P: 503, 505-507; YCBCR*_2P_sRGB: 520, 522, 523,
// 525-527) against codes an M4 Max's Metal stored (blocks of float draws and clears where a near rule stores other codes: 10-bit
// floats held to nearest, the exact sRGB curve on the 1/8192 grid; every byte of their passes: raw/test/ycbcrstoreoracle.mm),
// and the pack's store states (pack/ycbcr_store.metal, step for step ycbcrStoreDrawn) against the rule.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "../raw/runtime/raw_ycbcr_store.h"

namespace {

const uint16_t kKnots[513] = {
#include "../raw/pack/srgb_knots.h"
};

const uint16_t kEncode[8193] = {
#include "../raw/pack/srgb_encode.h"
};

float asFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

raw::YcbcrConvertedTarget targetOf(uint32_t format, bool full, raw::YcbcrMatrix matrix)
{
    const bool f444 = format == 503 || format == 507 || format == 523 || format == 527;
    raw::YcbcrConvertedTarget t;
    t.bits = format < 505 || (format >= 520 && format < 525) ? 8 : 10;
    t.srgb = format >= 520;
    t.full = full;
    t.matrix = matrix;
    t.across = !f444;
    t.down = format == 500 || format == 505 || format == 520 || format == 525;
    return t;
}

// One chroma block as Metal stored it: the surface's range and matrix, each texel's red, green and blue (float bits), its
// luma codes and the block's Cb, Cr
struct Block {
    uint32_t format;
    bool full;
    raw::YcbcrMatrix matrix;
    uint32_t rgb[4][3];
    uint16_t luma[4];
    uint16_t cb, cr;
};

constexpr Block kDrawn[] = {
    {503, false, raw::YcbcrMatrix::Bt601, {{1063418878u, 1065352395u, 1059178612u}}, {218}, 91, 122},
    {503, false, raw::YcbcrMatrix::Bt601, {{1042419713u, 1050726101u, 1023045631u}}, {67}, 102, 116},
    {503, true, raw::YcbcrMatrix::Bt709, {{1063418878u, 1065352395u, 1059178612u}}, {242}, 84, 118},
    {503, true, raw::YcbcrMatrix::Bt709, {{1042419713u, 1050726101u, 1023045631u}}, {66}, 96, 111},
    {505,
     false,
     raw::YcbcrMatrix::Bt601,
     {{1051734016u, 1063543909u, 1026113563u},
      {1050820701u, 1060977984u, 1055496193u},
      {3166704674u, 1064262614u, 1044364047u},
      {1041186814u, 1040351878u, 1002488002u}},
     {617, 572, 563, 167},
     359,
     336},
    {505,
     false,
     raw::YcbcrMatrix::Bt601,
     {{1060577383u, 1059385346u, 1036804097u},
      {1053148611u, 1049442302u, 1060324901u},
      {1064271693u, 1062989824u, 1056245762u},
      {986185729u, 1061449726u, 997763968u}},
     {592, 377, 798, 459},
     390,
     478},
    {505,
     true,
     raw::YcbcrMatrix::Bt2020,
     {{0u, 1030285546u, 1061548034u},
      {1064620693u, 1059738623u, 1058394113u},
      {1060649017u, 1064563263u, 1060555108u},
      {1061635913u, 3197737370u, 990154752u}},
     {86, 754, 897, 209},
     535,
     608},
    {505,
     true,
     raw::YcbcrMatrix::Bt2020,
     {{1051734016u, 1063543909u, 1026113563u},
      {1050820701u, 1060977984u, 1055496193u},
      {3166704674u, 1064262614u, 1044364047u},
      {1041186814u, 1040351878u, 1002488002u}},
     {714, 625, 660, 126},
     323,
     290},
    {506,
     false,
     raw::YcbcrMatrix::Bt709,
     {{986853375u, 1061374976u, 1058131311u}, {3197737370u, 3197737370u, 1059420112u}},
     {578, 105},
     652,
     332},
    {506,
     false,
     raw::YcbcrMatrix::Bt709,
     {{0u, 1030285546u, 1061548034u}, {1064620693u, 1059738623u, 1058394113u}},
     {148, 696},
     642,
     551},
    {506,
     true,
     raw::YcbcrMatrix::Bt601,
     {{0u, 1030285546u, 1061548034u}, {1064620693u, 1059738623u, 1058394113u}},
     {124, 760},
     655,
     546},
    {506,
     true,
     raw::YcbcrMatrix::Bt601,
     {{1060577383u, 1059385346u, 1036804097u}, {1053148611u, 1049442302u, 1060324901u}},
     {617, 365},
     466,
     564},
    {507, false, raw::YcbcrMatrix::Bt2020, {{1063418878u, 1065352395u, 1059178612u}}, {894}, 362, 474},
    {507, false, raw::YcbcrMatrix::Bt2020, {{1048666111u, 1029232859u, 1021415425u}}, {155}, 476, 602},
    {507, true, raw::YcbcrMatrix::Bt709, {{1063418878u, 1065352395u, 1059178612u}}, {970}, 337, 470},
    {507, true, raw::YcbcrMatrix::Bt709, {{0u, 1030285546u, 1061548034u}}, {99}, 885, 449},
    {520,
     false,
     raw::YcbcrMatrix::Bt601,
     {{0u, 1030285546u, 1061548034u},
      {1064620693u, 1059738623u, 1058394113u},
      {1060649017u, 1064563263u, 1060555108u},
      {1061635913u, 3197737370u, 990154752u}},
     {72, 207, 220, 75},
     136,
     144},
    {520,
     false,
     raw::YcbcrMatrix::Bt601,
     {{1014583295u, 0u, 1065353216u},
      {1061282815u, 3197737370u, 1039739320u},
      {1008771232u, 1061400917u, 1046525996u},
      {3197737370u, 1050962201u, 1053453925u}},
     {49, 84, 149, 110},
     161,
     113},
    {520,
     true,
     raw::YcbcrMatrix::Bt709,
     {{1040878218u, 1064467149u, 1049230439u},
      {1042698157u, 1049405391u, 1048770727u},
      {1060097426u, 928677887u, 996593663u},
      {0u, 1056351388u, 1060441408u}},
     {210, 136, 47, 148},
     124,
     111},
    {520,
     true,
     raw::YcbcrMatrix::Bt709,
     {{1039875795u, 1041249518u, 1039463671u},
      {1059260608u, 1062939647u, 959754239u},
      {1059442689u, 1037340673u, 1018212526u},
      {1062847452u, 1054760959u, 1062606211u}},
     {103, 215, 113, 193},
     94,
     149},
    {522,
     false,
     raw::YcbcrMatrix::Bt2020,
     {{1020690434u, 989810689u, 1034850303u}, {1058969645u, 1045262336u, 1059806207u}},
     {34, 146},
     157,
     152},
    {522,
     false,
     raw::YcbcrMatrix::Bt2020,
     {{1043773111u, 1050757122u, 1055150080u}, {1054996389u, 1059873794u, 977367040u}},
     {140, 181},
     92,
     116},
    {522,
     true,
     raw::YcbcrMatrix::Bt601,
     {{1059588199u, 963375105u, 1057923158u}, {1053708492u, 1047511040u, 1055045634u}},
     {86, 149},
     167,
     180},
    {522,
     true,
     raw::YcbcrMatrix::Bt601,
     {{1040412671u, 1049201036u, 3175456610u}, {1052106751u, 1058178212u, 1059974350u}},
     {113, 190},
     103,
     113},
    {523, false, raw::YcbcrMatrix::Bt709, {{1057374608u, 1046375635u, 1061334015u}}, {144}, 165, 152},
    {523, false, raw::YcbcrMatrix::Bt709, {{1050349569u, 1019282390u, 1065353216u}}, {85}, 211, 166},
    {523, true, raw::YcbcrMatrix::Bt601, {{1051159002u, 1022423280u, 1016107156u}}, {79}, 104, 183},
    {523, true, raw::YcbcrMatrix::Bt601, {{1059268006u, 1048297471u, 1049337854u}}, {159}, 119, 164},
    {525,
     false,
     raw::YcbcrMatrix::Bt2020,
     {{999499448u, 1038410981u, 1040728063u},
      {983429119u, 1030799360u, 1057180641u},
      {1048297470u, 959057920u, 1040887809u},
      {1051467774u, 1057128610u, 1062299598u}},
     {317, 266, 209, 694},
     638,
     484},
    {525,
     false,
     raw::YcbcrMatrix::Bt2020,
     {{1051734016u, 1063543909u, 1026113563u},
      {1050820701u, 1060977984u, 1055496193u},
      {3166704674u, 1064262614u, 1044364047u},
      {1041186814u, 1040351878u, 1002488002u}},
     {784, 758, 665, 395},
     368,
     353},
    {525,
     true,
     raw::YcbcrMatrix::Bt601,
     {{1014468608u, 1051093400u, 1063163816u},
      {1030344703u, 1058384457u, 3129978432u},
      {1056980793u, 3197737370u, 1028501483u},
      {1058182146u, 1045745663u, 1056423398u}},
     {513, 555, 254, 620},
     514,
     514},
    {525,
     true,
     raw::YcbcrMatrix::Bt601,
     {{1039875795u, 1041249518u, 1039463671u},
      {1059260608u, 1062939647u, 959754239u},
      {1059442689u, 1037340673u, 1018212526u},
      {1062847452u, 1054760959u, 1062606211u}},
     {409, 812, 484, 807},
     369,
     604},
    {526,
     false,
     raw::YcbcrMatrix::Bt601,
     {{1057374607u, 1057820673u, 1054727645u}, {1062621434u, 994529279u, 1036968276u}},
     {724, 361},
     499,
     698},
    {526,
     false,
     raw::YcbcrMatrix::Bt601,
     {{1014468608u, 1051093400u, 1063163816u}, {1030344703u, 1058384457u, 3129978432u}},
     {503, 539},
     486,
     304},
    {526,
     true,
     raw::YcbcrMatrix::Bt709,
     {{1051734016u, 1063543909u, 1026113563u}, {1050820701u, 1060977984u, 1055496193u}},
     {848, 823},
     318,
     378},
    {526,
     true,
     raw::YcbcrMatrix::Bt709,
     {{1059519786u, 1050371803u, 960614401u}, {1061159937u, 1060314659u, 1057898391u}},
     {610, 873},
     326,
     596},
    {527, false, raw::YcbcrMatrix::Bt601, {{1014583295u, 0u, 1065353216u}}, {198}, 940, 497},
    {527, false, raw::YcbcrMatrix::Bt601, {{1023225856u, 3197737370u, 1041258228u}}, {156}, 667, 568},
    {527, true, raw::YcbcrMatrix::Bt2020, {{1048666111u, 1029232859u, 1021415425u}}, {333}, 433, 661},
    {527, true, raw::YcbcrMatrix::Bt2020, {{1052452863u, 0u, 1036831946u}}, {193}, 600, 825},
};

// A clear's colour (float bits) and its codes
struct Clear {
    uint32_t format;
    bool full;
    raw::YcbcrMatrix matrix;
    uint32_t rgb[3];
    uint16_t luma, cb, cr;
};

constexpr Clear kClears[] = {
    {505, false, raw::YcbcrMatrix::Bt601, {1035123723u, 1065063428u, 1055707842u}, 639, 415, 149},
    {507, true, raw::YcbcrMatrix::Bt709, {1038784207u, 1064605867u, 1058761295u}, 769, 432, 98},
    {520, true, raw::YcbcrMatrix::Bt709, {1023806728u, 1065665882u, 1043439122u}, 202, 82, 32},
    {520, true, raw::YcbcrMatrix::Bt709, {1065627752u, 1025026890u, 1056073890u}, 106, 169, 223},
    {523, false, raw::YcbcrMatrix::Bt601, {1040004368u, 1064453347u, 1059569652u}, 188, 134, 65},
    {525, true, raw::YcbcrMatrix::Bt601, {1055927471u, 1058657582u, 1056500947u}, 783, 490, 475},
    {527, false, raw::YcbcrMatrix::Bt2020, {1036343884u, 1064910908u, 1057144582u}, 769, 482, 232},
};

TEST(RawYcbcrConvertedStore, TheEncodeTableIsTheInverseOfTheReadsDecode)
{
    uint16_t table[8193];
    raw::ycbcrSrgbEncodeTable(kKnots, table);
    EXPECT_EQ(memcmp(table, kEncode, sizeof table), 0);
    // the decode's flat steps take the middle of their run; a step it jumps the nearer neighbour (6728: 6727 then 6730)
    EXPECT_EQ(kEncode[2], 30);
    EXPECT_EQ(kEncode[4], 50);
    EXPECT_EQ(raw::ycbcrSrgbDecodeStep(kKnots, 7511), 6727u);
    EXPECT_EQ(raw::ycbcrSrgbDecodeStep(kKnots, 7512), 6730u);
    EXPECT_EQ(kEncode[6728], 7511);
    EXPECT_EQ(kEncode[8192], 8192);
}

TEST(RawYcbcrConvertedStore, DrawnBlocksAreMetalsCodes)
{
    for (const Block &b : kDrawn) {
        const raw::YcbcrConvertedTarget t = targetOf(b.format, b.full, b.matrix);
        const uint32_t w = t.across ? 2 : 1, h = t.down ? 2 : 1;
        std::vector<float> v(4 * w * h, 1.0f);
        for (uint32_t i = 0; i < w * h; i++)
            for (int c = 0; c < 3; c++)
                v[4 * i + c] = asFloat(b.rgb[i][c]);
        uint16_t luma[4], chroma[2], drawnLuma[4], drawnChroma[2];
        ASSERT_TRUE(
            raw::ycbcrStoreConverted(t, kEncode, v.data(), w, h, raw::YcbcrTexelSource::DrawnFloat, luma, chroma));
        ASSERT_TRUE(raw::ycbcrStoreDrawn(raw::ycbcrConvertedShape(t), kEncode, v.data(), w, h, drawnLuma, drawnChroma));
        for (uint32_t i = 0; i < w * h; i++) {
            EXPECT_EQ(luma[i], b.luma[i]) << b.format << " texel " << i;
            EXPECT_EQ(drawnLuma[i], b.luma[i]) << b.format << " texel " << i;
        }
        EXPECT_EQ(chroma[0], b.cb) << b.format;
        EXPECT_EQ(chroma[1], b.cr) << b.format;
        EXPECT_EQ(drawnChroma[0], b.cb) << b.format;
        EXPECT_EQ(drawnChroma[1], b.cr) << b.format;
    }
}

TEST(RawYcbcrConvertedStore, AClearsColourIsHeldToTheNearestHalf)
{
    for (const Clear &k : kClears) {
        raw::YcbcrConvertedTarget t = targetOf(k.format, k.full, k.matrix);
        t.across = t.down = false;
        const float colour[4] = {asFloat(k.rgb[0]), asFloat(k.rgb[1]), asFloat(k.rgb[2]), 1.0f};
        float held[4];
        for (int c = 0; c < 4; c++)  // as RawMetalEncode.mm puts it into the stand-in
            held[c] = float(_Float16(colour[c]));
        uint16_t luma, chroma[2], drawnLuma, drawnChroma[2];
        ASSERT_TRUE(raw::ycbcrStoreConverted(t, kEncode, colour, 1, 1, raw::YcbcrTexelSource::Clear, &luma, chroma));
        ASSERT_TRUE(raw::ycbcrStoreDrawn(raw::ycbcrConvertedShape(t), kEncode, held, 1, 1, &drawnLuma, drawnChroma));
        EXPECT_EQ(luma, k.luma) << k.format;
        EXPECT_EQ(chroma[0], k.cb) << k.format;
        EXPECT_EQ(chroma[1], k.cr) << k.format;
        EXPECT_EQ(drawnLuma, k.luma) << k.format;
        EXPECT_EQ(drawnChroma[0], k.cb) << k.format;
        EXPECT_EQ(drawnChroma[1], k.cr) << k.format;
    }
}

// A pass's floats: uniform ones beside [0, 1], the ties of every grid, and specials
std::vector<float> passTexels(uint32_t w, uint32_t h, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<float> rgba(4 * w * h);
    const float specials[] = {
        std::numeric_limits<float>::quiet_NaN(), INFINITY, -INFINITY, -0.0f, 0.0f, 1.0f, 0.5f, 1.5f, -0.25f};
    const uint32_t grids[] = {255, 1023, 2048, 8192};
    for (float &x : rgba) {
        const uint32_t pick = rng() % 8;
        if (pick == 0)
            x = specials[rng() % 9];
        else if (pick <= 4)
            x = (float(rng() % grids[pick - 1]) + 0.5f) / float(grids[pick - 1]);
        else
            x = std::uniform_real_distribution<float>(-0.05f, 1.05f)(rng);
    }
    return rgba;
}

TEST(RawYcbcrConvertedStore, TheStoreStatesAreTheRule)
{
    constexpr uint32_t w = 64, h = 32;
    const raw::YcbcrMatrix matrices[] = {raw::YcbcrMatrix::Bt601, raw::YcbcrMatrix::Bt709, raw::YcbcrMatrix::Bt2020};
    for (uint32_t format : {500u, 502u, 503u, 505u, 506u, 507u, 520u, 522u, 523u, 525u, 526u, 527u})
        for (bool full : {false, true})
            for (raw::YcbcrMatrix m : matrices) {
                const raw::YcbcrConvertedTarget t = targetOf(format, full, m);
                std::vector<float> rgba = passTexels(w, h, format * 8 + full * 4 + uint32_t(m));
                for (float &x :
                     rgba)  // the stand-in of a half draw or a clear holds its halves; of a float draw, floats
                    if (format % 2)
                        x = float(_Float16(x));
                const raw::YcbcrTexelSource source =
                    format % 2 ? raw::YcbcrTexelSource::DrawnHalf : raw::YcbcrTexelSource::DrawnFloat;
                std::vector<uint16_t> luma(w * h), chroma(w * h * 2), wantLuma(w * h), wantChroma(w * h * 2);
                ASSERT_TRUE(raw::ycbcrStoreDrawn(raw::ycbcrConvertedShape(t), kEncode, rgba.data(), w, h, luma.data(),
                                                 chroma.data()));
                ASSERT_TRUE(raw::ycbcrStoreConverted(t, kEncode, rgba.data(), w, h, source, wantLuma.data(),
                                                     wantChroma.data()));
                EXPECT_EQ(luma, wantLuma) << format << " " << full << " " << int(m);
                EXPECT_EQ(chroma, wantChroma) << format << " " << full << " " << int(m);
            }
}

TEST(RawYcbcrConvertedStore, APackedRowIsThreeCodesAWordFromBitZero)
{
    const uint16_t codes[] = {1023, 1, 512, 0x3ff, 0x155, 7, 1022};
    uint32_t words[3];
    raw::ycbcrPackRow(codes, 7, words);
    EXPECT_EQ(words[0], 1023u | 1u << 10 | 512u << 20);
    EXPECT_EQ(words[1], 0x3ffu | 0x155u << 10 | 7u << 20);
    EXPECT_EQ(words[2], 1022u);  // the row's tail: its slots past the last code 0

    raw::YcbcrStoreShape twin = raw::ycbcrConvertedShape(targetOf(525, true, raw::YcbcrMatrix::Bt709));
    raw::YcbcrStoreShape packed = raw::ycbcrPackedShape(twin);
    EXPECT_EQ(packed.packed, 1u);
    EXPECT_EQ(packed.shift, 0u);
    packed.packed = 0;
    packed.shift = twin.shift;
    EXPECT_EQ(memcmp(&packed, &twin, sizeof twin), 0);
}

// The rule's words of a packed target: its twin's codes, three to a word a row
std::vector<uint32_t> packedRows(const std::vector<uint16_t> &codes, uint32_t perRow, uint32_t rows)
{
    std::vector<uint32_t> words((perRow + 2) / 3 * rows);
    for (uint32_t y = 0; y < rows; y++)
        raw::ycbcrPackRow(&codes[y * perRow], perRow, &words[y * ((perRow + 2) / 3)]);
    return words;
}

TEST(RawYcbcrConvertedStore, ThePackedStatesStoreTheirTwinsCodesThreeAWord)
{
    const struct {
        uint32_t format;
        bool across, down;
    } twins[] = {{505, true, true},   {506, true, false}, {507, false, false}, {525, true, true},  {526, true, false},
                 {527, false, false}, {543, true, true},  {544, true, false},  {545, false, false}};

    for (const auto &twin : twins)
        for (uint32_t w : {34u, 38u}) {  // rows ending in a partial word, luma and chroma
            constexpr uint32_t h = 6;
            const raw::YcbcrStoreShape shape =
                twin.format >= 540 ? raw::ycbcrCodesShape(10, twin.across, twin.down)
                                   : raw::ycbcrConvertedShape(targetOf(twin.format, true, raw::YcbcrMatrix::Bt709));
            const uint32_t blocks = twin.across ? w / 2 : w, rows = twin.down ? h / 2 : h;
            const std::vector<float> rgba = passTexels(w, h, twin.format + w);
            std::vector<uint16_t> luma(w * h), chroma(2 * blocks * rows);
            ASSERT_TRUE(raw::ycbcrStoreDrawn(shape, kEncode, rgba.data(), w, h, luma.data(), chroma.data()));
            std::vector<uint32_t> lumaWords((w + 2) / 3 * h), chromaWords((2 * blocks + 2) / 3 * rows);
            ASSERT_TRUE(raw::ycbcrStoreDrawnPacked(raw::ycbcrPackedShape(shape), kEncode, rgba.data(), w, h,
                                                   lumaWords.data(), chromaWords.data()));
            EXPECT_EQ(lumaWords, packedRows(luma, w, h)) << twin.format << " " << w;
            EXPECT_EQ(chromaWords, packedRows(chroma, 2 * blocks, rows)) << twin.format << " " << w;
        }
}

TEST(RawYcbcrConvertedStore, ThirtyTwoBitStepsScaleAsTheSixtyFourBitProduct)
{
    std::mt19937 rng(20);
    const auto want = [](int32_t sum, uint32_t mul) {
        return int32_t(std::floor(double(int64_t(sum) * mul) / double(1 << 20)));
    };
    for (uint32_t mul : {255u, 256u, 512u, 1023u, 1024u, 2048u}) {
        for (int32_t sum : {0, 1, -1, 4095, 4096, -4096, -4097, (1 << 20) - 1, -(1 << 20), 1 << 28, -(1 << 28)})
            EXPECT_EQ(raw::ycbcrScaled(sum, mul), want(sum, mul)) << sum << " " << mul;
        for (int i = 0; i < 200000; i++) {
            const int32_t sum = int32_t(rng() % (1u << 29)) - (1 << 28);
            ASSERT_EQ(raw::ycbcrScaled(sum, mul), want(sum, mul)) << sum << " " << mul;
        }
    }
}

}  // namespace
