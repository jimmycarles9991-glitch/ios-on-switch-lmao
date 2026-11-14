// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * The EMC's hold (SwitchEMC.cpp, test/switch_emc_hold_test.cpp): while frames are being made (a frame line in the last
 * frame_ticks) or a hint's deadline has not passed, the EMC stays at its top rate with ACTMON's down watermark off.
 * The hold starts at an edge that already wakes the EMC's work loop (the frame core's start edge, a hint). Its end is
 * a moving deadline (the last frame + frame_ticks), so while the CPU governor's tick runs, the tick watches for it and
 * wakes the loop once when it passes, instead of a one-shot timer chasing it; without the tick (the PLL ladder, the
 * governor stopped) the loop arms its timer for the end as it stands. Plain C.
 *
 * Ownership: `holding` and `generation` are the loop's (it publishes `holding` with a relaxed store); `told` is the
 * tick's (XNU runs it on one CPU at a time). Every program() that keeps the hold starts a new generation, so a tick
 * that reported an end the loop then found was not one (a frame landed after the tick read the stamp) reports the
 * next one too.
 */
#ifndef SWITCH_EMC_HOLD_H
#define SWITCH_EMC_HOLD_H

#include <stdbool.h>
#include <stdint.h>

struct switch_emc_hold {
    uint64_t frame_ticks; /* a frame line holds the top rate this long */
    uint32_t holding;     /* the hold's generation while it is on, 0 when off */
    uint32_t generation;
    uint32_t told; /* the generation whose end the tick reported */
};

/* What program() does: the down watermark off (keep), and the hold timer armed for arm_at (0: not armed). */
struct switch_emc_hold_step {
    bool keep;
    uint64_t arm_at;
};

/* When the hold ends: the later of the hint deadline and the last frame line's hold. */
static inline uint64_t switch_emc_hold_end(const struct switch_emc_hold *hold, uint64_t hint_until, uint64_t last_frame)
{
    const uint64_t frames = last_frame ? last_frame + hold->frame_ticks : 0;
    return frames > hint_until ? frames : hint_until;
}

/* The EMC's work loop, programming ACTMON at `now`; `ticked`: the CPU governor's tick runs. */
static inline struct switch_emc_hold_step switch_emc_hold_program(struct switch_emc_hold *hold, uint64_t now,
                                                                  uint64_t hint_until, uint64_t last_frame, bool ticked)
{
    struct switch_emc_hold_step step;
    const uint64_t end = switch_emc_hold_end(hold, hint_until, last_frame);
    step.keep = now < end;
    step.arm_at = step.keep && !ticked ? end : 0;
    if (step.keep && ++hold->generation == 0)
        hold->generation = 1;
    __atomic_store_n(&hold->holding, step.keep ? hold->generation : 0u, __ATOMIC_RELAXED);
    return step;
}

/* The CPU governor's tick at `now`: whether it wakes the EMC's work loop because the hold ended. */
static inline bool switch_emc_hold_tick(struct switch_emc_hold *hold, uint64_t now, uint64_t hint_until,
                                        uint64_t last_frame)
{
    const uint32_t holding = __atomic_load_n(&hold->holding, __ATOMIC_RELAXED);
    if (!holding || holding == hold->told || now < switch_emc_hold_end(hold, hint_until, last_frame))
        return false;
    hold->told = holding;
    return true;
}

#endif
