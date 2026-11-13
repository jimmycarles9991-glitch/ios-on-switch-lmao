// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * The EMC governor's decisions (SwitchEMC.cpp runs them, test/switch_emc_governor_test.cpp checks them). Plain C.
 *
 * ACTMON's MC_ALL device counts memory-controller activity per sample in kHz x ms (L4T actmon_common.c, the
 * FREQ_SAMPLER type): count / sample_ms is the EMC clock that traffic would keep fully busy. The governor arms two
 * hardware watermarks at the rate it runs and does nothing else until one fires:
 *   up:   one sample busier than 60 % of this rate. The EMC goes straight to the top rate (there are only three: a frame
 *         must never wait for a ramp);
 *   down: eight samples in a row (32 ms) that the next rate down would carry at 40 % or less. One step down.
 * Never slower: while frames are being made (a frame line in the last 100 ms) or within 500 ms of a touch, launch or
 * missed frame, the EMC stays at the top rate; a frame line after a quiet spell or any of those hints raises it at once,
 * ahead of the frame's own traffic. So the low rates only run while nothing is drawn: the Home Screen at rest, the
 * screen off, background work.
 */
#ifndef SWITCH_EMC_GOVERNOR_H
#define SWITCH_EMC_GOVERNOR_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_EMC_SAMPLE_MS    4u
#define SWITCH_EMC_UP_PERCENT   60u
#define SWITCH_EMC_DOWN_PERCENT 40u
#define SWITCH_EMC_DOWN_SAMPLES 8u
#define SWITCH_EMC_HINT_HOLD_US 500000u
#define SWITCH_EMC_MAX_RATES    8u

#define SWITCH_EMC_EVENT_UP   1u
#define SWITCH_EMC_EVENT_DOWN 2u

/* What asked for a rate move (the debug move line names it). */
enum switch_emc_reason {
    SWITCH_EMC_REASON_ACTMON,
    SWITCH_EMC_REASON_HINT,
    SWITCH_EMC_REASON_DISPLAY,
    SWITCH_EMC_REASON_DISPLAY_MEMORY,
    SWITCH_EMC_REASON_GOVERN,
};

struct switch_emc_wmarks {
    uint32_t up, down;
    bool up_on, down_on;
};

/* The table's rates from the lowest to `ceiling_khz`, ascending and without repeats, into `out`; how many. */
static inline unsigned switch_emc_rates(const uint32_t *table, unsigned n, uint32_t ceiling_khz, uint32_t *out)
{
    unsigned count = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t khz = table[i];
        if (!khz || khz > ceiling_khz) {
            continue;
        }
        unsigned at = count;
        bool repeat = false;
        while (at > 0 && out[at - 1] >= khz) {
            repeat = repeat || out[at - 1] == khz;
            --at;
        }
        if (repeat || count == SWITCH_EMC_MAX_RATES) {
            continue;
        }
        for (unsigned j = count; j > at; --j) {
            out[j] = out[j - 1];
        }
        out[at] = khz;
        ++count;
    }
    return count;
}

/* The row the EMC runs at: the table's rate within 0.1 % of what the firmware reports (it may round), else n. */
static inline unsigned switch_emc_row(const uint32_t *khz, unsigned n, uint32_t now_khz)
{
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t slack = khz[i] / 1000u;
        if (now_khz + slack >= khz[i] && now_khz <= khz[i] + slack) {
            return i;
        }
    }
    return n;
}

/* The watermarks to arm at rate `cur`; `held` (frames or a hint) keeps the down watermark off. */
static inline struct switch_emc_wmarks switch_emc_watermarks(const uint32_t *khz, unsigned n, unsigned cur, bool held)
{
    struct switch_emc_wmarks w;
    w.up_on = cur + 1 < n;
    w.up = khz[cur] * SWITCH_EMC_SAMPLE_MS * SWITCH_EMC_UP_PERCENT / 100u;
    w.down_on = cur > 0 && !held;
    w.down = cur > 0 ? khz[cur - 1] * SWITCH_EMC_SAMPLE_MS * SWITCH_EMC_DOWN_PERCENT / 100u : 0;
    return w;
}

/* The rate to run after `events` (SWITCH_EMC_EVENT_*) at `cur`. */
static inline unsigned switch_emc_next(unsigned n, unsigned cur, uint32_t events, bool held)
{
    if (held || (events & SWITCH_EMC_EVENT_UP)) {
        return n - 1;
    }
    if ((events & SWITCH_EMC_EVENT_DOWN) && cur > 0) {
        return cur - 1;
    }
    return cur;
}

#endif /* SWITCH_EMC_GOVERNOR_H */
