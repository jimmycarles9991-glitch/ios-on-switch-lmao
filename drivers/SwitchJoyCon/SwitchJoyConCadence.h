// SPDX-License-Identifier: BSD-2-Clause-Patent
// How often a rail asks a running Joy-Con for a report (SwitchJoyConRail.cpp tick; the host test
// test/switch_joycon_cadence_test.cpp drives this header).
//
// Active rails poll at 10 ms (100 Hz, matching wired 91/12 10 ms) and stay active for 5000 ms while
// gamepads are published (1000 ms while unpublished). Quiet rails drop to 30 ms (~33.3 Hz) and wake
// immediately against a persistent resting anchor so slow stick pushes wake before leaving the dead zone.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchJoyConForward.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_CADENCE_ACTIVE_MS    10u
#define SWITCH_JOYCON_CADENCE_QUIET_MS     30u
#define SWITCH_JOYCON_CADENCE_HOLD_MS      5000u
#define SWITCH_JOYCON_CADENCE_IDLE_HOLD_MS 1000u

struct switch_joycon_cadence {
    bool quiet;
    bool anchored;
    uint64_t engaged_until;
    uint64_t sent_at;
    uint8_t anchor[12];
};

static inline uint32_t switch_joycon_cadence_hold_ms(bool gamepad_active)
{
    return gamepad_active ? SWITCH_JOYCON_CADENCE_HOLD_MS : SWITCH_JOYCON_CADENCE_IDLE_HOLD_MS;
}

static inline unsigned switch_joycon_cadence_wake_radius(const struct switch_joycon_stick *s)
{
    const unsigned r = s->dead_zone / 3u;
    return r ? r : 1u;
}

static inline void switch_joycon_cadence_reset(struct switch_joycon_cadence *c, uint64_t now)
{
    c->quiet = false;
    c->anchored = false;
    c->engaged_until = now + SWITCH_JOYCON_CADENCE_IDLE_HOLD_MS;
    c->sent_at = 0;
}

static inline bool switch_joycon_cadence_wake(struct switch_joycon_cadence *c, bool gamepad_active, uint64_t now)
{
    const uint64_t until = now + switch_joycon_cadence_hold_ms(gamepad_active);
    if (until > c->engaged_until)
        c->engaged_until = until;
    const bool woke = c->quiet;
    c->quiet = false;
    return woke;
}

// Checks whether `now` shows engagement relative to the resting anchor `anchor` (buttons held,
// button state edge, stick outside dead zone, or cumulative stick movement from rest).
static inline bool switch_joycon_cadence_engaged(const struct switch_joycon_stick *s, const uint8_t *anchor,
                                                 const uint8_t *now)
{
    if (switch_joycon_buttons_held(now) || anchor[3] != now[3] || anchor[4] != now[4] || anchor[5] != now[5])
        return true;
    if (!switch_joycon_stick_within(s, now, s->centre, s->dead_zone))
        return true;
    const uint16_t at[2] = {(uint16_t)switch_joycon_stick_axis(s, anchor, 0),
                            (uint16_t)switch_joycon_stick_axis(s, anchor, 1)};
    return !switch_joycon_stick_within(s, now, at, switch_joycon_cadence_wake_radius(s));
}

// Observes a live state report (0x30/0x31), keeping `anchor` pinned at rest while not engaged so slow
// cumulative stick pushes wake from quiet cadence before reaching the dead zone edge.
static inline bool switch_joycon_cadence_observe(struct switch_joycon_cadence *c, const struct switch_joycon_stick *s,
                                                 const uint8_t *report, size_t length, bool gamepad_active,
                                                 uint64_t now)
{
    if (length < sizeof(c->anchor) || report[0] == 0x21u)
        return false;
    if (!c->anchored) {
        for (unsigned i = 0; i < sizeof(c->anchor); ++i)
            c->anchor[i] = report[i];
        c->anchored = true;
        const bool active =
            switch_joycon_buttons_held(report) || !switch_joycon_stick_within(s, report, s->centre, s->dead_zone);
        return active ? switch_joycon_cadence_wake(c, gamepad_active, now) : false;
    }
    if (!switch_joycon_cadence_engaged(s, c->anchor, report))
        return false;
    for (unsigned i = 0; i < sizeof(c->anchor); ++i)
        c->anchor[i] = report[i];
    return switch_joycon_cadence_wake(c, gamepad_active, now);
}

// A rumble report asks this Joy-Con for a non-zero amplitude (L4T joycon_send_rumble_data: the left half of the 8 rumble
// bytes drives a Joy-Con (L), the right half a Joy-Con (R); zero amplitude is 0x40000000 under mask 0xFF80FE00, LE).
static inline bool switch_joycon_rumble_moving(bool left, const uint8_t *rumble, size_t length)
{
    if (length < 10u)
        return false;
    const uint8_t *half = rumble + (left ? 2 : 6);
    const uint32_t word = half[0] | (uint32_t)half[1] << 8 | (uint32_t)half[2] << 16 | (uint32_t)half[3] << 24;
    return (word & 0xff80fe00u) != 0x40000000u;
}

static inline uint32_t switch_joycon_cadence_interval(const struct switch_joycon_cadence *c)
{
    return c->quiet ? SWITCH_JOYCON_CADENCE_QUIET_MS : SWITCH_JOYCON_CADENCE_ACTIVE_MS;
}

// At a tick of the rail's clock: whether this one sends a request. `busy`: output queued, rumble moving or motion
// wanted. Half an active interval of slack, so a clock that fires a little early or late keeps the cadence.
static inline bool switch_joycon_cadence_due(struct switch_joycon_cadence *c, bool busy, uint64_t now)
{
    c->quiet = !busy && now >= c->engaged_until;
    if (now - c->sent_at + SWITCH_JOYCON_CADENCE_ACTIVE_MS / 2u < switch_joycon_cadence_interval(c))
        return false;
    c->sent_at = now;
    return true;
}

#ifdef __cplusplus
}
#endif
