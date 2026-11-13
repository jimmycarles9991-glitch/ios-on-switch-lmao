// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <vector>
#include "../raw/runtime/raw_keyline_probe.h"

namespace {

// Core Animation's 48-byte vertex: float4 position, two float2 texture coordinates, half4 colour (a3=27:32)
struct Vertex {
    float position[4];
    float texcoords[4];
    uint16_t color[4];
    uint16_t pad[4];
};

constexpr const char *kKey = "samples=1 topo=0 ds=0/0 c0=80:15:1:1:5:0:1:5:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 "
                             "a2=29:24:1:48:1:1 a3=27:32:1:48:1:1";

constexpr float kWidth = 1280, kHeight = 720;
// Core Animation's projection of its target's pixels, column-major
constexpr float kMatrix[16] = {2 / kWidth, 0, 0, 0, 0, -2 / kHeight, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
constexpr double kViewport[4] = {0, 0, kWidth, kHeight};
constexpr uint16_t kTenth = 0x2e66;  // half(0.1)
constexpr uint16_t kOne = 0x3c00;

// The quad's corners in a list's two triangles (6 vertices) or a strip's order (4)
void quad(std::vector<Vertex> &out, float x0, float y0, float x1, float y1, uint16_t alpha, bool list)
{
    const std::vector<std::pair<float, float>> strip = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    const std::vector<int> order = list ? std::vector<int>{0, 1, 2, 2, 1, 3} : std::vector<int>{0, 1, 2, 3};
    for (const int i : order)
        out.push_back({{strip[i].first, strip[i].second, 0, 1}, {}, {alpha, alpha, alpha, alpha}, {}});
}

raw::FillStream stream(const std::vector<Vertex> &vertices)
{
    raw::FillStream s;
    s.bytes = reinterpret_cast<const uint8_t *>(vertices.data());
    s.size = vertices.size() * sizeof(Vertex);
    s.stride = sizeof(Vertex);
    s.positionComponents = 4;
    s.colorOffset = raw::fillColorOffset(kKey);
    return s;
}

// The keyline at 1.59 pixels a point: a half point tall at the pocket's bottom, through the projection into its row
TEST(KeylineProbe, TheKeylinesStripIsOneThinQuadWithItsHalfColour)
{
    std::vector<Vertex> vertices;
    quad(vertices, 0, 107.2f, 1280, 108, kTenth, false);
    const raw::FillScan scan =
        raw::fillScan(stream(vertices), nullptr, 4, 4, reinterpret_cast<const uint8_t *>(kMatrix), kViewport);
    EXPECT_EQ(scan.quads, 1u);
    ASSERT_EQ(scan.thin, 1u);
    EXPECT_NEAR(scan.firstThin.box[1], 107.2f, 1e-3f);
    EXPECT_NEAR(scan.firstThin.box[2], 1280.0f, 1e-3f);
    EXPECT_NEAR(scan.firstThin.box[3], 108.0f, 1e-3f);
    ASSERT_TRUE(scan.firstThin.colored);
    EXPECT_FLOAT_EQ(scan.firstThin.color[3], 0.0999755859375f);
}

TEST(KeylineProbe, AKeylineBatchedAmongOtherQuadsIsTheThinOne)
{
    std::vector<Vertex> vertices;
    quad(vertices, 0, 0, 100, 50, kOne, true);
    quad(vertices, 0, 107.2f, 1280, 108, kTenth, true);
    quad(vertices, 0, 200, 300, 260, kOne, true);
    const raw::FillScan scan =
        raw::fillScan(stream(vertices), nullptr, 18, 6, reinterpret_cast<const uint8_t *>(kMatrix), kViewport);
    EXPECT_EQ(scan.quads, 3u);
    EXPECT_EQ(scan.thin, 1u);
    EXPECT_EQ(scan.firstThin.first, 6u);
    EXPECT_FLOAT_EQ(scan.firstThin.color[0], 0.0999755859375f);
    EXPECT_NEAR(scan.first.box[2], 100.0f, 1e-3f);
}

TEST(KeylineProbe, AnIndexedDrawsQuadsAreItsIndicesVertices)
{
    std::vector<Vertex> vertices;
    quad(vertices, 0, 0, 100, 50, kOne, false);
    quad(vertices, 0, 107.2f, 1280, 108, kTenth, false);
    const uint32_t indices[12] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    const raw::FillScan scan =
        raw::fillScan(stream(vertices), indices, 12, 6, reinterpret_cast<const uint8_t *>(kMatrix), kViewport);
    EXPECT_EQ(scan.quads, 2u);
    ASSERT_EQ(scan.thin, 1u);
    EXPECT_EQ(scan.firstThin.first, 6u);
    EXPECT_FLOAT_EQ(scan.firstThin.color[3], 0.0999755859375f);
}

TEST(KeylineProbe, AScanStopsAtTheStreamsEnd)
{
    std::vector<Vertex> vertices;
    quad(vertices, 0, 107.2f, 1280, 108, kTenth, true);
    const raw::FillScan scan =
        raw::fillScan(stream(vertices), nullptr, 12, 6, reinterpret_cast<const uint8_t *>(kMatrix), kViewport);
    EXPECT_EQ(scan.quads, 1u);
}

TEST(KeylineProbe, AKeyWithoutAHalfColourHasNoColourOffset)
{
    EXPECT_EQ(raw::fillColorOffset(kKey), 32);
    EXPECT_EQ(raw::fillColorOffset("a0=31:0:1:48:1:1 a3=29:32:1:48:1:1"), -1);
    EXPECT_EQ(raw::fillColorOffset(nullptr), -1);
}

TEST(KeylineProbe, HalvesDecodeAcrossTheirRange)
{
    EXPECT_FLOAT_EQ(raw::fillHalf(0x3c00), 1.0f);
    EXPECT_FLOAT_EQ(raw::fillHalf(0xbc00), -1.0f);
    EXPECT_FLOAT_EQ(raw::fillHalf(0x0001), 5.9604645e-8f);
    EXPECT_TRUE(std::isinf(raw::fillHalf(0x7c00)));
    EXPECT_TRUE(std::isnan(raw::fillHalf(0xffff)));
}

// A 1280 x 721 texture whose pass reaches 720 rows: the last one keeps what its memory held
TEST(KeylineProbe, AWriteARowShortOfAnEdgeIsAMiss)
{
    const raw::EdgeMiss bottom = raw::edgeMiss(1280, 721, 0, 0, 1280, 720);
    EXPECT_TRUE(bottom.any());
    EXPECT_EQ(bottom.bottom, 1u);
    EXPECT_EQ(raw::edgeMiss(1280, 720, 0, 2, 1278, 720).top, 2u);
    EXPECT_EQ(raw::edgeMiss(1280, 720, 0, 2, 1278, 720).right, 2u);
}

TEST(KeylineProbe, AWholeWriteOrAPartialOneIsNoMiss)
{
    EXPECT_FALSE(raw::edgeMiss(1280, 720, 0, 0, 1280, 720).any());
    EXPECT_FALSE(raw::edgeMiss(1280, 720, 0, 0, 1280, 717).any());
    EXPECT_FALSE(raw::edgeMiss(1280, 720, 10, 10, 20, 20).any());
    EXPECT_FALSE(raw::edgeMiss(1280, 720, 0, 0, 1281, 720).any());
    EXPECT_FALSE(raw::edgeMiss(1280, 720, 5, 0, 5, 720).any());
}

}  // namespace
