// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wi-Fi/Bluetooth coexistence on the BCM4356 combo chip (one 2.4 GHz antenna for both radios): what the Bluetooth
// controller has been asked to do with the air, as SwitchBluetooth sees bluetoothd's commands go out, kept so that
// SwitchWLAN can print it next to its own traffic (its 5 s stats line) and both drivers can log a change once. Plain C
// on byte buffers, so test/switch_coex_test.cpp drives it on the host. The includer declares snprintf (IOLib.h in the
// kernel, stdio.h on the host).
//
// What is followed (Bluetooth Core 4.2 parameters, little-endian; times in 0.625 ms slots):
// - LE Set Scan Parameters (0x200B) {type, interval, window, own address type, filter policy} and LE Set Scan Enable
//   (0x200C) {enable, filter duplicates}: the LE scan's duty cycle, window / interval. bluetoothd moves it between
//   30/300 ms (10%), 30/40 ms (75%) and 30/30 ms (100%) as its scan agents come and go (r7v 11-05 12:33-12:38). The
//   controller is 4.2, so the extended forms (0x2041/0x2042) are followed too only in case a later stack sends them.
// - Write Scan Enable (0x0C1A) and Write Page/Inquiry Scan Activity (0x0C1C/0x0C1E): the classic scans' duty cycles.
// - Broadcom's Set ACL Priority (0xFC57) {handle, priority}: bluetoothd's BD_VSC_WRITE_HI_PRIO_CONN, sent with
//   priority 1 when A2DP starts on a link and 0 when it stops (r7v 12:36:27.145 [0b 00 01] and 12:36:31.461); a
//   high-priority ACL is what the controller defends against Wi-Fi in its coexistence arbitration. AOSP's l2cap uses
//   0xFF for high; any non-zero byte counts as high here.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWITCH_COEX_OP_WRITE_SCAN_ENABLE = 0x0c1a,
    SWITCH_COEX_OP_PAGE_SCAN_ACTIVITY = 0x0c1c,
    SWITCH_COEX_OP_INQUIRY_SCAN_ACTIVITY = 0x0c1e,
    SWITCH_COEX_OP_LE_SCAN_PARAMS = 0x200b,
    SWITCH_COEX_OP_LE_SCAN_ENABLE = 0x200c,
    SWITCH_COEX_OP_LE_EXT_SCAN_PARAMS = 0x2041,
    SWITCH_COEX_OP_LE_EXT_SCAN_ENABLE = 0x2042,
    SWITCH_COEX_OP_BCM_ACL_PRIORITY = 0xfc57,
};

// What a command changed (switch_coex_bt_command): which part of the state to log.
enum {
    SWITCH_COEX_LE_PARAMS = 1u << 0,
    SWITCH_COEX_LE_ENABLE = 1u << 1,
    SWITCH_COEX_CLASSIC_SCAN = 1u << 2,
    SWITCH_COEX_PRIORITY = 1u << 3,
};

#define SWITCH_COEX_NO_HANDLE 0xffffu

struct switch_coex_bt {
    uint16_t le_interval, le_window;  // slots; 0 until bluetoothd set them
    uint8_t le_active;                // scan type 1 (scan requests sent), else passive
    uint8_t le_on;                    // LE Set Scan Enable
    uint8_t scan_enable;              // Write Scan Enable: bit 0 inquiry scan, bit 1 page scan
    uint8_t priority_high;            // the last Set ACL Priority said high
    uint16_t page_interval, page_window, inquiry_interval, inquiry_window;  // slots; 0 until set
    uint16_t priority_handle;  // the handle of the last Set ACL Priority that said high, else SWITCH_COEX_NO_HANDLE
    uint32_t changes;          // commands that changed any of this
};

static inline void switch_coex_bt_reset(struct switch_coex_bt *s)
{
    const struct switch_coex_bt empty = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, SWITCH_COEX_NO_HANDLE, 0};
    *s = empty;
}

static inline uint16_t switch_coex_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (unsigned)p[1] << 8);
}

// One command of the client's on its way to the controller: the state it sets. Returns the SWITCH_COEX_* parts it
// changed (0 for anything else, a short command, or the same values again).
static inline unsigned switch_coex_bt_command(struct switch_coex_bt *s, uint16_t opcode, const uint8_t *p,
                                              uint32_t length)
{
    const struct switch_coex_bt was = *s;
    unsigned changed = 0;
    switch (opcode) {
    case SWITCH_COEX_OP_LE_SCAN_PARAMS:
        if (length < 5)
            return 0;
        s->le_active = p[0] == 1 ? 1 : 0;
        s->le_interval = switch_coex_le16(p + 1);
        s->le_window = switch_coex_le16(p + 3);
        break;
    case SWITCH_COEX_OP_LE_EXT_SCAN_PARAMS:  // own address, filter, PHYs, then per PHY {type, interval, window}
        if (length < 8)
            return 0;
        s->le_active = p[3] == 1 ? 1 : 0;
        s->le_interval = switch_coex_le16(p + 4);
        s->le_window = switch_coex_le16(p + 6);
        break;
    case SWITCH_COEX_OP_LE_SCAN_ENABLE:
    case SWITCH_COEX_OP_LE_EXT_SCAN_ENABLE:
        if (length < 1)
            return 0;
        s->le_on = p[0] ? 1 : 0;
        break;
    case SWITCH_COEX_OP_WRITE_SCAN_ENABLE:
        if (length < 1)
            return 0;
        s->scan_enable = p[0] & 3u;
        break;
    case SWITCH_COEX_OP_PAGE_SCAN_ACTIVITY:
    case SWITCH_COEX_OP_INQUIRY_SCAN_ACTIVITY:
        if (length < 4)
            return 0;
        if (opcode == SWITCH_COEX_OP_PAGE_SCAN_ACTIVITY) {
            s->page_interval = switch_coex_le16(p);
            s->page_window = switch_coex_le16(p + 2);
        } else {
            s->inquiry_interval = switch_coex_le16(p);
            s->inquiry_window = switch_coex_le16(p + 2);
        }
        break;
    case SWITCH_COEX_OP_BCM_ACL_PRIORITY: {
        if (length < 3)
            return 0;
        const uint16_t handle = (uint16_t)(switch_coex_le16(p) & 0x0fffu);
        if (p[2]) {
            s->priority_high = 1;
            s->priority_handle = handle;
        } else if (s->priority_handle == handle || s->priority_handle == SWITCH_COEX_NO_HANDLE) {
            s->priority_high = 0;
            s->priority_handle = SWITCH_COEX_NO_HANDLE;
        }
        break;
    }
    default:
        return 0;
    }
    if (s->le_interval != was.le_interval || s->le_window != was.le_window || s->le_active != was.le_active)
        changed |= SWITCH_COEX_LE_PARAMS;
    if (s->le_on != was.le_on)
        changed |= SWITCH_COEX_LE_ENABLE;
    if (s->scan_enable != was.scan_enable || s->page_interval != was.page_interval ||
        s->page_window != was.page_window || s->inquiry_interval != was.inquiry_interval ||
        s->inquiry_window != was.inquiry_window)
        changed |= SWITCH_COEX_CLASSIC_SCAN;
    if (s->priority_high != was.priority_high || s->priority_handle != was.priority_handle)
        changed |= SWITCH_COEX_PRIORITY;
    if (changed)
        ++s->changes;
    return changed;
}

// A scan's duty cycle in percent (window over interval, at most 100); 0 when either is unknown.
static inline unsigned switch_coex_duty(uint16_t interval, uint16_t window)
{
    if (!interval || !window)
        return 0;
    const unsigned duty = (unsigned)window * 100u / interval;
    return duty > 100 ? 100 : duty;
}

// Slots as whole milliseconds (0.625 ms each), for a line.
static inline unsigned switch_coex_ms(uint16_t slots)
{
    return (unsigned)slots * 5u / 8u;
}

// The LE scan as one phrase: "LE scan on 30/300 ms passive (10%)", "LE scan off (30/300 ms)", "LE scan unset".
static inline int switch_coex_describe_le(const struct switch_coex_bt *s, char *out, size_t size)
{
    if (!s->le_interval)
        return snprintf(out, size, "LE scan %s, unset", s->le_on ? "on" : "off");
    if (!s->le_on) {
        return snprintf(out, size, "LE scan off (%u/%u ms)", switch_coex_ms(s->le_window),
                        switch_coex_ms(s->le_interval));
    }
    return snprintf(out, size, "LE scan on %u/%u ms %s (%u%%)", switch_coex_ms(s->le_window),
                    switch_coex_ms(s->le_interval), s->le_active ? "active" : "passive",
                    switch_coex_duty(s->le_interval, s->le_window));
}

// Everything, for a stats line: the LE scan, the classic scans that are on, the high-priority ACL.
// "LE scan on 30/300 ms passive (10%), page scan 11/1280 ms, ACL 000b high priority"
static inline int switch_coex_describe(const struct switch_coex_bt *s, char *out, size_t size)
{
    if (!size)
        return 0;
    out[0] = 0;
    int n = switch_coex_describe_le(s, out, size);
    size_t at = n > 0 ? (size_t)n : 0;
    if (at < size && (s->scan_enable & 2u)) {
        n = snprintf(out + at, size - at, ", page scan %u/%u ms", switch_coex_ms(s->page_window),
                     switch_coex_ms(s->page_interval));
        at += n > 0 ? (size_t)n : 0;
    }
    if (at < size && (s->scan_enable & 1u)) {
        n = snprintf(out + at, size - at, ", inquiry scan %u/%u ms", switch_coex_ms(s->inquiry_window),
                     switch_coex_ms(s->inquiry_interval));
        at += n > 0 ? (size_t)n : 0;
    }
    if (at < size && s->priority_high) {
        n = snprintf(out + at, size - at, ", ACL %04x high priority", s->priority_handle);
        at += n > 0 ? (size_t)n : 0;
    }
    return (int)(at < size ? at : size - 1);
}

// ---- AFH: keeping Bluetooth off the Wi-Fi channel -------------------------------------------------------------
//
// HCI Set AFH Host Channel Classification (0x0C3F) {79 bits, 1 = unknown (usable), 0 = bad; bit 79 reserved 0}: the
// host's say in which of the 79 1 MHz channels (2402 + n MHz) a link the controller is central of may hop on. No
// stack sends it for this chip (Linux btbcm, Android's libbt-vendor and switchroot's btlinux HAL do not; bluetoothd
// only has it as a client API nobody called on r7v), and whether the Wi-Fi firmware's "btafh" feature tells the
// Bluetooth core its channel over the chip's internal coexistence bus is not visible from the host, so SwitchBluetooth sends it itself from SwitchWLAN's infra channel: a 2.4 GHz channel's 20 MHz
// plus 1 MHz either side is marked bad (channel 6, 2437 MHz: Bluetooth channels 24-46, 56 left; the spec wants at
// least 20). On a 5 GHz network, or none, every channel is usable again. Read AFH Channel Map (0x1406) {handle} ->
// {status, handle, mode, map[10]} shows what a link hops on.
enum {
    SWITCH_COEX_OP_SET_AFH_CLASSIFICATION = 0x0c3f,
    SWITCH_COEX_OP_READ_AFH_MAP = 0x1406
};

#define SWITCH_COEX_AFH_CHANNELS 79u

// The classification for a Wi-Fi channel (MHz; 0 for none); *first/*last get the channels marked bad (both
// SWITCH_COEX_AFH_CHANNELS when none). Returns how many stay usable.
static inline unsigned switch_coex_afh_map(uint32_t wifi_mhz, uint8_t map[10], unsigned *first, unsigned *last)
{
    unsigned lo = SWITCH_COEX_AFH_CHANNELS;
    unsigned hi = SWITCH_COEX_AFH_CHANNELS;
    if (wifi_mhz >= 2412 && wifi_mhz <= 2484) {
        const int a = (int)wifi_mhz - 11 - 2402;
        const int b = (int)wifi_mhz + 11 - 2402;
        lo = a < 0 ? 0u : (unsigned)a;
        hi = b > 78 ? 78u : (unsigned)b;
    }
    unsigned usable = 0;
    for (unsigned n = 0; n < 80; ++n) {
        const bool bad = n >= SWITCH_COEX_AFH_CHANNELS || (lo < SWITCH_COEX_AFH_CHANNELS && n >= lo && n <= hi);
        if (!(n & 7))
            map[n / 8] = 0;
        if (!bad) {
            map[n / 8] = (uint8_t)(map[n / 8] | 1u << (n & 7));
            ++usable;
        }
    }
    if (first)
        *first = lo;
    if (last)
        *last = hi;
    return usable;
}

// A link's AFH map (Read AFH Channel Map's 10 bytes): how many channels it hops on, and how many of [first, last]
// (the Wi-Fi channel's) it still uses.
static inline unsigned switch_coex_afh_used(const uint8_t map[10], unsigned first, unsigned last, unsigned *in_range)
{
    unsigned used = 0;
    unsigned inside = 0;
    for (unsigned n = 0; n < SWITCH_COEX_AFH_CHANNELS; ++n) {
        if (!(map[n / 8] & (1u << (n & 7))))
            continue;
        ++used;
        if (n >= first && n <= last)
            ++inside;
    }
    if (in_range)
        *in_range = inside;
    return used;
}

// SwitchBluetooth.cpp: the controller's state as above plus its links, for SwitchWLAN's stats line. Fills *state and
// writes the links as "3 ACL, 1 active (000b)"; returns false while no client (bluetoothd) is connected. Any thread.
bool switch_bluetooth_coex(struct switch_coex_bt *state, char *links, size_t size);

#ifdef __cplusplus
}
#endif
