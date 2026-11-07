// SwitchBluetoothLeFilter.h against the byte layouts bluetoothd 21H461 sends (re/bt_le_filter.md: bcm_vsc.c builders)
// and AOSP's APCF commands: the capability probe, each filtering command taken into state, the three filters the
// state programs, APCF off while nothing filters, and the duplicate bypass.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "switch_bluetooth_le_filter_harness.h"

namespace {

using Bytes = std::vector<uint8_t>;

Bytes params(const switch_le_command &c)
{
    return Bytes(c.params, c.params + c.length);
}

std::vector<switch_le_command> program(const switch_le_filter &f)
{
    switch_le_command out[SWITCH_LE_MAX_COMMANDS];
    const unsigned n = h_le_program(&f, out, SWITCH_LE_MAX_COMMANDS);
    return std::vector<switch_le_command>(out, out + n);
}

bool take(switch_le_filter &f, uint16_t opcode, const Bytes &p, uint8_t *sub)
{
    return h_le_take(&f, opcode, p.data(), static_cast<uint8_t>(p.size()), sub);
}

switch_le_filter withApcf()
{
    switch_le_filter f{};
    f.apcf = true;
    return f;
}

Bytes rule(uint8_t op, uint8_t type, int8_t rssi)
{
    Bytes p(54, 0);
    p[0] = op;
    p[1] = 0x02;
    p[52] = type;
    p[53] = static_cast<uint8_t>(rssi);
    return p;
}

TEST(SwitchBluetoothLeFilter, CapabilityProbe)
{
    // status, max_adv_instance, rpa_offloading, total_scan_results u16, max_irk_list, filter_support, max_filter
    const uint8_t nexus[] = {0x00, 0x04, 0x01, 0x00, 0x10, 0x20, 0x01, 0x10, 0x01};
    EXPECT_TRUE(h_le_capabilities(nexus, sizeof(nexus)));
    const uint8_t noFilter[] = {0x00, 0x04, 0x01, 0x00, 0x10, 0x20, 0x00, 0x10, 0x01};
    EXPECT_FALSE(h_le_capabilities(noFilter, sizeof(noFilter)));
    const uint8_t tooFew[] = {0x00, 0x04, 0x01, 0x00, 0x10, 0x20, 0x01, 0x02, 0x01};
    EXPECT_FALSE(h_le_capabilities(tooFew, sizeof(tooFew)));
    const uint8_t unknown[] = {0x01};
    EXPECT_FALSE(h_le_capabilities(unknown, sizeof(unknown)));
}

TEST(SwitchBluetoothLeFilter, WithoutApcfNothingIsTaken)
{
    switch_le_filter f{};
    uint8_t sub = 0;
    EXPECT_FALSE(take(f, SWITCH_LE_OP_META, rule(0x32, 15, -70), &sub));
    EXPECT_FALSE(take(f, SWITCH_LE_OP_META, {0x05, 0x00, 0, 0, 0, 0, 0, 0, 0x02, 0x04, 0x00, 0, 0, 0}, &sub));
}

TEST(SwitchBluetoothLeFilter, NothingFilteringLeavesApcfOff)
{
    switch_le_filter f = withApcf();
    uint8_t sub = 0;
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x01, 0x27, 0x10, 0x00, 0x00}, &sub));
    EXPECT_EQ(sub, 0x01);
    const auto c = program(f);
    ASSERT_EQ(c.size(), 4u);  // disable, then each index's selection deleted
    EXPECT_EQ(params(c[0]), (Bytes{0x00, 0x00}));
    for (unsigned i = 1; i < 4; ++i)
        EXPECT_EQ(params(c[i]), (Bytes{0x01, 0x01, static_cast<uint8_t>(i)}));
}

TEST(SwitchBluetoothLeFilter, UuidsAppleRulesAndAddresses)
{
    switch_le_filter f = withApcf();
    uint8_t sub = 0;
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x05, 0x00, 0, 0, 0, 0, 0, 0, 0x02, 0x04, 0x00, 0, 0, 0}, &sub));
    EXPECT_EQ(sub, 0x05);
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x07, 0x00, 0, 0, 0, 0, 0, 0, 0x02, 0x00, 0xb2, 0xfc}, &sub));
    EXPECT_EQ(sub, 0x07);
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x07, 0x00, 0, 0, 0, 0, 0, 0, 0x02, 0x00, 0xb2, 0xfc}, &sub));
    EXPECT_EQ(f.uuidCount, 1u) << "the same UUID twice is one entry";
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, rule(0x32, 15, -70), &sub));
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, rule(0x32, 22, -90), &sub));
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, rule(0x32, 5, -90), &sub));
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x35, 0x00, 0x01, 1, 2, 3, 4, 5, 6}, &sub));
    EXPECT_EQ(sub, 0x35);

    const auto c = program(f);
    ASSERT_EQ(c.size(), 11u);
    for (const auto &command : c)
        EXPECT_EQ(command.opcode, SWITCH_LE_OP_APCF);
    EXPECT_EQ(params(c[4]), (Bytes{0x03, 0x00, 0x01, 0xb2, 0xfc, 0xff, 0xff}));
    EXPECT_EQ(params(c[5]), (Bytes{0x01, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00}));
    EXPECT_EQ(params(c[6]), (Bytes{0x06, 0x00, 0x02, 0x4c, 0x00, 0xff, 0xff}));
    EXPECT_EQ(params(c[7]), (Bytes{0x01, 0x00, 0x02, 0x20, 0x00, 0x00, 0x00, 0x00, 0xa6, 0x00}))
        << "the Apple filter at the lowest rule floor, -90";
    EXPECT_EQ(params(c[8]), (Bytes{0x02, 0x00, 0x03, 1, 2, 3, 4, 5, 6, 0x01}));
    EXPECT_EQ(params(c[9]), (Bytes{0x01, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00}));
    EXPECT_EQ(params(c[10]), (Bytes{0x00, 0x01}));

    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, rule(0x33, 22, -90), &sub));
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, rule(0x33, 5, -90), &sub));
    EXPECT_EQ(f.ruleCount, 1u);
    EXPECT_EQ(params(program(f)[7]), (Bytes{0x01, 0x00, 0x02, 0x20, 0x00, 0x00, 0x00, 0x00, 0xba, 0x00}))
        << "-70 once the -90 rules are gone";
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x1e}, &sub));
    EXPECT_EQ(f.ruleCount, 0u);
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x35, 0x02}, &sub));
    EXPECT_EQ(f.addressCount, 0u);
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, {0x07, 0x02, 0, 0, 0, 0, 0, 0, 0x02}, &sub));
    EXPECT_EQ(f.uuidCount, 0u);
    EXPECT_EQ(program(f).size(), 4u);
}

TEST(SwitchBluetoothLeFilter, UuidV2And128Bit)
{
    switch_le_filter f = withApcf();
    uint8_t sub = 0;
    Bytes v2 = {0x50, 0x00, 0, 0, 0, 0, 0, 0, 0x02, 0x00, 0x01, 0x00, 0x00};
    for (uint8_t i = 0; i < 16; ++i)
        v2.push_back(i);
    ASSERT_TRUE(take(f, SWITCH_LE_OP_META, v2, &sub));
    EXPECT_EQ(sub, 0x50);
    ASSERT_EQ(f.uuidCount, 1u);
    EXPECT_EQ(f.uuids[0].length, 16u);
    EXPECT_EQ(f.uuids[0].uuid[15], 15u);
    f.uuidFiltering = true;
    const auto c = program(f);
    EXPECT_EQ(c[4].length, 3u + 16u + 16u);
}

TEST(SwitchBluetoothLeFilter, UnknownSubOperationsStayWithTheController)
{
    switch_le_filter f = withApcf();
    uint8_t sub = 0;
    EXPECT_FALSE(take(f, SWITCH_LE_OP_META, {0x20}, &sub));
    EXPECT_FALSE(take(f, SWITCH_LE_OP_META, {0x61, 0x00}, &sub));
    EXPECT_FALSE(take(f, SWITCH_LE_OP_OLYMPIC, {0x0b, 0x00}, &sub));
    EXPECT_FALSE(take(f, 0x2006, {0x00, 0x01}, &sub));
}

TEST(SwitchBluetoothLeFilter, DuplicateBypass)
{
    switch_le_filter f = withApcf();
    uint8_t scan[2] = {0x01, 0x01};
    EXPECT_FALSE(h_le_scan_enable(&f, scan, sizeof(scan)));
    EXPECT_EQ(scan[1], 1u);
    uint8_t sub = 0;
    ASSERT_TRUE(take(f, SWITCH_LE_OP_OLYMPIC, {0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00}, &sub));
    EXPECT_EQ(sub, 0x1e);
    EXPECT_EQ(f.dupBypass, 0x400000u);
    EXPECT_TRUE(h_le_scan_enable(&f, scan, sizeof(scan)));
    EXPECT_EQ(scan[1], 0u);
}

}  // namespace
