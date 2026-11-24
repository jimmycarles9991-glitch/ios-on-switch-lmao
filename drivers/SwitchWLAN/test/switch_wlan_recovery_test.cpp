// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchWLANRecovery.h"

TEST(SwitchWLANRecovery, RequestedResetKeepsCompletionsAliveAndQuarantinesNewSubmissions)
{
    swlan::SwitchRadioRecovery recovery;
    EXPECT_TRUE(recovery.canSubmit(true, false, false));
    recovery.request();
    EXPECT_FALSE(recovery.canSubmit(true, false, false));
    EXPECT_TRUE(recovery.canService(true, false, false));
    EXPECT_FALSE(recovery.canReset(true));
    EXPECT_TRUE(recovery.canReset(false));
    recovery.reset();
    EXPECT_TRUE(recovery.canSubmit(true, false, false));
}

TEST(SwitchWLANRecovery, ExplicitFaultWakeBypassesTheIdleWatchdogDeadline)
{
    swlan::SwitchRadioRecovery recovery;
    EXPECT_FALSE(recovery.watchdogDue(true, false, false, 100, 5000));
    recovery.request();
    EXPECT_TRUE(recovery.watchdogDue(true, false, false, 100, 5000));
    EXPECT_FALSE(recovery.watchdogDue(false, false, false, 100, 5000));
    recovery.reset();
    EXPECT_TRUE(recovery.watchdogDue(true, false, false, 5000, 5000));
    EXPECT_TRUE(recovery.watchdogDue(true, true, false, 100, 5000));
    EXPECT_TRUE(recovery.watchdogDue(true, false, true, 100, 5000));
    EXPECT_FALSE(recovery.canService(true, true, false));
    EXPECT_FALSE(recovery.canService(true, false, true));
}
