// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <initializer_list>
#include "../SwitchWLANKeyState.h"

TEST(SwitchWLANKeys, RequiresBothFirmwareInstalls)
{
    swlan::wpa::InstalledKeys keys;
    EXPECT_FALSE(keys.ready());
    keys.record(true, true);
    EXPECT_FALSE(keys.ready());
    keys.record(false, true);
    EXPECT_TRUE(keys.ready());
    EXPECT_FALSE(keys.failed());
}

TEST(SwitchWLANKeys, RejectsPartialHandshakeAndFailedRekey)
{
    for (bool pairwise : {false, true}) {
        swlan::wpa::InstalledKeys keys;
        keys.record(!pairwise, true);
        keys.record(pairwise, false);
        EXPECT_FALSE(keys.ready());
        EXPECT_TRUE(keys.failed());

        keys = {};
        keys.record(true, true);
        keys.record(false, true);
        ASSERT_TRUE(keys.ready());
        keys.record(pairwise, false);
        EXPECT_FALSE(keys.ready());
        EXPECT_TRUE(keys.failed());
    }
}

TEST(SwitchWLANKeys, FailedHandshakeReplyCannotCompleteAssociation)
{
    swlan::wpa::InstalledKeys keys;
    keys.fail();
    keys.record(true, true);
    keys.record(false, true);
    EXPECT_FALSE(keys.ready());
    EXPECT_TRUE(keys.failed());
    keys = {};
    EXPECT_FALSE(keys.ready());
    EXPECT_FALSE(keys.failed());
}
