// Host harness for drivers/SwitchPower/SwitchGovernor.h (switch_governor_test.cpp): the header's static inline
// functions as exported C ones (built as C11 with -Wconversion -Wsign-conversion, so the header stays clean C), plus a
// trace runner (one step per window) so a whole load trace goes through the same code the kernel runs in one call.
#include <stddef.h>
#include <stdint.h>

#include "../SwitchGovernor.h"
#include "switch_governor_harness.h"

size_t gov_size(void)
{
    return sizeof(struct switch_gov);
}

void gov_init(struct switch_gov *g, const uint32_t *khz, unsigned count, unsigned start, uint32_t up_permille,
              uint32_t target_permille, uint64_t down_hold, uint64_t now)
{
    struct switch_gov_tunables t = {up_permille, target_permille, down_hold};
    switch_gov_init(g, khz, count, start, t, now);
}

unsigned gov_step(struct switch_gov *g, uint64_t now, uint32_t busy)
{
    return switch_gov_step(g, now, busy);
}

unsigned gov_boost(struct switch_gov *g, unsigned index, uint64_t until, uint64_t now)
{
    return switch_gov_boost(g, index, until, now);
}

unsigned gov_limit(struct switch_gov *g, unsigned lo, unsigned hi, uint64_t now)
{
    return switch_gov_limit(g, lo, hi, now);
}

unsigned gov_cur(const struct switch_gov *g)
{
    return g->cur;
}

unsigned gov_lo(const struct switch_gov *g)
{
    return g->lo;
}

unsigned gov_hi(const struct switch_gov *g)
{
    return g->hi;
}

uint32_t gov_khz(const struct switch_gov *g, unsigned i)
{
    return i < SWITCH_GOV_MAX_OPPS ? g->khz[i] : 0;
}

uint64_t gov_residency(const struct switch_gov *g, unsigned i)
{
    return i < SWITCH_GOV_MAX_OPPS ? g->residency[i] : 0;
}

void gov_account(struct switch_gov *g, uint64_t now)
{
    switch_gov_account(g, now);
}

// 0 ups, 1 downs, 2 jumps, 3 boosted, 4 clamped
uint32_t gov_counter(const struct switch_gov *g, unsigned which)
{
    const uint32_t values[5] = {g->ups, g->downs, g->jumps, g->boosted, g->clamped};
    return which < 5 ? values[which] : 0;
}

// One step per window: busy[i] for the window ending at times[i]; out[i] = the index after it.
void gov_run(struct switch_gov *g, const uint64_t *times, const uint32_t *busy, unsigned n, unsigned *out)
{
    for (unsigned i = 0; i < n; ++i)
        out[i] = switch_gov_step(g, times[i], busy[i]);
}

unsigned gov_cpu_table(const uint32_t *khz, const uint32_t *mv, unsigned rows, uint32_t max_khz, uint32_t vmin_mv,
                       uint32_t *khz_out, uint32_t *mv_out, unsigned cap)
{
    return switch_gov_cpu_table(khz, mv, rows, max_khz, vmin_mv, khz_out, mv_out, cap);
}

unsigned gov_index_at_mv(const uint32_t *mv, unsigned count, uint32_t limit_mv)
{
    return switch_gov_index_at_mv(mv, count, limit_mv);
}
