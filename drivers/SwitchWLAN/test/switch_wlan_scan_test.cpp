// SwitchWLANScan.h on the host, once per IPSW kind: a network from the radio, filled as APPLE80211_IOC_SCAN_RESULT's
// record and copied into a buffer the size that kind's framework hands over, reads back at the framework's offsets.
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "../SwitchWLANScan.h"

namespace {

using swlan::a80211::fill_scan_result;
using swlan::a80211::scan_result;

#ifdef SWITCH_IPSW_KIND_24B119
constexpr size_t kRecordSize = 0x8e8, kRates = 0x24, kTimestamp = 0x8d0, kPhyMode = 0x8e0;
#else
constexpr size_t kRecordSize = 0x48c;
#endif
constexpr size_t kRateCount = 0x22, kSsidLength = 0x60, kAge = 0x84, kElementsLength = 0x8a, kElements = 0x8c;

swlan_bss network(uint32_t mhz)
{
    swlan_bss b = {};
    b.freq_mhz = mhz;
    b.bw_mhz = 20;
    b.signal_dbm = -47;
    b.beacon_int = 100;
    b.caps = 0x411;
    b.ssid_len = 4;
    memcpy(b.ssid, "home", 4);
    const uint8_t address[6] = {2, 3, 4, 5, 6, 7};
    memcpy(b.bssid, address, 6);
    const uint8_t rates[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 12, 24, 48, 108};
    memcpy(b.ie, rates, sizeof rates);
    b.ie_len = sizeof rates;
    return b;
}

std::vector<uint8_t> framework_reads(const swlan_bss &b, uint64_t continuous_ns)
{
    scan_result r;
    memset(&r, 0xa5, sizeof r);
    fill_scan_result(r, b, continuous_ns);
    std::vector<uint8_t> out(kRecordSize, 0xaa);
    memcpy(out.data(), &r, sizeof r < out.size() ? sizeof r : out.size());
    return out;
}

template <typename T> T at(const std::vector<uint8_t> &bytes, size_t offset)
{
    T value;
    memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

TEST(SwitchWLANScan, RecordAsTheFrameworkReadsIt)
{
    swlan_bss b = network(2412);
    b.age_ms = 2500;
    const std::vector<uint8_t> r = framework_reads(b, 10000000000);
    EXPECT_EQ(sizeof(scan_result), kRecordSize);
    EXPECT_EQ(at<uint32_t>(r, kAge), 2500u);
    EXPECT_EQ(at<uint32_t>(r, 0), 1u);
    EXPECT_EQ(at<uint32_t>(r, 8), 1u);
    EXPECT_EQ(at<uint32_t>(r, 12), 0xau);
    EXPECT_EQ(at<int16_t>(r, 0x16), -47);
    EXPECT_EQ(memcmp(r.data() + 0x1c, b.bssid, 6), 0);
    EXPECT_EQ(r[kSsidLength], 4);
    EXPECT_EQ(memcmp(r.data() + kSsidLength + 1, "home", 4), 0);
    EXPECT_EQ(at<uint16_t>(r, kElementsLength), b.ie_len);
    EXPECT_EQ(memcmp(r.data() + kElements, b.ie, b.ie_len), 0);
    for (size_t i = kElements + b.ie_len; i < kRecordSize; ++i) {
#ifdef SWITCH_IPSW_KIND_24B119
        if (i >= kTimestamp && i < kTimestamp + 8)
            continue;
        if (i >= kPhyMode && i < kPhyMode + 4)
            continue;
#endif
        ASSERT_EQ(r[i], 0) << "at " << i;
    }
#ifdef SWITCH_IPSW_KIND_24B119
    EXPECT_EQ(r[kRateCount], 8);
    const uint32_t mbps[] = {1, 2, 5, 11, 6, 12, 24, 54};
    for (size_t i = 0; i < 8; ++i)
        EXPECT_EQ(at<uint32_t>(r, kRates + 4 * i), mbps[i]);
#else
    EXPECT_EQ(r[kRateCount], 0);
#endif
}

#ifdef SWITCH_IPSW_KIND_24B119
void element(swlan_bss &b, uint8_t id, uint8_t length)
{
    ASSERT_LE(static_cast<size_t>(b.ie_len) + 2 + length, sizeof b.ie);
    b.ie[b.ie_len++] = id;
    b.ie[b.ie_len++] = length;
    memset(b.ie + b.ie_len, 0, length);
    b.ie_len += length;
}

TEST(SwitchWLANScan, TimestampIsWhenTheNetworkWasSeen)
{
    swlan_bss b = network(2412);
    b.age_ms = 2500;
    std::vector<uint8_t> r = framework_reads(b, 10000000000);
    EXPECT_EQ(at<uint64_t>(r, kTimestamp), 7500000000u);
    b.age_ms += 60000;
    r = framework_reads(b, 70000000000);
    EXPECT_EQ(at<uint64_t>(r, kTimestamp), 7500000000u);
}

TEST(SwitchWLANScan, PhyModeFromTheElements)
{
    swlan_bss b = network(2412);
    EXPECT_EQ(at<uint32_t>(framework_reads(b, 1), kPhyMode), 0x8u);
    b.freq_mhz = 5180;
    EXPECT_EQ(at<uint32_t>(framework_reads(b, 1), kPhyMode), 0x2u);
    element(b, 61, 22);
    EXPECT_EQ(at<uint32_t>(framework_reads(b, 1), kPhyMode), 0x12u);
    element(b, 191, 12);
    EXPECT_EQ(at<uint32_t>(framework_reads(b, 1), kPhyMode), 0x90u);
}

TEST(SwitchWLANScan, TruncatedElementAndFullRateList)
{
    swlan_bss b = network(2412);
    element(b, 50, 24);
    memset(b.ie + 12, 12, 24);
    b.ie[b.ie_len++] = 61;
    b.ie[b.ie_len++] = 22;
    b.ie[b.ie_len++] = 1;
    const std::vector<uint8_t> r = framework_reads(b, 1);
    EXPECT_EQ(r[kRateCount], 15);
    EXPECT_EQ(at<uint32_t>(r, kPhyMode), 0x8u);
}
#endif

TEST(SwitchWLANScan, ElementsBoundedByTheNetworksOwn)
{
    swlan_bss b = network(2412);
    b.ie_len = UINT16_MAX;
    const std::vector<uint8_t> r = framework_reads(b, 1);
    EXPECT_EQ(at<uint16_t>(r, kElementsLength), sizeof b.ie);
}

}  // namespace
