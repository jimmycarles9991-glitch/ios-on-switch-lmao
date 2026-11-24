// Wi-Fi/Bluetooth coexistence state on the host: drivers/SwitchBluetooth/SwitchCoex.h and
// drivers/SwitchWLAN/SwitchWLANCoex.h, compiled as C (switch_coex_harness.c).
//
// The Bluetooth side is fed bluetoothd's commands as r7v (11-05 12:33-12:38) sent them: the LE scan's parameters as
// bluetoothd moved them (30/300 ms, 30/40 ms, 30/30 ms), the page scan, and Broadcom's Set ACL Priority (0xFC57) around
// the AirPods' A2DP start at 12:36:27. The Wi-Fi side gets wifid's Bluetooth flags (APPLE80211_IOC_BT_COEX_FLAGS bits,
// from wifid 21H461), sta_info replies laid out as bcmdhd's sta_info_t version 4, the firmware's coexistence state and
// the NVRAM file the driver loads.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <tuple>
#include <vector>

#include "bazel/testing/source_text.h"
#include "switch_coex_harness.h"

namespace {

using Bytes = std::vector<uint8_t>;
constexpr unsigned kLeParams = 1, kLeEnable = 2, kClassicScan = 4, kPriority = 8;

void put16(Bytes &b, size_t at, unsigned v)
{
    b[at] = (uint8_t)v;
    b[at + 1] = (uint8_t)(v >> 8);
}

void put32(Bytes &b, size_t at, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        b[at + i] = (uint8_t)(v >> (8 * i));
}

Bytes le16s(std::initializer_list<unsigned> values)
{
    Bytes b;
    for (unsigned v : values)
        b.insert(b.end(), {(uint8_t)v, (uint8_t)(v >> 8)});
    return b;
}

Bytes hex(const std::string &text)
{
    Bytes b;
    for (size_t i = 0; i < text.size(); i += 2)
        b.push_back((uint8_t)std::stoul(text.substr(i, 2), nullptr, 16));
    return b;
}

Bytes cat(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes scanParams(unsigned interval, unsigned window, uint8_t kind = 0)
{
    return cat(cat({kind}, le16s({interval, window})), {0x00, 0x01});
}

struct StaInfo {
    unsigned ver = 4, length = 200;
    uint32_t tx = 0, failed = 0, ucast = 0, mcast = 0, txRate = 0, rxRate = 0, decryptFailed = 0, idle = 0;
    uint32_t retried = 0, exhausted = 0;
};

// A sta_info_t as the firmware lays it out (natural alignment), the fields SwitchWLANCoex.h reads filled.
Bytes staInfo(const StaInfo &s)
{
    Bytes b(200, 0);
    put16(b, 0, s.ver);
    put16(b, 2, s.length);
    put32(b, 8, 0x10);  // flags (associated)
    put32(b, 12, s.idle);
    const uint32_t middle[] = {s.tx, s.failed, s.ucast, s.mcast, s.txRate, s.rxRate, 1000, s.decryptFailed};
    for (size_t i = 0; i < 8; ++i)
        put32(b, 52 + 4 * i, middle[i]);
    put32(b, 160, s.retried);
    put32(b, 164, s.exhausted);
    return b;
}

class Coex : public testing::Test {
protected:
    void SetUp() override
    {
        coex_reset();
        sta_reset();
    }

    static unsigned command(uint16_t opcode, const Bytes &params)
    {
        return coex_command(opcode, params.data(), (uint32_t)params.size());
    }

    static std::string describe(size_t size = 200)
    {
        std::vector<char> out(size, 0);
        const int n = coex_describe(out.data(), size);
        EXPECT_LT(n, (int)size);
        return out.data();
    }

    static std::array<uint32_t, 8> state()
    {
        std::array<uint32_t, 8> out{};
        coex_state(out.data());
        return out;
    }

    static std::string flags(uint32_t value)
    {
        char out[48] = {};
        bt_flags_describe(value, out, sizeof(out));
        return out;
    }

    static int sta(const Bytes &buffer)
    {
        return sta_parse(buffer.data(), buffer.size());
    }

    static std::string staLine()
    {
        char out[240] = {};
        sta_describe(out, sizeof(out));
        return out;
    }

    static std::array<uint32_t, 13> fields()
    {
        std::array<uint32_t, 13> out{};
        sta_fields(out.data());
        return out;
    }

    struct Afh {
        unsigned usable;
        std::array<unsigned, 2> range;
        std::array<uint8_t, 10> map;

        bool bit(unsigned n) const
        {
            return map[n / 8] >> (n % 8) & 1;
        }
    };

    static Afh afh(uint32_t mhz)
    {
        Afh a{};
        a.usable = afh_map(mhz, a.map.data(), a.range.data());
        return a;
    }
};

// --- Bluetooth: the LE scan ----------------------------------------------------------------------------------------
TEST_F(Coex, R7vLeScanDutyCycles)
{
    EXPECT_EQ(describe(), "LE scan off, unset");
    // 12:33:33.848: LE Set Scan Parameters [00 e001 3000 00 01], then enable [01 01]
    EXPECT_EQ(command(0x200b, hex("00e00130000100")), kLeParams);
    EXPECT_EQ(describe(), "LE scan off (30/300 ms)");
    EXPECT_EQ(command(0x200c, {0x01, 0x01}), kLeEnable);
    EXPECT_EQ(describe(), "LE scan on 30/300 ms passive (10%)");
    // bluetoothd's 30/40 ms (12:34:14) and 30/30 ms (12:33:54, WirelessProximity type 8)
    EXPECT_EQ(command(0x200b, scanParams(64, 48)), kLeParams);
    EXPECT_EQ(describe(), "LE scan on 30/40 ms passive (75%)");
    EXPECT_EQ(command(0x200b, scanParams(48, 48, 1)), kLeParams);
    EXPECT_EQ(describe(), "LE scan on 30/30 ms active (100%)");
    // the same parameters again change nothing; off and on are LE_ENABLE only
    EXPECT_EQ(command(0x200b, scanParams(48, 48, 1)), 0u);
    EXPECT_EQ(command(0x200c, {0x00, 0x01}), kLeEnable);
    EXPECT_EQ(describe(), "LE scan off (30/30 ms)");
    EXPECT_EQ(state()[7], 5u);
}

TEST_F(Coex, ExtendedScanAndShortCommands)
{
    // own address, filter, PHYs (1M), then {type 0, interval 0x01e0, window 0x0030}
    EXPECT_EQ(command(0x2041, hex("00000100e0013000")), kLeParams);
    EXPECT_EQ(command(0x2042, hex("010000000000")), kLeEnable);
    EXPECT_EQ(describe(), "LE scan on 30/300 ms passive (10%)");
    EXPECT_EQ(command(0x200b, {0x00, 0xe0, 0x01}), 0u);  // too short: ignored
    EXPECT_EQ(command(0x0c03, {}), 0u);                  // not followed
    EXPECT_EQ(command(0xfc57, {0x0b, 0x00}), 0u);
}

TEST_F(Coex, LeScanShapingWhileAwdlActive)
{
    auto u16 = [](const Bytes &b, size_t at) {
        return (unsigned)(b[at] | (unsigned)b[at + 1] << 8);
    };

    Bytes p75 = scanParams(64, 48, 0);
    EXPECT_EQ(coex_le_scan_shape(0, 0x200b, p75.data(), (uint32_t)p75.size()), 0);
    EXPECT_EQ(u16(p75, 1), 64u);
    EXPECT_EQ(u16(p75, 3), 48u);

    EXPECT_EQ(coex_le_scan_shape(1, 0x200b, p75.data(), (uint32_t)p75.size()), 1);
    EXPECT_EQ(u16(p75, 1), 160u);
    EXPECT_EQ(u16(p75, 3), 16u);
    EXPECT_EQ(p75[5], 0x00);
    EXPECT_EQ(p75[6], 0x01);

    Bytes p10 = scanParams(480, 48, 0);
    EXPECT_EQ(coex_le_scan_shape(1, 0x200b, p10.data(), (uint32_t)p10.size()), 1);
    EXPECT_EQ(u16(p10, 1), 160u);
    EXPECT_EQ(u16(p10, 3), 16u);

    Bytes p3 = scanParams(1544, 48, 0);
    EXPECT_EQ(coex_le_scan_shape(1, 0x200b, p3.data(), (uint32_t)p3.size()), 1);
    EXPECT_EQ(u16(p3, 1), 514u);
    EXPECT_EQ(u16(p3, 3), 16u);

    Bytes active = scanParams(48, 48, 1);
    EXPECT_EQ(coex_le_scan_shape(1, 0x200b, active.data(), (uint32_t)active.size()), 1);
    EXPECT_EQ(u16(active, 1), 80u);
    EXPECT_EQ(u16(active, 3), 16u);

    Bytes already = scanParams(160, 8, 0);
    EXPECT_EQ(coex_le_scan_shape(1, 0x200b, already.data(), (uint32_t)already.size()), 0);
    EXPECT_EQ(u16(already, 1), 160u);
    EXPECT_EQ(u16(already, 3), 8u);

    Bytes smallDuty = scanParams(32, 16, 0);
    EXPECT_EQ(coex_le_scan_shape(1, 0x200b, smallDuty.data(), (uint32_t)smallDuty.size()), 1);
    EXPECT_EQ(u16(smallDuty, 1), 160u);
    EXPECT_EQ(u16(smallDuty, 3), 16u);

    Bytes ext = hex("00000500400030000130003000");
    EXPECT_EQ(coex_le_scan_shape(1, 0x2041, ext.data(), (uint32_t)ext.size()), 1);
    EXPECT_EQ(u16(ext, 4), 160u);
    EXPECT_EQ(u16(ext, 6), 16u);
    EXPECT_EQ(u16(ext, 9), 80u);
    EXPECT_EQ(u16(ext, 11), 16u);

    Bytes shortCmd = {0x00, 0x40, 0x00};
    EXPECT_EQ(coex_le_scan_shape(1, 0x200b, shortCmd.data(), (uint32_t)shortCmd.size()), 0);
    EXPECT_EQ(coex_le_scan_shape(1, 0x200c, p75.data(), (uint32_t)p75.size()), 0);
}

// --- Bluetooth: classic scans --------------------------------------------------------------------------------------
TEST_F(Coex, PageAndInquiryScan)
{
    // 12:33:26.741 Write Page Scan Activity interval 2048 window 18, then Write Scan Enable 0x02
    EXPECT_EQ(command(0x0c1c, le16s({2048, 18})), kClassicScan);
    EXPECT_EQ(command(0x0c1a, {0x02}), kClassicScan);
    EXPECT_EQ(describe(), "LE scan off, unset, page scan 11/1280 ms");
    EXPECT_EQ(command(0x0c1e, le16s({4096, 18})), kClassicScan);
    EXPECT_EQ(command(0x0c1a, {0x03}), kClassicScan);
    EXPECT_EQ(describe(), "LE scan off, unset, page scan 11/1280 ms, inquiry scan 11/2560 ms");
}

// --- Bluetooth: Broadcom's Set ACL Priority around A2DP ------------------------------------------------------------
TEST_F(Coex, AclPriorityFollowsA2dp)
{
    // 12:36:27.147 fc57 [0b 00 01]: the AirPods' link high; 12:36:31.461 the same link back to normal
    EXPECT_EQ(command(0xfc57, hex("0b0001")), kPriority);
    EXPECT_EQ(state()[5], 1u);
    EXPECT_EQ(state()[6], 0x000bu);
    EXPECT_NE(describe().find("ACL 000b high priority"), std::string::npos);
    EXPECT_EQ(command(0xfc57, hex("0c0000")), 0u);  // another link going normal: no change
    EXPECT_EQ(command(0xfc57, hex("0b00ff")), 0u);  // 0xFF (AOSP's high) is still high
    EXPECT_EQ(command(0xfc57, hex("0b0000")), kPriority);
    EXPECT_EQ(state()[5], 0u);
    EXPECT_EQ(state()[6], 0xffffu);
    EXPECT_EQ(describe().find("priority"), std::string::npos);
}

TEST_F(Coex, DescribeNeverOverruns)
{
    command(0x200b, scanParams(64, 48));
    command(0x200c, {0x01, 0x00});
    command(0x0c1c, le16s({2048, 18}));
    command(0x0c1a, {0x03});
    command(0xfc57, hex("0b0001"));
    const std::string full = describe();
    for (size_t size : {(size_t)1, (size_t)2, (size_t)10, (size_t)33, full.size(), full.size() + 1}) {
        std::vector<char> out(size + 8, (char)0xaa);
        const int n = coex_describe(out.data(), size);
        EXPECT_LT(n, (int)size) << "size " << size;
        for (size_t i = size; i < size + 8; ++i)
            EXPECT_EQ((uint8_t)out[i], 0xaa) << "size " << size << " byte " << i;
        const std::string got(out.data(), strnlen(out.data(), out.size()));
        EXPECT_EQ(full.compare(0, got.size(), got), 0) << "size " << size << ": " << got;
    }
}

// --- Wi-Fi: wifid's flags ------------------------------------------------------------------------------------------
TEST_F(Coex, WifidFlags)
{
    EXPECT_EQ(flags(0), "none");
    EXPECT_EQ(flags(2), "A2DP");  // "Bluetooth A2DP device connected."
    EXPECT_EQ(flags(0xa), "A2DP+paging");
    EXPECT_EQ(flags(0x5), "SCO+inquiry");
    EXPECT_EQ(flags(0x12), "A2DP+0x10");
}

// --- Wi-Fi: the access point's sta_info ----------------------------------------------------------------------------
TEST_F(Coex, StaInfoFieldsAndWindow)
{
    StaInfo first;
    first.tx = 100, first.failed = 1, first.ucast = 80, first.mcast = 20, first.txRate = 65000, first.rxRate = 39000;
    first.retried = 5, first.exhausted = 1;
    EXPECT_TRUE(sta(staInfo(first)));
    EXPECT_EQ(fields(), (std::array<uint32_t, 13>{4, 200, 7, 100, 1, 80, 20, 65000, 39000, 0, 0, 5, 1}));
    // the next window: 22 sent and acknowledged, nothing unicast came in, broadcasts did (r7v 12:36:31)
    StaInfo next;
    next.tx = 122, next.failed = 1, next.ucast = 80, next.mcast = 26, next.txRate = 65000, next.rxRate = 1000;
    next.idle = 5, next.retried = 9, next.exhausted = 1;
    EXPECT_TRUE(sta(staInfo(next)));
    EXPECT_EQ(staLine(), "AP: tx 22 failed 0 retried 4 exhausted 0, rx unicast 0 multicast 6 decrypt-failed 0, rates "
                         "65/1 Mb/s, idle 5 s");
}

TEST_F(Coex, StaInfoVersionsAndLengths)
{
    EXPECT_FALSE(sta({0x04, 0x00}));  // too short
    StaInfo v2;
    v2.ver = 2, v2.length = 60;
    EXPECT_TRUE(sta(staInfo(v2)));
    EXPECT_EQ(staLine(), "");  // no version 3 fields: nothing to say
    StaInfo v3;
    v3.ver = 3, v3.length = 84, v3.tx = 7, v3.ucast = 3;
    EXPECT_TRUE(sta(staInfo(v3)));
    EXPECT_EQ(fields()[2], 1u);  // version 3 only, no retry counters
    EXPECT_EQ(staLine().rfind("AP: tx 7 failed 0, rx unicast 3 ", 0), 0u) << staLine();
    // a version 4 reply cut short by the firmware's length: the retry counters are past it
    StaInfo cut;
    cut.ver = 4, cut.length = 100, cut.retried = 9;
    EXPECT_TRUE(sta(staInfo(cut)));
    EXPECT_EQ(fields()[2], 3u);
    EXPECT_EQ(fields()[11], 0u);
}

// --- AFH: the Wi-Fi channel's range marked bad ---------------------------------------------------------------------
TEST_F(Coex, AfhMapForWifiChannels)
{
    const Afh six = afh(2437);  // r7v: channel 6
    EXPECT_EQ(six.usable, 56u);
    EXPECT_EQ(six.range, (std::array<unsigned, 2>{24, 46}));
    std::vector<unsigned> unset, want;
    for (unsigned n = 0; n < 80; ++n)
        if (!six.bit(n))
            unset.push_back(n);
    for (unsigned n = 24; n <= 46; ++n)
        want.push_back(n);
    want.push_back(79);
    EXPECT_EQ(unset, want);
    EXPECT_EQ(afh(2412).usable, 57u);  // channel 1 clamps at Bluetooth channel 0
    EXPECT_EQ(afh(2412).range, (std::array<unsigned, 2>{0, 21}));
    EXPECT_EQ(afh(2472).usable, 59u);  // channel 13 at 78
    EXPECT_EQ(afh(2472).range, (std::array<unsigned, 2>{59, 78}));
    unsigned fewest = 79;
    for (uint32_t f = 2412; f < 2473; f += 5)
        fewest = std::min(fewest, afh(f).usable);
    EXPECT_GE(fewest, 20u);      // the spec's minimum
    const Afh five = afh(5180);  // 5 GHz: every channel usable, bit 79 reserved 0
    EXPECT_EQ(five.usable, 79u);
    EXPECT_EQ(five.range, (std::array<unsigned, 2>{79, 79}));
    for (unsigned n = 0; n < 80; ++n)
        EXPECT_EQ(five.bit(n), n < 79) << "bit " << n;
    EXPECT_EQ(afh(0).usable, 79u);
}

TEST_F(Coex, AfhMapReadback)
{
    unsigned inside = 0;
    std::array<uint8_t, 10> everything{};
    for (unsigned n = 0; n < 79; ++n)
        everything[n / 8] |= (uint8_t)(1u << (n % 8));
    EXPECT_EQ(afh_used(everything.data(), 24, 46, &inside), 79u);
    EXPECT_EQ(inside, 23u);
    const Afh avoided = afh(2437);
    EXPECT_EQ(afh_used(avoided.map.data(), 24, 46, &inside), 56u);
    EXPECT_EQ(inside, 0u);
}

// --- Wi-Fi: power save as HOS runs it ------------------------------------------------------------------------------
TEST_F(Coex, Pm2SleepReturnAndPolicy)
{
    std::array<uint8_t, 24> out{};
    pm2_sleep_ret_ext(out.data());
    auto u16 = [&](size_t at) {
        return (unsigned)(out[at] | out[at + 1] << 8);
    };
    auto u32 = [&](size_t at) {
        return (uint32_t)out[at] | (uint32_t)out[at + 1] << 8 | (uint32_t)out[at + 2] << 16 |
               (uint32_t)out[at + 3] << 24;
    };
    // logic, pad, low, high, rx, tx, txrx, rxb, txb, txrxb ('<BBHHHHHIII')
    EXPECT_EQ(std::make_tuple(out[0], out[1], u16(2), u16(4), u16(6), u16(8), u16(10), u32(12), u32(16), u32(20)),
              std::make_tuple((uint8_t)2, (uint8_t)0, 2000u, 5000u, 10u, 0u, 0u, 0u, 0u, 0u));
    EXPECT_EQ(coex_pm(1, 1), 2u);
    EXPECT_EQ(coex_pm(1, 0), 0u);  // the firmware refused the sleep return: stay at PM 0
    EXPECT_EQ(coex_pm(0, 1), 0u);  // Bluetooth off
}

// The NVRAM's btcdyn_* as dctl_prof_t: version 1, flags 0x3, levels 99/0/22/24, hysteresis 5, mode 5, 0 mode switch
// rows, 1 desense row (5,-120,0,-52,-72).
Bytes btcdynProfile()
{
    Bytes profile = cat(Bytes{1, 3, 99, 0, 22, 24, 5, 5, 0, 1}, Bytes(20, 0));
    for (int8_t v : {5, -120, 0, -52, -72})
        profile.push_back((uint8_t)v);
    profile.resize(50, 0);
    return profile;
}

// --- Wi-Fi: the firmware's coexistence state -----------------------------------------------------------------------
TEST_F(Coex, BtcDynctlStatusAndProfile)
{
    Bytes status{0, 0x34, 0x12};
    for (int8_t v : {-9, -40, -55})
        status.push_back((uint8_t)v);
    status.insert(status.end(), {22, 5});
    char out[120] = {};
    EXPECT_GT(btc_status_describe(status.data(), status.size(), out, sizeof(out)), 0);
    EXPECT_STREQ(out, "btc mode 5 desense 22 (BT power -9 RSSI -40, Wi-Fi RSSI -55)");
    EXPECT_EQ(btc_status_describe(status.data(), 7, out, sizeof(out)), -1);
    const Bytes profile = btcdynProfile();
    char line[200] = {};
    btc_profile_describe(profile.data(), profile.size(), line, sizeof(line));
    EXPECT_STREQ(line, "btcdyn v1 flags 0x3, desense levels 99/0/22/24, default mode 5, desense rows 5,-120,0,-52,-72");
    btc_profile_describe(profile.data(), 40, line, sizeof(line));
    EXPECT_STREQ(line, "");
}

TEST_F(Coex, NvramValuesFromTheLoadedFile)
{
    const std::string text = switch_ios::test::SourceText("../../SwitchWLAN/firmware/brcm/brcmfmac4356A3-pcie.txt");
    ASSERT_FALSE(text.empty());
    const std::vector<std::pair<const char *, uint32_t>> want = {
        {"boardflags", 0x02400001}, {"boardflags2", 0xc0802000}, {"muxenab", 0x10},
        {"btc_params84", 0x8},      {"btcdyn_flags", 0x3},       {"xtalfreq", 37400},
    };
    for (const auto &[key, expected] : want) {
        uint32_t value = 0;
        EXPECT_TRUE(nvram_value(text.data(), text.size(), key, &value)) << key;
        EXPECT_EQ(value, expected) << key;
    }
    uint32_t value = 0;
    EXPECT_FALSE(nvram_value(text.data(), text.size(), "btc_mode", &value));  // HOS sets none
    EXPECT_FALSE(nvram_value("boardflags=zz\n", 14, "boardflags", &value));
}

TEST_F(Coex, DynamicCoexistenceOffAndTdmWithLinks)
{
    // the firmware's btc_dynctl as r8a read it back: version 1, flags 0x3, levels 99/0/22/24, default mode 5
    Bytes profile = btcdynProfile();
    EXPECT_TRUE(btc_profile_static(profile.data(), 50));
    EXPECT_EQ(Bytes(profile.begin(), profile.begin() + 10), (Bytes{1, 0, 99, 0, 22, 24, 5, 5, 0, 1}));  // flags only
    const std::vector<int8_t> rows(profile.begin() + 30, profile.begin() + 35);
    EXPECT_EQ(rows, (std::vector<int8_t>{5, -120, 0, -52, -72}));
    Bytes other = cat(Bytes{2, 3}, Bytes(48, 0));
    EXPECT_FALSE(btc_profile_static(other.data(), 50));  // another version: left alone
    EXPECT_EQ(other[1], 3);
    EXPECT_FALSE(btc_profile_static(profile.data(), 40));  // short
    EXPECT_EQ(coex_mode(0, 5), 5u);                        // no link: the firmware's own (hybrid)
    EXPECT_EQ(coex_mode(1, 5), 1u);                        // a Joy-Con: full TDM
    EXPECT_EQ(coex_mode(3, 5), 1u);                        // two Joy-Cons and the AirPods
}

TEST_F(Coex, DynamicCoexistenceOnlyWithoutLinks)
{
    const Bytes nvram = btcdynProfile();
    Bytes idle(50, 0xAA), linked(50, 0xAA);
    EXPECT_TRUE(btc_profile_for(nvram.data(), nvram.size(), 0, idle.data()));
    EXPECT_EQ(idle, Bytes(nvram.begin(), nvram.begin() + 50));  // the NVRAM's, flags 0x3
    EXPECT_TRUE(btc_profile_for(nvram.data(), nvram.size(), 2, linked.data()));
    EXPECT_EQ(linked[1], 0);
    linked[1] = nvram[1];
    EXPECT_EQ(linked, idle);  // only the flags differ
    Bytes other = cat(Bytes{2, 3}, Bytes(48, 0)), out(50, 0xAA);
    EXPECT_FALSE(btc_profile_for(other.data(), other.size(), 1, out.data()));
    EXPECT_FALSE(btc_profile_for(nvram.data(), 40, 1, out.data()));
    EXPECT_EQ(out, Bytes(50, 0xAA));
}

}  // namespace
