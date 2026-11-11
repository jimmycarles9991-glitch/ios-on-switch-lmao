// SPDX-License-Identifier: MIT
#include "../raw/pack/probe_bind.h"
#include "../raw/runtime/raw_direct_push.h"

#include <gtest/gtest.h>

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

}  // namespace
