// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Apple80211 requests and events as IO80211.framework (21H461) lays them out: the ioctls wifid sends on en0 and
// the kernel events it reads back. UniXNU-sheng's package/WifiNet.cpp layouts (BSD-2-Clause-Patent).
#pragma once

#include <net/if.h>
#include <net/if_media.h>
#include <stdint.h>
#include <sys/sockio.h>
#include <sys/types.h>
#include "SwitchWLANAssoc.h"

#define SIOCSA80211 0x802869c8UL  // _IOW('i', 200, struct apple80211req)
#define SIOCGA80211 0xc02869c9UL  // _IOWR('i', 201, struct apple80211req)

namespace swlan::a80211 {

struct apple80211req {
    char req_if_name[16];
    int32_t req_type;
    int32_t req_val;
    uint32_t req_len;
    user_addr_t req_data __attribute__((aligned(8)));
};

// request types
enum {
    kSsid = 1,
    kAuthType = 2,
    kCipherKey = 3,
    kChannel = 4,
    kPowerSave = 5,
    kTxPower = 7,
    kRate = 8,
    kBssid = 9,
    kScanReq = 10,
    kScanResult = 11,
    kCardCaps = 12,
    kState = 13,
    kPhyMode = 14,
    kOpMode = 15,
    kRssi = 16,
    kNoise = 17,
    kPower = 19,
    kAssociate = 20,
    kAssociateResult = 21,
    kDisassociate = 22,
    kSupportedChannels = 27,
    kLocale = 28,
    kDeauth = 29,
    kDriverVersion = 43,
    kHardwareVersion = 44,
    kCountryCode = 51,
    kRadioInfo = 54,
    kScanReqMultiple = 86,
    kScanCacheClear = 90,
    kVirtualIfCreate = 94,  // wifip2pd's awdl0 (SwitchWLANAwdl.cpp), only once en0 advertised AWDL
    kVirtualIfDelete = 95,
    kBtCoexFlags = 102,  // wifid's Bluetooth state (SwitchWLANCoex.h), the value in req_val
    kCurrentNetwork = 103,
    kBtPower = 104,
    kRssiBounds = 106,
    kChannelsInfo = 207,
    kHwSupportedChannels = 254,
    // IPConfiguration reads the association's ID at link up (4 bytes) and hands it back once it has cleaned up the
    // previous network's addresses (WiFiAcknowledgeConnectionID).
    kProtocolCleanupComplete = 560,
    kConnectionId = 563,
    kFilterMonitorEvents = 564,
};

// events
enum {
    kEvPowerChanged = 1,
    kEvSsidChanged = 2,
    kEvBssidChanged = 3,
    kEvLinkChanged = 4,
    kEvAssocDone = 9,
    kEvScanDone = 10
};

// APPLE80211_M_SUPPLICANT_EVENT (wifid starts monitoring 76 right after 80, ROAM_STATUS). IO80211.framework
// (21H461, the event 76 case of __eventRead) takes exactly these 40 bytes and makes the dictionary wifid reads:
// status and reason, transition-disable flags, the 4-way handshake's phase durations in us, joining a new network.
enum {
    kEvSupplicant = 76
};

struct supplicant_event {
    uint32_t status, reason, unused0, tsn_disabled_flags;
    uint32_t eapol_start_m1_us, m1_m2_us, m2_m3_us, m3_m4_us, total_us;
    uint8_t joining_new_network, unused1[3];
};

static_assert(sizeof(supplicant_event) == 40, "supplicant event layout");

// results of an association, as the framework numbers them
enum {
    kAssocOk = 1,
    kAssocFailed = 2,
    kAssocTimeout = 9,
    kAssocBadKey = 21
};

struct channel {
    uint32_t version, channel, flags;
};

struct channels_list {
    uint32_t version, count;
    channel list[401];
    uint32_t unused;
};  // 4824 bytes

struct scan_result {  // 1164 bytes
    uint32_t version;
    channel ch;
    uint16_t noise_known;
    int16_t noise, snr, rssi;
    uint16_t beacon_interval, capability;
    uint8_t bssid[6];
    uint8_t rate_count, unused0;
    uint32_t rates[15];
    uint8_t ssid_length, ssid[32], unused1[3];
    uint32_t age_ms;
    uint8_t flags, unused2;
    uint16_t elements_length;
    uint8_t elements[1024];
};

struct link_event {
    uint8_t down, unused0[3];
    int32_t rssi;
    uint16_t snr, noise;
    uint8_t unused1[4];
    uint8_t involuntary, unused2[3];
    uint32_t reason, subreason;
    uint8_t unused3[4];
};

static_assert(sizeof(scan_result) == 1164 && __builtin_offsetof(scan_result, ssid_length) == 0x60 &&
                  __builtin_offsetof(scan_result, age_ms) == 0x84 && __builtin_offsetof(scan_result, elements) == 0x8c,
              "scan result layout");
static_assert(sizeof(channels_list) == 4824, "channel list layout");
static_assert(sizeof(link_event) == 32, "link event layout");
static_assert(sizeof(struct ifmediareq64) == 44 && SIOCGIFMEDIA64 == SIOCGIFMEDIA,
              "media request as userland sends it");

}  // namespace swlan::a80211
