// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the Wi-Fi link runs at, for SwitchWLAN.cpp's once-per-join link line, its stats line and the channel widths
// wifid is shown: the firmware's chanspecs decoded, the access point's HT/VHT/HE capabilities and operation read
// from its information elements, the best PHY rate both ends allow, and "name value" counters out of a firmware
// dump. Plain C, no state: test/switch_wlan_msgbuf_test.cpp drives it on the host.
//
// The chanspec layouts are brcmutil's (Linux drivers/net/wireless/broadcom/brcm80211/brcmutil/d11.c and
// include/brcmu_wifi.h, ISC: Copyright (c) 2013 Broadcom Corporation; permission to use, copy, modify and/or
// distribute with or without fee is granted, provided the copyright notice and permission notice appear in all
// copies). The element layouts are IEEE 802.11-2020 9.4.2.55-56 (HT), 9.4.2.157-158 (VHT), 9.4.2.248 (HE).
// The includer declares snprintf, memset, memcmp and strlen (the kernel's libkern, the host's libc).
#ifndef SWITCH_WLAN_LINK_H
#define SWITCH_WLAN_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WLC_GET_VERSION's answer: brcmu_d11 BRCMU_D11N_IOTYPE (1) or BRCMU_D11AC_IOTYPE (2).
enum {
    SWLAN_IOTYPE_D11N = 1,
    SWLAN_IOTYPE_D11AC = 2
};

struct swlan_chanspec {
    uint8_t control;  // the 20 MHz channel beacons go on
    uint8_t center;   // the channel the whole width is centred on (the control channel at 20 MHz)
    uint16_t bw_mhz;  // 20, 40, 80, 160 (80+80 reads 160); 0 when the chanspec is not understood
    bool five_ghz;
};

// brcmu_d11n_decchspec / brcmu_d11ac_decchspec.
static inline bool swlan_chanspec_decode(uint16_t cs, int iotype, struct swlan_chanspec *out)
{
    memset(out, 0, sizeof(*out));
    const uint8_t ch = (uint8_t)(cs & 0xFF);
    out->center = out->control = ch;
    if (iotype == SWLAN_IOTYPE_D11N) {
        out->five_ghz = (cs & 0xF000) == 0x1000;
        switch (cs & 0x0C00) {
        case 0x0800:
            out->bw_mhz = 20;
            return true;
        case 0x0C00:
            out->bw_mhz = 40;
            if ((cs & 0x0300) == 0x0100)
                out->control = (uint8_t)(ch - 2);
            if ((cs & 0x0300) == 0x0200)
                out->control = (uint8_t)(ch + 2);
            return true;
        default:
            return false;
        }
    }
    out->five_ghz = (cs & 0xC000) == 0xC000;
    const unsigned sb = (cs & 0x0700u) >> 8;
    switch (cs & 0x3800) {
    case 0x1000:
        out->bw_mhz = 20;
        return true;
    case 0x1800:
        out->bw_mhz = 40;
        out->control = (uint8_t)(sb ? ch + 2 : ch - 2);
        return true;
    case 0x2000: {
        static const int8_t offset[4] = {-6, -2, 2, 6};
        out->bw_mhz = 80;
        out->control = (uint8_t)(ch + offset[sb & 3]);
        return true;
    }
    case 0x2800:
    case 0x3000: {
        static const int8_t offset[8] = {-14, -10, -6, -2, 2, 6, 10, 14};
        out->bw_mhz = 160;
        out->control = (uint8_t)(ch + offset[sb]);
        return true;
    }
    default:
        return false;
    }
}

// The access point as its beacon (or probe response) describes it.
struct swlan_peer {
    bool ht, vht, he;
    uint8_t ht_nss, vht_nss;   // spatial streams it receives (HT MCS bitmap, VHT RX MCS map)
    uint16_t ht_width;         // the HT operation's width: 20, or 40 when it runs a secondary channel
    uint16_t vht_width;        // the VHT operation's width: 0 (no VHT operation), 40 (as HT), 80, 160
    bool sgi20, sgi40, sgi80;  // short guard interval it receives at that width
    uint8_t primary;           // HT operation's primary channel
};

static inline void swlan_peer_parse(const uint8_t *ie, size_t length, struct swlan_peer *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t at = 0; at + 2 <= length && at + 2 + ie[at + 1] <= length; at += 2 + (size_t)ie[at + 1]) {
        const uint8_t id = ie[at];
        const uint8_t n = ie[at + 1];
        const uint8_t *v = ie + at + 2;
        if (id == 45 && n >= 7) {  // HT Capabilities: info (2), A-MPDU (1), MCS set (16)
            const uint16_t info = (uint16_t)(v[0] | (unsigned)v[1] << 8);
            out->ht = true;
            out->sgi20 = (info & 0x20) != 0;
            out->sgi40 = (info & 0x40) != 0;
            for (unsigned s = 0; s < 4; ++s) {
                if (v[3 + s])
                    out->ht_nss = (uint8_t)(s + 1);
            }
        } else if (id == 61 && n >= 2) {  // HT Operation: primary channel, then the secondary offset and STA width
            out->primary = v[0];
            out->ht_width = ((v[1] & 0x03) && (v[1] & 0x04)) ? 40 : 20;
        } else if (id == 191 && n >= 12) {  // VHT Capabilities: info (4), RX MCS map (2), ...
            const uint32_t info = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
            const uint16_t map = (uint16_t)(v[4] | (unsigned)v[5] << 8);
            out->vht = true;
            out->sgi80 = (info & 0x20) != 0;
            for (unsigned s = 0; s < 8; ++s) {
                if ((((unsigned)map >> (2 * s)) & 3) != 3)
                    out->vht_nss = (uint8_t)(s + 1);
            }
        } else if (id == 192 && n >= 3) {  // VHT Operation: width 0 (20/40 as HT), 1 (80, or 160/80+80 by seg 1)
            out->vht_width = 160;
            if (v[0] == 0) {
                out->vht_width = 40;
            } else if (v[0] == 1 && !v[2]) {
                out->vht_width = 80;
            }
        } else if (id == 255 && n >= 1 && v[0] == 35) {  // HE Capabilities (element id extension 35)
            out->he = true;
        }
    }
}

// The station's own limits: the BCM4356 is 2x2 with short guard interval at every width it has.
enum {
    SWLAN_OUR_NSS = 2
};

// The best PHY rate (kb/s) both ends allow on a link of `bw_mhz`: HT MCS 7 or VHT MCS 9 (MCS 8 at 20 MHz) per
// stream, short guard interval where the access point takes it. 0 when nothing is known.
static inline uint32_t swlan_link_best_kbps(const struct swlan_peer *ap, uint16_t bw_mhz)
{
    // kb/s per stream, long and short guard interval: HT MCS 7 at 20/40, VHT MCS 8 at 20, MCS 9 at 40/80/160
    static const uint32_t ht20[2] = {65000, 72200};
    static const uint32_t ht40[2] = {135000, 150000};
    static const uint32_t vht20[2] = {78000, 86700};
    static const uint32_t vht40[2] = {180000, 200000};
    static const uint32_t vht80[2] = {390000, 433300};
    static const uint32_t vht160[2] = {780000, 866700};
    const unsigned nss = ap->vht && ap->vht_nss ? ap->vht_nss : ap->ht_nss;
    const unsigned streams = nss < SWLAN_OUR_NSS ? nss : SWLAN_OUR_NSS;
    if (!streams || !ap->ht)
        return 0;
    bool sgi = ap->sgi20;
    if (bw_mhz >= 40)
        sgi = ap->sgi40;
    if (bw_mhz >= 80)
        sgi = ap->sgi80;
    const uint32_t *row = bw_mhz >= 40 ? ht40 : ht20;
    if (ap->vht) {
        row = vht20;
        if (bw_mhz >= 40)
            row = vht40;
        if (bw_mhz >= 80)
            row = vht80;
        if (bw_mhz >= 160)
            row = vht160;
    }
    return row[sgi ? 1 : 0] * streams;
}

static inline int swlan_peer_describe(const struct swlan_peer *ap, char *out, size_t size)
{
    if (!size)
        return 0;
    if (!ap->ht)
        return snprintf(out, size, "legacy (no HT)");
    const bool htSgi = ap->ht_width >= 40 ? ap->sgi40 : ap->sgi20;
    const unsigned htWidth = ap->ht_width ? ap->ht_width : 20;
    int n = snprintf(out, size, "HT %u SS %u MHz%s", ap->ht_nss, htWidth, htSgi ? " SGI" : "");
    size_t at = n > 0 ? (size_t)n : 0;
    if (ap->vht && at < size) {
        const unsigned vhtWidth = ap->vht_width ? ap->vht_width : htWidth;
        n = snprintf(out + at, size - at, ", VHT %u SS %u MHz%s", ap->vht_nss, vhtWidth, ap->sgi80 ? " SGI" : "");
        at += n > 0 ? (size_t)n : 0;
    }
    if (ap->he && at < size) {
        n = snprintf(out + at, size - at, ", HE");
        at += n > 0 ? (size_t)n : 0;
    }
    return (int)(at < size ? at : size - 1);
}

// "name value" in a firmware dump's text (wlc_ampdu_dump: "txampdu 1234 txmpdu 5678 ..."): the number after `name`
// as a whole word. False when the name is not there.
static inline bool swlan_dump_value(const char *text, size_t length, const char *name, uint32_t *value)
{
    const size_t n = strlen(name);
    for (size_t at = 0; at + n < length && text[at]; ++at) {
        if (memcmp(text + at, name, n) != 0)
            continue;
        const char before = at ? text[at - 1] : ' ';
        if ((before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') || before == '_')
            continue;
        size_t p = at + n;
        if (p >= length || (text[p] != ' ' && text[p] != ':' && text[p] != '='))
            continue;
        while (p < length && (text[p] == ' ' || text[p] == ':' || text[p] == '='))
            ++p;
        if (p >= length || text[p] < '0' || text[p] > '9')
            continue;
        uint32_t v = 0;
        while (p < length && text[p] >= '0' && text[p] <= '9')
            v = v * 10 + (uint32_t)(text[p++] - '0');
        *value = v;
        return true;
    }
    return false;
}

// A tenths figure for a log line without floating point (the kernel is built -mgeneral-regs-only): a / b as "12.3".
static inline int swlan_tenths(uint64_t a, uint64_t b, char *out, size_t size)
{
    if (!b)
        return snprintf(out, size, "-");
    const uint64_t t = (a * 10 + b / 2) / b;
    return snprintf(out, size, "%llu.%llu", (unsigned long long)(t / 10), (unsigned long long)(t % 10));
}

#ifdef __cplusplus
}
#endif

#endif
