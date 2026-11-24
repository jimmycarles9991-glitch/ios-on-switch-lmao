// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"
#include "../awdl/awdl_apple80211.h"
#include "switch_wlan_awdl_role_fixture.h"

TEST(SwitchWLANAwdlRole, MatchesBuildFixture)
{
    char expected[9] = {};
    char expectedVirtual[9] = {};
    for (unsigned index = 0; index < 8; ++index) {
        expected[index] = static_cast<char>((switch_wlan_awdl_role_fixture::kBytes >> (8 * index)) & 0xffu);
        expectedVirtual[index] = static_cast<char>((switch_wlan_awdl_role_fixture::kVirtualBytes >> (8 * index)) & 0xffu);
    }
    EXPECT_STREQ(SWITCH_A80211_AWDL_ROLE, expected);
    EXPECT_STREQ(SWITCH_A80211_AWDL_VIRTUAL_ROLE, expectedVirtual);
}

TEST(SwitchWLANAwdlRole, PublishedServiceUsesBuildRole)
{
    const auto publish =
        switch_ios::test::Block(switch_ios::test::SourceText("../SwitchWLANAwdl.cpp"), "void publishService()");
    EXPECT_TRUE(publish.has("s->setProperty(\"IO80211InterfaceRole\", SWITCH_A80211_AWDL_ROLE);"));
    EXPECT_TRUE(publish.has("s->setProperty(\"IO80211VirtualInterfaceRole\", SWITCH_A80211_AWDL_VIRTUAL_ROLE);"));
}

TEST(SwitchWLANAwdlRole, MonitorAttachmentReplaysOnlyRouteBackedPeersDuringCommittedActivation)
{
    const auto source = switch_ios::test::SourceText("../SwitchWLANAwdl.cpp");
    const auto monitor = switch_ios::test::Block(source, "case 3:");
    EXPECT_TRUE(monitor.has("if (switch_a80211_awdl_active(&gFacade))"));
    EXPECT_TRUE(monitor.has("if (desiredPeer(index, current) && gPeers[index].presence.ready(current)) "
                            "a80211_awdl_peer(&gFacade, current.address, 1, current.rssi);"));
}

TEST(SwitchWLANAwdlRole, AuthoritativeEmptyChannelSetDoesNotRequestAnotherClock)
{
    const auto clock =
        switch_ios::test::Block(switch_ios::test::SourceText("../SwitchWLANAwdl.cpp"), "void requestClock()");
    EXPECT_TRUE(clock.has("if (gCaps && gCaps->requires_radio_ready && gRadioState.social_channels_valid && "
                          "!gRadioState.social_channels) return;"));
}

#if defined(SWITCH_IPSW_KIND_24B119)
TEST(SwitchWLANAwdlRole, HomeTrialConsumesCommittedActivationAfterRadioRefresh)
{
    const auto source = switch_ios::test::SourceText("../SwitchWLANAwdl.cpp");
    const auto rearm = switch_ios::test::Block(source, "void rearm()");
    const auto advance = switch_ios::test::Block(source, "void advanceHomeTrial()");
    EXPECT_TRUE(rearm.before("refreshRadio();", "advanceHomeTrial();"));
    EXPECT_TRUE(advance.has("gHomeTrial.advance(now, gFacade.enabled, gActivationCommitted, !reason, applied)"));
}

TEST(SwitchWLANAwdlRole, ParentNotificationsDoNotDriveInterfacePolicy)
{
    const auto source = switch_ios::test::SourceText("../SwitchWLANAwdl.cpp");
    const auto parent = switch_ios::test::Block(source, "int switch_wlan_awdl_vif_state(");
    const auto child = switch_ios::test::Block(source, "int awdlRequest(");
    EXPECT_FALSE(parent.has("ensureRadio("));
    EXPECT_FALSE(parent.has("a80211_awdl_request("));
    EXPECT_FALSE(child.has("switch_wlan_awdl_vif_state("));
}
#endif
