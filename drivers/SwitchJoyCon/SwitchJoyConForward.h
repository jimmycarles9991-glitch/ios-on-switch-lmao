// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which of a rail Joy-Con's reports reach its HID device (SwitchJoyConRail.cpp runningReport; the host test
// test/switch_joycon_forward_test.cpp drives this header).
//
// While active (stick outside its dead zone, transitioning back to center, or any button held), every 0x30 report with
// changed buttons, battery or raw stick bytes is forwarded losslessly (zero quantization). Suppression applies only to
// a truly resting stick that has settled inside its dead zone with no buttons held. Replies (0x21), MCU reports (0x31),
// and all 0x30 reports while IMU motion is requested (subcommand 0x40 non-zero) always go on.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchJoyConStick.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_FORWARD_SETTLE_TICKS  4u
#define SWITCH_JOYCON_FORWARD_SETTLE_JITTER 8u

struct switch_joycon_forward {
    bool imu;      // a client asked for motion: the plugin's last subcommand 0x40 was non-zero
    bool primed;   // `last` holds a report handed on since the device was published
    bool settled;  // stick is truly resting inside its dead zone with no buttons held
    uint8_t still;
    uint16_t settle_pos[2];
    uint8_t last[12];  // the last report handed on, up to its sticks
};

static inline bool switch_joycon_buttons_held(const uint8_t *report)
{
    return (report[3] | (report[4] & 0x7fu) | report[5]) != 0u;
}

static inline bool switch_joycon_header_changed(const uint8_t *was, const uint8_t *now)
{
    return (was[2] >> 4) != (now[2] >> 4) || was[3] != now[3] || was[4] != now[4] || was[5] != now[5];
}

static inline bool switch_joycon_stick_same_raw(const struct switch_joycon_stick *s, const uint8_t *a, const uint8_t *b)
{
    const unsigned at = s->left ? 6u : 9u;
    return a[at] == b[at] && a[at + 1u] == b[at + 1u] && a[at + 2u] == b[at + 2u];
}

// A new HID device: its plugin has asked for nothing yet.
static inline void switch_joycon_forward_reset(struct switch_joycon_forward *f)
{
    f->imu = false;
    f->primed = false;
    f->settled = false;
    f->still = 0;
    f->settle_pos[0] = 0;
    f->settle_pos[1] = 0;
}

// Drops primed state while preserving `imu` (used to replay the live state after 0x03 mode switch).
static inline void switch_joycon_forward_reprime(struct switch_joycon_forward *f)
{
    f->primed = false;
    f->settled = false;
    f->still = 0;
}

// An output report the plugin sent: subcommand 0x40 turns the IMU on or off.
static inline void switch_joycon_forward_output(struct switch_joycon_forward *f, const uint8_t *report, size_t length)
{
    if (length >= 12u && report[0] == 0x01u && report[10] == 0x40u)
        f->imu = report[11] != 0u;
}

static inline void switch_joycon_forward_latch(struct switch_joycon_forward *f, const uint8_t *report)
{
    for (unsigned i = 0; i < sizeof(f->last); ++i)
        f->last[i] = report[i];
    f->primed = true;
}

// True: hand this input report to the HID device. Only settled resting 0x30 reports or identical duplicates are held back.
static inline bool switch_joycon_forward_report(struct switch_joycon_forward *f, const struct switch_joycon_stick *s,
                                                const uint8_t *report, size_t length)
{
    if (length < sizeof(f->last) || report[0] != 0x30u || f->imu)
        return true;
    const bool in_dz = switch_joycon_stick_within(s, report, s->centre, s->dead_zone);
    const bool buttons = switch_joycon_buttons_held(report);
    const uint16_t pos[2] = {(uint16_t)switch_joycon_stick_axis(s, report, 0),
                             (uint16_t)switch_joycon_stick_axis(s, report, 1)};
    if (!f->primed) {
        switch_joycon_forward_latch(f, report);
        f->settled = in_dz && !buttons;
        f->still = 0;
        f->settle_pos[0] = pos[0];
        f->settle_pos[1] = pos[1];
        return true;
    }
    const bool header = switch_joycon_header_changed(f->last, report);
    if (!in_dz || buttons) {
        f->settled = false;
        f->still = 0;
        if (!header && switch_joycon_stick_same_raw(s, f->last, report))
            return false;
        switch_joycon_forward_latch(f, report);
        return true;
    }
    if (!f->settled) {
        const unsigned jitter = s->dead_zone / 4u < SWITCH_JOYCON_FORWARD_SETTLE_JITTER
                                    ? s->dead_zone / 4u
                                    : SWITCH_JOYCON_FORWARD_SETTLE_JITTER;
        if (f->still == 0u || !switch_joycon_stick_within(s, report, f->settle_pos, jitter)) {
            f->settle_pos[0] = pos[0];
            f->settle_pos[1] = pos[1];
            f->still = 1u;
        } else if (++f->still >= SWITCH_JOYCON_FORWARD_SETTLE_TICKS) {
            f->settled = true;
        }
        if (!header && switch_joycon_stick_same_raw(s, f->last, report))
            return false;
        switch_joycon_forward_latch(f, report);
        return true;
    }
    const uint16_t anchor[2] = {(uint16_t)switch_joycon_stick_axis(s, f->last, 0),
                                (uint16_t)switch_joycon_stick_axis(s, f->last, 1)};
    const bool moved = !switch_joycon_stick_within(s, report, anchor, s->dead_zone / 4u);
    if (!header && !moved)
        return false;
    if (moved) {
        f->settled = false;
        f->still = 1u;
        f->settle_pos[0] = pos[0];
        f->settle_pos[1] = pos[1];
    }
    switch_joycon_forward_latch(f, report);
    return true;
}

#ifdef __cplusplus
}
#endif
