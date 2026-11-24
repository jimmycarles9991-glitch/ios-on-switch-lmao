/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "beacon.h"
#include "rx_status.h"
#include "channel.h"
#include <stddef.h>

static struct {
    void *wlc, *bsscfg, *information;
    struct switch_a3_beacon_anchor anchor;
    uint64_t next_tbtt;
    uint16_t interval;
    uint8_t dtim_period, dtim_count, multicast;
} switch_a3_beacon;

static __attribute__((noinline)) uint16_t switch_a3_beacon_read16(const void *address)
{
    const uint8_t *bytes = address;
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void *switch_a3_beacon_pointer(const void *base, size_t offset)
{
    return *(void *const *)((const uint8_t *)base + offset);
}

static int switch_a3_beacon_equal(const uint8_t *left, const uint8_t *right)
{
    for (unsigned index = 0; index < 6; ++index)
        if (left[index] != right[index])
            return 0;
    return 1;
}

static __attribute__((noinline)) uint32_t switch_a3_beacon_phase(uint64_t elapsed, uint32_t period)
{
    uint32_t remainder = 0;
    for (unsigned bit = 0; bit < 64; ++bit) {
        remainder = (remainder << 1) | (uint32_t)(elapsed >> 63);
        elapsed <<= 1;
        if (remainder >= period)
            remainder -= period;
    }
    return remainder;
}

void switch_a3_beacon_reset(void *wlc)
{
    if (!wlc || switch_a3_beacon.wlc == wlc)
        switch_a3_beacon.wlc = NULL;
}

static void *switch_a3_beacon_station(void *wlc)
{
    if (!wlc || !*((const uint8_t *)wlc + 25))
        return NULL;
    void *bsscfg = switch_a3_station_bsscfg(wlc);
    return bsscfg && *((const uint8_t *)bsscfg + 8) ? bsscfg : NULL;
}

int switch_a3_beacon_observe(void *wlc, const void *status, uint32_t status_bytes, const uint8_t *frame,
                             uint32_t frame_bytes, uint64_t received_tsf)
{
    void *bsscfg = switch_a3_beacon_station(wlc);
    if (!bsscfg || !status || status_bytes < 32 || !frame || frame_bytes < 42 || !received_tsf)
        return 0;
    const uint8_t *bssid = (const uint8_t *)bsscfg + 216;
    if ((switch_a3_beacon_read16(frame) & 0x47ffu) != 0x80u || (switch_a3_beacon_read16(frame + 22) & 15u) ||
        !switch_a3_beacon_equal(frame + 10, bssid) || !switch_a3_beacon_equal(frame + 16, bssid))
        return 0;
    if (!switch_a3_rx_phy_valid(wlc, status))
        return 0;
    const uint16_t chanspec = switch_a3_beacon_read16((const uint8_t *)wlc + 0x466);
    const uint16_t received_channel = switch_a3_beacon_read16((const uint8_t *)status + 22);
    const uint16_t primary = switch_a3_channel_control(chanspec);
    if (!chanspec || !received_channel || !primary || primary == UINT16_MAX ||
        switch_a3_channel_control(received_channel) != primary)
        return 0;
    void *information = switch_a3_beacon_pointer(bsscfg, 0x110);
    const uint16_t interval = switch_a3_beacon_read16(frame + 32);
    if (!information || !interval || interval != switch_a3_beacon_read16((const uint8_t *)information + 0x2e))
        return 0;
    const uint8_t *tim = NULL;
    for (uint32_t offset = 36; offset + 2 <= frame_bytes;) {
        const uint32_t length = frame[offset + 1];
        if (length > frame_bytes - offset - 2)
            return 0;
        if (frame[offset] == 5) {
            if (length < 4 || length > 254)
                return 0;
            tim = frame + offset + 2;
            if (!tim[1] || tim[0] >= tim[1] || (uint32_t)(tim[2] & 0xfeu) + length - 3 > 251)
                return 0;
            break;
        }
        offset += length + 2;
    }
    if (!tim || tim[1] != *((const uint8_t *)information + 0x60))
        return 0;
    uint64_t timestamp = 0;
    for (unsigned index = 8; index; --index)
        timestamp = (timestamp << 8) | frame[23 + index];
    if (switch_a3_beacon.wlc == wlc && switch_a3_beacon.bsscfg == bsscfg &&
        switch_a3_beacon_equal(switch_a3_beacon.anchor.bssid, bssid) && received_tsf >= switch_a3_beacon.anchor.radio_tsf &&
        timestamp <= switch_a3_beacon.anchor.ap_tsf)
        return 0;
    const uint32_t interval_us = (uint32_t)interval * 1024u;
    const uint32_t remaining = interval_us - switch_a3_beacon_phase(timestamp, interval_us);
    if (received_tsf > UINT64_MAX - remaining)
        return 0;
    switch_a3_beacon.wlc = wlc;
    switch_a3_beacon.bsscfg = bsscfg;
    switch_a3_beacon.information = information;
    switch_a3_beacon.anchor.radio_tsf = received_tsf;
    switch_a3_beacon.next_tbtt = received_tsf + remaining;
    switch_a3_beacon.anchor.ap_tsf = timestamp;
    switch_a3_beacon.anchor.chanspec = chanspec;
    switch_a3_beacon.interval = interval;
    switch_a3_beacon.dtim_count = tim[0];
    switch_a3_beacon.dtim_period = tim[1];
    switch_a3_beacon.multicast = tim[2] & 1u;
    for (unsigned index = 0; index < 6; ++index)
        switch_a3_beacon.anchor.bssid[index] = bssid[index];
    return 1;
}

static void *switch_a3_beacon_current(void *wlc)
{
    void *current = switch_a3_beacon_station(wlc);
    if (!current || switch_a3_beacon.wlc != wlc || switch_a3_beacon.bsscfg != current ||
        switch_a3_beacon.anchor.chanspec != switch_a3_beacon_read16((const uint8_t *)wlc + 0x466) ||
        !switch_a3_beacon_equal(switch_a3_beacon.anchor.bssid, (const uint8_t *)current + 216) ||
        switch_a3_beacon.information != switch_a3_beacon_pointer(current, 0x110))
        return NULL;
    return current;
}

#if DEVELOPMENT || DEBUG
const struct switch_a3_beacon_anchor *switch_a3_beacon_anchor(void *wlc)
{
    return switch_a3_beacon_current(wlc) ? &switch_a3_beacon.anchor : NULL;
}
#endif

int switch_a3_beacon_bound(void *wlc, uint16_t home_chanspec, uint64_t now, void **bsscfg, uint64_t *return_before_tsf)
{
    if (!bsscfg || !return_before_tsf)
        return -2;
    *bsscfg = NULL;
    *return_before_tsf = 0;
    void *current = switch_a3_beacon_current(wlc);
    if (!current || switch_a3_channel_control(home_chanspec) != switch_a3_channel_control(switch_a3_beacon.anchor.chanspec))
        return -25;
    const uint8_t *information = switch_a3_beacon.information;
    if (switch_a3_beacon.interval != switch_a3_beacon_read16(information + 0x2e) ||
        switch_a3_beacon.dtim_period != information[0x60] || now < switch_a3_beacon.anchor.radio_tsf ||
        now >= switch_a3_beacon.next_tbtt || (!switch_a3_beacon.dtim_count && switch_a3_beacon.multicast))
        return -16;
    *bsscfg = current;
    *return_before_tsf = switch_a3_beacon.next_tbtt;
    return 0;
}
