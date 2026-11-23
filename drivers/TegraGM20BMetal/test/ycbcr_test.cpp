// SPDX-License-Identifier: MIT
// ycbcr.cpp: which two-plane YCbCr textures the driver converts, by m2v's rules, and the conversion's shape
// (packed planes: three 10-bit codes a 32-bit word, m2v bind/ycbcr_convert.rs).
// The conversion's texels against Metal's own reads: harness/ycbcroracle.mm (this Mac's GPU).
#include "../ycbcr.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t fourcc(const char (&c)[5])
{
    return uint32_t(uint8_t(c[0])) << 24 | uint32_t(uint8_t(c[1])) << 16 | uint32_t(uint8_t(c[2])) << 8 | uint8_t(c[3]);
}

// A CoreVideo-like surface: rows `pitch` bytes apart, plane 1 after plane 0's rows
gm20b::YcbcrFacts surface(uint32_t format, const char (&code)[5], uint32_t w, uint32_t h, uint32_t pitch,
                          uint32_t chromaRows)
{
    gm20b::YcbcrFacts f;
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

TEST(GM20BYcbcr, TipsTenBitVideoIsConvertedFromItsSixteenBitPlanes)
{
    gm20b::YcbcrFacts f = surface(505, "x420", 2500, 1500, 5312, 750);
    f.matrix = "ITU_R_709_2";
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, gm20b::YcbcrConverted);
    EXPECT_EQ(p.image, 115u);
    EXPECT_EQ(p.lumaFormat, 20u);
    EXPECT_EQ(p.lumaWidth, 2500u);
    EXPECT_EQ(p.chromaFormat, 60u);
    EXPECT_EQ(p.chromaWidth, 1250u);
    EXPECT_EQ(p.chromaHeight, 750u);
    EXPECT_EQ(p.chroma, 5312u * 1500);
    EXPECT_EQ(p.addressMap, 1);
    const uint32_t shape[gm20b::kYcbcrShapeWords] = {
        2500, 1500, 0, uint32_t(-1), 14743, uint32_t(-1753), uint32_t(-4383), 17373, 1, 65535, 6, 0, 1023, 3, 1};
    EXPECT_EQ(memcmp(p.shape, shape, sizeof shape), 0);
}

TEST(GM20BYcbcr, EightBitShapeKeepsTheFourTwoZeroArithmetic)
{
    gm20b::YcbcrFacts f = surface(500, "420f", 64, 48, 64, 24);
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    const uint32_t shape[gm20b::kYcbcrShapeWords] = {
        64, 48, 1, uint32_t(-8), 11483, uint32_t(-2816), uint32_t(-5850), 14518, 8, 255, 0, 2, 255, 3, 1};
    EXPECT_EQ(memcmp(p.shape, shape, sizeof shape), 0);
    EXPECT_EQ(p.lumaFormat, 10u);
    EXPECT_EQ(p.chromaFormat, 30u);
}

TEST(GM20BYcbcr, EveryFormatOverEveryMeasuredSurfaceWhoseChromaRowsItHolds)
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
                    gm20b::YcbcrFacts f = surface(format, s.code, 33, 17, 256, rows);
                    gm20b::YcbcrPlan p;
                    // the format's chroma rows no more than the surface's (a finer one's rows read past the plane's end), of
                    // its own bits whatever the surface's (m2v-textures9 probes/ycbcr.txt 3f, 3g)
                    const bool expected = sub == 0 || s.sub != 0;
                    EXPECT_EQ(gm20b::planYcbcr(f, p) == nullptr, expected) << s.code << " " << format;
                    taken += expected;
                    if (!expected)
                        continue;
                    EXPECT_EQ(p.addressMap, (sub == 2 ? 0 : 1) | (reads == 2 ? 2 : 0)) << format;
                    EXPECT_EQ(p.image, reads == 0 ? 115u : reads == 1 ? 125u : bits == 8 ? 70u : 90u) << format;
                    EXPECT_EQ(p.chromaWidth, sub == 2 ? 33u : 17u) << format;
                    EXPECT_EQ(p.chromaHeight, sub == 0 ? 9u : 17u) << format;
                }
    EXPECT_EQ(taken,
              3 * 2 * (4 * 1 + 9 * 3));  // per output and depth: 4:2:0 surfaces take 4:2:0 alone, the rest all three
}

TEST(GM20BYcbcr, PlaneOneOfFullHeightChromaReadsItsCodesAsLuma)
{
    gm20b::YcbcrFacts f = surface(506, "x422", 33, 17, 128, 17);
    f.plane = 1;
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.luma, f.offset[1]);
    EXPECT_EQ(p.chromaWidth, 0u);
    EXPECT_EQ(p.shape[14], 0u);

    gm20b::YcbcrFacts g = surface(505, "x420", 33, 17, 128, 9);
    g.plane = 1;
    EXPECT_STREQ(gm20b::planYcbcr(g, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
}

// Metal reads a plane's rows on past its end into the allocation's last page (harness/ycbcroracle.mm's past cases): plane 1
// of a 4:2:0 surface as luma, a format's chroma finer than the surface's, a texture taller than its surface; a render target
// of them stays refused
TEST(GM20BYcbcr, RowsPastAPlaneAreReadWithinTheAllocationsPages)
{
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(505, "x420", 33, 17, 128, 9);
    f.plane = 1;
    f.mappedBytes = 16384;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.luma, f.offset[1]);
    EXPECT_EQ(p.chromaWidth, 0u);
    EXPECT_EQ(p.readable, 16384u);
    EXPECT_FALSE(p.nativePlanes);

    f = surface(502, "420v", 33, 17, 64, 9);
    f.mappedBytes = 16384;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.chromaHeight, 17u) << "4:2:2 chroma rows, read past the 4:2:0 plane";
    EXPECT_EQ(p.chroma, f.offset[1]);
    EXPECT_FALSE(p.nativePlanes);

    f = surface(500, "420f", 16, 8, 64, 4);
    f.height = 40;
    f.mappedBytes = 16384;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.chromaHeight, 20u);
    f.mappedBytes = 0;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.height = 300;
    f.mappedBytes = 16384;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");

    f = surface(500, "420f", 16, 8, 64, 4);
    f.height = 12;
    f.mappedBytes = 16384;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr)
        << "stored as read: rows on past each plane (m2v-textures5 probes/targets.mm)";
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.chromaHeight, 6u);
    f.target = false;
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr);
}

// Rows past the allocation's last page, which an M4 Max reads as 0 (m2v-textures4 probes/more.mm), go on into a window whose
// tail the kernel maps sparse (sparse_window.cpp): planned to the rows' end where the kernel maps VA sparse, refused where
// it does not
TEST(GM20BYcbcr, RowsPastTheLastPageGoOnIntoASparseWindow)
{
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(500, "420v", 64, 8, 64, 4);
    f.height = 512;
    f.mappedBytes = 16384;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.sparseTail = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_TRUE(p.window);
    EXPECT_EQ(p.readable, std::max(f.offset[0] + 64ull * 511 + 64, f.offset[1] + 64ull * 255 + 64)) << "the rows' end";
    EXPECT_FALSE(p.nativePlanes);
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "a store's rows past the last page drop";
    EXPECT_TRUE(p.window && p.target);
    f = surface(500, "420v", 64, 8, 64, 4);
    f.mappedBytes = 16384;
    f.sparseTail = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_FALSE(p.window) << "rows inside the pages need none";
    EXPECT_EQ(p.readable, 16384u);
}

// An M4 Max renders into each of these as it reads it (m2v-textures5 probes/targets.mm, Clear+Store over 420v and 422v): over
// plane 1 its luma codes alone into that plane at its pitch; taller than its surface, or of chroma finer or coarser than the
// surface's, each plane's rows at its pitch on past the plane's end, chroma over luma where they meet
TEST(GM20BYcbcr, TargetsStoreAsTheyRead)
{
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(540, "420v", 64, 64, 64, 32);
    f.plane = 1;
    f.width = f.height = 32;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "over plane 1";
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.luma, f.offset[1]);
    EXPECT_EQ(p.lumaPitch, 64u);
    EXPECT_EQ(p.chromaWidth, 0u) << "no chroma stored";

    f = surface(500, "420v", 64, 64, 64, 32);
    f.height = 128;
    f.mappedBytes = 16384;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "taller";
    EXPECT_EQ(p.chroma, f.offset[1]);
    EXPECT_EQ(p.chromaHeight, 64u);

    f = surface(502, "420v", 64, 64, 64, 32);
    f.mappedBytes = 16384;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "4:2:2 over 4:2:0";
    EXPECT_EQ(p.chromaHeight, 64u);
    EXPECT_EQ(p.store.down, 0u);

    f = surface(500, "422v", 64, 64, 64, 64);
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "4:2:0 over 4:2:2";
    EXPECT_EQ(p.chromaHeight, 32u);
    EXPECT_EQ(p.store.down, 1u);

    f = surface(540, "420v", 64, 64, 64, 32);
    f.width = 33;
    f.height = 17;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "of an odd size, made to be read alone: its edge blocks partial";
    EXPECT_EQ(p.chromaWidth, 17u);
    EXPECT_EQ(p.chromaHeight, 9u);

    f = surface(503, "420v", 64, 64, 64, 32);
    f.mappedBytes = 16384;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "4:4:4 chroma rows of 128 bytes 64 apart (overlap_rows.h)";
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.chromaWidth, 64u);
    EXPECT_EQ(p.chromaPitch, 64u);

    f = surface(542, "444v", 64, 64, 64, 64);
    f.mappedBytes = 16384;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "an RGB8_444_2P target the planar path leaves: its rows overlap";
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.output, gm20b::YcbcrCodes8);
    EXPECT_EQ(p.store.across, 0u);
    EXPECT_EQ(p.store.down, 0u);
}

TEST(GM20BYcbcr, PackedPlanesAreWordsOfThreeCodes)
{
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(508, "p420", 33, 17, 64, 9);
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, gm20b::YcbcrConvertedPacked);
    EXPECT_EQ(p.image, 115u);
    EXPECT_EQ(p.lumaFormat, 53u);
    EXPECT_EQ(p.chromaFormat, 53u);
    EXPECT_EQ(p.lumaWidth, 11u);    // 33 codes
    EXPECT_EQ(p.chromaWidth, 12u);  // 17 pairs: 34 codes
    EXPECT_EQ(p.chromaHeight, 9u);
    EXPECT_EQ(p.shape[11], 0u);
    EXPECT_EQ(p.shape[12], 1023u);

    f = surface(529, "pf22", 33, 17, 64, 17);
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, gm20b::YcbcrSrgbPacked);
    f = surface(548, "p444", 33, 17, 96, 17);
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, gm20b::YcbcrCodes10Packed);
    EXPECT_EQ(p.image, 90u);
    EXPECT_EQ(p.chromaWidth, 22u);  // 33 pairs: 66 codes

    f = surface(548, "p444", 33, 17, 80, 17);
    f.mappedBytes = 16384;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "chroma rows of 88 bytes at an 80-byte pitch: run on into the next";
    EXPECT_EQ(p.chromaWidth, 22u);
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "a target whose chroma rows overlap (overlap_rows.h)";
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.store.packed, 1u);
}

// cameracaptured's FigMetalContext (boot 21): RGB8_420_2P and YCBCR8_420_2P textures of 2x2 over an 8x8 '420f' surface of 64-byte
// rows, usage ShaderRead | ShaderWrite | RenderTarget. Metal reads and stores their top-left corner (an M4 Max: every channel
// of the conversion's reads, and no byte outside the corner stored).
TEST(GM20BYcbcr, ATextureSmallerThanItsSurfaceIsItsTopLeftCorner)
{
    for (const uint32_t format : {500u, 540u}) {
        gm20b::YcbcrFacts f = surface(format, "420f", 8, 8, 64, 4);
        f.width = f.height = 2;
        f.target = true;
        gm20b::YcbcrPlan p;
        ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << format;
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
TEST(GM20BYcbcr, ATextureWiderThanItsSurfaceReadsItsPlanesRows)
{
    gm20b::YcbcrFacts f = surface(540, "420v", 16, 8, 64, 4);
    f.width = 32;
    f.target = true;
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.lumaWidth, 32u);
    EXPECT_EQ(p.chromaWidth, 16u);
    EXPECT_EQ(p.chromaHeight, 4u);
    f.width = 66;
    f.mappedBytes = 16384;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "rows of 66 codes 64 bytes apart: a target whose rows overlap";
    EXPECT_TRUE(p.target);
    EXPECT_EQ(p.lumaWidth, 66u);
    EXPECT_EQ(p.lumaPitch, 64u);
}

TEST(GM20BYcbcr, APackedTargetSmallerThanItsSurfaceIsRefusedButSampled)
{
    gm20b::YcbcrFacts f = surface(546, "p420", 8, 8, 64, 4);
    f.width = f.height = 2;
    gm20b::YcbcrPlan p;
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr);
    f.target = true;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "a packed render target smaller than its surface");
    f.width = f.height = 8;
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr);
}

// An M4 Max answers nil for each, over 420f, x420, p420, 422f and x422 surfaces (8- and 10-bit, packed, sRGB and codes)
TEST(GM20BYcbcr, MetalMakesNoWritableSubsampledTextureOfAnOddSize)
{
    for (const uint32_t format : {500u, 505u, 508u, 520u, 540u, 541u, 543u, 544u, 546u}) {
        for (const uint64_t usage : {0ull, 2ull, 3ull, 4ull, 5ull}) {
            EXPECT_TRUE(gm20b::ycbcrMetalRefuses(format, 3, 3, usage)) << format << " " << usage;
            EXPECT_TRUE(gm20b::ycbcrMetalRefuses(format, 2, 3, usage)) << format << " " << usage;
            EXPECT_TRUE(gm20b::ycbcrMetalRefuses(format, 3, 4, usage)) << format << " " << usage;
            EXPECT_FALSE(gm20b::ycbcrMetalRefuses(format, 4, 4, usage)) << format << " " << usage;
        }
        EXPECT_FALSE(gm20b::ycbcrMetalRefuses(format, 3, 3, 1)) << format;
    }
    EXPECT_FALSE(gm20b::ycbcrMetalRefuses(503, 3, 3, 5));
    EXPECT_FALSE(gm20b::ycbcrMetalRefuses(542, 3, 3, 5));
    EXPECT_FALSE(gm20b::ycbcrMetalRefuses(80, 3, 3, 5));
}

// Under the P3 matrices an M4 Max converts nothing (harness/ycbcroracle.mm): a converted or sRGB format reads its codes as
// red Y, green Cb, blue Cr and samples as its surface's codes format, whose image it draws. A 4:2:0 or 4:2:2 target stores
// as that codes format (harness/ycbcrstoreoracle.mm), an sRGB one each code through the sRGB encode (harness/
// p3storeoracle.mm), a packed one as its twin; a 4:4:4 one's stores are ycbcr_load_test's
TEST(GM20BYcbcr, AP3MatrixReadsTheCodesLumaFirst)
{
    enum Stores {
        AsCodes,
        AsCodesEncoded,
        Own
    };

    struct Case {
        uint32_t format, codes;
        char code[5];
        gm20b::YcbcrOutput output;
        Stores stores;
    };

    const Case cases[] = {{500, 540, "420v", gm20b::YcbcrLumaFirst8, AsCodes},
                          {502, 541, "422f", gm20b::YcbcrLumaFirst8, AsCodes},
                          {505, 543, "x420", gm20b::YcbcrLumaFirst10, AsCodes},
                          {506, 544, "x422", gm20b::YcbcrLumaFirst10, AsCodes},
                          {520, 540, "420f", gm20b::YcbcrLumaFirst8, AsCodesEncoded},
                          {503, 542, "444v", gm20b::YcbcrLumaFirst8, Own},
                          {527, 545, "x444", gm20b::YcbcrLumaFirst10, Own},
                          {508, 546, "p420", gm20b::YcbcrLumaFirst10Packed, AsCodes},
                          {529, 547, "p422", gm20b::YcbcrLumaFirst10Packed, AsCodesEncoded}};
    for (const char *matrix : {"P3_D65", "DCI_P3"})
        for (const Case &c : cases)
            for (const bool target : {false, true}) {
                const bool halvedDown = c.code[1] == '4' && c.code[2] == '2' && c.code[3] == '0';
                gm20b::YcbcrFacts f = surface(c.format, c.code, 32, 16, 128, halvedDown ? 8 : 16);
                f.matrix = matrix;
                f.target = target;
                gm20b::YcbcrPlan p, codes;
                ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << c.format << " " << matrix;
                f.pixelFormat = c.codes;
                f.target = target && c.stores != Own;  // RGB8_444_2P renders through the frontend's stand-in
                ASSERT_EQ(gm20b::planYcbcr(f, codes), nullptr) << c.codes;
                EXPECT_EQ(p.output, c.output) << c.format;
                EXPECT_TRUE(gm20b::ycbcrLumaFirst(p.output));
                EXPECT_FALSE(gm20b::ycbcrLumaFirst(codes.output));
                EXPECT_EQ(p.image, codes.image) << c.format;
                EXPECT_EQ(p.addressMap, codes.addressMap) << c.format;
                EXPECT_EQ(memcmp(p.shape, codes.shape, sizeof p.shape), 0) << c.format;
                if (c.stores == Own)
                    continue;
                gm20b::YcbcrStoreShape stored = codes.store;
                stored.srgb = target && c.stores == AsCodesEncoded;
                EXPECT_EQ(memcmp(&p.store, &stored, sizeof stored), 0) << c.format;
            }
}

// One-plane 4:2:2 formats over a surface of no planes: an M4 Max reads its bytes as Y0 Cb Y1 Cr whatever the surface's
// order ('2vuy', 'yuvs' video range, 'yuvf' full), as the two-plane 4:2:2 format reads those codes split into two planes
TEST(GM20BYcbcr, AOnePlaneFormatIsSplitIntoTwoPlanesOfFourTwoTwo)
{
    const char codes[3][5] = {"2vuy", "yuvs", "yuvf"};
    for (const auto &code : codes)
        for (const uint32_t format : {501u, 521u}) {
            gm20b::YcbcrFacts f;
            f.pixelFormat = format;
            f.fourcc = fourcc(code);
            f.planes = 0;
            f.width = f.surfaceWidth = 33;
            f.height = f.surfaceHeight = 9;
            f.pitch[0] = 80;
            f.allocBytes = 80 * 9;
            f.simple = true;
            gm20b::YcbcrPlan p, two;
            ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << code << " " << format;
            EXPECT_TRUE(p.onePlane);
            EXPECT_EQ(p.output, format == 501 ? gm20b::YcbcrConverted : gm20b::YcbcrSrgb);
            EXPECT_EQ(p.luma, 0u);
            EXPECT_EQ(p.lumaPitch, 80u);
            EXPECT_EQ(p.lumaFormat, 10u);
            EXPECT_EQ(p.lumaWidth, 34u) << "Y0 Y1 of 17 pairs";
            EXPECT_EQ(p.chromaFormat, 30u);
            EXPECT_EQ(p.chromaWidth, 17u);
            EXPECT_EQ(p.chromaHeight, 9u);
            EXPECT_FALSE(p.nativePlanes);
            gm20b::YcbcrFacts g = f;
            g.pixelFormat = format + 1;
            g.fourcc = fourcc(code[3] == 'f' ? "422f" : "422v");
            g.planes = 2;
            g.offset[1] = 80 * 9;
            g.pitch[1] = 80;
            g.allocBytes = 80 * 18;
            ASSERT_EQ(gm20b::planYcbcr(g, two), nullptr);
            EXPECT_EQ(memcmp(p.shape, two.shape, sizeof p.shape), 0) << code << " " << format;
            EXPECT_EQ(p.addressMap, two.addressMap);
            // a target stores its two-plane twin's codes into the luma and chroma images, merged into its pairs
            // (m2v-textures5 probes/targets.mm, m2v-textures6 probes/onep.mm)
            f.target = g.target = true;
            ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << code << " " << format;
            ASSERT_EQ(gm20b::planYcbcr(g, two), nullptr);
            EXPECT_TRUE(p.target && p.onePlane);
            EXPECT_EQ(memcmp(&p.store, &two.store, sizeof p.store), 0) << "the two-plane 4:2:2 twin's store";
            EXPECT_EQ(p.store.words, 0u);
            f.target = false;
            f.planes = 2;
            ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "over a plane of a planar surface: that plane's pairs";
            EXPECT_EQ(p.luma, f.offset[0]);
            EXPECT_EQ(p.lumaPitch, 80u);
        }
    // any surface's plane, in video range where CoreVideo's format is not a full-range YCbCr one (m2v-textures3
    // probes/onep.mm: video over '420v')
    gm20b::YcbcrFacts f;
    f.pixelFormat = 501;
    f.fourcc = fourcc("420v");
    f.planes = 0;
    f.width = f.surfaceWidth = 32;
    f.height = f.surfaceHeight = 8;
    f.pitch[0] = 64;
    f.allocBytes = 64 * 8;
    f.simple = true;
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.shape[2], 0u) << "video range";
}

// YCBCR10_444_1P over 'v410' as a target: the words states into its plane's header, the two-plane 10-bit 4:4:4 codes
TEST(GM20BYcbcr, AOnePlaneWordsTargetStoresItsCodesAsWords)
{
    gm20b::YcbcrFacts f;
    f.pixelFormat = 504;
    f.fourcc = fourcc("v410");
    f.width = f.surfaceWidth = 64;
    f.height = f.surfaceHeight = 16;
    f.pitch[0] = 256;
    f.allocBytes = 256 * 16;
    f.simple = true;
    f.target = true;
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_TRUE(p.target && p.chromaGreenBlue && !p.onePlane);
    EXPECT_EQ(p.store.words, 1u);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.across, 0u);
    EXPECT_EQ(p.store.down, 0u);
    EXPECT_EQ(p.store.converted, 1u);
}

TEST(GM20BYcbcr, RefusesWhatM2vRefuses)
{
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(500, "422v", 33, 17, 64, 17);
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr);
    f = surface(502, "420v", 33, 17, 64, 9);
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.matrix = "SMPTE_240M_1995";
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.shape[4], 14743u);
    f.matrix = "EBU_3213";
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr) << "a matrix Metal names no coefficients for reads as none";
    EXPECT_EQ(p.shape[4], 13123u) << "BT.601's";

    f = surface(505, "x420", 33, 17, 64, 9);
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.mappedBytes = 16384;
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr) << "rows of 66 bytes at a 64-byte pitch: run on into the next";
    EXPECT_FALSE(p.nativePlanes);

    f = surface(505, "x420", 33, 17, 128, 9);
    f.allocBytes = f.offset[1] + 128 * 8 + 2 * 17 * 2 - 1;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.height = 18;
    f.allocBytes = f.offset[1] + 128 * 9;
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr) << "luma rows read on into plane 1";
    f.height = 20;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.width = f.height = 0;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "an empty texture");

    f = surface(505, "x420", 33, 17, 128, 9);
    f.simple = false;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "shape");

    EXPECT_FALSE(gm20b::isYcbcrFormat(80));
    EXPECT_TRUE(gm20b::isYcbcrFormat(545));
}

// A format reads by its own bits and packing over any surface's code, in full range only over a full-range one (an M4 Max:
// m2v-textures9 probes/ycbcr.txt 3f-3l, 3u, 3v, 3ac)
TEST(GM20BYcbcr, AFormatReadsItsOwnBitsOverAnySurfaceCode)
{
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(505, "420v", 33, 17, 128, 9);
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "10-bit codes over an 8-bit surface";
    EXPECT_EQ(p.lumaFormat, 20u) << "R16Unorm";
    EXPECT_EQ(p.shape[2], 0u) << "video range";
    f = surface(500, "xf20", 33, 17, 128, 9);
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "8-bit codes over a 10-bit surface";
    EXPECT_EQ(p.lumaFormat, 10u) << "R8Unorm";
    EXPECT_EQ(p.shape[2], 1u) << "full range";
    f = surface(546, "x420", 33, 17, 128, 9);
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "packed codes over an unpacked surface";
    EXPECT_EQ(p.lumaFormat, 53u) << "R32Uint words";
    for (const uint32_t code : {fourcc("b3a8"), fourcc("BGRA"), fourcc("abcd")}) {
        f = surface(500, "420v", 33, 17, 128, 9);
        f.fourcc = code;
        ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << code;
        EXPECT_EQ(p.shape[2], 0u) << code << ": video range";
        EXPECT_EQ(p.chroma, f.offset[1]) << code;
    }
}

// With no plane after the texture's, a two-plane format reads chroma codes 0 and stores its luma alone: over a surface of one
// plane or none, as over the last of several (an M4 Max: probes/ycbcr.txt 3a-3c, 3s, 3t, 3z, 3af, 3aj)
TEST(GM20BYcbcr, NoPlaneAfterItsOwnReadsChromaZero)
{
    gm20b::YcbcrPlan p;
    for (uint32_t planes : {0u, 1u}) {
        gm20b::YcbcrFacts f = surface(500, "BGRA", 32, 16, 128, 0);
        f.planes = planes;
        f.offset[1] = 0;
        f.pitch[1] = 0;
        f.allocBytes = 2048;
        ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << planes << " planes";
        EXPECT_EQ(p.luma, 0u);
        EXPECT_EQ(p.lumaPitch, 128u);
        EXPECT_EQ(p.chromaWidth, 0u);
        EXPECT_EQ(p.shape[14], 0u) << "chroma codes 0";
        f.target = true;
        ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << planes << " planes, a target";
        EXPECT_EQ(p.chromaWidth, 0u) << "its luma stored alone";
    }
    gm20b::YcbcrFacts f = surface(500, "420v", 32, 16, 64, 8);
    f.planes = 3;
    EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr) << "plane 0 of three: planes 0 and 1";
    EXPECT_EQ(p.chroma, f.offset[1]);
}

// Metal's GPU reads and stores a plane at its offset and rows rounded down to 16 bytes, two-plane and one-plane formats alike
// (an M4 Max: probes/ycbcr.txt 3m-3r, 3x, 3y, 3ak-3am)
TEST(GM20BYcbcr, PlanesOffSixteenAreReadRoundedDown)
{
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(500, "420v", 32, 16, 40, 8);
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "rows of 40";
    EXPECT_EQ(p.lumaPitch, 32u);
    EXPECT_EQ(p.chromaPitch, 32u);
    EXPECT_EQ(p.chroma, 640u);
    f = surface(500, "420v", 32, 16, 32, 8);
    f.offset[0] = 8;
    f.offset[1] = 520;
    f.allocBytes = 1024;
    f.target = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "planes at 8 and 520";
    EXPECT_EQ(p.luma, 0u);
    EXPECT_EQ(p.chroma, 512u);
    f = surface(501, "2vuy", 32, 16, 72, 0);
    f.planes = 1;
    f.offset[1] = 0;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << "a one-plane format, rows of 72";
    EXPECT_EQ(p.lumaPitch, 64u);
}

// Metal's GPU reads a plane's rows its bytesPerRow in 16-byte units less one apart, kept to 16 bits: rows under 16 bytes 1 MiB
// apart, read 0 past the allocation's last page where nothing else is mapped, and rows past 1 MiB wrapped (an M4 Max over
// '420v' 8x8 of rows 8 at 0 and 64, alloc 96: its rows 1-7 read at 1 MiB steps; rows 1 MiB + 32 read at 32)
TEST(GM20BYcbcr, RowsUnderSixteenBytesAreAMebibyteApart)
{
    EXPECT_EQ(gm20b::ycbcrPlaneStride(8), 1u << 20);
    EXPECT_EQ(gm20b::ycbcrPlaneStride(40), 32u);
    EXPECT_EQ(gm20b::ycbcrPlaneStride((1u << 20) + 16), 16u);
    EXPECT_EQ(gm20b::ycbcrPlaneStride((1u << 20) + 32), 32u);
    EXPECT_EQ(gm20b::ycbcrPlaneStride(1u << 20), 1u << 20);
    EXPECT_EQ(gm20b::ycbcrPlaneStride((2u << 20) + 48), 48u);
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(500, "420v", 8, 8, 8, 4);
    f.mappedBytes = 16384;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.sparseTail = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.lumaPitch, 1u << 20);
    EXPECT_EQ(p.chromaPitch, 1u << 20);
    EXPECT_EQ(p.chroma, 64u);
    EXPECT_TRUE(p.window);
    EXPECT_EQ(p.readable, 7ull * (1u << 20) + 8) << "the last luma row's 8 bytes 7 MiB on";
}

// Its GPU stores (a pass's into a target, a blit's into the texture) land rows under 16 bytes 16 apart, its CPU copies at the
// stated rows (an M4 Max over '420v' 8x8 of rows 8 and 8, 8 and 16, 16 and 8: m2v-textures12 probes/f2/targets-full.txt,
// copies-full.txt): each plane's store stride where it is not its read stride
TEST(GM20BYcbcr, RowsUnderSixteenBytesAreStoredSixteenApart)
{
    EXPECT_EQ(gm20b::planeStoreStride(8), 16u);
    EXPECT_EQ(gm20b::planeStoreStride(16), 16u);
    EXPECT_EQ(gm20b::planeStoreStride(40), 32u);
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(500, "420v", 8, 8, 8, 4);
    f.sparseTail = true;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.lumaStorePitch, 16u);
    EXPECT_EQ(p.chromaStorePitch, 16u);
    f.pitch[1] = 16;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.lumaStorePitch, 16u);
    EXPECT_EQ(p.chromaStorePitch, 0u) << "rows of 16 are read and stored 16 apart";
}

}  // namespace
