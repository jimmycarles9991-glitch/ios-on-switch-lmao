// xnu_sched_stats_harness.c: kernel/switch/sched/stats.h compiled as the kernel compiles it (C), for
// xnu_sched_stats_test.cpp.
#ifndef SWITCH_XNU_SCHED_STATS_HARNESS_H
#define SWITCH_XNU_SCHED_STATS_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned xnu_sched_buckets(void);
unsigned xnu_sched_bucket(uint64_t us);

// One window: xnu_sched_reset, xnu_sched_note per run, then the reads.
void xnu_sched_reset(void);
void xnu_sched_note(uint64_t us);
void xnu_sched_set_counts(uint32_t failsafe_runs, uint32_t limit_blocks, uint64_t limit_us);
void xnu_sched_take_longest(uint64_t us, uint64_t began_ms, uint64_t start_ms);
uint64_t xnu_sched_before_ms(void);
uint64_t xnu_sched_longest(uint64_t us, uint64_t began_ms);
void xnu_sched_timeline_reset(void);
void xnu_sched_timeline_dispatch(uint64_t made_us, uint64_t now_us);
size_t xnu_sched_timeline_take(uint64_t now_us, char *out, size_t size);
uint64_t xnu_sched_percentile(unsigned pct);
uint64_t xnu_sched_max(void);
uint32_t xnu_sched_runs(void);

// The window's entry for a thread of this class.
size_t xnu_sched_format(char *out, size_t size, const char *label, int pid, uint8_t base_pri, uint8_t sched_pri,
                        uint8_t mode, uint8_t qos, uint8_t bucket);

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_XNU_SCHED_STATS_HARNESS_H
