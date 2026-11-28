// SPDX-License-Identifier: BSD-2-Clause-Patent
// When the rail Joy-Cons are a game controller in use (SwitchJoyConRail.cpp; the host test
// test/switch_joycon_use_test.cpp drives this header).
//
// UIKitCore 24B119 enables a UIFocusSystem in every scene while any Generic Desktop Game Pad / Joystick IOHIDDevice
// exists, and its programmatic focus updates stall the main thread during touch scrolling. Rail gamepads are therefore
// published only on deliberate sustained gamepad engagement (resisting accidental palm/grip brushes), preempted cleanly
// once touch resumes after a short grace window while no motion/rumble is active, and idled after 900 s without input.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchJoyConStick.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_USE_IDLE_MS              900000u
#define SWITCH_JOYCON_USE_TOUCH_GRACE_MS       2500u
#define SWITCH_JOYCON_USE_BUTTON_PUBLISH_TICKS 2u
#define SWITCH_JOYCON_USE_STICK_PUBLISH_TICKS  3u
#define SWITCH_JOYCON_USE_STRONG_PUBLISH_TICKS 2u

// Standard input bytes 3..5: right buttons (Y X B A SR SL R ZR), shared (- + RS LS Home Capture, grip 0x80), left
// buttons (down up right left SR SL L ZL). SL and SR face the rail.
#define SWITCH_JOYCON_USE_RIGHT  0xcfu
#define SWITCH_JOYCON_USE_SHARED 0x0fu
#define SWITCH_JOYCON_USE_LEFT   0xcfu

struct switch_joycon_use {
    bool seen;   // `pushed_at` holds a position
    bool armed;  // the stick was back within half its dead zone since the last push counted
    uint8_t btn_ticks;
    uint8_t stk_ticks;
    uint16_t pushed_at[2];
};

static inline void switch_joycon_use_reset(struct switch_joycon_use *u)
{
    u->seen = false;
    u->armed = false;
    u->btn_ticks = 0;
    u->stk_ticks = 0;
}

static inline bool switch_joycon_use_buttons(const uint8_t *report)
{
    return (report[3] & SWITCH_JOYCON_USE_RIGHT) || (report[4] & SWITCH_JOYCON_USE_SHARED) ||
           (report[5] & SWITCH_JOYCON_USE_LEFT);
}

// Evaluates a standard report (0x21, 0x30 or 0x31). While `published` is false, requires sustained deliberate
// engagement (button held across 2 polls, strong stick push > 2x dead zone across 2 polls, or stick push across
// 3 polls) so accidental grip brushes do not publish gamepads.
static inline bool switch_joycon_use_observe(struct switch_joycon_use *u, const struct switch_joycon_stick *s,
                                             const uint8_t *report, size_t length, bool published)
{
    if (length < 12u || (report[0] != 0x21u && report[0] != 0x30u && report[0] != 0x31u))
        return false;
    if (switch_joycon_use_buttons(report)) {
        u->stk_ticks = 0;
        if (published)
            return true;
        if (u->btn_ticks < 255u)
            ++u->btn_ticks;
        if (u->btn_ticks >= SWITCH_JOYCON_USE_BUTTON_PUBLISH_TICKS) {
            u->btn_ticks = 0;
            return true;
        }
        return false;
    }
    u->btn_ticks = 0;
    const uint16_t now[2] = {(uint16_t)switch_joycon_stick_axis(s, report, 0),
                             (uint16_t)switch_joycon_stick_axis(s, report, 1)};
    if (switch_joycon_stick_within(s, report, s->centre, s->dead_zone)) {
        u->stk_ticks = 0;
        if (switch_joycon_stick_within(s, report, s->centre, s->dead_zone / 2u))
            u->armed = true;
        return false;
    }
    if (published) {
        const bool pushed = u->armed || (u->seen && !switch_joycon_stick_within(s, report, u->pushed_at, s->dead_zone));
        if (pushed || !u->seen) {
            u->pushed_at[0] = now[0];
            u->pushed_at[1] = now[1];
            u->seen = true;
        }
        u->armed = false;
        u->stk_ticks = 0;
        return pushed;
    }
    if (!u->armed) {
        if (!u->seen) {
            u->pushed_at[0] = now[0];
            u->pushed_at[1] = now[1];
            u->seen = true;
            u->stk_ticks = 0;
            return false;
        }
        if (switch_joycon_stick_within(s, report, u->pushed_at, s->dead_zone)) {
            u->stk_ticks = 0;
            return false;
        }
    }
    if (u->stk_ticks < 255u)
        ++u->stk_ticks;
    const bool strong = !switch_joycon_stick_within(s, report, s->centre, (unsigned)s->dead_zone * 2u);
    const uint8_t need = strong ? SWITCH_JOYCON_USE_STRONG_PUBLISH_TICKS : SWITCH_JOYCON_USE_STICK_PUBLISH_TICKS;
    if (u->stk_ticks >= need) {
        u->pushed_at[0] = now[0];
        u->pushed_at[1] = now[1];
        u->seen = true;
        u->armed = false;
        u->stk_ticks = 0;
        return true;
    }
    return false;
}

static inline bool switch_joycon_use_report(struct switch_joycon_use *u, const struct switch_joycon_stick *s,
                                            const uint8_t *report, size_t length)
{
    return switch_joycon_use_observe(u, s, report, length, true);
}

static inline bool switch_joycon_use_idle(uint64_t last_use_ms, uint64_t now_ms)
{
    return now_ms - last_use_ms >= SWITCH_JOYCON_USE_IDLE_MS;
}

// True when touch activity occurred after the last gamepad use, the grace window has elapsed, and no
// active motion (IMU) or rumble holds the controller session alive.
static inline bool switch_joycon_use_touch_preempt(uint64_t last_use_ms, uint64_t last_touch_ms, bool busy_hold,
                                                   uint64_t now_ms)
{
    if (busy_hold || last_touch_ms == 0u || last_touch_ms <= last_use_ms)
        return false;
    return now_ms >= last_use_ms && (now_ms - last_use_ms >= SWITCH_JOYCON_USE_TOUCH_GRACE_MS);
}

#ifdef __cplusplus
}
#endif
