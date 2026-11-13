// SPDX-License-Identifier: MIT
// raw/runtime/raw_packed_plane.h: R10Unorm_PACKED and RG10Unorm_PACKED over a packed 10-bit surface's planes. Which ones the
// raw path takes, the codes its conversion state reads and its store state writes (the layout an M4 Max's Metal reads,
// raw/test/packedplaneoracle.mm), each code held as the 10-bit YCbCr code targets hold it (raw_ycbcr_store_rule.h).
#include "../raw/runtime/raw_packed_plane.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr uint32_t kP420 = 0x70343230, kP422 = 0x70343232, kP444 = 0x70343434, kX420 = 0x78343230;

// A CoreVideo 'p4xx' surface of w x h: rows `pitch` bytes apart, plane 1 after plane 0's rows
raw::YcbcrFacts surface(uint32_t format, uint32_t plane, uint32_t fourcc, uint32_t w, uint32_t h, uint32_t pitch,
                        uint32_t chromaRows)
{
    raw::YcbcrFacts f;
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

TEST(RawPackedPlane, LumaWordsOfAFourTwoZeroSurface)
{
    raw::YcbcrFacts f = surface(588, 0, kP420, 100, 4, 192, 2);
    f.width = 100;
    f.height = 4;
    f.target = true;
    raw::YcbcrPlan p;
    ASSERT_EQ(raw::planPackedPlane(f, p), nullptr);
    EXPECT_EQ(p.output, raw::YcbcrPackedPlane);
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

TEST(RawPackedPlane, ChromaPairsOfEachSubsampling)
{
    struct Case {
        uint32_t fourcc, chromaWidth, chromaHeight, words;
    } cases[] = {{kP420, 51, 2, 34}, {kP422, 51, 4, 34}, {kP444, 101, 4, 68}};

    for (const Case &c : cases) {
        raw::YcbcrFacts f = surface(589, 1, c.fourcc, 101, 4, 320, c.chromaHeight);
        f.width = c.chromaWidth;
        f.height = c.chromaHeight;
        raw::YcbcrPlan p;
        ASSERT_EQ(raw::planPackedPlane(f, p), nullptr) << c.fourcc;
        EXPECT_EQ(p.luma, 320u * 4) << c.fourcc;
        EXPECT_EQ(p.lumaWidth, c.words) << c.fourcc;
        EXPECT_EQ(p.shape[2], 2u) << c.fourcc;
        EXPECT_FALSE(p.target);
    }
}

TEST(RawPackedPlane, ViewsMetalReadsAreTakenOnly)
{
    raw::YcbcrPlan p;
    raw::YcbcrFacts f = surface(588, 1, kP420, 64, 4, 128, 2);
    f.width = 32;
    f.height = 2;
    EXPECT_NE(raw::planPackedPlane(f, p), nullptr);  // R10Unorm_PACKED over the chroma plane
    f = surface(589, 0, kP420, 64, 4, 128, 2);
    f.width = 64;
    f.height = 4;
    EXPECT_NE(raw::planPackedPlane(f, p), nullptr);  // RG10Unorm_PACKED over the luma plane
    f = surface(588, 0, kX420, 64, 4, 128, 2);
    f.width = 64;
    f.height = 4;
    EXPECT_NE(raw::planPackedPlane(f, p), nullptr);  // a surface of 16-bit codes
    f = surface(588, 0, kP420, 64, 4, 128, 2);
    f.width = 32;
    f.height = 4;
    EXPECT_NE(raw::planPackedPlane(f, p), nullptr);  // not the plane's size
    f.width = 64;
    f.pitch[0] = 80;  // 22 words need 88 bytes
    EXPECT_NE(raw::planPackedPlane(f, p), nullptr);
    f.pitch[0] = 128;
    f.offset[0] = 8;
    EXPECT_NE(raw::planPackedPlane(f, p), nullptr);
    f.offset[0] = 0;
    f.simple = false;
    EXPECT_NE(raw::planPackedPlane(f, p), nullptr);
    f.simple = true;
    EXPECT_EQ(raw::planPackedPlane(f, p), nullptr);
}

TEST(RawPackedPlane, TheYcbcrPlannerRoutesThem)
{
    raw::YcbcrFacts f = surface(589, 1, kP420, 64, 4, 128, 2);
    f.width = 32;
    f.height = 2;
    raw::YcbcrPlan p;
    ASSERT_EQ(raw::planYcbcr(f, p), nullptr);
    EXPECT_EQ(p.output, raw::YcbcrPackedPlane);
    EXPECT_TRUE(raw::ycbcrRenderedFormat(588) && raw::ycbcrRenderedFormat(589));
    EXPECT_EQ(raw::ycbcrTargetRefusal(588), nullptr);
    EXPECT_EQ(raw::ycbcrTargetRefusal(589), nullptr);
    EXPECT_EQ(raw::ycbcrTargetWrites(588), 8u);
    EXPECT_EQ(raw::ycbcrTargetWrites(589), 12u);
    EXPECT_EQ(raw::ycbcrTargetWrites(540), 14u);
    EXPECT_EQ(raw::ycbcrStandInFormat(588), 55u);   // R32Float: R10Unorm_PACKED's fragments return one component
    EXPECT_EQ(raw::ycbcrStandInFormat(589), 105u);  // RG32Float
    EXPECT_EQ(raw::ycbcrStandInFormat(546), raw::kYcbcrTargetStandIn);
}

TEST(RawPackedPlane, TexelsReadThreeCodesAWordInRowOrder)
{
    // codes 0x101 .. 0x107 of a row, bits 30-31 set: R10 texel x is code x, RG10 texel x codes 2x and 2x + 1
    const uint32_t row[3] = {0xc0000000u | 0x103u << 20 | 0x102u << 10 | 0x101u,
                             0xc0000000u | 0x106u << 20 | 0x105u << 10 | 0x104u, 0xc0000000u | 0x107u};
    for (uint32_t x = 0; x < 7; x++) {
        uint32_t codes[2];
        raw::packedPlaneTexel(row, x, 1, codes);
        EXPECT_EQ(codes[0], 0x101u + x);
        EXPECT_EQ(codes[1], 0u);
    }
    for (uint32_t x = 0; x < 3; x++) {
        uint32_t codes[2];
        raw::packedPlaneTexel(row, x, 2, codes);
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

TEST(RawPackedPlane, StoredWordsAreTheTwinsCodesInTheReadLayout)
{
    for (uint32_t channels : {1u, 2u})
        for (uint32_t width : {96u, 97u, 98u}) {
            const uint32_t height = 3, codes = width * channels, perRow = raw::packedPlaneWords(codes);
            const std::vector<float> rgba = stand_in(width * height, width + 10 * channels);
            std::vector<uint32_t> words(perRow * height, 0xa5a5a5a5u);
            raw::packedPlaneDrawn(raw::packedPlaneShape(channels), rgba.data(), width, height, words.data());
            for (uint32_t y = 0; y < height; y++) {
                const uint32_t *row = &words[y * perRow];
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t got[2];
                    raw::packedPlaneTexel(row, x, channels, got);
                    for (uint32_t c = 0; c < channels; c++)
                        EXPECT_EQ(got[c], raw::ycbcrTexelCode(rgba[4 * (y * width + x) + c],
                                                              raw::YcbcrTexelSource::DrawnFloat, 1023))
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

}  // namespace
