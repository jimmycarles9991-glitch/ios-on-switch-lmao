// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin_submit_state.h"

namespace {

TEST(NvkSubmitState, RejectionDoesNotInventChannelDeath)
{
    darwin_submit_state state = {};
    darwin_submit_reject(&state, 0);
    EXPECT_EQ(state.dead, 0u);
    EXPECT_EQ(darwin_submit_blocked(&state), DARWIN_SUBMIT_REJECTED);
}

TEST(NvkSubmitState, RejectedSubmissionDropsAllLaterChunks)
{
    darwin_submit_state state = {};
    unsigned submitted = 0;
    for (unsigned chunk = 0; chunk < 8; ++chunk) {
        if (darwin_submit_blocked(&state))
            continue;
        if (chunk == 2)
            darwin_submit_reject(&state, 0);
        else
            ++submitted;
    }
    EXPECT_EQ(submitted, 2u);
    EXPECT_EQ(state.dead, 0u);
}

TEST(NvkSubmitState, FailureRetiresAfterTheAcceptedPrefix)
{
    darwin_submit_state state = {};
    darwin_submit_reject(&state, 0);
    const auto signal = darwin_submit_finish(&state, true, 41);
    EXPECT_TRUE(signal.pending);
    EXPECT_EQ(signal.seqno, 41u);
    EXPECT_EQ(signal.error, DARWIN_SUBMIT_REJECTED);
    EXPECT_EQ(darwin_submit_blocked(&state), 0u);
}

TEST(NvkSubmitState, WrappedSequenceZeroStillRequiresRetirement)
{
    darwin_submit_state state = {};
    darwin_submit_reject(&state, 0);
    const auto signal = darwin_submit_finish(&state, true, 0);
    EXPECT_TRUE(signal.pending);
    EXPECT_EQ(signal.seqno, 0u);
    EXPECT_EQ(signal.error, DARWIN_SUBMIT_REJECTED);
}

TEST(NvkSubmitState, NoAcceptedWorkCanFailImmediately)
{
    darwin_submit_state state = {};
    darwin_submit_reject(&state, 0);
    const auto signal = darwin_submit_finish(&state, false, 0);
    EXPECT_FALSE(signal.pending);
    EXPECT_EQ(signal.error, DARWIN_SUBMIT_REJECTED);
}

TEST(NvkSubmitState, FailureIsConsumedOnceWithoutReopening)
{
    darwin_submit_state state = {};
    darwin_submit_reject(&state, 0);
    EXPECT_EQ(darwin_submit_finish(&state, false, 0).error, DARWIN_SUBMIT_REJECTED);
    EXPECT_EQ(darwin_submit_finish(&state, true, 3).error, 0u);
    EXPECT_EQ(state.dead, 0u);
}

TEST(NvkSubmitState, ConfirmedDeathPersistsUntilContextReplacement)
{
    darwin_submit_state state = {};
    darwin_submit_reject(&state, 4);
    EXPECT_EQ(darwin_submit_finish(&state, true, 7).error, 4u);
    EXPECT_EQ(state.dead, 4u);
    EXPECT_EQ(darwin_submit_blocked(&state), 4u);
    EXPECT_EQ(darwin_submit_finish(&state, true, 7).error, 4u);
}

TEST(NvkSubmitState, LaterConfirmedDeathDoesNotEraseOriginalRejection)
{
    darwin_submit_state state = {};
    darwin_submit_reject(&state, 0);
    darwin_submit_reject(&state, 2);
    EXPECT_EQ(state.dead, 2u);
    EXPECT_EQ(darwin_submit_finish(&state, true, 8).error, DARWIN_SUBMIT_REJECTED);
    EXPECT_EQ(darwin_submit_blocked(&state), 2u);
}

}  // namespace
