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

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_XNU_STALL_HARNESS_H
