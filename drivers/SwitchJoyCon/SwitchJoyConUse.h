// SPDX-License-Identifier: BSD-2-Clause-Patent
// When the rail Joy-Cons are a game controller in use (SwitchJoyConRail.cpp; the host test
// test/switch_joycon_use_test.cpp drives this header).
//
// iPadOS treats a connected gamepad as an input the user navigates with. UIKitCore 24B119 sets up a focus system in
// every scene of an app linked on or after iOS 26 (_UISupportsGameControllers) whenever an IOHIDDevice with usage pair
// Generic Desktop/Game Pad or Joystick exists (_UIFocusGameControllerObserver: an IOHIDManager on those pairs, active
// while it holds a device), and its programmatic focus updates then run every second. A Bluetooth controller is such a
// device only from the press that connects it until GameController disconnects it after 900 s without input
// (GCGamepadHIDServicePlugin defaultIdleTimeoutDuration; -[_GCNintendoJoyConDevice(Idle) requestIdleDisconnect:]
// asks bluetoothd, a no-op for a Joy-Con on the rail). The rail pair keeps that lifecycle: its gamepad devices exist
// from a gamepad press or stick push on either Joy-Con until both have been idle that long. Home and Capture are the
// iPad's Home button and screenshot key and never count as gamepad use.
//
// A stick push is the stick leaving its calibrated dead zone (SwitchJoyConStick.h) after it was back within half of
// it, or, while it stays out, moving a dead zone's width from where the last push was counted. Noise and drift inside
// the dead zone never count, a stick that rests outside it (a worn one) counts nothing until it moves that far, and
// noise at the dead zone's edge cannot count twice.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchJoyConStick.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_USE_IDLE_MS 900000u

// Standard input bytes 3..5: right buttons (Y X B A SR SL R ZR), shared (- + RS LS Home Capture, grip 0x80), left
// buttons (down up right left SR SL L ZL). SL and SR face the rail.
#define SWITCH_JOYCON_USE_RIGHT  0xcfu
#define SWITCH_JOYCON_USE_SHARED 0x0fu
#define SWITCH_JOYCON_USE_LEFT   0xcfu

struct switch_joycon_use {
    bool seen;   // `pushed_at` holds a position
    bool armed;  // the stick was back within half its dead zone since the last push counted
    uint16_t pushed_at[2];
};

static inline void switch_joycon_use_reset(struct switch_joycon_use *u)
{
    u->seen = false;
    u->armed = false;
}

static inline bool switch_joycon_use_buttons(const uint8_t *report)
{
    return (report[3] & SWITCH_JOYCON_USE_RIGHT) || (report[4] & SWITCH_JOYCON_USE_SHARED) ||
           (report[5] & SWITCH_JOYCON_USE_LEFT);
}

// True: this report (0x21, 0x30 or 0x31) is the user playing.
static inline bool switch_joycon_use_report(struct switch_joycon_use *u, const struct switch_joycon_stick *s,
                                            const uint8_t *report, size_t length)
{
    if (length < 12 || (report[0] != 0x21 && report[0] != 0x30 && report[0] != 0x31))
        return false;
    if (switch_joycon_use_buttons(report))
        return true;
    const uint16_t now[2] = {(uint16_t)switch_joycon_stick_axis(s, report, 0),
                             (uint16_t)switch_joycon_stick_axis(s, report, 1)};
    if (switch_joycon_stick_within(s, report, s->centre, s->dead_zone)) {
        if (switch_joycon_stick_within(s, report, s->centre, s->dead_zone / 2u))
            u->armed = true;
        return false;
    }
    const bool pushed = u->armed || (u->seen && !switch_joycon_stick_within(s, report, u->pushed_at, s->dead_zone));
    if (pushed || !u->seen) {
        u->pushed_at[0] = now[0];
        u->pushed_at[1] = now[1];
        u->seen = true;
    }
    u->armed = false;
    return pushed;
}

static inline bool switch_joycon_use_idle(uint64_t last_use_ms, uint64_t now_ms)
{
    return now_ms - last_use_ms >= SWITCH_JOYCON_USE_IDLE_MS;
}

#ifdef __cplusplus
}
#endif
