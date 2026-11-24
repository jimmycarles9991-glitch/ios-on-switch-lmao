// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include "../SwitchWLANA3Wire.h"

namespace {
TEST(SwitchWlanA3Wire, ControlReplyRequiresCorrelationAndExactSize)
{
    switch_a3_control request = {};
    request.request_id = 19;
    request.session = 7;
    request.clock_epoch = 2;
    switch_a3_reply wire = {};
    wire.magic = SWITCH_A3_REPLY_MAGIC;
    wire.version = SWITCH_A3_WIRE_VERSION;
    wire.header_bytes = wire.total_bytes = sizeof wire;
    wire.request_id = 19;
    wire.session = 7;
    wire.clock_epoch = 2;
    wire.firmware_generation = 1;
    wire.state_revision = 1;
    switch_a3_reply decoded = {};
    EXPECT_TRUE(switch_a3_reply_decode(&wire, sizeof wire, &request, &decoded));
    ++wire.request_id;
    EXPECT_FALSE(switch_a3_reply_decode(&wire, sizeof wire, &request, &decoded));
    --wire.request_id;
    ++wire.session;
    EXPECT_FALSE(switch_a3_reply_decode(&wire, sizeof wire, &request, &decoded));
    --wire.session;
    ++wire.clock_epoch;
    EXPECT_FALSE(switch_a3_reply_decode(&wire, sizeof wire, &request, &decoded));
    request.clock_epoch = 0;
    EXPECT_TRUE(switch_a3_reply_decode(&wire, sizeof wire, &request, &decoded));
    EXPECT_FALSE(switch_a3_reply_decode(&wire, sizeof wire - 1, &request, &decoded));
    EXPECT_FALSE(switch_a3_reply_decode(&request, sizeof request, &request, &decoded));
}

TEST(SwitchWlanA3Wire, ReceiveValidatesGenerationsAndRemovesFcsOnce)
{
    std::array<uint8_t, sizeof(switch_a3_raw_rx) + 1600 + 1> storage = {};
    uint8_t *bytes = storage.data() + 1;
    switch_a3_raw_rx wire = {};
    wire.magic = SWITCH_A3_RAW_RX_MAGIC;
    wire.version = SWITCH_A3_RAW_RX_VERSION;
    wire.header_bytes = sizeof wire;
    wire.frame_bytes = 1600;
    wire.flags = SWITCH_A3_RX_FCS_PRESENT;
    wire.session = 7;
    wire.clock_epoch = 3;
    wire.config_generation = 4;
    wire.tsf = UINT64_C(0x100000001);
    memcpy(bytes, &wire, sizeof wire);
    switch_a3_raw_rx decoded = {};
    size_t length = 0;
    EXPECT_TRUE(switch_a3_rx_decode(bytes, sizeof wire + 1600, 7, 3, 4, &decoded, &length));
    EXPECT_EQ(length, 1596u);
    EXPECT_EQ(decoded.tsf, wire.tsf);
    EXPECT_FALSE(switch_a3_rx_decode(bytes, sizeof wire + 1600, 8, 3, 4, &decoded, &length));
    EXPECT_FALSE(switch_a3_rx_decode(bytes, sizeof wire + 1600, 7, 4, 4, &decoded, &length));
    EXPECT_FALSE(switch_a3_rx_decode(bytes, sizeof wire + 1600, 7, 3, 5, &decoded, &length));
    EXPECT_FALSE(switch_a3_rx_decode(bytes, sizeof wire + 1599, 7, 3, 4, &decoded, &length));
    wire.tsf = 0;
    memcpy(bytes, &wire, sizeof wire);
    EXPECT_FALSE(switch_a3_rx_decode(bytes, sizeof wire + 1600, 7, 3, 4, &decoded, &length));
    wire.tsf = 1;
    wire.frame_bytes = 27;
    memcpy(bytes, &wire, sizeof wire);
    EXPECT_FALSE(switch_a3_rx_decode(bytes, sizeof wire + 27, 7, 3, 4, &decoded, &length));
    wire.frame_bytes = 28;
    wire.flags = 0x80;
    memcpy(bytes, &wire, sizeof wire);
    EXPECT_FALSE(switch_a3_rx_decode(bytes, sizeof wire + 28, 7, 3, 4, &decoded, &length));
}

TEST(SwitchWlanA3Wire, TransmitEnvelopeCannotCrossAnEpochOrContainAnInvalidFrame)
{
    std::array<uint8_t, 14 + sizeof(switch_a3_tx_header) + 1528> bytes = {};
    static const uint8_t marker[6] = SWITCH_A3_TX_MARKER_BYTES;
    memcpy(bytes.data(), marker, sizeof marker);
    bytes[12] = 0x88;
    bytes[13] = 0xb5;
    switch_a3_tx_header wire = {};
    wire.magic = SWITCH_A3_TX_MAGIC;
    wire.version = SWITCH_A3_WIRE_VERSION;
    wire.header_bytes = sizeof wire;
    wire.total_bytes = sizeof wire + 1528;
    wire.frame_bytes = 1528;
    wire.session = 2;
    wire.clock_epoch = 3;
    wire.schedule_generation = 4;
    wire.request_id = 5;
    wire.firmware_generation = 1;
    bytes[6] = 0x02;
    bytes[7] = 0x57;
    bytes[8] = static_cast<uint8_t>(wire.request_id);
    memcpy(bytes.data() + 14, &wire, sizeof wire);
    bytes[14 + sizeof wire] = 0x08;
    switch_a3_tx_header decoded = {};
    EXPECT_TRUE(switch_a3_tx_decode(bytes.data(), bytes.size(), 2, 3, &decoded));
    EXPECT_FALSE(switch_a3_tx_decode(bytes.data(), bytes.size(), 3, 3, &decoded));
    EXPECT_FALSE(switch_a3_tx_decode(bytes.data(), bytes.size(), 2, 4, &decoded));
    ++bytes[8];
    EXPECT_FALSE(switch_a3_tx_decode(bytes.data(), bytes.size(), 2, 3, &decoded));
    --bytes[8];
    EXPECT_FALSE(switch_a3_tx_decode(bytes.data(), bytes.size() - 1, 2, 3, &decoded));
    bytes[14 + sizeof wire] = 0x09;
    EXPECT_FALSE(switch_a3_tx_decode(bytes.data(), bytes.size(), 2, 3, &decoded));
    bytes[14 + sizeof wire] = 0xd4;
    EXPECT_FALSE(switch_a3_tx_decode(bytes.data(), bytes.size(), 2, 3, &decoded));
}

TEST(SwitchWlanA3Wire, CompletionRequiresAnAckValidityBit)
{
    switch_a3_tx_status wire = {};
    wire.magic = SWITCH_A3_TX_STATUS_MAGIC;
    wire.version = SWITCH_A3_WIRE_VERSION;
    wire.header_bytes = wire.total_bytes = sizeof wire;
    wire.session = 1;
    wire.clock_epoch = 2;
    wire.schedule_generation = 3;
    wire.request_id = 4;
    wire.flags = SWITCH_A3_TX_ACKED;
    switch_a3_tx_status decoded = {};
    EXPECT_FALSE(switch_a3_tx_status_decode(&wire, sizeof wire, 1, 2, 3, &decoded));
    wire.flags |= SWITCH_A3_TX_ACK_VALID;
    EXPECT_TRUE(switch_a3_tx_status_decode(&wire, sizeof wire, 1, 2, 3, &decoded));
    EXPECT_FALSE(switch_a3_tx_status_decode(&wire, sizeof wire, 1, 2, 4, &decoded));
    wire.status = 100;
    EXPECT_FALSE(switch_a3_tx_status_decode(&wire, sizeof wire, 1, 2, 3, &decoded));
}
}  // namespace
