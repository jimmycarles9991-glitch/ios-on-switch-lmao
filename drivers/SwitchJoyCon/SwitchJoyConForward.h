// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which of a rail Joy-Con's reports reach its HID device (SwitchJoyConRail.cpp runningReport; the host test
// test/switch_joycon_forward_test.cpp drives this header).
//
// The rail asks for a report every 15 or 30 ms (SwitchJoyConCadence.h) because a rail Joy-Con only speaks when spoken
// to, and each report handed to the HID device wakes backboardd's HID event system and JoyConHIDServicePlugin. So a
// 0x30 report goes on only when it says something new: its buttons, its battery level or charging, or its stick moved
// from where it was in the last one handed on, by the Joy-Con's own dead zone (SwitchJoyConStick.h) over 16 while it is
// out of the dead zone (10 counts for 0xAE, about L4T's stick fuzz: joycon-serdev.c 140, 250 of 32767 is 10 counts of a
// Joy-Con's ~1270-count half axis), and over 4 while it rests in it (43 counts: a stick at rest jitters over 7, deku
// packet_parse/j2c.txt 1837-1844 x 2086-2092, and a worn one more; a client sees the dead zone as the centre, so
// nothing in it is news but drift that adds up). The connection bits (byte 2's low nibble) and the timer (byte 1) are
// not news.
//
// No report goes on just to keep the device alive. JoyConHIDServicePlugin's idle clock follows input, not reports
// (boot 17, 24B119: its "isIdle for N seconds" grew to 300 s with the plugin in 0x30 mode and all ~66 reports a second
// handed on), and a Bluetooth Joy-Con in 0x3F sends only on a button change (deku bluetooth_hid_subcommands_notes.md
// 81). That it takes a 0x30-mode Joy-Con falling silent for minutes is the device's to show: HW Test input.joycon_idle,
// and the plugin's log keeping "isIdle" growing with no "Cancel" or "Device not active". While a client wants motion (the plugin's
// subcommand 0x40 with a non-zero argument), every 0x30 report goes on: its IMU samples change every time. Replies (0x21)
// and MCU reports (0x31) always go on.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchJoyConStick.h"

#ifdef __cplusplus
extern "C" {
#endif

struct switch_joycon_forward {
    bool imu;          // a client asked for motion: the plugin's last subcommand 0x40 was non-zero
    bool primed;       // `last` holds a report handed on since the device was published
    uint8_t last[12];  // the last report handed on, up to its sticks
};

// How far the stick may move from `was` to `now` (reports) without news.
static inline unsigned switch_joycon_stick_noise(const struct switch_joycon_stick *s, const uint8_t *was,
                                                 const uint8_t *now)
{
    const bool resting = switch_joycon_stick_within(s, was, s->centre, s->dead_zone) &&
                         switch_joycon_stick_within(s, now, s->centre, s->dead_zone);
    return resting ? s->dead_zone / 4u : s->dead_zone / 16u;
}

// Buttons, battery or the stick differ in a way the plugin would see (both reports at least 12 bytes).
static inline bool switch_joycon_report_news(const struct switch_joycon_stick *s, const uint8_t *was,
                                             const uint8_t *now)
{
    if (was[2] >> 4 != now[2] >> 4 || was[3] != now[3] || was[4] != now[4] || was[5] != now[5])
        return true;
    const uint16_t at[2] = {(uint16_t)switch_joycon_stick_axis(s, was, 0),
                            (uint16_t)switch_joycon_stick_axis(s, was, 1)};
    return !switch_joycon_stick_within(s, now, at, switch_joycon_stick_noise(s, was, now));
}

// A new HID device: its plugin has asked for nothing yet.
static inline void switch_joycon_forward_reset(struct switch_joycon_forward *f)
{
    f->imu = false;
    f->primed = false;
}

// An output report the plugin sent: subcommand 0x40 turns the IMU on or off.
static inline void switch_joycon_forward_output(struct switch_joycon_forward *f, const uint8_t *report, size_t length)
{
    if (length >= 12 && report[0] == 0x01 && report[10] == 0x40)
        f->imu = report[11] != 0;
}

// True: hand this input report to the HID device. Only 0x30 reports are ever held back.
static inline bool switch_joycon_forward_report(struct switch_joycon_forward *f, const struct switch_joycon_stick *s,
                                                const uint8_t *report, size_t length)
{
    if (length < sizeof(f->last) || report[0] != 0x30 || f->imu)
        return true;
    if (f->primed && !switch_joycon_report_news(s, f->last, report))
        return false;
    for (unsigned i = 0; i < sizeof(f->last); ++i)
        f->last[i] = report[i];
    f->primed = true;
    return true;
}

#ifdef __cplusplus
}
#endif
