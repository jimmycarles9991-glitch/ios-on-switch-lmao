/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "timer.h"
#include "clock.h"

extern void *switch_a3_timeout_allocate(void *context);
extern int switch_a3_timeout_arm(void *timeout, uint32_t delay, void (*callback)(void *), void *argument);
extern void switch_a3_timeout_cancel(void *timeout);

static struct switch_a3_timer switch_a3_timer;
static switch_a3_timer_callback switch_a3_legacy_callback;
static void *switch_a3_legacy_context;
#if !SWITCH_A3_CANDIDATE
static uint32_t switch_a3_legacy_fired;
#endif

static int switch_a3_timer_owned(const struct switch_a3_timer *timer, void *wlc)
{
    return !timer->lost && timer->wlc == wlc && timer->timeout && *(void **)((uint8_t *)wlc + 0x5f8) == timer->queue &&
           *((uint8_t *)timer->queue + 52);
}

static __attribute__((noinline, noclone)) int switch_a3_timer_lost(struct switch_a3_timer *timer)
{
    timer->snapshot.armed = 0;
    timer->callback = 0;
    timer->snapshot.status = -1;
    timer->fault = SWITCH_A3_FAULT_TIMER_OWNER;
    timer->lost = 1;
    return -1;
}

int switch_a3_timer_cancel_owned(struct switch_a3_timer *timer, void *wlc)
{
    if (!timer)
        return -2;
    if (timer->wlc != wlc || !timer->timeout)
        return 0;
    timer->snapshot.armed = 0;
    timer->callback = 0;
    if (!switch_a3_timer_owned(timer, wlc))
        return switch_a3_timer_lost(timer);
    switch_a3_timeout_cancel(timer->timeout);
    return 0;
}

int switch_a3_timer_initialize_owned(struct switch_a3_timer *timer, void *wlc)
{
    if (!timer || !wlc)
        return -2;
    if (timer->lost)
        return -1;
    void *queue = *(void **)((uint8_t *)wlc + 0x5f8);
    if (!queue || !*((uint8_t *)queue + 52)) {
        if (timer->timeout)
            return switch_a3_timer_lost(timer);
        return -25;
    }
    if (timer->wlc == wlc && timer->queue == queue && timer->timeout)
        return 0;
    if (timer->timeout)
        return switch_a3_timer_lost(timer);
    timer->wlc = wlc;
    timer->queue = queue;
    timer->callback = 0;
    timer->snapshot.armed = 0;
    timer->timeout = switch_a3_timeout_allocate(queue);
    timer->snapshot.status = timer->timeout ? 0 : -27;
    return timer->snapshot.status;
}

static void switch_a3_timer_expired(void *argument)
{
    struct switch_a3_timer *timer = argument;
    if (!timer || !timer->snapshot.armed)
        return;
    timer->snapshot.armed = 0;
    switch_a3_timer_callback callback = timer->callback;
    void *context = timer->context;
    if (!switch_a3_timer_owned(timer, timer->wlc)) {
        switch_a3_timer_lost(timer);
        if (callback)
            callback(context, timer->snapshot.requested_tsf, 0, timer->snapshot.session_generation,
                     timer->snapshot.clock_generation);
        return;
    }
    timer->snapshot.actual_tsf = 0;
    timer->snapshot.status = switch_a3_clock_read(timer->wlc, &timer->snapshot.actual_tsf);
    if (timer->snapshot.status)
        timer->snapshot.actual_tsf = 0;
    timer->fault = timer->snapshot.status ? SWITCH_A3_FAULT_CLOCK_UNSTABLE : SWITCH_A3_FAULT_NONE;
    if (!timer->snapshot.status && timer->snapshot.actual_tsf < timer->observed_tsf) {
        timer->snapshot.status = -35;
        timer->fault = SWITCH_A3_FAULT_CLOCK_BACKWARD;
    } else if (!timer->snapshot.status && timer->snapshot.actual_tsf < timer->snapshot.requested_tsf) {
        timer->fault = SWITCH_A3_FAULT_TIMER_EARLY;
        if (!timer->residual) {
            timer->residual = 1;
            timer->observed_tsf = timer->snapshot.actual_tsf;
            timer->snapshot.armed = 1;
            if (switch_a3_timeout_arm(timer->timeout,
                                      (uint32_t)(timer->snapshot.requested_tsf - timer->observed_tsf),
                                      switch_a3_timer_expired, timer))
                return;
            timer->snapshot.armed = 0;
            timer->fault = SWITCH_A3_FAULT_TIMER_ARM;
        }
        timer->snapshot.status = SWITCH_A3_TIMER_DELIVERY_FAILED;
    }
#if !SWITCH_A3_CANDIDATE
    if (timer == &switch_a3_timer)
        ++switch_a3_legacy_fired;
#endif
    timer->callback = 0;
    if (callback)
        callback(context, timer->snapshot.requested_tsf, timer->snapshot.actual_tsf, timer->snapshot.session_generation,
                 timer->snapshot.clock_generation);
}

static int switch_a3_timer_set(struct switch_a3_timer *timer, void *wlc, uint64_t deadline, uint32_t session,
                               uint32_t epoch, switch_a3_timer_callback callback, void *context, int defer)
{
    if (!timer || timer->wlc != wlc || !timer->timeout || !callback || !session || !epoch)
        return -2;
    if (!switch_a3_timer_owned(timer, wlc))
        return switch_a3_timer_lost(timer);
    if (defer && timer->deferred && timer->snapshot.armed && timer->snapshot.session_generation == session &&
        timer->snapshot.clock_generation == epoch && timer->callback == callback && timer->context == context)
        return 0;
    uint64_t now;
    int status = switch_a3_clock_read(wlc, &now);
    if (status) {
        timer->fault = SWITCH_A3_FAULT_CLOCK_UNSTABLE;
        return status;
    }
    if (defer) {
        if (now == UINT64_MAX)
            return -29;
        deadline = now + 1u;
    } else if (deadline <= now) {
        return SWITCH_A3_TIMER_ELAPSED;
    } else if (deadline - now > 0x7fffffffu) {
        return -29;
    }
    status = switch_a3_timer_cancel_owned(timer, wlc);
    if (status)
        return status;
    timer->snapshot.requested_tsf = deadline;
    timer->snapshot.session_generation = session;
    timer->snapshot.clock_generation = epoch;
    timer->observed_tsf = now;
    timer->residual = 0;
    timer->fault = SWITCH_A3_FAULT_NONE;
    timer->deferred = defer;
    timer->callback = callback;
    timer->context = context;
    timer->snapshot.armed = 1;
    status = switch_a3_timeout_arm(timer->timeout, (uint32_t)(deadline - now), switch_a3_timer_expired, timer);
    timer->snapshot.status = status ? 0 : -1;
    if (!status) {
        timer->snapshot.armed = 0;
        timer->callback = 0;
        timer->fault = SWITCH_A3_FAULT_TIMER_ARM;
    }
    return timer->snapshot.status;
}

int switch_a3_timer_arm_owned(struct switch_a3_timer *timer, void *wlc, uint64_t deadline, uint32_t session,
                              uint32_t epoch, switch_a3_timer_callback callback, void *context)
{
    return switch_a3_timer_set(timer, wlc, deadline, session, epoch, callback, context, 0);
}

int switch_a3_timer_defer_owned(struct switch_a3_timer *timer, void *wlc, uint32_t session, uint32_t epoch,
                                switch_a3_timer_callback callback, void *context)
{
    return switch_a3_timer_set(timer, wlc, 0, session, epoch, callback, context, 1);
}

int switch_a3_timer_initialize(void *wlc)
{
    return switch_a3_timer_initialize_owned(&switch_a3_timer, wlc);
}

static void switch_a3_timer_legacy_expired(void *context, uint64_t requested, uint64_t actual, uint32_t session,
                                           uint32_t epoch)
{
    (void)context;
    if (!switch_a3_timer.snapshot.status && switch_a3_legacy_callback)
        switch_a3_legacy_callback(switch_a3_legacy_context, requested, actual, session, epoch);
}

int switch_a3_timer_arm(void *wlc, uint64_t deadline, uint32_t session, uint32_t epoch,
                        switch_a3_timer_callback callback, void *context)
{
    if (!callback)
        return -2;
    const int status = switch_a3_timer_arm_owned(&switch_a3_timer, wlc, deadline, session, epoch,
                                                switch_a3_timer_legacy_expired, 0);
    if (!status) {
        switch_a3_legacy_callback = callback;
        switch_a3_legacy_context = context;
    }
    return status == SWITCH_A3_TIMER_ELAPSED ? -29 : status;
}

int switch_a3_timer_cancel(void *wlc)
{
    return switch_a3_timer_cancel_owned(&switch_a3_timer, wlc);
}

#if !SWITCH_A3_CANDIDATE
void switch_a3_timer_snapshot(struct switch_a3_deadline_snapshot *snapshot)
{
    snapshot->requested_tsf = switch_a3_timer.snapshot.requested_tsf;
    snapshot->actual_tsf = switch_a3_timer.snapshot.actual_tsf;
    snapshot->session_generation = switch_a3_timer.snapshot.session_generation;
    snapshot->clock_generation = switch_a3_timer.snapshot.clock_generation;
    snapshot->fired = switch_a3_legacy_fired;
    snapshot->armed = switch_a3_timer.snapshot.armed;
    snapshot->status = switch_a3_timer.snapshot.status;
    snapshot->reserved = 0;
}
#endif
