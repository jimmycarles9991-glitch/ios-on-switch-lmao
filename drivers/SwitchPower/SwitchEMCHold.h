// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * The EMC's hold (SwitchEMC.cpp, test/switch_emc_hold_test.cpp): while frames are being made (a frame line in the last
 * frame_ticks), the CPUs work (a window of the CPU governor's tick with busy_floor of CPU time or more ended in the last
 * frame_ticks) or a hint's deadline has not passed, the EMC stays at its top rate with ACTMON's down watermark off.
 * The hold starts at an edge that wakes the EMC's work loop (the frame core's start edge, a hint, the tick that ends a
 * window of work while the EMC is not held at its top). Its end is a moving deadline: the tick watches for it and wakes
 * the loop once when it passes, instead of a one-shot timer chasing it. Without the tick (the PLL ladder, the governor
 * stopped) nothing accounts for the CPUs' work, so the hold does not end until the tick runs again. Plain C.
 *
 * Ownership: `holding` and `generation` are the loop's (it publishes `holding` with a relaxed store); `told` and
 * `busy_until` are the tick's (XNU runs it on one CPU at a time; the loop reads `busy_until` after the tick's wake or
 * from its own events, where a stale value is the window before). Every program() that keeps the hold starts a new
 * generation, so a tick that reported an end the loop then found was not one (a frame landed after the tick read the
 * stamp) reports the next one too.
 */
#ifndef SWITCH_EMC_HOLD_H
#define SWITCH_EMC_HOLD_H

#include <stdbool.h>
#include <stdint.h>

struct switch_emc_hold {
    uint64_t frame_ticks; /* a frame line, or a window of CPU work, holds the top rate this long */
    uint64_t busy_floor;  /* the CPU time in one window that makes it a window of work */
    uint64_t busy_until;  /* the last window of work's hold */
    uint32_t holding;     /* the hold's generation while it is on, 0 when off */
    uint32_t generation;
    uint32_t told; /* the generation whose end the tick reported */
};

/* When the hold ends: the latest of the hint deadline, the last frame line's hold and the last window of work's. */
static inline uint64_t switch_emc_hold_end(const struct switch_emc_hold *hold, uint64_t hint_until, uint64_t last_frame)
{
    const uint64_t frames = last_frame ? last_frame + hold->frame_ticks : 0;
    const uint64_t busy = __atomic_load_n(&hold->busy_until, __ATOMIC_RELAXED);
    const uint64_t end = frames > hint_until ? frames : hint_until;
    return busy > end ? busy : end;
}

/* The EMC's work loop, programming ACTMON at `now`; `ticked`: the CPU governor's tick runs. Whether the hold keeps the
 * down watermark off. */
static inline bool switch_emc_hold_program(struct switch_emc_hold *hold, uint64_t now, uint64_t hint_until,
                                           uint64_t last_frame, bool ticked)
{
    const bool keep = !ticked || now < switch_emc_hold_end(hold, hint_until, last_frame);
    if (keep && ++hold->generation == 0)
        hold->generation = 1;
    __atomic_store_n(&hold->holding, keep ? hold->generation : 0u, __ATOMIC_RELAXED);
    return keep;
}

/* The CPU governor's tick at `now`, ending a window in which the CPUs were busy for `busy` (summed over them): whether
 * it wakes the EMC's work loop, because the window was work and the EMC is not held at its top rate (`top_held`), or
 * because the hold ended. */
static inline bool switch_emc_hold_tick(struct switch_emc_hold *hold, uint64_t now, uint64_t busy, uint64_t hint_until,
                                        uint64_t last_frame, bool top_held)
{
    if (busy >= hold->busy_floor) {
        __atomic_store_n(&hold->busy_until, now + hold->frame_ticks, __ATOMIC_RELAXED);
        return !top_held;
    }
    const uint32_t holding = __atomic_load_n(&hold->holding, __ATOMIC_RELAXED);
    if (!holding || holding == hold->told || now < switch_emc_hold_end(hold, hint_until, last_frame))
        return false;
    hold->told = holding;
    return true;
}

#endif
