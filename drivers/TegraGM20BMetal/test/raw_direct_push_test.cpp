// SPDX-License-Identifier: MIT
#include "../raw/pack/probe_bind.h"
#include "../raw/runtime/raw_direct_push.h"

#include <gtest/gtest.h>
#include <algorithm>

namespace {

TEST(RawDirectPush, KeepsConstantsAndExcludesEveryBufferResidual)
{
    for (uint32_t stage = 1; stage <= 2; stage++)
        for (uint32_t slot = 0; slot < 31; slot++) {
            NmbDirectState state = {};
            for (NmbDirectDraw &draw : state.draws) {
                draw.pushMask = 0x80000007u;
                draw.push[2] = 0x12345678;
                draw.push[31] = 0xffffffff;
            }
            state.draws[3].push[0] = raw::probeBindOffset(stage, slot);
            state.draws[3].push[1] = raw::probeStorageCode(stage, slot);
            EXPECT_EQ(raw::constantPushWords(state), 0x80000004u) << stage << " " << slot;
        }
}

TEST(RawDirectPush, RejectsWordsChangedOrMissingInAnyProbeDraw)
{
    for (uint32_t probe = 0; probe < 4; probe++) {
        NmbDirectState state = {};
        for (NmbDirectDraw &draw : state.draws)
            draw.pushMask = 7;
        state.draws[probe].push[0] = 1;
        state.draws[probe].pushMask &= ~2u;
        EXPECT_EQ(raw::constantPushWords(state), 4u) << probe;
    }
}

TEST(RawDirectPush, EqualProbeBlendWordsStayDynamic)
{
    NmbDirectState state = {};
    for (auto &draw : state.draws) {
        draw.pushMask = UINT32_MAX;
        draw.blendColorWord = 5;
        draw.halfBlendColorWord = 29;
    }
    EXPECT_TRUE(raw::validDynamicPush(state));
    EXPECT_EQ(raw::constantPushWords(state), 0x0ffff00fu);
    state.draws[3].blendColorWord = 6;
    EXPECT_FALSE(raw::validDynamicPush(state));
}

TEST(RawDirectPush, RejectsTruncatedOverlappingAndMissingFields)
{
    EXPECT_TRUE(raw::validDynamicPush(25, 1, UINT32_MAX));
    EXPECT_FALSE(raw::validDynamicPush(26, 0, UINT32_MAX));
    EXPECT_FALSE(raw::validDynamicPush(0, 30, UINT32_MAX));
    EXPECT_FALSE(raw::validDynamicPush(5, 12, UINT32_MAX));
    EXPECT_FALSE(raw::validDynamicPush(5, 0, UINT32_MAX & ~(1u << 11)));
    EXPECT_FALSE(raw::validDynamicPush(UINT32_MAX, 0, UINT32_MAX));
}

TEST(RawDirectPush, PatchesBothFieldsWithoutChangingOtherWords)
{
    raw::PackPipeline pipeline = {};
    pipeline.blendColorWord = 5;
    pipeline.halfBlendColorWord = 29;
    const uint32_t color[] = {0x3f800000, 0x80000000, 0x7fc00001, 0x3f000000};
    const uint32_t extended[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t push[32];
    std::fill_n(push, 32, 0xfeed);
    raw::patchBlendPush(pipeline, color, extended, push);
    for (unsigned i = 0; i < 32; i++)
        EXPECT_EQ(push[i], i >= 4 && i < 12 ? extended[i - 4] : i >= 28 ? color[i - 28] : 0xfeed);
    pipeline.blendColorWord = 17;
    pipeline.halfBlendColorWord = 0;
    raw::patchBlendPush(pipeline, color, extended, push);
    EXPECT_EQ(push[16], extended[0]);
    EXPECT_EQ(push[23], extended[7]);
    EXPECT_EQ(push[28], color[0]);
}

TEST(RawDirectPush, LegacyConstantBlendCannotReuseFrozenPush)
{
    raw::PackPipeline pipeline = {};
    const char *dynamic = "samples=1 topo=0 ds=0/0 c0=554:15:1:11:0:0:1:14:0";
    EXPECT_FALSE(raw::usableDynamicPush(pipeline, dynamic));
    EXPECT_TRUE(raw::usableDynamicPush(pipeline, "samples=1 c0=554:15:0:11:0:0:1:14:0"));
    EXPECT_TRUE(raw::usableDynamicPush(pipeline, "samples=1 c0=554:15:1:1:0:0:1:0:0"));
    pipeline.flags = raw::PipelinePushMetadata;
    EXPECT_TRUE(raw::usableDynamicPush(pipeline, dynamic));
    pipeline.blendColorWord = 1;
    EXPECT_FALSE(raw::usableDynamicPush(pipeline, dynamic));
    pipeline.pushMask = 255;
    EXPECT_TRUE(raw::usableDynamicPush(pipeline, dynamic));
}

}  // namespace
