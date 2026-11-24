#include <gtest/gtest.h>

#include "switch_bluetooth_le_filter_harness.h"

namespace {

TEST(SwitchBluetoothHostFilter, CapableControllerUsesHostForScanFiltering)
{
    const uint8_t caps[] = {0, 4, 1, 0, 0x10, 0x20, 1, 16, 1};
    switch_le_filter filter{};
    filter.apcf = h_le_capabilities(caps, sizeof(caps));
    ASSERT_FALSE(filter.apcf);

    uint8_t typeFive[54] = {0x32, 2};
    typeFive[52] = 5;
    typeFive[53] = static_cast<uint8_t>(-90);
    uint8_t sub = 0;
    EXPECT_TRUE(h_le_host_command(SWITCH_LE_OP_META, typeFive, sizeof(typeFive)));
    EXPECT_FALSE(h_le_take(&filter, SWITCH_LE_OP_META, typeFive, sizeof(typeFive), &sub));
    EXPECT_EQ(filter.ruleCount, 0u);

    const uint8_t meta[] = {0x01, 0x05, 0x07, 0x50, 0x61, 0x33, 0x1e, 0x35};
    for (uint8_t op : meta)
        EXPECT_TRUE(h_le_host_command(SWITCH_LE_OP_META, &op, 1)) << +op;
    const uint8_t enable[] = {0, 1};
    EXPECT_TRUE(h_le_host_command(SWITCH_LE_OP_APCF, enable, sizeof(enable)));
    const uint8_t other = 0x20;
    EXPECT_FALSE(h_le_host_command(SWITCH_LE_OP_META, &other, 1));
    EXPECT_FALSE(h_le_host_command(0xfd4d, enable, sizeof(enable)));
    EXPECT_FALSE(h_le_host_command(SWITCH_LE_OP_SCAN_ENABLE, enable, sizeof(enable)));
}

TEST(SwitchBluetoothHostFilter, DuplicateBypassFollowsTheClientAcrossScanRequests)
{
    switch_le_filter filter{};
    uint8_t scan[] = {1, 1};
    uint8_t bypass[] = {0x1e, 0, 0, 0, 0, 0, 0x40, 0};
    uint8_t sub = 0;

    EXPECT_FALSE(h_le_scan_enable(&filter, scan, sizeof(scan)));
    EXPECT_EQ(scan[1], 1u);
    EXPECT_FALSE(h_le_host_command(SWITCH_LE_OP_OLYMPIC, bypass, sizeof(bypass)));
    ASSERT_TRUE(h_le_take(&filter, SWITCH_LE_OP_OLYMPIC, bypass, sizeof(bypass), &sub));
    EXPECT_EQ(sub, 0x1eu);
    EXPECT_TRUE(h_le_scan_enable(&filter, scan, sizeof(scan)));
    EXPECT_EQ(scan[0], 1u);
    EXPECT_EQ(scan[1], 0u);

    bypass[6] = 0;
    ASSERT_TRUE(h_le_take(&filter, SWITCH_LE_OP_OLYMPIC, bypass, sizeof(bypass), &sub));
    scan[1] = 1;
    EXPECT_FALSE(h_le_scan_enable(&filter, scan, sizeof(scan)));
    EXPECT_EQ(scan[1], 1u);
}

}  // namespace
