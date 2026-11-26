// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <initializer_list>
#include <vector>
#include "../SwitchDPCEC.h"
#include "bazel/testing/source_text.h"

namespace {
struct Branch {
    switch_dp_cec state{};
    uint32_t wait = 0;

    switch_dp_aux_request take()
    {
        switch_dp_aux_request r{};
        EXPECT_TRUE(switch_dp_cec_next(&state, &r, &wait));
        EXPECT_EQ(wait, 0u);
        return r;
    }

    void complete(std::initializer_list<uint8_t> bytes = {0})
    {
        std::array<uint8_t, 16> data{};
        std::copy(bytes.begin(), bytes.end(), data.begin());
        switch_dp_cec_complete(&state, data.data(), true);
    }

    void enable(std::initializer_list<uint8_t> oui = {0, 0xe0, 0x4c})
    {
        switch_dp_cec_start(&state);
        EXPECT_EQ(take().address, 0x500u);
        complete(oui);
        EXPECT_EQ(take().address, SWITCH_DP_CEC_CONTROL);
        complete();
        EXPECT_EQ(take().address, SWITCH_DP_CEC_RX_INFO);
        complete();
        EXPECT_EQ(take().address, SWITCH_DP_CEC_TX_CONTROL);
        complete();
        const auto control = take();
        EXPECT_EQ(control.data[0], 0x1f);
        complete();
    }

    std::vector<uint8_t> packet(uint8_t result)
    {
        const auto data = take();
        EXPECT_EQ(data.address, SWITCH_DP_CEC_TX_END - data.size);
        EXPECT_EQ(data.op, SWITCH_DP_AUX_NATIVE_WRITE);
        complete();
        const auto control = take();
        EXPECT_EQ(control.address, SWITCH_DP_CEC_TX_CONTROL);
        EXPECT_EQ(control.data[0], (data.size - 1u) | 0x10u);
        complete();
        EXPECT_EQ(state.phase, SWITCH_DP_CEC_WAIT);
        switch_dp_aux_request pending{};
        EXPECT_FALSE(switch_dp_cec_next(&state, &pending, &wait));
        EXPECT_EQ(wait, std::max(data.size * 24u, 50u) + 1010u);
        EXPECT_FALSE(switch_dp_cec_next(&state, &pending, &wait));
        EXPECT_EQ(wait, 0u);
        state.phase = SWITCH_DP_CEC_STATUS;
        state.deadline = true;
        EXPECT_EQ(take().address, SWITCH_DP_CEC_TX_CONTROL);
        complete({result});
        return {data.data, data.data + data.size};
    }

    void claim()
    {
        EXPECT_EQ(packet(0x20), (std::vector<uint8_t>{0x44}));
        EXPECT_EQ(take().data[0], 0x14);
        complete();
    }
};

TEST(CecWake, OledAndOriginalDocksUseTheVerifiedMailbox)
{
    Branch oled;
    oled.enable();
    EXPECT_TRUE(oled.state.enabled);
    Branch original;
    original.enable({0, 0x80, 0xe1});
    EXPECT_TRUE(original.state.enabled);
    Branch other;
    switch_dp_cec_start(&other.state);
    other.take();
    other.complete({1, 0x80, 0xe1});
    EXPECT_EQ(other.state.phase, SWITCH_DP_CEC_OFF);
    EXPECT_FALSE(other.state.enabled);
}

TEST(CecWake, ClaimsAddressBeforeImageViewOnAndAdvertisesTheRealPhysicalAddress)
{
    Branch b;
    b.enable();
    b.state.physical = 0x2100;
    b.state.active = true;
    b.claim();
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x40, 0x04}));
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x4f, 0x84, 0x21, 0, 4}));
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x4f, 0x82, 0x21, 0}));
    switch_dp_aux_request request{};
    EXPECT_FALSE(switch_dp_cec_next(&b.state, &request, &b.wait));
}

TEST(CecWake, BusyPlaybackAddressesAdvanceThroughAllThreeCandidates)
{
    Branch b;
    b.enable();
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x44}));
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x88}));
    EXPECT_EQ(b.packet(0x20), (std::vector<uint8_t>{0xbb}));
    EXPECT_EQ(b.take().data[0], 0x1b);
    b.complete();
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0xb0, 4}));
}

TEST(CecWake, AnOccupiedBusNeverImpersonatesAnExistingPlayer)
{
    Branch b;
    b.enable();
    b.packet(0);
    b.packet(0);
    b.packet(0);
    EXPECT_EQ(b.state.phase, SWITCH_DP_CEC_STOP);
    EXPECT_EQ(b.take().data[0] & 0x10u, 0u);
    b.complete();
    EXPECT_EQ(b.state.phase, SWITCH_DP_CEC_OFF);
}

TEST(CecWake, LowHpdCanWakeTheTvBeforeAnEdidExists)
{
    Branch b;
    b.enable();
    b.claim();
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x40, 4}));
    switch_dp_aux_request request{};
    EXPECT_FALSE(switch_dp_cec_next(&b.state, &request, &b.wait));
    EXPECT_EQ(b.wait, 0u);
    b.state.physical = 0x1000;
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x4f, 0x84, 0x10, 0, 4}));
    EXPECT_FALSE(switch_dp_cec_next(&b.state, &request, &b.wait));
    b.state.active = true;
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x4f, 0x82, 0x10, 0}));
}

TEST(CecWake, AStillBusyTransmitterAtTheFullDeadlineStopsWithoutPolling)
{
    Branch b;
    b.enable();
    b.packet(0x10);
    EXPECT_EQ(b.state.phase, SWITCH_DP_CEC_STOP);
    b.take();
    b.complete();
    switch_dp_aux_request request{};
    EXPECT_FALSE(switch_dp_cec_next(&b.state, &request, &b.wait));
    EXPECT_EQ(b.wait, 0u);
}

TEST(CecWake, ReceiverRepliesUseTheClaimedAddressAndDontInventAVendor)
{
    Branch b;
    b.enable();
    b.claim();
    b.state.wake = SWITCH_DP_CEC_DONE;
    b.state.service = true;
    EXPECT_EQ(b.take().address, 0x201u);
    b.complete({0x40});
    const auto clear = b.take();
    EXPECT_EQ(clear.address, 0x201u);
    EXPECT_EQ(clear.data[0], 0x40);
    b.complete();
    EXPECT_EQ(b.take().address, SWITCH_DP_CEC_RX_INFO);
    b.complete({0x11});
    EXPECT_EQ(b.take().address, SWITCH_DP_CEC_RX_BUFFER);
    b.complete({0x04, 0x8f});
    EXPECT_EQ(b.take().address, SWITCH_DP_CEC_RX_INFO);
    b.complete();
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x40, 0x90, 0}));
    const uint8_t vendor[] = {0x04, 0x8c};
    switch_dp_cec_receive(&b.state, vendor, sizeof(vendor));
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x40, 0, 0x8c, 0}));
}

TEST(CecWake, FailedOptionalAuxProbeDoesNotClaimAController)
{
    Branch b;
    switch_dp_cec_start(&b.state);
    b.take();
    const uint8_t zero = 0;
    switch_dp_cec_complete(&b.state, &zero, false);
    EXPECT_EQ(b.state.phase, SWITCH_DP_CEC_OFF);
    EXPECT_FALSE(b.state.enabled);
    EXPECT_TRUE(b.state.retry_on_hpd);
}

TEST(CecWake, SuspendReleasesTheBridgeAddressBeforeStoppingAux)
{
    Branch b;
    b.enable();
    b.claim();
    b.state.physical = 0x2100;
    b.state.active = b.state.irq = b.state.service = true;
    b.state.reply_length = b.state.rx_stage = 1;
    switch_dp_cec_release(&b.state);
    const auto stop = b.take();
    EXPECT_EQ(stop.address, SWITCH_DP_CEC_CONTROL);
    EXPECT_EQ(stop.data[0] & 0x10u, 0u);
    b.complete();
    EXPECT_EQ(b.state.phase, SWITCH_DP_CEC_OFF);
    EXPECT_EQ(b.state.logical, 15);
    EXPECT_FALSE(b.state.enabled);
    EXPECT_FALSE(b.state.active);
    EXPECT_EQ(b.state.physical, SWITCH_DP_CEC_INVALID_PHYSICAL);
    switch_dp_aux_request request{};
    EXPECT_FALSE(switch_dp_cec_next(&b.state, &request, &b.wait));
}

TEST(CecWake, ANewHdmiTopologyAllocatesAgainInsteadOfKeepingTheOldAddress)
{
    Branch b;
    b.enable();
    b.claim();
    switch_dp_cec_release(&b.state);
    b.take();
    b.complete();
    b.enable();
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x44}));
    EXPECT_EQ(b.packet(0x20), (std::vector<uint8_t>{0x88}));
    EXPECT_EQ(b.take().data[0], 0x18);
    b.complete();
    EXPECT_EQ(b.state.logical, 8);
    EXPECT_EQ(b.state.physical, SWITCH_DP_CEC_INVALID_PHYSICAL);
}

TEST(CecWake, HdmiBounceClaimsANewAddressWithoutRepeatingImageViewOn)
{
    Branch b;
    b.enable();
    b.claim();
    EXPECT_EQ(b.packet(0), (std::vector<uint8_t>{0x40, 4}));
    switch_dp_cec_reconnect(&b.state);
    b.take();
    b.complete({0, 0xe0, 0x4c});
    for (int i = 0; i < 4; ++i) {
        b.take();
        b.complete();
    }
    b.claim();
    EXPECT_EQ(b.state.wake, SWITCH_DP_CEC_PHYSICAL);
    switch_dp_aux_request request{};
    EXPECT_FALSE(switch_dp_cec_next(&b.state, &request, &b.wait));
    switch_dp_cec_start(&b.state);
    EXPECT_FALSE(b.state.wake_sent);
}

TEST(CecWake, PhysicalAddressComesOnlyFromAValidHdmiVendorBlock)
{
    std::array<uint8_t, 256> edid{};
    const uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    std::copy(std::begin(header), std::end(header), edid.begin());
    edid[126] = 1;
    edid[128] = 2;
    edid[130] = 10;
    const uint8_t vendor[] = {0x65, 3, 12, 0, 0x21, 0};
    std::copy(std::begin(vendor), std::end(vendor), edid.begin() + 132);
    const auto sum = [&](unsigned block) {
        uint8_t value = 0;
        for (unsigned i = block; i < block + 127; ++i)
            value = static_cast<uint8_t>(value + edid[i]);
        edid[block + 127] = static_cast<uint8_t>(0u - value);
    };
    sum(0);
    sum(128);
    const auto physical = [&] {
        switch_dp_mode_list modes{};
        const auto original = edid;
        EXPECT_TRUE(switch_dp_edid_block_modes(edid.data(), true, &modes));
        const uint32_t valid = switch_dp_edid_block_modes(edid.data() + 128, false, &modes) ? 3u : 1u;
        EXPECT_EQ(edid, original);
        return switch_dp_cec_physical(edid.data(), valid);
    };
    EXPECT_EQ(physical(), 0x2100);
    edid[136] = 0x20;
    edid[137] = 0x10;
    sum(128);
    EXPECT_EQ(physical(), SWITCH_DP_CEC_INVALID_PHYSICAL);
    edid[136] = 0x21;
    edid[137] = 0;
    EXPECT_EQ(physical(), SWITCH_DP_CEC_INVALID_PHYSICAL);
}

TEST(CecWake, ReleaseWaitUsesBoundedDeadline)
{
    const auto source = switch_ios::test::SourceText("../SwitchDPCEC.cpp");
    const auto release = switch_ios::test::Block(source, "void SwitchDisplayPort::cecRelease()");
    EXPECT_TRUE(release.has("clock_interval_to_deadline("));
    EXPECT_TRUE(release.has("gate->commandSleep(&cecReleasePending, deadline, THREAD_UNINT)"));
    EXPECT_TRUE(release.has("THREAD_TIMED_OUT"));
}
}
