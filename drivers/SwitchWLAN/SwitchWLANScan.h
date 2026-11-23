// SPDX-License-Identifier: BSD-2-Clause-Patent
// APPLE80211_IOC_SCAN_RESULT's record as the selected IPSW kind's IO80211.framework and CoreWiFi read it (the kind
// from switch_ipsw_volumes.h), filled from the radio's swlan_bss. Plain C++ so the host tests build it per kind.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "SwitchWLANStation.h"

namespace swlan::a80211 {

struct channel {
    uint32_t version, channel, flags;
};

struct scan_result {
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
#ifdef SWITCH_IPSW_KIND_24B119
    uint8_t elements[2048], ssid_hint[64], unused3[4];
    uint64_t timestamp_ns;
    uint8_t rsn_priority, unused4[3];
    uint32_t rsn_parameters, phy_mode;
    uint8_t mrsno_connection, mlo_connection, emlsr_connection, unused5;
#elif SWITCH_IPSW_KIND_21H461
    uint8_t elements[1024];
#else
#error "SwitchWLANScan.h: no APPLE80211_IOC_SCAN_RESULT layout for this IPSW kind"
#endif
};

static_assert(offsetof(scan_result, ssid_length) == 0x60 && offsetof(scan_result, age_ms) == 0x84 &&
                  offsetof(scan_result, elements) == 0x8c,
              "scan result layout");
#ifdef SWITCH_IPSW_KIND_24B119
static_assert(sizeof(scan_result) == 0x8e8 && offsetof(scan_result, timestamp_ns) == 0x8d0 &&
                  offsetof(scan_result, phy_mode) == 0x8e0,
              "24B119 scan result layout");
#else
static_assert(sizeof(scan_result) == 0x48c, "21H461 scan result layout");
#endif
static_assert(sizeof(swlan_bss::ie) <= sizeof(scan_result::elements), "a network's elements fit its record");

// APPLE80211_C_FLAG_*: 20 MHz 0x2, 40 MHz 0x4 (0x200: its extension channel above), 80 MHz 0x400, 160 MHz 0x800;
// 2.4 GHz 0x8, 5 GHz 0x10; active scanning 0x80; DFS 0x100.
inline void fill_channel(channel &c, uint32_t mhz, uint16_t bw = 20, uint8_t center = 0)
{
    c.version = 1;
    if (mhz >= 4900) {
        c.channel = (mhz - 5000) / 5;
        c.flags = 0x10;
    } else {
        c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5;
        c.flags = 0x8;
    }
    if (bw >= 160) {
        c.flags |= 0x800;
    } else if (bw >= 80) {
        c.flags |= 0x400;
    } else if (bw >= 40) {
        c.flags |= 0x4u | (center > c.channel ? 0x200u : 0u);
    } else {
        c.flags |= 0x2;
    }
}

#ifdef SWITCH_IPSW_KIND_24B119
// The rates (Mb/s) of the Supported and Extended Supported Rates elements, and APPLE80211_MODE_* from them and the
// HT Operation, VHT and HE Capabilities elements: 11a 0x2, 11b 0x4, 11g 0x8, 11n 0x10, 11ac 0x80, 11ax 0x100.
inline void fill_rates_and_phy_mode(scan_result &r)
{
    constexpr unsigned kMaxRates = sizeof r.rates / sizeof r.rates[0];
    bool ht = false, vht = false, he = false;
    for (size_t at = 0; at + 2 <= r.elements_length;) {
        const uint8_t id = r.elements[at], length = r.elements[at + 1];
        if (at + 2 + length > r.elements_length)
            break;
        const uint8_t *data = r.elements + at + 2;
        if (id == 1 || id == 50) {
            for (unsigned i = 0; i < length && r.rate_count < kMaxRates; ++i)
                r.rates[r.rate_count++] = (data[i] & 0x7fu) / 2u;
        } else if (id == 61 && length >= 22) {
            ht = true;
        } else if (id == 191 && length >= 12) {
            vht = true;
        } else if (id == 255 && length && data[0] == 35) {
            he = true;
        }
        at += 2 + length;
    }
    bool ofdm = false, dsss = false, pbcc = false, other = false;
    for (unsigned i = 0; i < r.rate_count; ++i) {
        switch (r.rates[i]) {
        case 6:
        case 9:
        case 12:
        case 18:
        case 24:
        case 36:
        case 48:
        case 54:
            ofdm = true;
            break;
        case 1:
        case 2:
        case 5:
        case 11:
            dsss = true;
            break;
        case 22:
        case 33:
            pbcc = true;
            break;
        default:
            other = true;
            break;
        }
    }
    uint32_t mode = 0;
    if (r.ch.flags & 0x10) {
        if ((dsss || ofdm) && !other && !(ht && vht))
            mode |= 0x2;
    } else if (r.ch.flags & 0x8) {
        if (!ht && !other) {
            if (dsss && !ofdm)
                mode |= 0x4;
            if (ofdm || pbcc)
                mode |= 0x8;
        }
    }
    if (ht)
        mode |= 0x10u | (vht ? 0x80u : 0u);
    if (he)
        mode |= 0x100;
    r.phy_mode = mode;
}
#endif

// continuous_ns: mach_continuous_time in nanoseconds, the clock b.age_ms was measured on (24B119 only).
inline void fill_scan_result(scan_result &r, const swlan_bss &b, [[maybe_unused]] uint64_t continuous_ns)
{
    __builtin_memset(&r, 0, sizeof r);
    r.version = 1;
    fill_channel(r.ch, b.freq_mhz, b.bw_mhz, b.center);
    r.noise_known = 1;
    r.noise = -95;
    r.rssi = b.signal_dbm;
    r.snr = static_cast<int16_t>(b.signal_dbm + 95);
    r.beacon_interval = b.beacon_int;
    r.capability = b.caps;
    __builtin_memcpy(r.bssid, b.bssid, sizeof r.bssid);
    r.ssid_length = b.ssid_len;
    __builtin_memcpy(r.ssid, b.ssid, sizeof r.ssid);
    r.age_ms = b.age_ms;
    r.elements_length = b.ie_len < sizeof b.ie ? b.ie_len : sizeof b.ie;
    __builtin_memcpy(r.elements, b.ie, r.elements_length);
#ifdef SWITCH_IPSW_KIND_24B119
    // CoreWiFi ages a result by this timestamp against CLOCK_MONOTONIC_RAW, which keeps counting through sleep.
    r.timestamp_ns = continuous_ns - uint64_t{b.age_ms} * 1000000;
    fill_rates_and_phy_mode(r);
#endif
}

}  // namespace swlan::a80211
