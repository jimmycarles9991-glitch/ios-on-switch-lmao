// SPDX-License-Identifier: BSD-2-Clause-Patent
// The on-demand clock governor's decisions, in plain C with no kernel dependency, so the host tests
// (test/switch_governor_test.cpp) check this same header. Its one user is the A57 cluster's DFLL
// (SwitchPerfGovernor.cpp, from the scheduler's perfcontrol callouts). The GPU does not use it: it runs
// 768 MHz whenever it has work and drops only when truly idle (TegraGM20B gpu_clock.c, "idle clock").
// The caller owns the time base (mach absolute time in the kernel, microseconds in the tests), the locking and the
// hardware; this file only maps "how busy was the last window" plus hints and thermal limits to an index into an
// ascending table of operating points, the way Linux's ondemand/schedutil governors and Horizon's boost mode do:
//   * up at once: a window at or above up_permille busy goes straight to the ceiling (ondemand's jump: when the
//     clock is saturated its real demand is unknown); below that, the lowest point whose target_permille share
//     covers the demand (busy x clock now, schedutil's frequency-invariant rule);
//   * down with hysteresis: a lower target must persist for down_hold before the clock follows, and then it goes to
//     the highest target seen during the hold, so a dip shorter than the hold never costs a frame;
//   * boosts (touch, a missed frame, an app launch, the boot): a floor until a deadline, applied at once;
//   * limits [lo, hi]: the thermal ceiling and the cold floor, applied at once in either direction.
// Nothing here can pick a point outside the table or outside [lo, hi]; the table itself is built from the L4T CVB/DVFS
// rows by the caller and never goes above Nintendo's own Mariko maximum (switch_gov_cpu_table below: 1785 MHz).
#ifndef SWITCH_GOVERNOR_H
#define SWITCH_GOVERNOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GOV_MAX_OPPS 24u

struct switch_gov_tunables {
    uint32_t up_permille;      // busy at or above this: the ceiling at once
    uint32_t target_permille;  // the load the proportional target aims for (busy x clock / target)
    uint64_t down_hold;        // a lower target must hold this long (caller's time units) before the clock follows
};

struct switch_gov {
    struct switch_gov_tunables t;
    uint32_t khz[SWITCH_GOV_MAX_OPPS];  // ascending
    unsigned count;
    unsigned lo, hi;  // allowed indices, inclusive: the cold floor and the thermal ceiling
    unsigned cur;     // the index decided last
    unsigned held;    // the highest target seen since the targets first fell below cur
    int lowering;     // 1 while the targets are below cur (since lowering_since)
    uint64_t lowering_since;
    unsigned boost;        // floor index of the boost in force
    uint64_t boost_until;  // time before which `boost` holds
    // Accounting for the caller's periodic log line: time at each index (from `last`), steps up, steps down, jumps to
    // the ceiling from saturation, boosts that raised the clock, and limit changes that moved it.
    uint64_t last;
    uint64_t residency[SWITCH_GOV_MAX_OPPS];
    uint32_t ups, downs, jumps, boosted, clamped;
};

static inline unsigned switch_gov_clamp(const struct switch_gov *g, unsigned index)
{
    if (index < g->lo)
        index = g->lo;
    if (index > g->hi)
        index = g->hi;
    return index;
}

static inline void switch_gov_account(struct switch_gov *g, uint64_t now)
{
    if (now > g->last)
        g->residency[g->cur] += now - g->last;
    g->last = now;
}

// `count` points from `khz` (ascending; at most SWITCH_GOV_MAX_OPPS are kept), starting at index `start`.
static inline void switch_gov_init(struct switch_gov *g, const uint32_t *khz, unsigned count, unsigned start,
                                   struct switch_gov_tunables t, uint64_t now)
{
    unsigned i = 0;
    if (count > SWITCH_GOV_MAX_OPPS)
        count = SWITCH_GOV_MAX_OPPS;
    if (!count)
        count = 1;  // a caller with no table gets a one-point governor (index 0, whatever khz[0] is)
    g->t = t;
    g->count = count;
    for (i = 0; i < SWITCH_GOV_MAX_OPPS; ++i) {
        g->khz[i] = i < count && khz ? khz[i] : 0;
        g->residency[i] = 0;
    }
    g->lo = 0;
    g->hi = count - 1;
    g->cur = start < count ? start : count - 1;
    g->held = g->cur;
    g->lowering = 0;
    g->lowering_since = 0;
    g->boost = 0;
    g->boost_until = 0;
    g->last = now;
    g->ups = g->downs = g->jumps = g->boosted = g->clamped = 0;
}

// New limits (the thermal floor and ceiling), applied at once. lo > hi (a cap below the floor) keeps hi: the ceiling
// wins, as the DFLL's own voltage cap does. Returns the index now in force.
static inline unsigned switch_gov_limit(struct switch_gov *g, unsigned lo, unsigned hi, uint64_t now)
{
    const unsigned before = g->cur;
    switch_gov_account(g, now);
    if (hi >= g->count)
        hi = g->count - 1;
    if (lo > hi)
        lo = hi;
    g->lo = lo;
    g->hi = hi;
    g->cur = switch_gov_clamp(g, g->cur);
    if (g->cur != before) {
        ++g->clamped;
        g->lowering = 0;
    }
    return g->cur;
}

// A boost: at least `index` until `until`. Boosts combine as the highest floor and the latest deadline. Raises the
// clock at once; never lowers it. Returns the index now in force.
static inline unsigned switch_gov_boost(struct switch_gov *g, unsigned index, uint64_t until, uint64_t now)
{
    switch_gov_account(g, now);
    if (index >= g->count)
        index = g->count - 1;
    if (now >= g->boost_until || index > g->boost)
        g->boost = index;  // the last one expired (this one alone), or this floor is higher
    if (until > g->boost_until)
        g->boost_until = until;
    index = switch_gov_clamp(g, g->boost);
    if (index > g->cur) {
        g->cur = index;
        ++g->boosted;
        ++g->ups;
    }
    if (index >= g->cur)
        g->lowering = 0;  // a boost at the clock in force restarts its down hold
    return g->cur;
}

// The target for one window that was `busy_permille` busy (0..1000) at the clock in force, before hysteresis.
static inline unsigned switch_gov_target(const struct switch_gov *g, uint32_t busy_permille, uint64_t now)
{
    unsigned want = 0;
    if (busy_permille > 1000)
        busy_permille = 1000;
    if (busy_permille >= g->t.up_permille) {
        want = g->hi;
    } else {
        // demand in kHz x permille: what the window needed at the clock it ran at
        const uint64_t demand = (uint64_t)g->khz[g->cur] * busy_permille;
        want = g->lo;
        while (want < g->hi && (uint64_t)g->khz[want] * g->t.target_permille < demand)
            ++want;
    }
    if (now < g->boost_until && g->boost > want)
        want = g->boost;
    return switch_gov_clamp(g, want);
}

// One window ended at `now`, `busy_permille` busy. Returns the index now in force.
static inline unsigned switch_gov_step(struct switch_gov *g, uint64_t now, uint32_t busy_permille)
{
    const unsigned want = switch_gov_target(g, busy_permille, now);
    switch_gov_account(g, now);
    if (want >= g->cur) {
        if (want > g->cur) {
            ++g->ups;
            if (want == g->hi && busy_permille >= g->t.up_permille)
                ++g->jumps;
        }
        g->cur = want;
        g->lowering = 0;
        return g->cur;
    }
    if (!g->lowering) {
        g->lowering = 1;
        g->lowering_since = now;
        g->held = want;
    } else if (want > g->held) {
        g->held = want;
    }
    if (now - g->lowering_since >= g->t.down_hold) {
        g->cur = g->held;
        ++g->downs;
        g->lowering = 0;
    }
    return g->cur;
}

// The highest index whose voltage is at or below `mv` (rows ascending in both clock and voltage): the thermal
// ceiling under a voltage cap, or the cold floor's index. 0 if even the first row is above it.
static inline unsigned switch_gov_index_at_mv(const uint32_t *mv, unsigned count, uint32_t limit_mv)
{
    unsigned i = 0;
    while (i + 1 < count && mv[i + 1] <= limit_mv)
        ++i;
    return i;
}

// iPadOS's thermal ceiling (thermalmonitord's DVD factor on ApplePMGR, a 16.16 fraction of the top point's clock,
// 0x10000 and above = none; drivers/SwitchPower/CONTRACT.md) as an index: the highest point at or below that share of
// the top clock, 0 if even the first point is above it.
static inline unsigned switch_gov_index_at_fraction(const uint32_t *khz, unsigned count, uint32_t fraction16)
{
    uint64_t limit = 0;
    unsigned i = 0;
    if (!count)
        return 0;
    if (fraction16 >= 0x10000u)
        return count - 1;
    limit = (uint64_t)khz[count - 1] * fraction16 >> 16;
    while (i + 1 < count && khz[i + 1] <= limit)
        ++i;
    return i;
}

// The CPU table from the DFLL's OPP rows (L4T cvb.c's table for this chip: ascending kHz with their CVB voltages, the
// voltages floored at the CVB Vmin): every row up to max_khz (Nintendo's 1785 MHz boost clock), starting at the
// highest row whose voltage is at or below vmin_mv. Rows under that one share its voltage: the DFLL runs them at the
// same DVCO rate and voltage with its output skipper (clk-dfll.c dfll_calculate_rate_request), so they save next to
// nothing; the governor's lowest point is that row. Returns the rows written to khz_out/mv_out (at most cap).
static inline unsigned switch_gov_cpu_table(const uint32_t *khz, const uint32_t *mv, unsigned rows, uint32_t max_khz,
                                            uint32_t vmin_mv, uint32_t *khz_out, uint32_t *mv_out, unsigned cap)
{
    unsigned first = 0;
    unsigned i = 0;
    unsigned n = 0;
    for (i = 0; i < rows; ++i) {
        if (khz[i] > max_khz)
            break;
        if (mv[i] <= vmin_mv)
            first = i;
    }
    for (i = first; i < rows && khz[i] <= max_khz && n < cap; ++i, ++n) {
        khz_out[n] = khz[i];
        mv_out[n] = mv[i];
    }
    return n;
}

#ifdef __cplusplus
}
#endif
#endif
