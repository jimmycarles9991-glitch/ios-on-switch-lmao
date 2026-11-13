// SPDX-License-Identifier: MIT
// Rule blur.twod's classifier on synthetic tap sets: which passes of Core Animation's blur chain are 2^n boxes.
#include <gtest/gtest.h>
#include <cstring>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_blur_twod.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_ca_route.h"

namespace {

using raw::BlurFunction;
using raw::BlurUniform;

TEST(BlurTwod, FunctionsByName)
{
    EXPECT_EQ(raw::blurFunctionOf("PRGhAXm_Tds4A2Xhf_Isrc"), BlurFunction::Ds4);
    EXPECT_EQ(raw::blurFunctionOf("PRGhAXm_Tdb2A2Xhf_Isrc"), BlurFunction::Db2);
    EXPECT_EQ(raw::blurFunctionOf("PRGhAXm_Tnb7A2Xhf_Isrc"), BlurFunction::None);
    EXPECT_EQ(raw::caRouteOf("PRGhAXm_Tnb7A2Xhf_Isrc"), raw::CaRoute::Blur);
    EXPECT_EQ(raw::caRouteOf("PBGRAXm_A2Xhfc"), raw::CaRoute::Fill);
    EXPECT_EQ(raw::caRouteOf("PBGRAXm_TimgA2Xhfc_Isrc"), raw::CaRoute::None);
    EXPECT_EQ(raw::caRouteOf(nullptr), raw::CaRoute::None);
}

TEST(BlurTwod, OneCentredBilinearSampleAtTwoToOneIsOneStep)
{
    BlurUniform u;
    u.weight[0] = 1;
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Db2, u, 512, 256, 256, 128), 1u);
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Ds4, u, 512, 256, 256, 128), 1u);
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Ds4, u, 512, 256, 128, 64), 0u);  // 4:1: one sample covers 2x2 of 4x4
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Ds4, u, 300, 200, 100, 100), 0u);
}

TEST(BlurTwod, FourQuarterTapsAtFourToOneAreTwoSteps)
{
    const uint32_t sw = 512, sh = 256;
    BlurUniform u;
    const float d[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int k = 0; k < 4; k++) {
        u.offset[k][0] = d[k][0] / float(sw);
        u.offset[k][1] = d[k][1] / float(sh);
        u.weight[k] = 0.25f;
    }
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Ds4, u, sw, sh, sw / 4, sh / 4), 2u);
    u.weight[3] = 0.3f;
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Ds4, u, sw, sh, sw / 4, sh / 4), 0u);
}

TEST(BlurTwod, DownsampleEightIsMirroredPairsAveraged)
{
    const uint32_t sw = 256, sh = 256;
    BlurUniform u;
    const float d[4][2] = {{1, 1}, {1, -1}, {1, 1}, {1, -1}};
    for (int k = 0; k < 4; k++) {
        u.offset[k][0] = d[k][0] / float(sw);
        u.offset[k][1] = d[k][1] / float(sh);
    }
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Ds8, u, sw, sh, 64, 64), 2u);
}

TEST(BlurTwod, AGaussianIsNoBox)
{
    BlurUniform u;
    u.weight[0] = 0.4f;
    u.weight[1] = 0.3f;
    u.offset[0][0] = 1.5f / 512;
    EXPECT_EQ(raw::blurTwodSteps(BlurFunction::Db2, u, 512, 512, 256, 256), 0u);
}

TEST(BlurTwod, TheUniformsHalvesAndTheProvenMargins)
{
    uint8_t bytes[80] = {};
    const float offsets[2] = {0.25f, -0.5f};
    const _Float16 weights[2] = {_Float16(0.5f), _Float16(0.125f)};
    memcpy(bytes, offsets, sizeof offsets);
    memcpy(bytes + 64, weights, sizeof weights);
    const BlurUniform u = raw::blurUniform(bytes);
    EXPECT_EQ(u.offset[0][0], 0.25f);
    EXPECT_EQ(u.offset[0][1], -0.5f);
    EXPECT_EQ(u.weight[0], 0.5f);
    EXPECT_EQ(u.weight[1], 0.125f);
    EXPECT_TRUE(raw::blurTwodFormat(80, 1));
    EXPECT_FALSE(raw::blurTwodFormat(80, 2));
    EXPECT_TRUE(raw::blurTwodFormat(115, 3));
    EXPECT_FALSE(raw::blurTwodFormat(55, 1));
    EXPECT_FALSE(raw::blurTwodRoute());
}

}  // namespace
