// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchWLANStationChange.h"

namespace {
int prepared, resumed, prepareResult;

int prepare()
{
    ++prepared;
    return prepareResult;
}

void resume()
{
    ++resumed;
}

class SwitchWLANStationChange : public testing::Test {
protected:
    void SetUp() override
    {
        prepared = resumed = prepareResult = 0;
    }
};

TEST_F(SwitchWLANStationChange, AsyncScanAndNestedJoinResumeOnlyAtOutermostExit)
{
    swlan::StationChanges state(prepare, resume);
    ASSERT_EQ(state.enter(), 0);
    {
        const swlan::StationChanges::Scope join(state);
        ASSERT_TRUE(join);
        state.leave();
        {
            const swlan::StationChanges::Scope keys(state);
            ASSERT_TRUE(keys);
            EXPECT_EQ(prepared, 1);
        }
        EXPECT_EQ(resumed, 0);
    }
    EXPECT_EQ(resumed, 1);
}

TEST_F(SwitchWLANStationChange, FailureDoesNotResumeOrPreventLaterOperation)
{
    swlan::StationChanges state(prepare, resume);
    prepareResult = -1;
    {
        const swlan::StationChanges::Scope failed(state);
        EXPECT_FALSE(failed);
    }
    EXPECT_EQ(resumed, 0);
    prepareResult = 0;
    const auto failedJoin = [&] {
        const swlan::StationChanges::Scope join(state);
        EXPECT_TRUE(join);
        return false;
    };
    EXPECT_FALSE(failedJoin());
    EXPECT_EQ(prepared, 2);
    EXPECT_EQ(resumed, 1);
}

TEST_F(SwitchWLANStationChange, ChipLossDoesNotReplayOldLifetime)
{
    swlan::StationChanges state(prepare, resume);
    {
        const swlan::StationChanges::Scope join(state);
        ASSERT_TRUE(join);
        state.reset();
    }
    EXPECT_EQ(resumed, 0);
    {
        const swlan::StationChanges::Scope next(state);
        EXPECT_TRUE(next);
    }
    EXPECT_EQ(prepared, 2);
    EXPECT_EQ(resumed, 1);
}
}
