// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchWLAN's side of Wi-Fi/Bluetooth coexistence (the BCM4356's one 2.4 GHz antenna): what wifid says Bluetooth
// is doing, and what the firmware says the access point's link did, for the 5 s stats line. Plain C on byte buffers,
// so SwitchBluetooth/test's coex test drives it on the host. The includer declares snprintf.
//
// wifid (21H461, __WiFiManagerBluetoothServiceCallback / __WiFiManagerBluetoothLocalDeviceCallback) keeps a bit mask
// of what Bluetooth does and hands every change to the driver as APPLE80211_IOC_BT_COEX_FLAGS (102), the value in
// req_val: 1 a hands-free (SCO) device, 2 an A2DP stream (MobileBluetooth's service events 201 and 202), 4 an
// inquiry, 8 a page. Its BT power goes as APPLE80211_IOC_BT_POWER (104). Apple's own Broadcom driver
// (AppleBCMWLANCore::setBT_COEX_FLAGS, the 21H461 DriverKit dext) only stores the flags and logs "BT A2DP Started";
// coexistence itself is the firmware's arbitration with the Bluetooth core. "BT callback failure, result 120" in
// wifid's log is the same callback seeing a non-zero result with a local-device event it ignores (discoverability
// changed, from Settings): nothing to do with the driver.
//
// sta_info (brcmfmac brcmf_sta_info_le, bcmdhd sta_info_t; natural alignment, version 3 and 4 fields) for the access
// point, by its address: packets sent and failed (no acknowledgement after every retry), unicast and multicast data
// received, the last rates, decryption failures, frames that needed a retry or exhausted them. Fields past the length
// the firmware reports are not read.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWLAN_BT_SCO = 1u << 0,
    SWLAN_BT_A2DP = 1u << 1,
    SWLAN_BT_INQUIRY = 1u << 2,
    SWLAN_BT_PAGING = 1u << 3,
};

// "A2DP+paging", "none".
static inline int swlan_bt_flags_describe(uint32_t flags, char *out, size_t size)
{
    static const char *const names[] = {"SCO", "A2DP", "inquiry", "paging"};
    if (!size)
        return 0;
    out[0] = 0;
    size_t at = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!(flags & (1u << i)) || at >= size)
            continue;
        const int n = snprintf(out + at, size - at, "%s%s", at ? "+" : "", names[i]);
        at += n > 0 ? (size_t)n : 0;
    }
    if (flags & ~0xfu && at < size) {
        const int n = snprintf(out + at, size - at, "%s0x%x", at ? "+" : "", flags & ~0xfu);
        at += n > 0 ? (size_t)n : 0;
    }
    if (!at)
        return snprintf(out, size, "none");
    return (int)(at < size ? at : size - 1);
}

enum {
    SWLAN_STA_V3 = 1u << 0,     // packets, failures, unicast/multicast received, rates, decryption
    SWLAN_STA_V4 = 1u << 1,     // totals
    SWLAN_STA_RETRY = 1u << 2,  // frames retried, retries exhausted
};

struct swlan_sta {
    uint16_t ver, len;
    uint32_t flags, idle;  // idle: seconds since a data frame came from the access point
    uint32_t tx_pkts, tx_failures, rx_ucast, rx_mcast, tx_rate_kbps, rx_rate_kbps, rx_decrypt_ok, rx_decrypt_failed;
    uint32_t tx_total, rx_total;
    uint32_t tx_retried, tx_retry_exhausted;
    unsigned have;  // SWLAN_STA_*
};

static inline uint32_t swlan_coex_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Offsets in sta_info_t: {ver, len, cap, pad} 0, flags 8, idle 12, ea 16, rateset {count, rates[16]} 24, in 44,
// listen interval 48; version 3: tx_pkts 52 ... rx_decrypt_failures 80; version 4: tx_tot_pkts 84, rx_tot_pkts 88,
// tx_mcast_pkts 92, six byte counters 96-143, rssi[4] 144, nf[4] 148, aid 152, ht caps 154, vht flags 156, pad,
// tx_pkts_retried 160, tx_pkts_retry_exhausted 164.
static inline bool swlan_sta_parse(const uint8_t *b, size_t n, struct swlan_sta *out)
{
    for (size_t i = 0; i < sizeof(*out); ++i)
        ((uint8_t *)out)[i] = 0;
    if (n < 16)
        return false;
    out->ver = (uint16_t)(b[0] | (unsigned)b[1] << 8);
    out->len = (uint16_t)(b[2] | (unsigned)b[3] << 8);
    const size_t len = out->len < n ? out->len : n;
    if (len < 16)
        return false;
    out->flags = swlan_coex_le32(b + 8);
    out->idle = swlan_coex_le32(b + 12);
    if (out->ver >= 3 && len >= 84) {
        out->tx_pkts = swlan_coex_le32(b + 52);
        out->tx_failures = swlan_coex_le32(b + 56);
        out->rx_ucast = swlan_coex_le32(b + 60);
        out->rx_mcast = swlan_coex_le32(b + 64);
        out->tx_rate_kbps = swlan_coex_le32(b + 68);
        out->rx_rate_kbps = swlan_coex_le32(b + 72);
        out->rx_decrypt_ok = swlan_coex_le32(b + 76);
        out->rx_decrypt_failed = swlan_coex_le32(b + 80);
        out->have |= SWLAN_STA_V3;
    }
    if (out->ver >= 4 && len >= 92) {
        out->tx_total = swlan_coex_le32(b + 84);
        out->rx_total = swlan_coex_le32(b + 88);
        out->have |= SWLAN_STA_V4;
    }
    if (out->ver >= 4 && len >= 168) {
        out->tx_retried = swlan_coex_le32(b + 160);
        out->tx_retry_exhausted = swlan_coex_le32(b + 164);
        out->have |= SWLAN_STA_RETRY;
    }
    return true;
}

// The access point's side of a stats window, from two sta_info readings: "AP: tx 85 failed 0 retried 3, rx unicast
// 70 multicast 9 decrypt-failed 0, rates 65/39 Mb/s, idle 0 s". Empty when the newer reading has no version 3 fields.
static inline int swlan_sta_describe(const struct swlan_sta *was, const struct swlan_sta *now, char *out, size_t size)
{
    if (!size)
        return 0;
    out[0] = 0;
    if (!(now->have & SWLAN_STA_V3))
        return 0;
    const bool base = (was->have & SWLAN_STA_V3) != 0;
    int n = snprintf(out, size, "AP: tx %u failed %u", base ? now->tx_pkts - was->tx_pkts : now->tx_pkts,
                     base ? now->tx_failures - was->tx_failures : now->tx_failures);
    size_t at = n > 0 ? (size_t)n : 0;
    if (at < size && (now->have & SWLAN_STA_RETRY)) {
        const bool rb = (was->have & SWLAN_STA_RETRY) != 0;
        n = snprintf(out + at, size - at, " retried %u exhausted %u",
                     rb ? now->tx_retried - was->tx_retried : now->tx_retried,
                     rb ? now->tx_retry_exhausted - was->tx_retry_exhausted : now->tx_retry_exhausted);
        at += n > 0 ? (size_t)n : 0;
    }
    if (at < size) {
        n = snprintf(out + at, size - at, ", rx unicast %u multicast %u decrypt-failed %u, rates %u/%u Mb/s, idle %u s",
                     base ? now->rx_ucast - was->rx_ucast : now->rx_ucast,
                     base ? now->rx_mcast - was->rx_mcast : now->rx_mcast,
                     base ? now->rx_decrypt_failed - was->rx_decrypt_failed : now->rx_decrypt_failed,
                     now->tx_rate_kbps / 1000, now->rx_rate_kbps / 1000, now->idle);
        at += n > 0 ? (size_t)n : 0;
    }
    return (int)(at < size ? at : size - 1);
}

// ---- Power save under coexistence -----------------------------------------------------------------------------
//
// Nintendo's wlan sysmodule, the one this firmware and NVRAM come from, never runs the station without power save:
// SetPowerSaveMode (wlan:inf 38, switchbrew WLAN_services) sets "pm2_sleep_ret_ext" and then WLC_SET_PM 2, mode 0
// with {logic 2 (AND), low 2000 ms, high 5000 ms, rx packets 10}, everything else 0: the radio stays awake 2 s after
// the last frame (5 s after a busy period) and only then sleeps between beacons. Its coexistence with Bluetooth
// (dynamic desense, hybrid mode 5, the NVRAM's btcdyn_*) was tuned and shipped in that mode only. r7v ran PM 0 and,
// from the AirPods' A2DP start at 12:36:27 until their link went at 12:40:00, got broadcasts and beacons but no
// unicast (ARP to the router failed from 12:37:21, 10 s after the link went it answered again): the pattern of an
// access point holding our frames because the firmware's coexistence protection told it we sleep (PM=1 null frames)
// while the firmware, in no power-save mode of its own, never polls for them by the beacon's TIM. That reading is
// not proven; the stats line tests it (pm_dur, unicast/group counts, the access point's sta_info, btc_dynctl_status).
// Either way the vendor's operating point is the one to run: while Bluetooth is on, HOS's sleep return, then PM 2;
// PM 0 when it is off. The radio stays up 2-5 s after any frame, so traffic in progress sees no added latency; only
// the first frame the access point sends after 2 s of silence waits for the next beacon (at most ~100 ms).
enum {
    SWLAN_PM_OFF = 0,
    SWLAN_PM_FAST = 2
};

#define SWLAN_PM2_SLEEP_RET_EXT_SIZE 24u

// wl_pm2_sleep_ret_ext_t {u8 logic, pad, u16 low_ms, high_ms, rx/tx/txrx packet thresholds, u32 rx/tx/txrx byte
// thresholds}, little-endian, as HOS fills it for mode 0.
static inline void swlan_pm2_sleep_ret_ext(uint8_t out[SWLAN_PM2_SLEEP_RET_EXT_SIZE])
{
    for (unsigned i = 0; i < SWLAN_PM2_SLEEP_RET_EXT_SIZE; ++i)
        out[i] = 0;
    out[0] = 2;  // WL_DFRTS_LOGIC_AND
    out[2] = 2000 & 0xff;
    out[3] = 2000 >> 8;
    out[4] = 5000 & 0xff;
    out[5] = 5000 >> 8;
    out[6] = 10;
}

// The power-save mode for the station: PM 2 while Bluetooth is on and the firmware took the sleep return, else PM 0.
static inline unsigned swlan_coex_pm(bool bt_power, bool sleep_return_set)
{
    return bt_power && sleep_return_set ? SWLAN_PM_FAST : SWLAN_PM_OFF;
}

// ---- The firmware's own coexistence state ----------------------------------------------------------------------
//
// btc_dynctl_status (bcmdhd wlioctl.h dynctl_status_t, packed, get only): what the dynamic coexistence (btcdyn, the
// NVRAM's btcdyn_flags 0x3: dynamic control and desense on) decided last: {u8 sim_on, u16 bt_pwr_shm, s8 bt_pwr,
// s8 bt_rssi, s8 wl_rssi, u8 dsns_level, u8 btc_mode}. btc_dynctl (dctl_prof_t, packed, 50 bytes) is the profile the
// NVRAM set: {u8 version (1), flags, default/low/mid/high desense level, s8 mode switch hysteresis, u8 default btc
// mode, mode switch rows, desense rows, then 4 + 4 rows of {mode, bt_pwr, bt_rssi, wl_rssi_high, wl_rssi_low}}.
struct swlan_btc_status {
    int8_t bt_pwr, bt_rssi, wl_rssi;
    uint8_t desense, mode;
};

static inline bool swlan_btc_status_parse(const uint8_t *b, size_t n, struct swlan_btc_status *out)
{
    if (n < 8)
        return false;
    out->bt_pwr = (int8_t)b[3];
    out->bt_rssi = (int8_t)b[4];
    out->wl_rssi = (int8_t)b[5];
    out->desense = b[6];
    out->mode = b[7];
    return true;
}

// "btc mode 5 desense 22 (BT power -9 RSSI -40, Wi-Fi RSSI -55)"
static inline int swlan_btc_status_describe(const struct swlan_btc_status *s, char *out, size_t size)
{
    return snprintf(out, size, "btc mode %u desense %u (BT power %d RSSI %d, Wi-Fi RSSI %d)", s->mode, s->desense,
                    s->bt_pwr, s->bt_rssi, s->wl_rssi);
}

// With a Bluetooth link up, the NVRAM's dynamic coexistence (btcdyn, hybrid mode 5) turns the receiver down
// (btc_dynctl_status desense 22 at Wi-Fi RSSI -52 or less, 24 above): the access point's rate to us fell from 130-144
// Mb/s to 14-65 (40 Mbps with no link, 13-14 with one Joy-Con, 5-10 with two and the AirPods) and the controller
// flushed A2DP packets it could not get on air within their 60 ms. So while any link is up the dynamic control and its
// desense are off (btc_dynctl flags 0) and the radios take turns (btc_mode 1, full TDM: Bluetooth's slots its own,
// nothing received through Bluetooth's transmissions, so nothing to desense against; what Apple's BCM4355C0 iPads and
// the Nexus 6's BCM4356 ship). With no link the firmware runs its own coexistence as the NVRAM configures it (dynamic
// control on, mode 5, no desense), the state HOS runs: mode 5 with the dynamic control off has no such baseline.
enum {
    SWLAN_BTC_FULL_TDM = 1,
    SWLAN_BTC_HYBRID = 5
};

// dctl_prof_t (50 bytes, version 1) with the dynamic control off: flags 0 (no dynamic control, desense or mode
// switching); the rest as the NVRAM made it. False for any other layout (the profile is left alone).
static inline bool swlan_btc_profile_static(uint8_t *b, size_t n)
{
    if (n < 50 || b[0] != 1)
        return false;
    b[1] = 0;
    return true;
}

// The profile for the links Bluetooth has, from the one the firmware held at bring-up (the NVRAM's): static with any
// link, the NVRAM's as it is with none. False (out untouched) unless that is a version 1 profile.
static inline bool swlan_btc_profile_for(const uint8_t *nvram, size_t n, unsigned links, uint8_t out[50])
{
    if (n < 50 || nvram[0] != 1)
        return false;
    for (unsigned i = 0; i < 50; ++i)
        out[i] = nvram[i];
    if (links)
        out[1] = 0;
    return true;
}

// The mode for the links Bluetooth has: full TDM with any, else the firmware's idle mode.
static inline unsigned swlan_coex_mode(unsigned links, unsigned idle_mode)
{
    return links ? SWLAN_BTC_FULL_TDM : idle_mode;
}

// "btcdyn v1 flags 0x3, desense levels 99/0/22/24, default mode 5, desense rows 5,-120,0,-52,-72"; 0 when the
// answer is too short or not version 1.
static inline int swlan_btc_profile_describe(const uint8_t *b, size_t n, char *out, size_t size)
{
    if (!size)
        return 0;
    out[0] = 0;
    if (n < 50 || b[0] != 1)
        return 0;
    int k = snprintf(out, size, "btcdyn v1 flags %#x, desense levels %u/%u/%u/%u, default mode %u, desense rows", b[1],
                     b[2], b[3], b[4], b[5], b[7]);
    size_t at = k > 0 ? (size_t)k : 0;
    const unsigned rows = b[9] < 4 ? b[9] : 4;
    for (unsigned r = 0; r < rows && at < size; ++r) {
        const uint8_t *row = b + 10 + 20 + 5 * (size_t)r;
        k = snprintf(out + at, size - at, " %d,%d,%d,%d,%d", (int8_t)row[0], (int8_t)row[1], (int8_t)row[2],
                     (int8_t)row[3], (int8_t)row[4]);
        at += k > 0 ? (size_t)k : 0;
    }
    if (!rows && at < size) {
        k = snprintf(out + at, size - at, " none");
        at += k > 0 ? (size_t)k : 0;
    }
    return (int)(at < size ? at : size - 1);
}

// ---- The board's coexistence wiring, from the NVRAM we load ---------------------------------------------------
//
// The Wi-Fi and Bluetooth cores share the die and talk over its internal GCI (Broadcom's si_gci_init sets up SECI over
// it when boardflags2's BFL2_BTCLEGACY, 0x80, is clear); the firmware has no seci/gci/eci/wci2 iovar or NVRAM key to
// switch it. What the host can see is the NVRAM's claim: boardflags BFL_BTCOEX (0x1) and BFL_FEM_BT (0x400000, one
// antenna shared through the front end's switch), boardflags2 BFL2_BT_SHARE_ANT0 (0x800000), and muxenab's SECI pin
// routing (0x1000) for an external radio, off on this board (0x10 is the host wake line only).
// A key's value (hex or decimal) in NVRAM text "key=value" lines; false if absent or not a number.
static inline bool swlan_nvram_value(const char *text, size_t length, const char *key, uint32_t *out)
{
    size_t keyLength = 0;
    while (key[keyLength])
        ++keyLength;
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && text[end] != '\n' && text[end] != 0)
            ++end;
        if (end - at > keyLength && text[at + keyLength] == '=') {
            bool same = true;
            for (size_t i = 0; i < keyLength && same; ++i)
                same = text[at + i] == key[i];
            if (same) {
                size_t i = at + keyLength + 1;
                unsigned base = 10;
                if (i + 1 < end && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                    base = 16;
                    i += 2;
                }
                uint32_t v = 0;
                bool any = false;
                for (; i < end; ++i) {
                    const char c = text[i];
                    unsigned d = 0;
                    if (c >= '0' && c <= '9') {
                        d = (unsigned)(c - '0');
                    } else if (base == 16 && c >= 'a' && c <= 'f') {
                        d = (unsigned)(c - 'a' + 10);
                    } else if (base == 16 && c >= 'A' && c <= 'F') {
                        d = (unsigned)(c - 'A' + 10);
                    } else {
                        break;
                    }
                    v = v * base + d;
                    any = true;
                }
                if (!any)
                    return false;
                *out = v;
                return true;
            }
        }
        at = end + 1;
    }
    return false;
}

#ifdef __cplusplus
}
#endif
