// SPDX-License-Identifier: MIT
#include "../pack/probe_bind.h"
#include "../direct_push.h"

#include <gtest/gtest.h>
#include <algorithm>

namespace {

TEST(GM20BDirectPush, KeepsConstantsAndExcludesEveryBufferResidual)
{
    for (uint32_t stage = 1; stage <= 2; stage++)
        for (uint32_t slot = 0; slot < 31; slot++) {
            NmbDirectState state = {};
            for (NmbDirectDraw &draw : state.draws) {
                draw.pushMask = 0x80000007u;
                draw.push[2] = 0x12345678;
                draw.push[31] = 0xffffffff;
            }
            state.draws[3].push[0] = gm20b::probeBindOffset(stage, slot);
            state.draws[3].push[1] = gm20b::probeStorageCode(stage, slot);
            EXPECT_EQ(gm20b::constantPushWords(state), 0x80000004u) << stage << " " << slot;
        }
}

TEST(GM20BDirectPush, RejectsWordsChangedOrMissingInAnyProbeDraw)
{
    for (uint32_t probe = 0; probe < 4; probe++) {
        NmbDirectState state = {};
        for (NmbDirectDraw &draw : state.draws)
            draw.pushMask = 7;
        state.draws[probe].push[0] = 1;
        state.draws[probe].pushMask &= ~2u;
        EXPECT_EQ(gm20b::constantPushWords(state), 4u) << probe;
    }
}

TEST(GM20BDirectPush, EqualProbeBlendWordsStayDynamic)
{
    NmbDirectState state = {};
    for (auto &draw : state.draws) {
        draw.pushMask = UINT32_MAX;
        draw.halfBlendColorWord = 29;
    }
    EXPECT_TRUE(gm20b::validDynamicPush(state));
    EXPECT_EQ(gm20b::constantPushWords(state), 0x0fffffffu);
    state.draws[3].halfBlendColorWord = 25;
    EXPECT_FALSE(gm20b::validDynamicPush(state));
}

bool placed(uint32_t first, uint32_t firstCount, uint32_t second, uint32_t pushMask)
{
    const gm20b::DynamicField fields[] = {{first, firstCount}, {second, 4}};
    return gm20b::dynamicFieldsPlaced(fields, pushMask, 0);
}

TEST(GM20BDirectPush, RejectsTruncatedOverlappingAndMissingFields)
{
    EXPECT_TRUE(placed(25, 8, 1, UINT32_MAX));
    EXPECT_FALSE(placed(26, 8, 0, UINT32_MAX));
    EXPECT_FALSE(placed(0, 8, 30, UINT32_MAX));
    EXPECT_FALSE(placed(5, 8, 12, UINT32_MAX));
    EXPECT_FALSE(placed(5, 8, 0, UINT32_MAX & ~(1u << 11)));
    EXPECT_FALSE(placed(UINT32_MAX, 8, 0, UINT32_MAX));
}

TEST(GM20BDirectPush, PatchesTheBlendColourWithoutChangingOtherWords)
{
    gm20b::PackPipeline pipeline = {};
    pipeline.halfBlendColorWord = 29;
    const uint32_t color[] = {0x3f800000, 0x80000000, 0x7fc00001, 0x3f000000};
    uint32_t push[32];
    std::fill_n(push, 32, 0xfeed);
    gm20b::patchBlendPush(pipeline, color, push);
    for (unsigned i = 0; i < 32; i++)
        EXPECT_EQ(push[i], i >= 28 ? color[i - 28] : 0xfeed);
    pipeline.halfBlendColorWord = 17;
    gm20b::patchBlendPush(pipeline, color, push, 16, 16);
    EXPECT_EQ(push[0], color[0]);
    EXPECT_EQ(push[3], color[3]);
    EXPECT_EQ(push[4], 0xfeedu);
}

TEST(GM20BDirectPush, LegacyConstantBlendCannotReuseFrozenPush)
{
    gm20b::PackPipeline pipeline = {};
    const char *dynamic = "samples=1 topo=0 ds=0/0 c0=115:15:1:11:0:0:1:14:0";
    EXPECT_FALSE(gm20b::usableDynamicPush(pipeline, dynamic));
    EXPECT_TRUE(gm20b::usableDynamicPush(pipeline, "samples=1 c0=115:15:0:11:0:0:1:14:0"));
    EXPECT_TRUE(gm20b::usableDynamicPush(pipeline, "samples=1 c0=115:15:1:1:0:0:1:0:0"));
    pipeline.flags = gm20b::PipelinePushMetadata;
    EXPECT_TRUE(gm20b::usableDynamicPush(pipeline, dynamic));
    pipeline.halfBlendColorWord = 1;
    EXPECT_FALSE(gm20b::usableDynamicPush(pipeline, dynamic));
    pipeline.pushMask = 255;
    EXPECT_TRUE(gm20b::usableDynamicPush(pipeline, dynamic));
}

}  // namespace
