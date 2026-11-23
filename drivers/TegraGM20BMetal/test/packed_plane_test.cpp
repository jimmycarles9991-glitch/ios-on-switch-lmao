// SPDX-License-Identifier: MIT
// packed_plane.h: R10Unorm_PACKED and RG10Unorm_PACKED over a packed 10-bit surface's planes. Which ones the
// raw path takes, the codes its conversion state reads and its store state writes (the layout an M4 Max's Metal reads,
// harness/packedplaneoracle.mm), each code held as the 10-bit YCbCr code targets hold it (ycbcr_store_rule.h).
#include "../packed_plane.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr uint32_t kP420 = 0x70343230, kP422 = 0x70343232, kP444 = 0x70343434, kX420 = 0x78343230, kV420 = 0x34323076;

// A CoreVideo 'p4xx' surface of w x h: rows `pitch` bytes apart, plane 1 after plane 0's rows
gm20b::YcbcrFacts surface(uint32_t format, uint32_t plane, uint32_t fourcc, uint32_t w, uint32_t h, uint32_t pitch,
                          uint32_t chromaRows)
{
    gm20b::YcbcrFacts f;
    f.pixelFormat = format;
    f.fourcc = fourcc;
    f.plane = plane;
    f.planes = 2;
    f.surfaceWidth = w;
    f.surfaceHeight = h;
    f.offset[1] = uint64_t(pitch) * h;
    f.pitch[0] = f.pitch[1] = pitch;
    f.allocBytes = f.offset[1] + uint64_t(pitch) * chromaRows;
    f.simple = true;
    return f;
}

TEST(GM20BPackedPlane, LumaWordsOfAFourTwoZeroSurface)
{
    gm20b::YcbcrFacts f = surface(588, 0, kP420, 100, 4, 192, 2);
    f.width = 100;
    f.height = 4;
    f.target = true;
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planPackedPlane(f, p), nullptr);
    EXPECT_EQ(p.output, gm20b::YcbcrPackedPlane);
    EXPECT_EQ(p.image, 90u);
    EXPECT_EQ(p.lumaFormat, 53u);
    EXPECT_EQ(p.lumaWidth, 34u);  // 100 codes, three a word
    EXPECT_EQ(p.lumaPitch, 192u);
    EXPECT_EQ(p.luma, 0u);
    EXPECT_EQ(p.chromaWidth, 0u);
    EXPECT_EQ(p.shape[0], 100u);
    EXPECT_EQ(p.shape[1], 4u);
    EXPECT_EQ(p.shape[2], 1u);
    EXPECT_EQ(p.store.plane, 1u);
    EXPECT_EQ(p.store.max, 1023u);
    EXPECT_EQ(p.store.towardZero, 1u);
    EXPECT_EQ(p.store.shift, 0u);
    EXPECT_EQ(p.addressMap, 0);
}

TEST(GM20BPackedPlane, ChromaPairsOfEachSubsampling)
{
    struct Case {
        uint32_t fourcc, chromaWidth, chromaHeight, words;
    } cases[] = {{kP420, 51, 2, 34}, {kP422, 51, 4, 34}, {kP444, 101, 4, 68}};

    for (const Case &c : cases) {
        gm20b::YcbcrFacts f = surface(589, 1, c.fourcc, 101, 4, 320, c.chromaHeight);
        f.width = c.chromaWidth;
        f.height = c.chromaHeight;
        gm20b::YcbcrPlan p;
        ASSERT_EQ(gm20b::planPackedPlane(f, p), nullptr) << c.fourcc;
        EXPECT_EQ(p.luma, 320u * 4) << c.fourcc;
        EXPECT_EQ(p.lumaWidth, c.words) << c.fourcc;
        EXPECT_EQ(p.shape[2], 2u) << c.fourcc;
        EXPECT_FALSE(p.target);
    }
}

// Metal makes each over any surface's plane at any size and reads it by the format's codes a texel, at the plane's offset
// and rows rounded down to 16 (an M4 Max, validation on and off: 588 over plane 1 and 589 over plane 0 of 'p420', 588 over
// 'x420', a 32x4 over a 64x4 plane, rows 88 read at 80, plane 1 at 1544 read at 1536, rows of 56 for 22 words overlapping)
TEST(GM20BPackedPlane, MetalReadsEveryPlaneAtItsRoundedLayout)
{
    struct Case {
        uint32_t format, plane, fourcc, width, height, pitch;
        uint64_t offset;
        uint32_t words, readPitch;
        uint64_t readOffset;
    } cases[] = {
        {588, 1, kP420, 32, 2, 128, 512, 11, 128, 512},    // R10Unorm_PACKED over the chroma plane
        {589, 0, kP420, 64, 4, 128, 0, 43, 128, 0},        // RG10Unorm_PACKED over the luma plane
        {588, 0, kX420, 64, 4, 128, 0, 22, 128, 0},        // a surface of 16-bit codes
        {588, 0, kP420, 32, 4, 128, 0, 11, 128, 0},        // smaller than its plane
        {588, 0, kP420, 64, 4, 88, 0, 22, 80, 0},          // rows off 16
        {588, 1, kP420, 32, 2, 128, 1544, 11, 128, 1536},  // an offset off 16
        {588, 0, kP420, 64, 4, 56, 0, 22, 48, 0},          // rows that overlap
    };

    for (const Case &c : cases) {
        gm20b::YcbcrFacts f = surface(c.format, c.plane, c.fourcc, 64, 4, 128, 2);
        f.pitch[c.plane] = c.pitch;
        f.offset[c.plane] = c.offset;
        f.allocBytes = 2048;
        f.width = c.width;
        f.height = c.height;
        gm20b::YcbcrPlan p;
        ASSERT_EQ(gm20b::planPackedPlane(f, p), nullptr) << c.format << " plane " << c.plane << " rows " << c.pitch;
        EXPECT_EQ(p.lumaWidth, c.words) << c.format << " rows " << c.pitch;
        EXPECT_EQ(p.lumaPitch, c.readPitch) << c.format << " rows " << c.pitch;
        EXPECT_EQ(p.luma, c.readOffset) << c.format << " rows " << c.pitch;
        EXPECT_EQ(p.shape[2], c.format == 589 ? 2u : 1u);
        EXPECT_EQ(p.readable, 2048u) << "to the allocation's end";
        EXPECT_FALSE(p.window);
    }
    gm20b::YcbcrPlan p;
    gm20b::YcbcrFacts f = surface(588, 0, kP420, 64, 4, 128, 2);
    f.width = 64;
    f.height = 4;
    f.simple = false;
    EXPECT_STREQ(gm20b::planPackedPlane(f, p), "shape");
}

// Rows past the allocation's last page read 0 (an M4 Max over a 589 whose rows end past the mapping): through a sparse
// window where the kernel maps VA sparse, else not taken
TEST(GM20BPackedPlane, RowsPastTheLastPageReadThroughAWindow)
{
    gm20b::YcbcrFacts f = surface(589, 1, kP420, 64, 64, 128, 32);
    f.offset[1] = 16384 - 128;
    f.allocBytes = 16384;
    f.mappedBytes = 16384;
    f.width = 32;
    f.height = 4;
    gm20b::YcbcrPlan p;
    EXPECT_STREQ(gm20b::planPackedPlane(f, p), "rows past the allocation's last page (an M4 Max reads 0 there)");
    f.sparseTail = true;
    ASSERT_EQ(gm20b::planPackedPlane(f, p), nullptr);
    EXPECT_TRUE(p.window);
    EXPECT_EQ(p.readable, 16384u - 128 + 3 * 128 + 22 * 4);
}

// Metal makes them of 2 and 3 levels too, every lod reading level 0 (an M4 Max: m2v-textures11 probes/h4/mips.txt): the
// plan's image holds as many levels. A two-plane YCbCr format of several levels stays Metal's nil (no IOSurface texture is
// mipmapped)
TEST(GM20BPackedPlane, SeveralLevelsAreTakenAsTheyAreMade)
{
    for (const uint32_t levels : {2u, 3u}) {
        gm20b::YcbcrFacts f = surface(588, 0, kP420, 32, 16, 64, 8);
        f.width = 32;
        f.height = 16;
        f.levels = levels;
        gm20b::YcbcrPlan p;
        ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr) << levels;
        EXPECT_EQ(p.levels, levels);
        EXPECT_EQ(p.lumaWidth, 11u) << "level 0's rows: every level reads them";
        EXPECT_EQ(p.addressMap, 0);
    }
    gm20b::YcbcrFacts f = surface(500, 0, kV420, 32, 16, 32, 8);
    f.width = 32;
    f.height = 16;
    f.levels = 2;
    gm20b::YcbcrPlan p;
    EXPECT_STREQ(gm20b::planYcbcr(f, p), "shape");
}

TEST(GM20BPackedPlane, TheYcbcrPlannerRoutesThem)
{
    gm20b::YcbcrFacts f = surface(589, 1, kP420, 64, 4, 128, 2);
    f.width = 32;
    f.height = 2;
    gm20b::YcbcrPlan p;
    ASSERT_EQ(gm20b::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, gm20b::YcbcrPackedPlane);
    EXPECT_TRUE(gm20b::ycbcrRenderedFormat(588) && gm20b::ycbcrRenderedFormat(589));
    EXPECT_EQ(gm20b::ycbcrTargetRefusal(588), nullptr);
    EXPECT_EQ(gm20b::ycbcrTargetRefusal(589), nullptr);
    EXPECT_EQ(gm20b::ycbcrStandInFormat(588), 55u);   // R32Float: R10Unorm_PACKED's fragments return one component
    EXPECT_EQ(gm20b::ycbcrStandInFormat(589), 105u);  // RG32Float
    EXPECT_EQ(gm20b::ycbcrStandInFormat(546), gm20b::kYcbcrTargetStandIn);
}

TEST(GM20BPackedPlane, TexelsReadThreeCodesAWordInRowOrder)
{
    // codes 0x101 .. 0x107 of a row, bits 30-31 set: R10 texel x is code x, RG10 texel x codes 2x and 2x + 1
    const uint32_t row[3] = {0xc0000000u | 0x103u << 20 | 0x102u << 10 | 0x101u,
                             0xc0000000u | 0x106u << 20 | 0x105u << 10 | 0x104u, 0xc0000000u | 0x107u};
    for (uint32_t x = 0; x < 7; x++) {
        uint32_t codes[2];
        gm20b::packedPlaneTexel(row, x, 1, codes);
        EXPECT_EQ(codes[0], 0x101u + x);
        EXPECT_EQ(codes[1], 0u);
    }
    for (uint32_t x = 0; x < 3; x++) {
        uint32_t codes[2];
        gm20b::packedPlaneTexel(row, x, 2, codes);
        EXPECT_EQ(codes[0], 0x101u + 2 * x);
        EXPECT_EQ(codes[1], 0x102u + 2 * x);
    }
}

// Floats beside the half midpoints and the ties of 1/1023, out of range and not numbers
std::vector<float> stand_in(uint32_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<float> v(4 * n);
    for (float &x : v) {
        const uint32_t k = rng();
        float base;
        switch (k % 5) {
        case 0: {
            base = float((double((k >> 8) % 1023) + 0.5) / 1023.0);
            break;
        }
        case 1: {
            _Float16 a, b;
            const uint16_t h = uint16_t(k >> 8) % 0x3c00, h1 = h + 1;
            memcpy(&a, &h, 2);
            memcpy(&b, &h1, 2);
            base = float((double(a) + double(b)) / 2);
            break;
        }
        case 2: {
            const float special[] = {NAN, INFINITY, -INFINITY, -0.0f, -0.3f, 1.7f, 0.0f, 1.0f};
            x = special[(k >> 8) % 8];
            continue;
        }
        default:
            base = float((k >> 8) % 1000000) / 1000000.0f;
        }
        uint32_t u;
        memcpy(&u, &base, 4);
        u += uint32_t(int((k >> 3) % 7) - 3);
        memcpy(&x, &u, 4);
    }
    return v;
}

TEST(GM20BPackedPlane, StoredWordsAreTheTwinsCodesInTheReadLayout)
{
    for (uint32_t channels : {1u, 2u})
        for (uint32_t width : {96u, 97u, 98u}) {
            const uint32_t height = 3, codes = width * channels, perRow = gm20b::packedPlaneWords(codes);
            const std::vector<float> rgba = stand_in(width * height, width + 10 * channels);
            std::vector<uint32_t> words(perRow * height, 0xa5a5a5a5u);
            gm20b::packedPlaneDrawn(gm20b::packedPlaneShape(channels), rgba.data(), width, height, words.data());
            for (uint32_t y = 0; y < height; y++) {
                const uint32_t *row = &words[y * perRow];
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t got[2];
                    gm20b::packedPlaneTexel(row, x, channels, got);
                    for (uint32_t c = 0; c < channels; c++)
                        EXPECT_EQ(got[c], gm20b::ycbcrTexelCode(rgba[4 * (y * width + x) + c],
                                                                gm20b::YcbcrTexelSource::DrawnFloat, 1023))
                            << channels << " codes a texel, texel " << x << " channel " << c;
                }
                for (uint32_t w = 0; w < perRow; w++)
                    EXPECT_EQ(row[w] >> 30, 0u);
                const uint32_t tail = codes % 3;
                if (tail)
                    EXPECT_EQ(row[perRow - 1] >> (10 * tail), 0u) << width;
            }
        }
}

// A pass that loads such a texture and draws nothing stores the words it loaded, on every code of either view: the load state
// draws each code over 1023 into the stand-in, the store state holds it as the half toward zero and rounds h x 1023 to the
// nearest code. So does a stand-in value four float steps either side of the quotient (a division the device rounds otherwise)
TEST(GM20BPackedPlane, LoadThenStoreGivesBackEveryCode)
{
    for (uint32_t channels : {1u, 2u}) {
        const uint32_t width = 1024 / channels, perRow = gm20b::packedPlaneWords(1024);
        std::vector<uint32_t> words(perRow, 0);
        for (uint32_t n = 0; n < 1024; n++)
            words[n / 3] |= n << (10 * (n % 3));
        std::vector<float> rgba(4 * width);
        for (uint32_t x = 0; x < width; x++) {
            float value[2];
            gm20b::packedPlaneLoad(words.data(), x, channels, value);
            rgba[4 * x] = value[0];
            rgba[4 * x + 1] = value[1];
            rgba[4 * x + 3] = 1.0f;
        }
        std::vector<uint32_t> stored(perRow, 0xa5a5a5a5u);
        gm20b::packedPlaneDrawn(gm20b::packedPlaneShape(channels), rgba.data(), width, 1, stored.data());
        EXPECT_EQ(stored, words) << channels << " codes a texel";
    }
    const gm20b::YcbcrStoreShape shape = gm20b::packedPlaneShape(1);
    for (uint32_t code = 0; code < 1024; code++) {
        float near = float(code) / 1023.0f;
        float below = near, above = near;
        for (int step = 0; step < 4; step++) {
            below = std::nextafter(below, 0.0f);
            above = std::nextafter(above, 2.0f);
        }
        EXPECT_EQ(gm20b::ycbcrStoreCode(shape, code ? below : near), code);
        EXPECT_EQ(gm20b::ycbcrStoreCode(shape, code < 1023 ? above : near), code);
    }
}

}  // namespace
