// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr.cpp: which two-plane YCbCr textures the raw path converts, by m2v's rules, and the conversion's shape
// (packed planes: three 10-bit codes a 32-bit word, m2v bind/ycbcr_convert.rs).
// The conversion's texels against Metal's own reads: raw/test/ycbcroracle.mm (this Mac's GPU).
#include "../raw/runtime/raw_ycbcr.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

constexpr uint32_t fourcc(const char (&c)[5])
{
    return uint32_t(uint8_t(c[0])) << 24 | uint32_t(uint8_t(c[1])) << 16 | uint32_t(uint8_t(c[2])) << 8 | uint8_t(c[3]);
}

// A CoreVideo-like surface: rows `pitch` bytes apart, plane 1 after plane 0's rows
raw::YcbcrFacts surface(uint32_t format, const char (&code)[5], uint32_t w, uint32_t h, uint32_t pitch,
                        uint32_t chromaRows)
{
    raw::YcbcrFacts f;
    f.pixelFormat = format;
    f.fourcc = fourcc(code);
    f.planes = 2;
    f.width = f.surfaceWidth = w;
    f.height = f.surfaceHeight = h;
    f.offset[1] = uint64_t(pitch) * h;
    f.pitch[0] = f.pitch[1] = pitch;
    f.allocBytes = f.offset[1] + uint64_t(pitch) * chromaRows;
    f.simple = true;
    return f;
}

TEST(RawYcbcr, TipsTenBitVideoIsConvertedFromItsSixteenBitPlanes)
{
    raw::YcbcrFacts f = surface(505, "x420", 2500, 1500, 5312, 750);
    f.matrix = "ITU_R_709_2";
    raw::YcbcrPlan p;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, raw::YcbcrConverted);
    EXPECT_EQ(p.image, 115u);
    EXPECT_EQ(p.lumaFormat, 20u);
    EXPECT_EQ(p.lumaWidth, 2500u);
    EXPECT_EQ(p.chromaFormat, 60u);
    EXPECT_EQ(p.chromaWidth, 1250u);
    EXPECT_EQ(p.chromaHeight, 750u);
    EXPECT_EQ(p.chroma, 5312u * 1500);
    EXPECT_EQ(p.addressMap, 1);
    const uint32_t shape[raw::kYcbcrShapeWords] = {
        2500, 1500, 0, uint32_t(-1), 14743, uint32_t(-1753), uint32_t(-4383), 17373, 1, 65535, 6, 0, 1023, 3, 1};
    EXPECT_EQ(memcmp(p.shape, shape, sizeof shape), 0);
}

TEST(RawYcbcr, EightBitShapeKeepsTheFourTwoZeroArithmetic)
{
    raw::YcbcrFacts f = surface(500, "420f", 64, 48, 64, 24);
    raw::YcbcrPlan p;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    const uint32_t shape[raw::kYcbcrShapeWords] = {
        64, 48, 1, uint32_t(-8), 11483, uint32_t(-2816), uint32_t(-5850), 14518, 8, 255, 0, 2, 255, 3, 1};
    EXPECT_EQ(memcmp(p.shape, shape, sizeof shape), 0);
    EXPECT_EQ(p.lumaFormat, 10u);
    EXPECT_EQ(p.chromaFormat, 30u);
}

TEST(RawYcbcr, EveryFormatOverEveryMeasuredSurfaceWhoseChromaItFits)
{
    struct Surface {
        const char code[5];
        uint32_t bits, sub;
    } const surfaces[] = {{"420v", 8, 0},  {"420f", 8, 0},  {"422v", 8, 1},  {"422f", 8, 1},  {"444v", 8, 2},
                          {"444f", 8, 2},  {"_RB4", 8, 2},  {"x420", 10, 0}, {"xf20", 10, 0}, {"x422", 10, 1},
                          {"xf22", 10, 1}, {"x444", 10, 2}, {"xf44", 10, 2}};

    const uint32_t formats[3][2][3] = {
        {{500, 502, 503}, {505, 506, 507}}, {{520, 522, 523}, {525, 526, 527}}, {{540, 541, 542}, {543, 544, 545}}};
    int taken = 0;
    for (const Surface &s : surfaces)
        for (int reads = 0; reads < 3; reads++)
            for (uint32_t bits : {8u, 10u})
                for (uint32_t sub = 0; sub < 3; sub++) {
                    const uint32_t format = formats[reads][bits == 10][sub];
                    const uint32_t rows = s.sub == 0 ? 9 : 17;
                    raw::YcbcrFacts f = surface(format, s.code, 33, 17, 256, rows);
                    raw::YcbcrPlan p;
                    // the format's chroma no finer than the surface's
                    const bool expected = bits == s.bits && sub <= s.sub;
                    EXPECT_EQ(raw::planYcbcr(f, p) == nullptr, expected) << s.code << " " << format;
                    taken += expected;
                    if (!expected)
                        continue;
                    EXPECT_EQ(p.addressMap, (sub == 2 ? 0 : 1) | (reads == 2 ? 2 : 0)) << format;
                    EXPECT_EQ(p.image, reads == 0 ? 115u : reads == 1 ? 125u : bits == 8 ? 70u : 90u) << format;
                    EXPECT_EQ(p.chromaWidth, sub == 2 ? 33u : 17u) << format;
                    EXPECT_EQ(p.chromaHeight, sub == 0 ? 9u : 17u) << format;
                }
    EXPECT_EQ(taken, 3 * (15 + 12));  // per output: 8-bit 2 x 1 + 2 x 2 + 3 x 3 pairs, 10-bit 2 x 1 + 2 x 2 + 2 x 3
}

TEST(RawYcbcr, PlaneOneOfFullHeightChromaReadsItsCodesAsLuma)
{
    raw::YcbcrFacts f = surface(506, "x422", 33, 17, 128, 17);
    f.plane = 1;
    raw::YcbcrPlan p;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.luma, f.offset[1]);
    EXPECT_EQ(p.chromaWidth, 0u);
    EXPECT_EQ(p.shape[14], 0u);

    raw::YcbcrFacts g = surface(505, "x420", 33, 17, 128, 9);
    g.plane = 1;
    EXPECT_STREQ(raw::planYcbcr(g, p), "plane 1 of a 4:2:0 surface (Metal reads its rows past the plane)");
}

TEST(RawYcbcr, PackedPlanesAreWordsOfThreeCodes)
{
    raw::YcbcrPlan p;
    raw::YcbcrFacts f = surface(508, "p420", 33, 17, 64, 9);
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, raw::YcbcrConvertedPacked);
    EXPECT_EQ(p.image, 115u);
    EXPECT_EQ(p.lumaFormat, 53u);
    EXPECT_EQ(p.chromaFormat, 53u);
    EXPECT_EQ(p.lumaWidth, 11u);    // 33 codes
    EXPECT_EQ(p.chromaWidth, 12u);  // 17 pairs: 34 codes
    EXPECT_EQ(p.chromaHeight, 9u);
    EXPECT_EQ(p.shape[11], 0u);
    EXPECT_EQ(p.shape[12], 1023u);

    f = surface(529, "pf22", 33, 17, 64, 17);
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, raw::YcbcrSrgbPacked);
    f = surface(548, "p444", 33, 17, 96, 17);
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, raw::YcbcrCodes10Packed);
    EXPECT_EQ(p.image, 90u);
    EXPECT_EQ(p.chromaWidth, 22u);  // 33 pairs: 66 codes

    f = surface(548, "p444", 33, 17, 80, 17);
    EXPECT_STREQ(raw::planYcbcr(f, p), "row shorter than its codes");
    f = surface(546, "x420", 33, 17, 128, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "pixel format");
    f = surface(543, "p420", 33, 17, 128, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "pixel format");
}

TEST(RawYcbcr, RefusesWhatM2vRefuses)
{
    raw::YcbcrPlan p;
    raw::YcbcrFacts f = surface(505, "420v", 33, 17, 128, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "pixel format");

    f = surface(500, "422v", 33, 17, 64, 17);
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr);
    f = surface(502, "420v", 33, 17, 64, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "chroma finer in the format than in the surface");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.matrix = "SMPTE_240M_1995";
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.shape[4], 14743u);
    f.matrix = "P3_D65";
    EXPECT_STREQ(raw::planYcbcr(f, p), "matrix");

    f = surface(505, "x420", 33, 17, 120, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "plane offset or row not 16-byte");

    f = surface(505, "x420", 33, 17, 64, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "row shorter than its codes");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.allocBytes = f.offset[1] + 128 * 8 + 2 * 17 * 2 - 1;
    EXPECT_STREQ(raw::planYcbcr(f, p), "planes past the allocation");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.width = 32;
    EXPECT_STREQ(raw::planYcbcr(f, p), "texture size not the surface's");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.simple = false;
    EXPECT_STREQ(raw::planYcbcr(f, p), "shape or usage");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.planes = 3;
    EXPECT_STREQ(raw::planYcbcr(f, p), "not two planes");

    EXPECT_FALSE(raw::isYcbcrFormat(80));
    EXPECT_TRUE(raw::isYcbcrFormat(545));
}

}  // namespace
