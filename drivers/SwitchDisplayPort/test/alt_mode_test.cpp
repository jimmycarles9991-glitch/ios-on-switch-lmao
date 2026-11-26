// SPDX-License-Identifier: GPL-2.0-only
// SwitchPDAltModeSteps.h against a model BM92T36 with a partner behind it: Nintendo's dock, USB-C DisplayPort
// adapters, a plain USB-PD charger, and dual-role sources (a Mac, powered hubs) that leave us UFP.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../SwitchPDAltModeSteps.h"

namespace {

constexpr uint16_t kInsert = SWITCH_PD_ALT_STATUS1_INSERT;
constexpr uint16_t kUfp = SWITCH_PD_ALT_DATA_ROLE_UFP << SWITCH_PD_ALT_STATUS1_DR_SHIFT;
constexpr uint16_t kDfp = SWITCH_PD_ALT_DATA_ROLE_DFP << SWITCH_PD_ALT_STATUS1_DR_SHIFT;

// ID Header VDOs (USB PD R3.3 V1.0 Table 6.34), little-endian in INCOMING_VDM bytes 5-8.
constexpr uint32_t kMacHeader = 0xD14005ACu;    // host and device, UFP PDUSB Peripheral, DFP PDUSB Host, receptacle
constexpr uint32_t kBrickHeader = 0x01C01234u;  // no USB, DFP Power Brick, receptacle

struct Partner {
    bool dock = true;
    bool identity = true;        // ACKs Discover Identity (false: NAKs it)
    bool silent = false;         // never answers Discover Identity
    uint32_t header = 0;         // an ID Header VDO other than the dock's or the adapter's, answered with a UFP VDO
    bool drd_reply = false;      // the seven-object DRD reply: UFP VDO, pad, DFP VDO
    bool dp_svid = true;
    uint8_t caps[4] = {0x45, 0x00, 0x0C, 0x00};  // receptacle, UFP_D pins C and D
    int hub_busy = 0;                            // HUB_CONTROL answers busy this many times
    bool refuse_swap = false;
    bool refuse_pin_d = false;
    bool refuse_pin_c = false;
};

std::vector<uint8_t> identityAck(uint32_t header, bool drd)
{
    std::vector<uint8_t> reply = {0, 0x41, 0x80, 0x00, 0xFF};
    const uint32_t objects[] = {header, 0, 0x12340001u, 0x01000000u, 0, 0x02000000u};
    const size_t count = drd ? 6 : 4;
    for (size_t i = 0; i < count; ++i) {
        for (unsigned b = 0; b < 4; ++b) {
            reply.push_back(static_cast<uint8_t>(objects[i] >> (8 * b)));
        }
    }
    reply[0] = static_cast<uint8_t>(reply.size() - 1);
    return reply;
}

// The controller and partner: each command becomes the alerts the chip raises, the last reply sits in INCOMING_VDM.
struct Model {
    Partner partner;
    uint16_t status1 = kInsert | kUfp;
    uint16_t dp_status = 0;
    std::vector<uint16_t> alerts;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> pending_reply;
    std::vector<uint8_t> later_reply;
    std::vector<uint16_t> commands;
    std::vector<std::vector<uint8_t>> messages;
    std::vector<uint8_t> configured;

    void reply(std::vector<uint8_t> bytes)
    {
        pending_reply = std::move(bytes);
        alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
        alerts.push_back(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
    }

    void vdm(const std::vector<uint8_t> &m)
    {
        const uint8_t command = m[1];
        if (m[3] == 0x7E && m[4] == 0x05 && m[2] == 0x00) {
            const uint8_t dock_command = m[7];
            const uint8_t busy = dock_command == 0x20 && partner.hub_busy-- > 0 ? 1 : 0;
            reply({12, 0x40, 0x00, 0x7E, 0x05, busy, 2, static_cast<uint8_t>(dock_command + 1), 0, 0, 0, 0, 0});
            return;
        }
        switch (command) {
        case 0x01:
            if (partner.silent) {
                alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            } else if (!partner.identity) {
                reply({4, 0x81, 0x80, 0x00, 0xFF});
            } else if (partner.header != 0) {
                reply(identityAck(partner.header, partner.drd_reply));
            } else if (partner.dock) {
                reply({16, 0x41, 0x80, 0x00, 0xFF, 0x7E, 0x05, 0x00, 5 << 3, 0, 0, 0, 0, 0x00, 0x01, 0x03, 0x20});
            } else {
                reply({16, 0x41, 0x80, 0x00, 0xFF, 0xD1, 0x2B, 0x00, 5 << 3, 0, 0, 0, 0, 0x00, 0x01, 0x11, 0x11});
            }
            break;
        case 0x02:
            if (partner.dp_svid) {
                reply({8, 0x42, 0x80, 0x00, 0xFF, 0x00, 0x00, 0x01, 0xFF});
            } else {
                reply({8, 0x42, 0x80, 0x00, 0xFF, 0x00, 0x00, 0x7E, 0x05});
            }
            break;
        case 0x03:
            reply({8, 0x43, 0x80, 0x01, 0xFF, 0x05, 0x00, 0x0C, 0x00});
            break;
        case 0x04:
            reply({4, 0x44, 0x81, 0x7E, 0x05});
            break;
        default:
            FAIL() << "unexpected VDM " << int(command);
        }
    }

    void command(const switch_pd_alt_out &out)
    {
        commands.push_back(out.command);
        std::vector<uint8_t> message(out.message, out.message + out.message_len);
        if (out.message_len) {
            messages.push_back(message);
        }
        switch (out.command) {
        case SWITCH_PD_ALT_CMD_SEND_VDM:
            vdm(message);
            break;
        case SWITCH_PD_ALT_CMD_ACCEPT_VDM:
            incoming = pending_reply;
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            if (!later_reply.empty()) {
                pending_reply = later_reply;
                later_reply.clear();
                alerts.push_back(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
            }
            break;
        case SWITCH_PD_ALT_CMD_DR_SWAP:
            if (partner.refuse_swap) {
                status1 = static_cast<uint16_t>((status1 & ~(7u << 4)) | (6u << 4));
            } else {
                const bool currentlyDfp = ((status1 >> 8) & 3u) == SWITCH_PD_ALT_DATA_ROLE_DFP;
                status1 = static_cast<uint16_t>((status1 & ~((7u << 4) | (3u << 8))) | (currentlyDfp ? kUfp : kDfp));
            }
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        case SWITCH_PD_ALT_CMD_DP_DISCOVER:
            incoming = {4, partner.caps[0], partner.caps[1], partner.caps[2], partner.caps[3]};
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        case SWITCH_PD_ALT_CMD_DP_CONFIGURE: {
            configured.assign(out.message + 1, out.message + 5);
            const uint8_t pin = static_cast<uint8_t>(out.message[2] | out.message[3]);
            const bool refused = (pin == SWITCH_PD_ALT_PIN_D && partner.refuse_pin_d) ||
                                 (pin == SWITCH_PD_ALT_PIN_C && partner.refuse_pin_c);
            status1 = static_cast<uint16_t>((status1 & ~(7u << 4)) | (refused ? (6u << 4) : 0u));
            if (!refused) {
                dp_status = SWITCH_PD_ALT_DP_STATUS_DP_EN;
            }
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        }
        case SWITCH_PD_ALT_CMD_DP_EXIT:
            dp_status = 0;
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        default:
            FAIL() << "unexpected command " << out.command;
        }
    }
};

struct Session {
    Model model;
    switch_pd_alt alt{};
    std::vector<std::string> lines;
    uint32_t timer_ms = 0;
    bool port_session = true;
    bool at_rest = true;
    bool dock_power = true;
    bool is_source = false;

    explicit Session(bool want_dp = true)
    {
        switch_pd_alt_init(&alt, want_dp);
    }

    void perform(switch_pd_alt_out out)
    {
        if (out.act == SWITCH_PD_ALT_READ) {
            out = switch_pd_alt_reply(&alt, model.incoming.data(), model.incoming.size());
        }
        out = switch_pd_alt_before_swap(&alt, out, model.status1);
        if (out.line) {
            lines.emplace_back(out.line);
        }
        if (out.act == SWITCH_PD_ALT_COMMAND) {
            model.command(out);
        }
        timer_ms = out.timer_ms;
    }

    void event(uint16_t alert)
    {
        const switch_pd_alt_in in{alert, model.status1, model.dp_status, port_session, at_rest, dock_power, is_source};
        const switch_pd_alt_step before = alt.step;
        const switch_pd_alt_out out = switch_pd_alt_event(&alt, &in);
        if (out.act == SWITCH_PD_ALT_NONE && out.line == nullptr && out.timer_ms == 0 && alt.step == before) {
            return;  // as switch_pd_alt_alert: nothing to perform, the step timer runs on
        }
        perform(out);
    }

    // Alerts until the chip is quiet, then the step timers (the hub's delays) until the sequence rests.
    void settle()
    {
        event(0);
        for (int guard = 0; guard < 200; ++guard) {
            if (!model.alerts.empty()) {
                const uint16_t alert = model.alerts.front();
                model.alerts.erase(model.alerts.begin());
                event(alert);
            } else if (alt.step == SWITCH_PD_ALT_HUB_POWER || alt.step == SWITCH_PD_ALT_HUB_RETRY) {
                perform(switch_pd_alt_timer(&alt, at_rest));
            } else {
                return;
            }
        }
        FAIL() << "no rest";
    }
};

TEST(AltMode, DockGetsPinCFourLanesAndItsHub)
{
    Session run;
    run.model.partner.hub_busy = 2;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.alt.dock);
    EXPECT_EQ(run.alt.pin, SWITCH_PD_ALT_PIN_C);
    EXPECT_EQ(run.alt.dp_lanes, 4);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_TAKEN);
    EXPECT_TRUE(run.alt.hub_enabled);
    EXPECT_EQ(run.model.configured, (std::vector<uint8_t>{0x06, 0x00, 0x04, 0x00}));
    EXPECT_EQ(run.model.commands.front(), SWITCH_PD_ALT_CMD_DR_SWAP);
    EXPECT_TRUE(run.lines.empty());
}

TEST(AltMode, DockSequenceIsBm92txxs)
{
    Session run;
    run.settle();
    const std::vector<std::vector<uint8_t>> expected = {
        {4, 0x01, 0x80, 0x00, 0xFF},
        {4, 0x04, 0x81, 0x7E, 0x05},
        {4, 0x06, 0x00, 0x04, 0x00},
        {8, 0x00, 0x00, 0x7E, 0x05, 0, 1, 0x16, 0x00},
        {12, 0x00, 0x00, 0x7E, 0x05, 1, 1, 0x01, 0x00, 64, 0, 0, 128},
        {8, 0x00, 0x00, 0x7E, 0x05, 1, 1, 0x20, 0x00},
    };
    EXPECT_EQ(run.model.messages, expected);
}

TEST(AltMode, DockOfferingOnlyCTakesAllFourLanes)
{
    Session run;
    run.model.partner.caps[2] = SWITCH_PD_ALT_PIN_C;
    run.settle();
    EXPECT_EQ(run.alt.pin, SWITCH_PD_ALT_PIN_C);
    EXPECT_EQ(run.alt.dp_lanes, 4);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_TAKEN);
    EXPECT_EQ(run.model.configured, (std::vector<uint8_t>{0x06, 0x00, 0x04, 0x00}));
    EXPECT_TRUE(run.alt.hub_enabled);
}

TEST(AltMode, AdapterPlugGetsPinDInTheStandardField)
{
    Session run;
    run.model.partner.dock = false;
    run.model.partner.caps[0] = 0x05;  // plug: UFP_D pins in 15:8
    run.model.partner.caps[1] = 0x0C;
    run.model.partner.caps[2] = 0x00;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_FALSE(run.alt.dock);
    EXPECT_EQ(run.alt.dp_lanes, 2);
    EXPECT_EQ(run.model.configured, (std::vector<uint8_t>{0x06, 0x08, 0x00, 0x00}));
    EXPECT_FALSE(run.alt.hub_enabled);
}

TEST(AltMode, AdapterWithoutDisplayPortFreesTheLanes)
{
    Session run;
    run.model.partner.dock = false;
    run.model.partner.dp_svid = false;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, ChargerWithoutIdentityFreesTheLanes)
{
    Session run;
    run.model.partner.identity = false;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, UfpNonDockSourceWaitsForPartner)
{
    Session run;
    run.dock_power = false;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.model.commands.empty());
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    EXPECT_TRUE(run.lines.empty());
}

TEST(AltMode, LanesArePendingUntilConfigured)
{
    Session run;
    run.event(0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_PENDING);
    Session no_dp(false);
    no_dp.event(0);
    EXPECT_EQ(no_dp.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, WithoutDisplayPortTheDockStillGetsItsHub)
{
    Session run(false);
    run.settle();
    EXPECT_TRUE(run.alt.hub_enabled);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    for (const uint16_t command : run.model.commands) {
        EXPECT_NE(command, SWITCH_PD_ALT_CMD_DP_DISCOVER);
        EXPECT_NE(command, SWITCH_PD_ALT_CMD_DP_CONFIGURE);
    }
}

TEST(AltMode, NothingStartsWhileSwitchPDUsesTheCommandRegister)
{
    Session run;
    run.at_rest = false;
    run.event(SWITCH_PD_ALT_ALERT_CONTRACT);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_IDLE);
    EXPECT_TRUE(run.model.commands.empty());
    run.at_rest = true;
    run.model.status1 |= SWITCH_PD_ALT_STATUS1_CMD_BUSY;
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_IDLE);
}

TEST(AltMode, ContractAlertPausesAndResendsTheStep)
{
    Session run;
    run.event(0);  // DR swap
    run.model.alerts.clear();
    run.model.status1 = kInsert | kUfp;  // the contract alert cut the swap short
    run.at_rest = false;
    run.event(SWITCH_PD_ALT_ALERT_CONTRACT);
    EXPECT_TRUE(run.alt.paused);
    EXPECT_EQ(run.model.commands.size(), 1u);
    run.at_rest = true;
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
    EXPECT_FALSE(run.alt.paused);
    EXPECT_EQ(run.model.commands.size(), 2u);
    EXPECT_EQ(run.model.commands.back(), SWITCH_PD_ALT_CMD_DR_SWAP);
    run.settle();
    EXPECT_EQ(run.alt.dp_lanes, 4);
}

TEST(AltMode, DetachEndsTheSessionAndFreesTheLanes)
{
    Session run;
    run.settle();
    run.model.status1 = 0;
    run.event(SWITCH_PD_ALT_ALERT_PLUGPULL);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_IDLE);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    EXPECT_FALSE(run.alt.hub_enabled);
}

TEST(AltMode, UnansweredDiscoveryFailsWithLanesFree)
{
    Session run;
    run.event(0);
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);  // swap done: Discover Identity out, no answer
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    EXPECT_EQ(run.timer_ms, SWITCH_PD_ALT_DISCOVER_RETRY_MS);
    run.model.alerts.clear();
    run.perform(switch_pd_alt_timer(&run.alt, true));
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    EXPECT_TRUE(run.alt.vdm_retried);
    EXPECT_EQ(run.timer_ms, SWITCH_PD_ALT_DISCOVER_FINAL_MS);
    run.model.alerts.clear();
    run.perform(switch_pd_alt_timer(&run.alt, true));
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_FAILED);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    unsigned sendCount = 0;
    for (const uint16_t cmd : run.model.commands) {
        if (cmd == SWITCH_PD_ALT_CMD_SEND_VDM) {
            ++sendCount;
        }
    }
    EXPECT_EQ(sendCount, 2u);
    ASSERT_EQ(run.lines.size(), 1u);
}

TEST(AltMode, UnansweredConfigureStillEnablesTheDockHub)
{
    Session run;
    run.event(0);
    for (int i = 0; i < 50 && run.alt.step != SWITCH_PD_ALT_DP_CONFIGURE; ++i) {
        const uint16_t alert = run.model.alerts.front();
        run.model.alerts.erase(run.model.alerts.begin());
        run.event(alert);
    }
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DP_CONFIGURE);
    run.model.alerts.clear();
    run.perform(switch_pd_alt_timer(&run.alt, true));
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DOCK_QUERY);
    run.settle();
    EXPECT_TRUE(run.alt.hub_enabled);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, StaleDisplayPortIsExitedWithoutAReset)
{
    Session run;
    run.model.dp_status = SWITCH_PD_ALT_DP_STATUS_DP_EN;
    run.model.status1 = kInsert | kDfp;
    run.settle();
    EXPECT_EQ(run.model.commands.front(), SWITCH_PD_ALT_CMD_DP_EXIT);
    EXPECT_EQ(run.alt.dp_lanes, 4);
}

TEST(AltMode, RefusedSwapFails)
{
    Session run;
    run.model.partner.refuse_swap = true;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_FAILED);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, BusyHubGivesUpAfterTenRetries)
{
    Session run;
    run.model.partner.hub_busy = 100;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_FALSE(run.alt.hub_enabled);
    EXPECT_EQ(run.alt.dp_lanes, 4);
}

// A dual-role source other than the dock's power signature (the session begins UFP, so the swap decision is ours).
Session ufpSource(uint32_t header)
{
    Session run;
    run.dock_power = false;
    run.model.partner.dock = false;
    run.model.partner.header = header;
    return run;
}

bool hasCommand(const Session &run, uint16_t command)
{
    for (const uint16_t sent : run.model.commands) {
        if (sent == command) {
            return true;
        }
    }
    return false;
}

TEST(SourceIdentity, PartnerSwappingUsAfterwardsRestartsAsDfp)
{
    Session run = ufpSource(kBrickHeader);
    run.settle();
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.model.commands.empty());
    run.model.status1 = static_cast<uint16_t>((run.model.status1 & ~(3u << 8)) | kDfp);
    run.event(1u << 10);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    EXPECT_FALSE(hasCommand(run, SWITCH_PD_ALT_CMD_DR_SWAP));
}

TEST(SourceIdentity, PartnerSwapWaitsWhileCommandRegisterIsBusy)
{
    Session run = ufpSource(kBrickHeader);
    run.settle();
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.model.commands.empty());
    run.model.status1 = static_cast<uint16_t>((run.model.status1 & ~(3u << 8)) | kDfp | SWITCH_PD_ALT_STATUS1_CMD_BUSY);
    run.event(1u << 10);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.model.commands.empty());
    run.model.status1 = static_cast<uint16_t>(run.model.status1 & ~SWITCH_PD_ALT_STATUS1_CMD_BUSY);
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    EXPECT_FALSE(hasCommand(run, SWITCH_PD_ALT_CMD_DR_SWAP));
    EXPECT_TRUE(hasCommand(run, SWITCH_PD_ALT_CMD_SEND_VDM));
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 2);
}

TEST(SourceIdentity, OurSwapIsNotSentOnceThePartnersMadeUsDfp)
{
    switch_pd_alt alt{};
    switch_pd_alt_init(&alt, false);
    alt.step = SWITCH_PD_ALT_DR_SWAP;
    alt.as_ufp = true;
    const switch_pd_alt_out swap = switch_pd_alt_command(SWITCH_PD_ALT_CMD_DR_SWAP, SWITCH_PD_ALT_REPLY_MS);
    const switch_pd_alt_out out = switch_pd_alt_before_swap(&alt, swap, kInsert | kDfp);
    EXPECT_EQ(out.command, SWITCH_PD_ALT_CMD_SEND_VDM);
    EXPECT_EQ(alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    EXPECT_FALSE(alt.as_ufp);
    EXPECT_EQ(switch_pd_alt_before_swap(&alt, swap, kInsert | kUfp).command, SWITCH_PD_ALT_CMD_DR_SWAP);
}

TEST(SourceIdentity, DockPowerSignatureSwapsBeforeAnyMessage)
{
    Session run;
    run.settle();
    EXPECT_EQ(run.model.commands.front(), SWITCH_PD_ALT_CMD_DR_SWAP);
    EXPECT_TRUE(run.alt.hub_enabled);
    EXPECT_TRUE(run.lines.empty());
}

TEST(SourceIdentity, IdentityResponseDecoding)
{
    const std::vector<uint8_t> mac = identityAck(kMacHeader, true);
    EXPECT_EQ(mac[0], 28);
    EXPECT_TRUE(switch_pd_alt_identity_response(mac.data(), mac.size()));
    const uint8_t request[] = {4, 0x01, 0x80, 0x00, 0xFF};
    EXPECT_FALSE(switch_pd_alt_identity_response(request, sizeof(request)));
    const uint8_t other_svid[] = {4, 0x41, 0x80, 0x01, 0xFF};
    EXPECT_FALSE(switch_pd_alt_identity_response(other_svid, sizeof(other_svid)));
    const uint8_t nak[] = {4, 0x81, 0x80, 0x00, 0xFF};
    EXPECT_TRUE(switch_pd_alt_identity_response(nak, sizeof(nak)));
}

TEST(AltMode, PinDRefusalFallsBackToPinC)
{
    Session run;
    run.dock_power = false;
    run.model.status1 = kInsert | kDfp;
    run.model.partner.dock = false;
    run.model.partner.refuse_pin_d = true;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.pin, SWITCH_PD_ALT_PIN_C);
    EXPECT_EQ(run.alt.dp_lanes, 4);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_TAKEN);
    EXPECT_EQ(run.model.configured, (std::vector<uint8_t>{0x06, 0x04, 0x00, 0x00}));
    unsigned configureCount = 0;
    for (const uint16_t cmd : run.model.commands) {
        if (cmd == SWITCH_PD_ALT_CMD_DP_CONFIGURE)
            ++configureCount;
    }
    EXPECT_EQ(configureCount, 2u);
    EXPECT_TRUE(run.lines.empty());
}

TEST(AltMode, PinDAndPinCRefusalGivesUpAfterOneRetry)
{
    Session run;
    run.dock_power = false;
    run.model.status1 = kInsert | kDfp;
    run.model.partner.dock = false;
    run.model.partner.refuse_pin_d = true;
    run.model.partner.refuse_pin_c = true;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    unsigned configureCount = 0;
    for (const uint16_t cmd : run.model.commands) {
        if (cmd == SWITCH_PD_ALT_CMD_DP_CONFIGURE)
            ++configureCount;
    }
    EXPECT_EQ(configureCount, 2u);
    EXPECT_EQ(run.lines, (std::vector<std::string>{"no DisplayPort: Configure refused"}));
}

TEST(PinChoice, MultifunctionPartnerCanRetainSuperSpeed)
{
    const uint8_t both_receptacle[] = {4, 0x45, 0x00, 0x1C, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(both_receptacle, 5, false).pin, SWITCH_PD_ALT_PIN_D);
    const uint8_t both_plug[] = {4, 0x05, 0x3C, 0x00, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(both_plug, 5, false).pin, SWITCH_PD_ALT_PIN_D);
    const uint8_t only_c[] = {4, 0x05, 0x04, 0x00, 0x00};
    const switch_pd_alt_config c = switch_pd_alt_choose_pins(only_c, 5, false);
    EXPECT_EQ(c.pin, SWITCH_PD_ALT_PIN_C);
    EXPECT_EQ(c.lanes, 4);
    const uint8_t only_e[] = {4, 0x05, 0x10, 0x00, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(only_e, 5, false).pin, 0);
}

TEST(PinChoice, DockIsReadAsAReceptacle)
{
    const uint8_t dock[] = {4, 0x05, 0x00, 0x0C, 0x00};
    const switch_pd_alt_config config = switch_pd_alt_choose_pins(dock, 5, true);
    EXPECT_EQ(config.pin, SWITCH_PD_ALT_PIN_C);
    EXPECT_EQ(config.vdo[1], 0);
    EXPECT_EQ(config.vdo[2], SWITCH_PD_ALT_PIN_C);
}

TEST(AltMode, DockRepliesUseOnlyDeclaredObjectsAndMatchTheirOrigin)
{
    const uint8_t shortHub[] = {4, 0x40, 0, 0x7E, 5, 0, 2, 0x21, 0};
    const uint8_t wrongVendor[] = {8, 0x40, 0, 0x01, 0xFF, 0, 2, 0x21, 0};
    const uint8_t request[] = {8, 0, 0, 0x7E, 5, 0, 2, 0x21, 0};
    const uint8_t wrongCommand[] = {8, 0x40, 0, 0x7E, 5, 0, 2, 0x1D, 0};
    for (auto *bytes : {shortHub, wrongVendor, request, wrongCommand}) {
        Session run;
        run.alt.step = SWITCH_PD_ALT_HUB_ENABLE;
        switch_pd_alt_reply(&run.alt, bytes, sizeof(shortHub));
        EXPECT_FALSE(run.alt.hub_enabled);
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    }
}

TEST(AltMode, DockResumeRestoresTheSignalAfterItsDisableAcknowledgement)
{
    Session run;
    run.settle();
    ASSERT_TRUE(run.alt.dock);
    EXPECT_EQ(switch_pd_alt_display_lanes(&run.alt, true), 0u);
    run.perform(switch_pd_alt_go(&run.alt, SWITCH_PD_ALT_DOCK_DP_SLEEP));
    EXPECT_EQ(switch_pd_alt_display_lanes(&run.alt, false), 0u);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_TAKEN);
    EXPECT_TRUE(run.alt.hub_enabled);
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(switch_pd_alt_display_lanes(&run.alt, false), 4u);
    EXPECT_TRUE(run.alt.hub_enabled);
    ASSERT_GE(run.model.messages.size(), 2u);
    const auto count = run.model.messages.size();
    EXPECT_EQ(run.model.messages[count - 2], (std::vector<uint8_t>{8, 0, 0, 0x7E, 5, 1, 1, 0x1C, 0}));
    EXPECT_EQ(run.model.messages[count - 1], (std::vector<uint8_t>{8, 0, 0, 0x7E, 5, 0, 1, 0x1C, 0}));
}

TEST(PinChoice, AWrongCountIsRefused)
{
    const uint8_t caps[] = {8, 0x45, 0x00, 0x0C, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(caps, 5, false).pin, 0);
}

TEST(HostIdentity, MacIdentifiedAsDfpInSinkYieldsDataRoleToUfp)
{
    Session run;
    run.dock_power = false;
    run.is_source = false;
    run.model.status1 = kInsert | kDfp;
    run.model.partner.dock = false;
    run.model.partner.header = kMacHeader;
    run.model.partner.drd_reply = true;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.alt.partner_is_usb_host);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    EXPECT_EQ(run.alt.dp_lanes, 0u);
    EXPECT_EQ(switch_pd_alt_data_role(run.model.status1), SWITCH_PD_ALT_DATA_ROLE_UFP);
    EXPECT_TRUE(hasCommand(run, SWITCH_PD_ALT_CMD_DR_SWAP));
    ASSERT_FALSE(run.lines.empty());
    EXPECT_EQ(run.lines.back(), "yielded data role to USB host");
}

TEST(HostIdentity, MacIdentifiedWhileSourcingFinishesWithoutDrSwap)
{
    Session run;
    run.dock_power = false;
    run.is_source = true;
    run.model.status1 = kInsert | kDfp;
    run.model.partner.dock = false;
    run.model.partner.header = kMacHeader;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.alt.partner_is_usb_host);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    EXPECT_FALSE(hasCommand(run, SWITCH_PD_ALT_CMD_DR_SWAP));
}

TEST(HostIdentity, BeforeSwapKeepsDrSwapToUfpIntactAndSkipsIfAlreadyUfp)
{
    switch_pd_alt alt{};
    switch_pd_alt_init(&alt, true);
    alt.step = SWITCH_PD_ALT_DR_SWAP_TO_UFP;
    const switch_pd_alt_out swap = switch_pd_alt_command(SWITCH_PD_ALT_CMD_DR_SWAP, SWITCH_PD_ALT_REPLY_MS);
    const switch_pd_alt_out dfp = switch_pd_alt_before_swap(&alt, swap, kInsert | kDfp);
    EXPECT_EQ(dfp.act, SWITCH_PD_ALT_COMMAND);
    EXPECT_EQ(dfp.command, SWITCH_PD_ALT_CMD_DR_SWAP);
    EXPECT_EQ(alt.step, SWITCH_PD_ALT_DR_SWAP_TO_UFP);
    const switch_pd_alt_out ufp = switch_pd_alt_before_swap(&alt, swap, kInsert | kUfp);
    EXPECT_EQ(ufp.act, SWITCH_PD_ALT_NONE);
    EXPECT_EQ(alt.step, SWITCH_PD_ALT_DONE);
}

TEST(AltMode, PrSwapAlertPausesActiveVdmAndResendsAtRest)
{
    Session run;
    run.dock_power = false;
    run.model.status1 = kInsert | kDfp;
    run.model.partner.dock = false;
    run.event(0);  // sends Discover Identity
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    run.model.alerts.clear();
    run.at_rest = false;
    run.event(SWITCH_PD_ALT_ALERT_PR_SWAP);
    EXPECT_TRUE(run.alt.paused);
    const size_t sentBefore = run.model.commands.size();
    run.at_rest = true;
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
    EXPECT_FALSE(run.alt.paused);
    EXPECT_EQ(run.model.commands.size(), sentBefore + 1);
    EXPECT_EQ(run.model.commands.back(), SWITCH_PD_ALT_CMD_SEND_VDM);
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 2u);
}

TEST(AltMode, VdmAbortedRetriesOnceAndOtherErrorsFailImmediately)
{
    {
        Session run;
        run.dock_power = false;
        run.model.status1 = kInsert | kDfp;
        run.model.partner.dock = false;
        run.event(0);  // Discover Identity
        run.model.alerts.clear();
        const size_t cmds = run.model.commands.size();
        run.model.status1 = static_cast<uint16_t>(kInsert | kDfp | (2u << 4));  // LASTCMD_ABORTED
        run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
        EXPECT_TRUE(run.alt.paused);
        EXPECT_TRUE(run.alt.vdm_retried);
        EXPECT_EQ(run.timer_ms, 100u);
        EXPECT_EQ(run.model.commands.size(), cmds);
        run.model.status1 = kInsert | kDfp;
        run.perform(switch_pd_alt_timer(&run.alt, true));
        EXPECT_FALSE(run.alt.paused);
        EXPECT_EQ(run.model.commands.size(), cmds + 1);
        run.settle();
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        EXPECT_EQ(run.alt.dp_lanes, 2u);
    }
    {
        Session run;
        run.dock_power = false;
        run.model.status1 = kInsert | kDfp;
        run.model.partner.dock = false;
        run.event(0);
        run.model.alerts.clear();
        run.model.status1 = static_cast<uint16_t>(kInsert | kDfp | (6u << 4));  // rejected
        run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_FAILED);
        EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    }
    {
        Session run;
        run.dock_power = false;
        run.model.status1 = kInsert | kDfp;
        run.model.partner.dock = false;
        run.event(0);
        run.model.alerts.clear();
        run.event(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);  // sends ACCEPT_VDM
        ASSERT_TRUE(run.alt.accepted);
        run.model.alerts.clear();
        run.model.status1 = static_cast<uint16_t>(kInsert | kDfp | (6u << 4));
        run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_FAILED);
    }
}

TEST(AltMode, UnsolicitedVdmBeforePortSessionIsDrainedBeforeDiscovery)
{
    Session run;
    run.dock_power = false;
    run.model.partner.dock = false;
    run.model.status1 = kInsert | kDfp;
    run.model.pending_reply = {4, 0x01, 0x80, 0x00, 0xFF};
    run.port_session = false;
    run.at_rest = false;
    run.event(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_IDLE);
    EXPECT_TRUE(run.alt.vdm_pending);
    EXPECT_TRUE(run.model.commands.empty());
    run.event(SWITCH_PD_ALT_ALERT_PR_SWAP);
    EXPECT_TRUE(run.alt.vdm_pending);

    run.port_session = true;
    run.at_rest = true;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 2u);
    ASSERT_GE(run.model.commands.size(), 2u);
    EXPECT_EQ(run.model.commands[0], SWITCH_PD_ALT_CMD_ACCEPT_VDM);
    EXPECT_EQ(run.model.commands[1], SWITCH_PD_ALT_CMD_SEND_VDM);
}

TEST(AltMode, DrainVdmErrorAbortAndTimeoutStillConfigureDisplayPort)
{
    {
        Session run;
        run.dock_power = false;
        run.model.partner.dock = false;
        run.model.status1 = kInsert | kDfp;
        run.port_session = false;
        run.event(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
        run.port_session = true;
        run.event(0);
        ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DRAIN_VDM);
        run.model.alerts.clear();
        run.model.status1 = static_cast<uint16_t>(kInsert | kDfp | (6u << 4));
        run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
        run.model.status1 = kInsert | kDfp;
        run.settle();
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        EXPECT_EQ(run.alt.dp_lanes, 2u);
    }
    {
        Session run;
        run.dock_power = false;
        run.model.partner.dock = false;
        run.model.status1 = kInsert | kDfp;
        run.model.pending_reply = {4, 0x01, 0x80, 0x00, 0xFF};
        run.port_session = false;
        run.event(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
        run.port_session = true;
        run.event(0);
        ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DRAIN_VDM);
        run.model.alerts.clear();
        run.model.status1 = static_cast<uint16_t>(kInsert | kDfp | (2u << 4));
        run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DRAIN_VDM);
        EXPECT_TRUE(run.alt.paused);
        EXPECT_EQ(run.timer_ms, 100u);
        run.model.status1 = kInsert | kDfp;
        run.perform(switch_pd_alt_timer(&run.alt, true));
        EXPECT_FALSE(run.alt.paused);
        EXPECT_EQ(run.model.commands.back(), SWITCH_PD_ALT_CMD_ACCEPT_VDM);
        run.settle();
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        EXPECT_EQ(run.alt.dp_lanes, 2u);
    }
    {
        Session run;
        run.dock_power = false;
        run.model.partner.dock = false;
        run.model.status1 = kInsert | kDfp;
        run.port_session = false;
        run.event(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
        run.port_session = true;
        run.event(0);
        ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DRAIN_VDM);
        EXPECT_EQ(run.timer_ms, SWITCH_PD_ALT_DRAIN_MS);
        run.model.alerts.clear();
        run.perform(switch_pd_alt_timer(&run.alt, true));
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
        run.settle();
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        EXPECT_EQ(run.alt.dp_lanes, 2u);
    }
}

TEST(AltMode, DrainVdmInDonePreservesUfpAndAllowsPartnerDrSwap)
{
    {
        Session run = ufpSource(kBrickHeader);
        run.settle();
        ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        ASSERT_TRUE(run.alt.as_ufp);
        run.model.pending_reply = {4, 0x01, 0x80, 0x00, 0xFF};
        run.event(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
        ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DRAIN_VDM);
        run.settle();
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        EXPECT_TRUE(run.alt.as_ufp);
        EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);

        run.model.status1 = kInsert | kDfp;
        run.event(1u << 10);
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
        run.settle();
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        EXPECT_EQ(run.alt.dp_lanes, 2u);
    }
    {
        Session run = ufpSource(kBrickHeader);
        run.settle();
        ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        run.model.pending_reply = {4, 0x01, 0x80, 0x00, 0xFF};
        run.model.status1 = kInsert | kDfp;
        run.event(static_cast<uint16_t>((1u << 10) | SWITCH_PD_ALT_ALERT_VDM_RECEIVED));
        ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DRAIN_VDM);
        EXPECT_EQ(run.model.commands[0], SWITCH_PD_ALT_CMD_ACCEPT_VDM);
        run.settle();
        EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
        EXPECT_EQ(run.alt.dp_lanes, 2u);
        ASSERT_FALSE(run.lines.empty());
        EXPECT_EQ(run.lines.front(), "the partner swapped the data roles: Discover Identity as DFP");
    }
}

TEST(AltMode, DiscoverIdentityRetriesOnceAfterHalfSecondAndSucceeds)
{
    Session run;
    run.dock_power = false;
    run.model.partner.dock = false;
    run.model.partner.silent = true;
    run.model.status1 = kInsert | kDfp;
    run.event(0);
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    EXPECT_EQ(run.timer_ms, SWITCH_PD_ALT_DISCOVER_RETRY_MS);
    run.settle();
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    run.model.partner.silent = false;
    run.perform(switch_pd_alt_timer(&run.alt, true));
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    EXPECT_TRUE(run.alt.vdm_retried);
    EXPECT_EQ(run.timer_ms, SWITCH_PD_ALT_DISCOVER_FINAL_MS);
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 2u);
}

TEST(AltMode, UnsolicitedNonResponseVdmDuringDiscoverIdRetriesAndSucceeds)
{
    Session run;
    run.dock_power = false;
    run.model.partner.dock = false;
    run.model.status1 = kInsert | kDfp;
    run.event(0);
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DISCOVER_ID);
    run.model.pending_reply = {4, 0x01, 0x80, 0x00, 0xFF};
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 2u);
    unsigned discoverCount = 0;
    for (const auto &msg : run.model.messages) {
        if (msg == (std::vector<uint8_t>{4, 0x01, 0x80, 0x00, 0xFF})) {
            ++discoverCount;
        }
    }
    EXPECT_EQ(discoverCount, 2u);
}

}  // namespace
