// SPDX-License-Identifier: BSD-2-Clause-Patent
// How often the rail asks a running Joy-Con for a report (SwitchJoyConRail.cpp tick; the host test
// test/switch_joycon_cadence_test.cpp drives this header).
//
// A rail Joy-Con only answers (deku README 101-123; deku packet_parse/j2c.txt: in 5 s after docking, every packet of
// the Joy-Con's answers one of the console's, which come every 15.00 ms, median), and the console's request is the
// only way the first press of an idle Joy-Con gets to it. A request every 30 ms instead of 15 adds at most 15 ms to that
// first press, under one 16.7 ms frame; 91/12 cannot ask for anything slower (hekate joycon.c 192: "5/10/15
// supported"; 311: a wrong rate answers status 1) and the Joy-Con's own HID timeout is 2 s (hekate joycon.c 1255-1257).
// So a Joy-Con is quiet, asked every 30 ms, once a second has passed without input that changed, a button held or its
// stick out of its dead zone, and while nothing else needs the 15 ms cadence: output reports queued, rumble that is
// moving (L4T refreshes it every 50 ms, joycon-serdev.c 470, 1845-1851), or motion that a client asked for. A quiet
// Joy-Con that is engaged again is active at once: the rail's clock, at 30 ms while both are quiet, comes back 15 ms
// after the reply that showed it, so a release after a first press is seen as soon as before.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchJoyConForward.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_CADENCE_ACTIVE_MS 15u
#define SWITCH_JOYCON_CADENCE_QUIET_MS  30u
#define SWITCH_JOYCON_CADENCE_HOLD_MS   1000u

struct switch_joycon_cadence {
    bool quiet;
    uint64_t engaged_until;
    uint64_t sent_at;
};

static inline void switch_joycon_cadence_reset(struct switch_joycon_cadence *c, uint64_t now)
{
    c->quiet = false;
    c->engaged_until = now + SWITCH_JOYCON_CADENCE_HOLD_MS;
    c->sent_at = 0;
}

// The user is doing something in this report: it differs from the last one (SwitchJoyConForward.h), or a button is
// held (the grip bit aside), or the stick is out of its dead zone.
static inline bool switch_joycon_cadence_engaged(const struct switch_joycon_stick *s, const uint8_t *was,
                                                 const uint8_t *now)
{
    return switch_joycon_report_news(s, was, now) || (now[3] | (now[4] & 0x7fu) | now[5]) != 0u ||
           !switch_joycon_stick_within(s, now, s->centre, s->dead_zone);
}

// True: a quiet Joy-Con is engaged again (the rail's clock is due in 15 ms).
static inline bool switch_joycon_cadence_input(struct switch_joycon_cadence *c, bool engaged, uint64_t now)
{
    if (!engaged)
        return false;
    c->engaged_until = now + SWITCH_JOYCON_CADENCE_HOLD_MS;
    const bool woke = c->quiet;
    c->quiet = false;
    return woke;
}

// A rumble report asks this Joy-Con for a non-zero amplitude (L4T joycon_send_rumble_data: the left half of the 8 rumble
// bytes drives a Joy-Con (L), the right half a Joy-Con (R); zero amplitude is 0x40000000 under mask 0xFF80FE00, LE).
static inline bool switch_joycon_rumble_moving(bool left, const uint8_t *rumble, size_t length)
{
    if (length < 10)
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
