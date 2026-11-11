// SPDX-License-Identifier: BSD-2-Clause-Patent
// The association as the selected IPSW kind's IO80211.framework and CoreWiFi lay it out (the kind from
// switch_ipsw_volumes.h): APPLE80211_IOC_ASSOCIATE's apple80211_assoc_data (the network wifid joins, its BSSID when the
// join pins one, and the key; only what the driver reads is named) and the events that report the join.
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

inline constexpr bool kJoinEventData = true;  // BSSID_CHANGED and ASSOC_DONE carry bssid_changed and join_report
inline constexpr bool kAnswersDeauthGet = true;
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

// 21H461's framework takes BSSID_CHANGED and ASSOC_DONE with nothing after the interface name and reads the result
// back (APPLE80211_IOC_ASSOCIATE_RESULT).
inline constexpr bool kJoinEventData = false;
inline constexpr bool kAnswersDeauthGet = false;
#else
#error "SwitchWLANAssoc.h: no APPLE80211_IOC_ASSOCIATE layout for this IPSW kind"
#endif

// 24B119's join events. APPLE80211_M_BSSID_CHANGED: CoreWiFi drops one that is not exactly these 28 bytes and reads
// only the reason (IO80211's BSSID_CHANGE_REASON_*, 1-5).
struct bssid_changed {
    uint8_t leading[0x18];
    uint32_t reason;
};

inline constexpr uint32_t kBssidChangeLinkUpJoin = 3;

// APPLE80211_M_ASSOC_DONE completes an Apple80211AssociateAsync (_dispatchAsyncAssocEvent), which reads this report
// whatever the event's length: a short one leaves the 0xaa its read buffer was filled with. The ten records are per
// BSS tried (JOIN_STATUS_SUBSTATE_INFO, for analytics). result_valid nonzero takes result as the join's
// APPLE80211_IOC_ASSOCIATE_RESULT (1: joined); zero reads that back.
struct join_bss {
    uint8_t bssid[6], unused0[2];
    uint32_t substate_flags;
    int32_t auth_status, auth_reason, assoc_status, assoc_reason;
    int32_t setssid_status, setssid_reason, supplicant_status, supplicant_reason;
    int32_t rssi;
    uint32_t age_ms;
};

struct join_report {
    join_bss bss[10];
    int32_t join_status;
    uint32_t unused1;
    int32_t extended_status;  // 1000-1010: an extended failure; anything else none
    uint32_t result, result_valid;
};

// APPLE80211_IOC_DEAUTH asked (a get, 16 bytes): why the last link went down, and from where.
struct deauth_info {
    uint32_t version, reason;
    uint8_t bssid[6], unused[2];
};

static_assert(sizeof(bssid_changed) == 28 && offsetof(bssid_changed, reason) == 0x18, "24B119 BSSID change layout");
static_assert(sizeof(join_bss) == 0x34 && offsetof(join_bss, rssi) == 0x2c && sizeof(join_report) == 0x21c &&
                  offsetof(join_report, join_status) == 0x208 && offsetof(join_report, extended_status) == 0x210 &&
                  offsetof(join_report, result) == 0x214 && offsetof(join_report, result_valid) == 0x218,
              "24B119 join report layout");
static_assert(sizeof(deauth_info) == 16, "deauthentication layout");

}  // namespace swlan::a80211
