// xnu_stall_harness.c: kernel/switch/diag/stall.h's decisions, compiled as the kernel compiles them (C), for
// xnu_stall_test.cpp.
#ifndef SWITCH_XNU_STALL_HARNESS_H
#define SWITCH_XNU_STALL_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The sample's flags as the scheduler sets them; times in ticks.
struct xnu_stall_sample {
    bool waiting, runnable, running, run_loop_idle;
    uint64_t now, last_run, made_runnable;
};

uint64_t xnu_stall_threshold_ns(void);
uint64_t xnu_stall_not_runnable(void);
// 0 none, 1 waiting, 2 runnable.
int xnu_stall_kind(const struct xnu_stall_sample *s, uint64_t threshold);

// One episode tracker: xnu_stall_reset, then xnu_stall_step per sample. 0 quiet, 1 began, 2 ended.
void xnu_stall_reset(void);
int xnu_stall_step(const struct xnu_stall_sample *s, uint64_t threshold);
uint64_t xnu_stall_since(void);
int xnu_stall_episode_kind(void);

const char *xnu_stall_wait_name(unsigned hint);
unsigned xnu_stall_wait_names(void);
// 0 raw, 1 dsc, 2 main.
int xnu_stall_user_where(uint64_t pc, uint64_t sr_base, uint64_t sr_size, uint64_t sr_slide, uint64_t header,
                         uint64_t *value);

// What ran while a stall lasted: a snapshot of `n` threads' times (`tids`, `cpus`), sorted as stall.c sorts it, then
// the `k` (at most 8) threads that gained the most when the threads now run `now_cpus`; their ids, most first, in
// `top_tids` and gains in `top_gains`, 0 past the last. Returns how many entered.
unsigned xnu_stall_most(const uint64_t *tids, const uint64_t *cpus, unsigned n, bool complete, const uint64_t *now_tids,
                        const uint64_t *now_cpus, unsigned now_n, unsigned k, uint64_t *top_tids, uint64_t *top_gains);

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_XNU_STALL_HARNESS_H
