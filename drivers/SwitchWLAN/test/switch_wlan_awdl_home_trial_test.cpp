// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchWLANAwdlHomeTrial.h"

namespace {
using Trial = swlan::SwitchAwdlHomeTrial;
using Phase = Trial::Phase;
using Action = Trial::Action;
constexpr uint64_t second = 1000000000ULL;

TEST(SwitchAwdlHomeTrial, FirstCommittedEnableStartsWithoutServiceRegistration)
{
    Trial trial{};
    EXPECT_EQ(trial.advance(second, true, true, true, true), Action::None);
    trial.arm();
    EXPECT_EQ(trial.advance(second, false, true, true, true), Action::None);
    EXPECT_EQ(trial.advance(90 * second, true, false, false, false), Action::None);
    EXPECT_EQ(trial.phase, Phase::Armed);
    EXPECT_EQ(trial.deadlineNs, 0u);

    constexpr uint64_t commit = 100 * second;
    EXPECT_EQ(trial.advance(commit, true, true, true, false), Action::Request);
    EXPECT_EQ(trial.phase, Phase::Waiting);
    EXPECT_EQ(trial.deadlineNs, commit + 10 * second);
    EXPECT_EQ(trial.advance(commit, true, true, true, false), Action::Install);
    EXPECT_EQ(trial.phase, Phase::Applying);
    EXPECT_EQ(trial.advance(commit, true, true, true, false), Action::None);
    EXPECT_EQ(trial.advance(commit, true, true, true, true), Action::Start);
    EXPECT_EQ(trial.phase, Phase::Active);
    EXPECT_EQ(trial.advance(commit, true, true, true, true), Action::None);
}

TEST(SwitchAwdlHomeTrial, SixtySecondsStartAtAppliedSchedule)
{
    Trial trial{};
    trial.arm();
    ASSERT_EQ(trial.advance(0, true, true, false, false), Action::Request);
    EXPECT_EQ(trial.advance(4 * second, true, true, false, false), Action::None);
    ASSERT_EQ(trial.advance(5 * second, true, true, true, false), Action::Install);
    EXPECT_EQ(trial.advance(8 * second, true, true, true, false), Action::None);
    ASSERT_EQ(trial.advance(9 * second, true, true, true, true), Action::Start);
    EXPECT_EQ(trial.deadlineNs, 69 * second);
    EXPECT_EQ(trial.advance(69 * second - 1, true, true, false, false), Action::None);
    EXPECT_EQ(trial.advance(69 * second, true, true, true, true), Action::Finish);
    trial.cancel();
    EXPECT_EQ(trial.phase, Phase::Off);
    EXPECT_EQ(trial.advance(70 * second, true, true, true, true), Action::None);
}

TEST(SwitchAwdlHomeTrial, NeverInstallsOrStartsAfterSetupExpires)
{
    for (bool installed : {false, true}) {
        SCOPED_TRACE(installed);
        Trial trial{};
        trial.arm();
        ASSERT_EQ(trial.advance(second, true, true, false, false), Action::Request);
        if (installed)
            ASSERT_EQ(trial.advance(2 * second, true, true, true, false), Action::Install);
        EXPECT_EQ(trial.advance(11 * second - 1, true, true, false, false), Action::None);
        EXPECT_EQ(trial.advance(11 * second, true, true, true, true), Action::Timeout);
        trial.cancel();
        EXPECT_EQ(trial.advance(12 * second, true, true, true, true), Action::None);
    }
}

TEST(SwitchAwdlHomeTrial, CancellationIgnoresLateApplyAndTimerEvents)
{
    for (const Phase cancelAt : {Phase::Waiting, Phase::Applying, Phase::Active}) {
        SCOPED_TRACE(static_cast<int>(cancelAt));
        Trial trial{};
        trial.arm();
        ASSERT_EQ(trial.advance(3 * second, true, true, false, false), Action::Request);
        if (cancelAt != Phase::Waiting)
            ASSERT_EQ(trial.advance(3 * second, true, true, true, false), Action::Install);
        if (cancelAt == Phase::Active)
            ASSERT_EQ(trial.advance(3 * second, true, true, true, true), Action::Start);
        ASSERT_EQ(trial.phase, cancelAt);
        trial.cancel();
        EXPECT_EQ(trial.phase, Phase::Off);
        EXPECT_EQ(trial.advance(4 * second, true, true, true, true), Action::None);
        EXPECT_EQ(trial.advance(64 * second, true, true, true, true), Action::None);
    }
}
}
