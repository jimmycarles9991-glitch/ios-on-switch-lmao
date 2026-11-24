/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_TIMER_H
#define SWITCH_A3_TIMER_H
#include "wire.h"
#include "fault.h"

typedef void (*switch_a3_timer_callback)(void *, uint64_t, uint64_t, uint32_t, uint32_t);

enum {
    SWITCH_A3_TIMER_ELAPSED = 1
};

struct switch_a3_timer {
    struct {
        uint64_t requested_tsf, actual_tsf;
        uint32_t session_generation, clock_generation;
        int32_t status;
        uint8_t armed;
    } snapshot;
    uint64_t observed_tsf;
    void *wlc, *queue, *timeout;
    switch_a3_timer_callback callback;
    void *context;
    uint8_t lost, deferred, residual, fault;
};

int switch_a3_timer_initialize_owned(struct switch_a3_timer *timer, void *wlc);
int switch_a3_timer_arm_owned(struct switch_a3_timer *timer, void *wlc, uint64_t deadline, uint32_t session,
                              uint32_t epoch, switch_a3_timer_callback callback, void *context);
int switch_a3_timer_defer_owned(struct switch_a3_timer *timer, void *wlc, uint32_t session, uint32_t epoch,
                                switch_a3_timer_callback callback, void *context);
int switch_a3_timer_cancel_owned(struct switch_a3_timer *timer, void *wlc);
int switch_a3_timer_initialize(void *wlc);
int switch_a3_timer_arm(void *wlc, uint64_t deadline, uint32_t session_generation, uint32_t clock_generation,
                        switch_a3_timer_callback callback, void *context);
int switch_a3_timer_cancel(void *wlc);
void switch_a3_timer_snapshot(struct switch_a3_deadline_snapshot *snapshot);

#endif
