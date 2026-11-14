// switch_governor_harness.c's exported wrappers of drivers/SwitchPower/SwitchGovernor.h, for switch_governor_test.cpp.
// The governor's state is opaque here (gov_size bytes): the test drives it only through these, as the kernel does
// through the header.
#ifndef SWITCH_SWITCH_GOVERNOR_HARNESS_H
#define SWITCH_SWITCH_GOVERNOR_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct switch_gov;

size_t gov_size(void);
void gov_init(struct switch_gov *g, const uint32_t *khz, unsigned count, unsigned start, uint32_t up_permille,
              uint32_t target_permille, uint64_t down_hold, uint64_t now);
unsigned gov_step(struct switch_gov *g, uint64_t now, uint32_t busy);
unsigned gov_boost(struct switch_gov *g, unsigned index, uint64_t until, uint64_t now);
unsigned gov_limit(struct switch_gov *g, unsigned lo, unsigned hi, uint64_t now);
unsigned gov_cur(const struct switch_gov *g);
unsigned gov_lo(const struct switch_gov *g);
unsigned gov_hi(const struct switch_gov *g);
uint32_t gov_khz(const struct switch_gov *g, unsigned i);
uint64_t gov_residency(const struct switch_gov *g, unsigned i);
void gov_account(struct switch_gov *g, uint64_t now);
// 0 ups, 1 downs, 2 jumps, 3 boosted, 4 clamped
uint32_t gov_counter(const struct switch_gov *g, unsigned which);
// One step per window: busy[i] for the window ending at times[i]; out[i] = the index after it.
void gov_run(struct switch_gov *g, const uint64_t *times, const uint32_t *busy, unsigned n, unsigned *out);
unsigned gov_cpu_table(const uint32_t *khz, const uint32_t *mv, unsigned rows, uint32_t max_khz, uint32_t vmin_mv,
                       uint32_t *khz_out, uint32_t *mv_out, unsigned cap);
unsigned gov_index_at_mv(const uint32_t *mv, unsigned count, uint32_t limit_mv);

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_SWITCH_GOVERNOR_HARNESS_H
