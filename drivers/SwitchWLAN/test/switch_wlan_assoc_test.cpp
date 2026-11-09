// SwitchWLANAssoc.h on the host, once per IPSW kind: an APPLE80211_IOC_ASSOCIATE request laid out byte by byte as that
// kind's IO80211.framework builds it reads back as the network, BSSID and key wifid asked for.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../SwitchWLANAssoc.h"

namespace {

using swlan::a80211::assoc_request;

struct Offsets {
    size_t authUpper, ssidLength, ssid, bssid, keyLength, keyCipher, key;
};

#ifdef SWITCH_IPSW_KIND_24B119
// 24B119: Apple80211Associate2's stores (macOS 27's IO80211-1585 shares the structure), and boot 15's request.
constexpr Offsets kFramework = {0x0c, 0x10, 0x14, 0x34, 0x40, 0x44, 0x4c};
#else
constexpr Offsets kFramework = {0x08, 0x0c, 0x10, 0x30, 0x3c, 0x40, 0x48};
#endif

void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    memcpy(b.data() + at, &v, 4);
}

std::vector<uint8_t> request(const std::string &ssid, const uint8_t bssid[6], const std::string &key, uint32_t cipher)
{
    std::vector<uint8_t> b(0x290, 0xaa);
    put32(b, 0, 1);
#ifdef SWITCH_IPSW_KIND_24B119
    put32(b, kFramework.authUpper, 8);
#else
    b[kFramework.authUpper] = 8;
    b[kFramework.authUpper + 1] = 0;
#endif
    put32(b, kFramework.ssidLength, static_cast<uint32_t>(ssid.size()));
    memset(b.data() + kFramework.ssid, 0, 32);
    memcpy(b.data() + kFramework.ssid, ssid.data(), ssid.size());
    memcpy(b.data() + kFramework.bssid, bssid, 6);
    put32(b, kFramework.keyLength, static_cast<uint32_t>(key.size()));
    put32(b, kFramework.keyCipher, cipher);
    memset(b.data() + kFramework.key, 0, 64);
    memcpy(b.data() + kFramework.key, key.data(), key.size());
    return b;
}

assoc_request parse(const std::vector<uint8_t> &b)
{
    assoc_request a;
    memcpy(&a, b.data(), sizeof a < b.size() ? sizeof a : b.size());
    return a;
}

TEST(SwitchWLANAssoc, NetworkBssidAndPassphrase)
{
    const uint8_t bssid[6] = {0x98, 0xb6, 0xe9, 0x01, 0x02, 0x03};
    const assoc_request a = parse(request("garden-iot", bssid, "correct horse", 10));
    ASSERT_EQ(a.ssid_length, 10u);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(a.ssid), a.ssid_length), "garden-iot");
    EXPECT_EQ(memcmp(a.bssid, bssid, 6), 0);
    ASSERT_EQ(a.key_length, 13u);
    EXPECT_EQ(a.key_cipher, 10u);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(a.key), a.key_length), "correct horse");
    EXPECT_EQ(static_cast<unsigned>(a.auth_upper), 8u);
}

TEST(SwitchWLANAssoc, PairwiseMasterKey)
{
    const uint8_t bssid[6] = {};
    const std::string pmk(32, '\x5a');
    const assoc_request a = parse(request("home", bssid, pmk, 6));
    EXPECT_EQ(a.ssid_length, 4u);
    EXPECT_EQ(a.key_length, 32u);
    EXPECT_EQ(a.key_cipher, 6u);
    EXPECT_EQ(memcmp(a.key, pmk.data(), 32), 0);
}

}  // namespace
