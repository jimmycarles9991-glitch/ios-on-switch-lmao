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

TEST(RawYcbcr, EveryFormatOverEveryMeasuredSurfaceWhoseChromaRowsItHolds)
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
                    // the format's chroma rows no more than the surface's (a finer one's rows read past the plane's end)
                    const bool expected = bits == s.bits && (sub == 0 || s.sub != 0);
                    EXPECT_EQ(raw::planYcbcr(f, p) == nullptr, expected) << s.code << " " << format;
                    taken += expected;
                    if (!expected)
                        continue;
                    EXPECT_EQ(p.addressMap, (sub == 2 ? 0 : 1) | (reads == 2 ? 2 : 0)) << format;
                    EXPECT_EQ(p.image, reads == 0 ? 115u : reads == 1 ? 125u : bits == 8 ? 70u : 90u) << format;
                    EXPECT_EQ(p.chromaWidth, sub == 2 ? 33u : 17u) << format;
                    EXPECT_EQ(p.chromaHeight, sub == 0 ? 9u : 17u) << format;
                }
    EXPECT_EQ(taken, 3 * (17 + 14));  // per output: 8-bit 2 x 1 + 2 x 3 + 3 x 3 pairs, 10-bit 2 x 1 + 2 x 3 + 2 x 3
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
    EXPECT_STREQ(raw::planYcbcr(g, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
}

// Metal reads a plane's rows on past its end into the allocation's last page (raw/test/ycbcroracle.mm's past cases): plane 1
// of a 4:2:0 surface as luma, a format's chroma finer than the surface's, a texture taller than its surface; a render target
// of them stays refused
TEST(RawYcbcr, RowsPastAPlaneAreReadWithinTheAllocationsPages)
{
    raw::YcbcrPlan p;
    raw::YcbcrFacts f = surface(505, "x420", 33, 17, 128, 9);
    f.plane = 1;
    f.mappedBytes = 16384;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.luma, f.offset[1]);
    EXPECT_EQ(p.chromaWidth, 0u);
    EXPECT_EQ(p.readable, 16384u);
    EXPECT_FALSE(p.nativePlanes);

    f = surface(502, "420v", 33, 17, 64, 9);
    f.mappedBytes = 16384;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.chromaHeight, 17u) << "4:2:2 chroma rows, read past the 4:2:0 plane";
    EXPECT_EQ(p.chroma, f.offset[1]);
    EXPECT_FALSE(p.nativePlanes);

    f = surface(500, "420f", 16, 8, 64, 4);
    f.height = 40;
    f.mappedBytes = 16384;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.chromaHeight, 20u);
    f.mappedBytes = 0;
    EXPECT_STREQ(raw::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.height = 300;
    f.mappedBytes = 16384;
    EXPECT_STREQ(raw::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");

    f = surface(500, "420f", 16, 8, 64, 4);
    f.height = 12;
    f.mappedBytes = 16384;
    f.target = true;
    EXPECT_NE(raw::planYcbcr(f, p), nullptr);
    f.target = false;
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr);
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
    f.mappedBytes = 16384;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr) << "chroma rows of 88 bytes at an 80-byte pitch: run on into the next";
    EXPECT_EQ(p.chromaWidth, 22u);
    f.target = true;
    EXPECT_STREQ(raw::planYcbcr(f, p), "a render target of more codes a row than its plane's pitch holds");
    f = surface(546, "x420", 33, 17, 128, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "pixel format");
    f = surface(543, "p420", 33, 17, 128, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "pixel format");
}

// cameracaptured's FigMetalContext (boot 21): RGB8_420_2P and YCBCR8_420_2P textures of 2x2 over an 8x8 '420f' surface of 64-byte
// rows, usage ShaderRead | ShaderWrite | RenderTarget. Metal reads and stores their top-left corner (an M4 Max: every channel
// of the conversion's reads, and no byte outside the corner stored).
TEST(RawYcbcr, ATextureSmallerThanItsSurfaceIsItsTopLeftCorner)
{
    for (const uint32_t format : {500u, 540u}) {
        raw::YcbcrFacts f = surface(format, "420f", 8, 8, 64, 4);
        f.width = f.height = 2;
        f.target = raw::ycbcrUsageRenders(7);
        raw::YcbcrPlan p;
        ASSERT_EQ(raw::planYcbcr(f, p), nullptr) << format;
        EXPECT_TRUE(p.target);
        EXPECT_EQ(p.luma, 0u);
        EXPECT_EQ(p.lumaPitch, 64u);
        EXPECT_EQ(p.lumaWidth, 2u);
        EXPECT_EQ(p.chroma, 64u * 8);
        EXPECT_EQ(p.chromaPitch, 64u);
        EXPECT_EQ(p.chromaWidth, 1u);
        EXPECT_EQ(p.chromaHeight, 1u);
        EXPECT_EQ(p.shape[0], 2u);
        EXPECT_EQ(p.shape[1], 2u);
        EXPECT_EQ(p.store.across, 1u);
        EXPECT_EQ(p.store.down, 1u);
    }
}

// Wider than its surface, its rows still in the plane's: Metal reads and stores them (an M4 Max, 32x8 and 24x4 of a 16x8
// surface of 64-byte rows)
TEST(RawYcbcr, ATextureWiderThanItsSurfaceReadsItsPlanesRows)
{
    raw::YcbcrFacts f = surface(540, "420v", 16, 8, 64, 4);
    f.width = 32;
    f.target = true;
    raw::YcbcrPlan p;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.lumaWidth, 32u);
    EXPECT_EQ(p.chromaWidth, 16u);
    EXPECT_EQ(p.chromaHeight, 4u);
    f.width = 66;
    EXPECT_STREQ(raw::planYcbcr(f, p), "a render target of more codes a row than its plane's pitch holds");
}

TEST(RawYcbcr, APackedTargetSmallerThanItsSurfaceIsRefusedButSampled)
{
    raw::YcbcrFacts f = surface(546, "p420", 8, 8, 64, 4);
    f.width = f.height = 2;
    raw::YcbcrPlan p;
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr);
    f.target = true;
    EXPECT_STREQ(raw::planYcbcr(f, p), "a packed render target smaller than its surface");
    f.width = f.height = 8;
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr);
}

// An M4 Max answers nil for each, over 420f, x420, p420, 422f and x422 surfaces (8- and 10-bit, packed, sRGB and codes)
TEST(RawYcbcr, MetalMakesNoWritableSubsampledTextureOfAnOddSize)
{
    for (const uint32_t format : {500u, 505u, 508u, 520u, 540u, 541u, 543u, 544u, 546u}) {
        for (const uint64_t usage : {0ull, 2ull, 3ull, 4ull, 5ull}) {
            EXPECT_TRUE(raw::ycbcrMetalRefuses(format, 3, 3, usage)) << format << " " << usage;
            EXPECT_TRUE(raw::ycbcrMetalRefuses(format, 2, 3, usage)) << format << " " << usage;
            EXPECT_TRUE(raw::ycbcrMetalRefuses(format, 3, 4, usage)) << format << " " << usage;
            EXPECT_FALSE(raw::ycbcrMetalRefuses(format, 4, 4, usage)) << format << " " << usage;
        }
        EXPECT_FALSE(raw::ycbcrMetalRefuses(format, 3, 3, 1)) << format;
    }
    EXPECT_FALSE(raw::ycbcrMetalRefuses(503, 3, 3, 5));
    EXPECT_FALSE(raw::ycbcrMetalRefuses(542, 3, 3, 5));
    EXPECT_FALSE(raw::ycbcrMetalRefuses(80, 3, 3, 5));
}

TEST(RawYcbcr, AnyUsageIsTakenAndUnknownRenders)
{
    EXPECT_TRUE(raw::ycbcrUsageRenders(0));
    EXPECT_TRUE(raw::ycbcrUsageRenders(4));
    EXPECT_TRUE(raw::ycbcrUsageRenders(7));
    EXPECT_FALSE(raw::ycbcrUsageRenders(1));
    EXPECT_FALSE(raw::ycbcrUsageRenders(3));
}

// Under the P3 matrices an M4 Max converts nothing (raw/test/ycbcroracle.mm): a converted or sRGB format reads its codes as
// red Y, green Cb, blue Cr and samples as its surface's codes format, whose image it draws; an unpacked 4:2:0 or 4:2:2
// converted target stores as that codes format (raw/test/ycbcrstoreoracle.mm), any other target is refused
TEST(RawYcbcr, AP3MatrixReadsTheCodesLumaFirst)
{
    struct Case {
        uint32_t format, codes;
        char code[5];
        raw::YcbcrOutput output;
        bool stored;
    };

    const Case cases[] = {
        {500, 540, "420v", raw::YcbcrLumaFirst8, true},        {502, 541, "422f", raw::YcbcrLumaFirst8, true},
        {505, 543, "x420", raw::YcbcrLumaFirst10, true},       {506, 544, "x422", raw::YcbcrLumaFirst10, true},
        {520, 540, "420f", raw::YcbcrLumaFirst8, false},       {503, 542, "444v", raw::YcbcrLumaFirst8, false},
        {527, 545, "x444", raw::YcbcrLumaFirst10, false},      {508, 546, "p420", raw::YcbcrLumaFirst10Packed, false},
        {529, 547, "p422", raw::YcbcrLumaFirst10Packed, false}};
    for (const char *matrix : {"P3_D65", "DCI_P3"})
        for (const Case &c : cases)
            for (const bool target : {false, true}) {
                const bool halvedDown = c.code[1] == '4' && c.code[2] == '2' && c.code[3] == '0';
                raw::YcbcrFacts f = surface(c.format, c.code, 32, 16, 128, halvedDown ? 8 : 16);
                f.matrix = matrix;
                f.target = target;
                raw::YcbcrPlan p, codes;
                if (target && !c.stored) {
                    EXPECT_STREQ(raw::planYcbcr(f, p), "a render target under a P3 matrix of an sRGB, packed or "
                                                       "4:4:4 format, whose stores no rule gives")
                        << c.format;
                    continue;
                }
                ASSERT_EQ(raw::planYcbcr(f, p), nullptr) << c.format << " " << matrix;
                f.pixelFormat = c.codes;
                ASSERT_EQ(raw::planYcbcr(f, codes), nullptr) << c.codes;
                EXPECT_EQ(p.output, c.output) << c.format;
                EXPECT_TRUE(raw::ycbcrLumaFirst(p.output));
                EXPECT_FALSE(raw::ycbcrLumaFirst(codes.output));
                EXPECT_EQ(p.image, codes.image) << c.format;
                EXPECT_EQ(p.addressMap, codes.addressMap) << c.format;
                EXPECT_EQ(memcmp(p.shape, codes.shape, sizeof p.shape), 0) << c.format;
                EXPECT_EQ(memcmp(&p.store, &codes.store, sizeof p.store), 0) << c.format;
            }
}

// One-plane 4:2:2 formats over a surface of no planes: an M4 Max reads its bytes as Y0 Cb Y1 Cr whatever the surface's
// order ('2vuy', 'yuvs' video range, 'yuvf' full), as the two-plane 4:2:2 format reads those codes split into two planes
TEST(RawYcbcr, AOnePlaneFormatIsSplitIntoTwoPlanesOfFourTwoTwo)
{
    const char codes[3][5] = {"2vuy", "yuvs", "yuvf"};
    for (const auto &code : codes)
        for (const uint32_t format : {501u, 521u}) {
            raw::YcbcrFacts f;
            f.pixelFormat = format;
            f.fourcc = fourcc(code);
            f.planes = 0;
            f.width = f.surfaceWidth = 33;
            f.height = f.surfaceHeight = 9;
            f.pitch[0] = 80;
            f.allocBytes = 80 * 9;
            f.simple = true;
            raw::YcbcrPlan p, two;
            ASSERT_EQ(raw::planYcbcr(f, p), nullptr) << code << " " << format;
            EXPECT_TRUE(p.onePlane);
            EXPECT_EQ(p.output, format == 501 ? raw::YcbcrConverted : raw::YcbcrSrgb);
            EXPECT_EQ(p.luma, 0u);
            EXPECT_EQ(p.lumaPitch, 80u);
            EXPECT_EQ(p.lumaFormat, 10u);
            EXPECT_EQ(p.lumaWidth, 34u) << "Y0 Y1 of 17 pairs";
            EXPECT_EQ(p.chromaFormat, 30u);
            EXPECT_EQ(p.chromaWidth, 17u);
            EXPECT_EQ(p.chromaHeight, 9u);
            EXPECT_FALSE(p.nativePlanes);
            raw::YcbcrFacts g = f;
            g.pixelFormat = format + 1;
            g.fourcc = fourcc(code[3] == 'f' ? "422f" : "422v");
            g.planes = 2;
            g.offset[1] = 80 * 9;
            g.pitch[1] = 80;
            g.allocBytes = 80 * 18;
            ASSERT_EQ(raw::planYcbcr(g, two), nullptr);
            EXPECT_EQ(memcmp(p.shape, two.shape, sizeof p.shape), 0) << code << " " << format;
            EXPECT_EQ(p.addressMap, two.addressMap);
            f.target = true;
            EXPECT_STREQ(raw::planYcbcr(f, p), "a one-plane YCbCr render target, whose stores no rule gives");
            f.target = false;
            f.planes = 2;
            EXPECT_STREQ(raw::planYcbcr(f, p), "a one-plane format over a plane of a planar surface");
        }
    raw::YcbcrFacts f;
    f.pixelFormat = 501;
    f.fourcc = fourcc("420v");
    f.planes = 0;
    raw::YcbcrPlan p;
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
    EXPECT_STREQ(raw::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.matrix = "SMPTE_240M_1995";
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.shape[4], 14743u);
    f.matrix = "EBU_3213";
    EXPECT_STREQ(raw::planYcbcr(f, p), "matrix");

    f = surface(505, "x420", 33, 17, 120, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "plane offset or row not 16-byte");

    f = surface(505, "x420", 33, 17, 64, 9);
    EXPECT_STREQ(raw::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.mappedBytes = 16384;
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr) << "rows of 66 bytes at a 64-byte pitch: run on into the next";
    EXPECT_FALSE(p.nativePlanes);

    f = surface(505, "x420", 33, 17, 128, 9);
    f.allocBytes = f.offset[1] + 128 * 8 + 2 * 17 * 2 - 1;
    EXPECT_STREQ(raw::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.height = 18;
    f.allocBytes = f.offset[1] + 128 * 9;
    EXPECT_EQ(raw::planYcbcr(f, p), nullptr) << "luma rows read on into plane 1";
    f.height = 20;
    EXPECT_STREQ(raw::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.width = f.height = 0;
    EXPECT_STREQ(raw::planYcbcr(f, p), "an empty texture");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.simple = false;
    EXPECT_STREQ(raw::planYcbcr(f, p), "shape");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.planes = 3;
    EXPECT_STREQ(raw::planYcbcr(f, p), "not two planes");

    EXPECT_FALSE(raw::isYcbcrFormat(80));
    EXPECT_TRUE(raw::isYcbcrFormat(545));
}

}  // namespace
