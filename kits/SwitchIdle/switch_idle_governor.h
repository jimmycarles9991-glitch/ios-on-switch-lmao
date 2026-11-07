// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * Which idle state a core takes, and what it learns from each idle period. Plain C over timebase ticks (19.2 MHz), so
 * the host tests run exactly what the kernel runs.
 *
 * The numbers are the T210B01 C7 state of the console's own device tree (/cpus/idle-states/c7: wakeup-latency 130 us,
 * min-residency 1000 us). Never slower: a core takes C7 only when it expects to stay idle past the state's break-even,
 * and while frames are being made (a frame line fired in the last 100 ms) only past two refresh periods, so a core that
 * takes part in every frame always wakes from WFI. Its wake timer fires the wake latency ahead of the deadline, so a
 * timer is never late for C7.
 *
 * The prediction is how long until something other than the core's own timer wakes it. A wake by an interrupt or an
 * IPI sooner than predicted is believed at once (work has started: the next idle is short too); longer idles raise it
 * by an eighth of the difference each time. An idle that ran to its timer says nothing shorter.
 */
#ifndef SWITCH_IDLE_GOVERNOR_H
#define SWITCH_IDLE_GOVERNOR_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_IDLE_WFI 0u
#define SWITCH_IDLE_C7  1u

#define SWITCH_IDLE_TICKS_US(us) ((uint64_t)(us) * 96u / 5u)

#define SWITCH_IDLE_C7_RESIDENCY    SWITCH_IDLE_TICKS_US(1000)
#define SWITCH_IDLE_C7_WAKE_LATENCY SWITCH_IDLE_TICKS_US(130)
#define SWITCH_IDLE_FRAME_WINDOW    SWITCH_IDLE_TICKS_US(100000)
#define SWITCH_IDLE_FRAME_RESIDENCY SWITCH_IDLE_TICKS_US(33334)
#define SWITCH_IDLE_TIMER_SLACK     SWITCH_IDLE_TICKS_US(50)

static inline uint32_t switch_idle_choose(uint64_t predicted, uint64_t now, uint64_t deadline, uint64_t frame_at)
{
    if (deadline <= now) {
        return SWITCH_IDLE_WFI;
    }
    const uint64_t until = deadline - now;
    const uint64_t expect = until < predicted ? until : predicted;
    uint64_t need = SWITCH_IDLE_C7_RESIDENCY;
    if (frame_at != 0 && now - frame_at < SWITCH_IDLE_FRAME_WINDOW) {
        need = SWITCH_IDLE_FRAME_RESIDENCY;
    }
    return expect >= need ? SWITCH_IDLE_C7 : SWITCH_IDLE_WFI;
}

/* When C7's wake timer must fire, as an absolute time: the wake latency ahead of the deadline. Only for a deadline
 * switch_idle_choose took C7 for. */
static inline uint64_t switch_idle_c7_wake_at(uint64_t deadline)
{
    return deadline - SWITCH_IDLE_C7_WAKE_LATENCY;
}

/* The wake timer's count for `wake_at`, in 1..max ticks: a deadline beyond the timer's reach wakes the core at its
 * reach, and the core then idles again for the rest. */
static inline uint64_t switch_idle_timer_ticks(uint64_t now, uint64_t wake_at, uint64_t max)
{
    if (wake_at <= now) {
        return 1;
    }
    const uint64_t ticks = wake_at - now;
    return ticks > max ? max : ticks;
}

/* Whether an idle period that should have ended at `wake_at` was ended by that timer (or later), not sooner. */
static inline bool switch_idle_timer_bound(uint64_t now, uint64_t wake_at)
{
    return now + SWITCH_IDLE_TIMER_SLACK >= wake_at;
}

static inline uint64_t switch_idle_learn(uint64_t predicted, uint64_t idle, bool timer_bound)
{
    if (idle < predicted) {
        return timer_bound ? predicted : idle;
    }
    return predicted + (idle - predicted) / 8u;
}

#endif /* SWITCH_IDLE_GOVERNOR_H */
