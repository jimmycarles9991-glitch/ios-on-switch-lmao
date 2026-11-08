// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which of a rail Joy-Con's 0x30 reports reach its HID device (SwitchJoyConRail.cpp runningReport; the host test
// test/switch_joycon_forward_test.cpp drives this header).
//
// The rail asks for a report every 15 ms because a rail Joy-Con only speaks when spoken to, so two seated Joy-Cons
// answer about 133 times a second. While JoyConHIDServicePlugin has the Joy-Con in input report mode 0x3F ("simple
// HID"), a Bluetooth Joy-Con sends a report only when a button or the stick changes; each report handed to the HID
// device wakes backboardd's HID event system. So in that mode a report goes on when its buttons, battery or a stick
// axis (by more than the stick's resting noise) differ from the last one handed on, and at least once a second.
// In the full mode (0x30, which the plugin asks for when a game wants every report) and while the IMU is on
// (subcommand 0x40: its samples change every report), every report goes on.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_FORWARD_STICK_NOISE  8u  // of 4096: above the stick's jitter at rest, far below one degree
#define SWITCH_JOYCON_FORWARD_KEEPALIVE_MS 1000u

struct switch_joycon_forward {
    bool simple;       // the plugin's last input report mode was 0x3F
    bool imu;          // the plugin's last IMU subcommand turned it on
    bool primed;       // `last` holds a report handed on since the device was published
    uint8_t last[10];  // its battery byte, buttons (3) and sticks (6): report bytes 2..11
    uint64_t last_ms;
};

static inline unsigned switch_joycon_axis(const uint8_t *stick, unsigned axis)
{
    return axis == 0 ? (stick[0] | (stick[1] & 0x0fu) << 8) : ((unsigned)stick[1] >> 4 | (unsigned)stick[2] << 4);
}

static inline bool switch_joycon_forward_moved(const uint8_t *was, const uint8_t *now)
{
    for (unsigned stick = 0; stick < 2; ++stick) {
        for (unsigned axis = 0; axis < 2; ++axis) {
            const unsigned a = switch_joycon_axis(was + static_cast<size_t>(3) * stick, axis);
            const unsigned b = switch_joycon_axis(now + static_cast<size_t>(3) * stick, axis);
            if ((a > b ? a - b : b - a) > SWITCH_JOYCON_FORWARD_STICK_NOISE)
                return true;
        }
    }
    return false;
}

// A new HID device starts in the full mode, as the Joy-Con does.
static inline void switch_joycon_forward_reset(struct switch_joycon_forward *f)
{
    f->simple = false;
    f->imu = false;
    f->primed = false;
    f->last_ms = 0;
}

// An output report the plugin sent: subcommand 0x03 sets the input report mode, 0x40 the IMU.
static inline void switch_joycon_forward_output(struct switch_joycon_forward *f, const uint8_t *report, size_t length)
{
    if (length < 12 || report[0] != 0x01)
        return;
    if (report[10] == 0x03) {
        f->simple = report[11] == 0x3f;
    } else if (report[10] == 0x40) {
        f->imu = report[11] != 0;
    }
}

// True: hand this input report to the HID device. Only 0x30 reports are ever held back.
static inline bool switch_joycon_forward_report(struct switch_joycon_forward *f, const uint8_t *report, size_t length,
                                                uint64_t now_ms)
{
    if (length < 12 || report[0] != 0x30 || !f->simple || f->imu)
        return true;
    const uint8_t *now = report + 2;
    const bool changed = !f->primed || f->last[0] >> 4 != now[0] >> 4 || f->last[1] != now[1] || f->last[2] != now[2] ||
                         f->last[3] != now[3] || switch_joycon_forward_moved(f->last + 4, now + 4);
    if (!changed && now_ms - f->last_ms < SWITCH_JOYCON_FORWARD_KEEPALIVE_MS)
        return false;
    for (unsigned i = 0; i < sizeof(f->last); ++i)
        f->last[i] = now[i];
    f->last_ms = now_ms;
    f->primed = true;
    return true;
}

#ifdef __cplusplus
}
#endif
