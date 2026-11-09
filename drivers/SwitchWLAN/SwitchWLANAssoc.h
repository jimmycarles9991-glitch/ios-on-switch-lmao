// SPDX-License-Identifier: BSD-2-Clause-Patent
// APPLE80211_IOC_ASSOCIATE's apple80211_assoc_data as the selected IPSW kind's IO80211.framework lays it out (the
// kind from switch_ipsw_volumes.h): the network wifid joins, its BSSID when the join pins one, and the key. Only what
// the driver reads is named.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace swlan::a80211 {

#ifdef SWITCH_IPSW_KIND_24B119
// 24B119's framework widened the authentication types to 32 bits, so everything from the SSID length on sits four bytes
// later than in 21H461 (boot 15: authentication 8 at +12, a 10-byte name's length at +16). A passphrase comes in
// the key as cipher 10.
struct assoc_request {
    uint32_t version;
    uint16_t mode, unused0;
    uint32_t auth_lower, auth_upper;
    uint32_t ssid_length;
    uint8_t ssid[32];
    uint8_t bssid[6], unused1[2];
    uint32_t key_version, key_length, key_cipher;
    uint16_t key_flags, key_index;
    uint8_t key[64];
};

static_assert(offsetof(assoc_request, auth_upper) == 0x0c && offsetof(assoc_request, ssid_length) == 0x10 &&
                  offsetof(assoc_request, ssid) == 0x14 && offsetof(assoc_request, bssid) == 0x34 &&
                  offsetof(assoc_request, key_length) == 0x40 && offsetof(assoc_request, key_cipher) == 0x44 &&
                  offsetof(assoc_request, key) == 0x4c,
              "24B119 association layout");
#elif SWITCH_IPSW_KIND_21H461
struct assoc_request {  // 592 bytes
    uint32_t version;
    uint16_t mode, auth_lower, auth_upper, unused0;
    uint32_t ssid_length;
    uint8_t ssid[32];
    uint8_t bssid[6], unused1[2];
    uint32_t key_version, key_length, key_cipher;
    uint16_t key_flags, key_index;
    uint8_t key[64];
    uint8_t rest[0x250 - 0x88];
};

static_assert(sizeof(assoc_request) == 592 && offsetof(assoc_request, ssid) == 0x10 &&
                  offsetof(assoc_request, key) == 0x48,
              "21H461 association layout");
#else
#error "SwitchWLANAssoc.h: no APPLE80211_IOC_ASSOCIATE layout for this IPSW kind"
#endif

}  // namespace swlan::a80211
