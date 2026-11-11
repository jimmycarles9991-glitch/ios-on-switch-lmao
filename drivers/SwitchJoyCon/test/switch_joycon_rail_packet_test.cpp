// What the rail sends (SwitchJoyConRailPacket.h) against Horizon's own attach, as dekuNukem captured it on a Switch
// (packet_parse/c2j.txt lines 7, 9 and 11: subcommands 0x02, 0x08 0x00 and the pairing request, type 4): the frame and
// both CRCs of every packet, the 0x31-byte subcommand report, and the host record a pairing request carries.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "../SwitchJoyConRailPacket.h"

namespace {

using Bytes = std::vector<uint8_t>;

// c2j.txt line 11, whole: header (12) and output report 0x01 (0x31), the trailing seven bytes Horizon's own buffer.
const Bytes kHorizonPairing = {0x19, 0x01, 0x03, 0x38, 0x00, 0x92, 0x00, 0x31, 0x00, 0x00, 0xe2, 0xff, 0x01,
                               0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x4b, 0xb1,
                               0xf0, 0x8a, 0xbb, 0x7c, 0x00, 0x04, 0x3c, 0x4e, 0x69, 0x6e, 0x74, 0x65, 0x6e,
                               0x64, 0x6f, 0x20, 0x53, 0x77, 0x69, 0x74, 0x63, 0x68, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x68, 0x00, 0x85, 0xaf, 0xa4, 0x69, 0x2a, 0x00};
constexpr uint8_t kHorizonHost[6] = {0x7c, 0xbb, 0x8a, 0xf0, 0xb1, 0x4b};

Bytes horizonReport(uint8_t counter, uint8_t id, const Bytes &data)
{
    Bytes report(SWITCH_JOYCON_SUBCOMMAND_REPORT, 0);
    report[0] = 0x01;
    report[1] = counter;
    report[10] = id;
    std::copy(data.begin(), data.end(), report.begin() + 11);
    return report;
}

Bytes frame(const Bytes &report)
{
    Bytes packet(SWITCH_JOYCON_RAIL_HEADER + report.size());
    packet.resize(
        switch_joycon_rail_packet(packet.data(), 0x92, 0x00, report.data(), static_cast<uint16_t>(report.size())));
    return packet;
}

Bytes ours(uint8_t counter, uint8_t id, const uint8_t *data, size_t length)
{
    Bytes report(SWITCH_JOYCON_SUBCOMMAND_REPORT);
    switch_joycon_subcommand_report(report.data(), counter, id, data, length);
    return report;
}

TEST(SwitchJoyConRailPacket, FramesHorizonsPacketsWithItsCrcs)
{
    const Bytes info = frame(horizonReport(0x4, 0x02, {}));
    EXPECT_EQ(Bytes(info.begin(), info.begin() + 12),
              (Bytes{0x19, 0x01, 0x03, 0x38, 0x00, 0x92, 0x00, 0x31, 0x00, 0x00, 0x55, 0xb3}));
    const Bytes shipment = frame(horizonReport(0x5, 0x08, {0x00}));
    EXPECT_EQ(shipment[10], 0x69);
    EXPECT_EQ(shipment[11], 0xf6);
    EXPECT_EQ(frame(Bytes(kHorizonPairing.begin() + 12, kHorizonPairing.end())), kHorizonPairing);
}

TEST(SwitchJoyConRailPacket, SubcommandReportsAreHorizonsLengthWithNeutralRumble)
{
    const uint8_t off = 0;
    const Bytes report = ours(0x15, 0x08, &off, 1);
    ASSERT_EQ(report.size(), 0x31u);
    EXPECT_EQ(report[0], 0x01);
    EXPECT_EQ(report[1], 0x05);
    EXPECT_EQ(Bytes(report.begin() + 2, report.begin() + 10), (Bytes{0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40}));
    const Bytes horizon = horizonReport(0x5, 0x08, {0x00});
    EXPECT_EQ(Bytes(report.begin() + 10, report.end()), Bytes(horizon.begin() + 10, horizon.end()));
}

TEST(SwitchJoyConRailPacket, PairingRequestCarriesHorizonsHostRecord)
{
    std::array<uint8_t, SWITCH_JOYCON_PAIRING_REQUEST> request{};
    const size_t length = switch_joycon_pairing_request(request.data(), SWITCH_JOYCON_PAIR_UNLESS_PAIRED, kHorizonHost);
    ASSERT_EQ(length, 0x21u);
    const Bytes report = ours(0x6, 0x01, request.data(), length);
    // Up to and including the capability; Horizon's report goes on with its own buffer's leftovers.
    EXPECT_EQ(Bytes(report.begin() + 10, report.begin() + 42),
              Bytes(kHorizonPairing.begin() + 22, kHorizonPairing.begin() + 54));
    EXPECT_EQ(report[41], SWITCH_JOYCON_HOST_CAPABILITY);
    EXPECT_EQ(Bytes(report.begin() + 42, report.end()), Bytes(7, 0));
}

TEST(SwitchJoyConRailPacket, OnlyHostAndUnlessPairedCarryTheRecord)
{
    std::array<uint8_t, SWITCH_JOYCON_PAIRING_REQUEST> request{};
    request.fill(0xee);
    EXPECT_EQ(switch_joycon_pairing_request(request.data(), SWITCH_JOYCON_PAIR_KEY, kHorizonHost), 1u);
    EXPECT_EQ(request[0], SWITCH_JOYCON_PAIR_KEY);
    EXPECT_EQ(request[1], 0x00);
    const size_t length = switch_joycon_pairing_request(request.data(), SWITCH_JOYCON_PAIR_HOST, kHorizonHost);
    EXPECT_EQ(length, SWITCH_JOYCON_PAIRING_REQUEST);
    EXPECT_EQ(request[0], SWITCH_JOYCON_PAIR_HOST);
    EXPECT_EQ(request[1], 0x4b);
    EXPECT_EQ(request[6], 0x7c);
    EXPECT_EQ(request[30], SWITCH_JOYCON_HOST_CAPABILITY);
}

}  // namespace
