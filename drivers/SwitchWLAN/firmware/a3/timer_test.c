/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "clock.h"
#include "timer.h"
#include <stddef.h>

static uint64_t switch_samples[3];
static uint32_t switch_reads, switch_allocations, switch_cancellations, switch_arms, switch_calls;
static uint32_t switch_fail_allocate, switch_fail_arm, switch_native_armed, switch_delay, switch_rearm;
static int32_t switch_rearm_status;
static void (*switch_due)(void *);
static void *switch_argument;
static uint64_t switch_requested, switch_actual;
static uint32_t switch_session, switch_epoch;
static uint64_t switch_wlc[192], switch_queue[7], switch_other_queue[7], switch_timeout[3];
uint32_t switch_timer_test_results[11];
volatile uint32_t switch_timer_test_done;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static void switch_now(uint64_t now)
{
    switch_reads = 0;
    for (size_t index = 0; index < 3; ++index)
        switch_samples[index] = now;
}

void switch_a3_tsf_read(void *wlc, uint32_t *low, uint32_t *high)
{
    (void)wlc;
    const size_t index = switch_reads < 3 ? switch_reads : 2;
    ++switch_reads;
    *low = (uint32_t)switch_samples[index];
    *high = (uint32_t)(switch_samples[index] >> 32);
}

void *switch_a3_timeout_allocate(void *queue)
{
    ++switch_allocations;
    if (queue != switch_queue || switch_fail_allocate)
        return NULL;
    return switch_timeout;
}

int switch_a3_timeout_arm(void *timeout, uint32_t delay, void (*callback)(void *), void *argument)
{
    ++switch_arms;
    if (timeout != switch_timeout || !delay || switch_native_armed || switch_fail_arm)
        return 0;
    switch_delay = delay;
    switch_due = callback;
    switch_argument = argument;
    switch_native_armed = 1;
    return 1;
}

void switch_a3_timeout_cancel(void *timeout)
{
    if (timeout == switch_timeout) {
        ++switch_cancellations;
        switch_native_armed = 0;
    }
}

static void switch_callback(void *context, uint64_t requested, uint64_t actual, uint32_t session, uint32_t epoch)
{
    if (context != switch_queue)
        switch_calls += 100;
    ++switch_calls;
    switch_requested = requested;
    switch_actual = actual;
    switch_session = session;
    switch_epoch = epoch;
    if (switch_rearm) {
        switch_rearm = 0;
        switch_rearm_status = switch_a3_timer_arm(switch_wlc, actual + 100, 9, 10, switch_callback, switch_queue);
    }
}

static void switch_fire(uint64_t now)
{
    switch_now(now);
    switch_native_armed = 0;
    switch_due(switch_argument);
}

static uint32_t switch_test_clock(void)
{
    uint64_t value = 77;
    switch_now(UINT64_C(0x300000064));
    SWITCH_REQUIRE(!switch_a3_clock_read(switch_wlc, &value));
    SWITCH_REQUIRE(value == UINT64_C(0x300000064) && switch_reads == 2);
    switch_reads = 0;
    switch_samples[0] = UINT64_C(0x3fffffff0);
    switch_samples[1] = UINT64_C(0x4fffffffe);
    switch_samples[2] = UINT64_C(0x400000005);
    SWITCH_REQUIRE(!switch_a3_clock_read(switch_wlc, &value));
    SWITCH_REQUIRE(value == UINT64_C(0x400000005) && switch_reads == 3);
    switch_reads = 0;
    switch_samples[0] = UINT64_C(0x300000001);
    switch_samples[1] = UINT64_C(0x400000002);
    switch_samples[2] = UINT64_C(0x500000003);
    SWITCH_REQUIRE(switch_a3_clock_read(switch_wlc, &value) == -35);
    SWITCH_REQUIRE(value == UINT64_C(0x400000005));
    SWITCH_REQUIRE(switch_a3_clock_read(NULL, &value) == -2);
    SWITCH_REQUIRE(switch_a3_clock_read(switch_wlc, NULL) == -2);
    return 0;
}

static uint32_t switch_test_initialize(void)
{
    SWITCH_REQUIRE(switch_a3_timer_initialize(NULL) == -2);
    SWITCH_REQUIRE(switch_a3_timer_initialize(switch_wlc) == -25);
    *(void **)((uint8_t *)switch_wlc + 0x5f8) = switch_queue;
    SWITCH_REQUIRE(switch_a3_timer_initialize(switch_wlc) == -25);
    *((uint8_t *)switch_queue + 52) = 1;
    switch_fail_allocate = 1;
    SWITCH_REQUIRE(switch_a3_timer_initialize(switch_wlc) == -27);
    switch_fail_allocate = 0;
    SWITCH_REQUIRE(!switch_a3_timer_initialize(switch_wlc));
    SWITCH_REQUIRE(!switch_a3_timer_initialize(switch_wlc) && switch_allocations == 2);
    return 0;
}

static uint32_t switch_test_bounds(void)
{
    switch_now(1000);
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 1000, 7, 8, switch_callback, switch_queue) == -29);
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 999, 7, 8, switch_callback, switch_queue) == -29);
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, UINT64_C(0x800003e8), 7, 8, switch_callback, switch_queue) == -29);
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 1100, 0, 8, switch_callback, switch_queue) == -2);
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 1100, 7, 0, switch_callback, switch_queue) == -2);
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 1100, 7, 8, NULL, switch_queue) == -2);
    SWITCH_REQUIRE(!switch_arms);
    return 0;
}

static uint32_t switch_test_expire(void)
{
    switch_now(1000);
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, 1100, 7, 8, switch_callback, switch_queue));
    SWITCH_REQUIRE(switch_delay == 100 && switch_native_armed);
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 1000, 9, 10, switch_callback, switch_other_queue) == -29);
    switch_fire(1105);
    struct switch_a3_deadline_snapshot snapshot;
    switch_a3_timer_snapshot(&snapshot);
    SWITCH_REQUIRE(switch_calls == 1 && switch_requested == 1100 && switch_actual == 1105);
    SWITCH_REQUIRE(switch_session == 7 && switch_epoch == 8 && !snapshot.armed && snapshot.fired == 1);
    SWITCH_REQUIRE(!snapshot.status && !snapshot.reserved);
    return 0;
}

static uint32_t switch_test_cancel(void)
{
    switch_now(1200);
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, 1300, 7, 8, switch_callback, switch_queue));
    SWITCH_REQUIRE(!switch_a3_timer_cancel(switch_wlc));
    switch_fire(1400);
    SWITCH_REQUIRE(switch_calls == 1 && !switch_native_armed);
    return 0;
}

static uint32_t switch_test_rearm(void)
{
    switch_now(1500);
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, 1600, 7, 8, switch_callback, switch_queue));
    const uint32_t cancelled = switch_cancellations;
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, 1700, 9, 10, switch_callback, switch_queue));
    SWITCH_REQUIRE(switch_cancellations == cancelled + 1 && switch_delay == 200);
    switch_fire(1700);
    SWITCH_REQUIRE(switch_calls == 2 && switch_requested == 1700 && switch_session == 9 && switch_epoch == 10);
    return 0;
}

static uint32_t switch_test_reentrant(void)
{
    switch_now(1800);
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, 1900, 7, 8, switch_callback, switch_queue));
    switch_rearm = 1;
    switch_fire(1901);
    SWITCH_REQUIRE(!switch_rearm_status && switch_native_armed && switch_calls == 3);
    switch_fire(2001);
    SWITCH_REQUIRE(switch_calls == 4 && switch_requested == 2001 && switch_session == 9 && switch_epoch == 10);
    return 0;
}

static uint32_t switch_test_arm_failure(void)
{
    switch_now(2100);
    switch_fail_arm = 1;
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 2200, 7, 8, switch_callback, switch_queue) == -1);
    switch_fail_arm = 0;
    struct switch_a3_deadline_snapshot snapshot;
    switch_a3_timer_snapshot(&snapshot);
    SWITCH_REQUIRE(!snapshot.armed && snapshot.status == -1);
    switch_fire(2200);
    SWITCH_REQUIRE(switch_calls == 4);
    return 0;
}

static uint32_t switch_test_early(void)
{
    switch_now(2300);
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, 2400, 7, 8, switch_callback, switch_queue));
    switch_fire(2399);
    struct switch_a3_deadline_snapshot snapshot;
    switch_a3_timer_snapshot(&snapshot);
    SWITCH_REQUIRE(snapshot.armed && !snapshot.status && snapshot.actual_tsf == 2399 && switch_delay == 1);
    switch_fire(2399);
    switch_a3_timer_snapshot(&snapshot);
    SWITCH_REQUIRE(!snapshot.armed && snapshot.status == SWITCH_A3_TIMER_DELIVERY_FAILED);
    SWITCH_REQUIRE(switch_calls == 4);
    return 0;
}

static uint32_t switch_test_wrap_deadline(void)
{
    switch_now(UINT64_C(0x3fffffff0));
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, UINT64_C(0x400000010), 7, 8, switch_callback, switch_queue));
    SWITCH_REQUIRE(switch_delay == 32);
    switch_fire(UINT64_C(0x400000010));
    SWITCH_REQUIRE(switch_calls == 5 && switch_actual == UINT64_C(0x400000010));
    return 0;
}

static uint32_t switch_test_queue_loss(void)
{
    switch_now(3000);
    SWITCH_REQUIRE(!switch_a3_timer_arm(switch_wlc, 3100, 7, 8, switch_callback, switch_queue));
    const uint32_t arms = switch_arms, cancels = switch_cancellations;
    *((uint8_t *)switch_other_queue + 52) = 1;
    *(void **)((uint8_t *)switch_wlc + 0x5f8) = switch_other_queue;
#if SWITCH_QUEUE_VARIANT == 0
    switch_fire(3100);
#elif SWITCH_QUEUE_VARIANT == 1
    SWITCH_REQUIRE(switch_a3_timer_arm(switch_wlc, 3200, 7, 8, switch_callback, switch_queue) == -1);
#elif SWITCH_QUEUE_VARIANT == 2
    SWITCH_REQUIRE(switch_a3_timer_initialize(switch_wlc) == -1);
#elif SWITCH_QUEUE_VARIANT == 3
    SWITCH_REQUIRE(switch_a3_timer_cancel(switch_wlc) == -1);
#else
    *(void **)((uint8_t *)switch_wlc + 0x5f8) = switch_queue;
    *((uint8_t *)switch_queue + 52) = 0;
    SWITCH_REQUIRE(switch_a3_timer_initialize(switch_wlc) == -1);
#endif
    struct switch_a3_deadline_snapshot snapshot;
    switch_a3_timer_snapshot(&snapshot);
    SWITCH_REQUIRE(!snapshot.armed && snapshot.status == -1 && switch_calls == 5);
    SWITCH_REQUIRE(switch_arms == arms && switch_cancellations == cancels);
    *(void **)((uint8_t *)switch_wlc + 0x5f8) = switch_queue;
    *((uint8_t *)switch_queue + 52) = 1;
    SWITCH_REQUIRE(switch_a3_timer_initialize(switch_wlc) == -1);
    return 0;
}

void switch_timer_test_run(void)
{
    switch_timer_test_results[0] = switch_test_clock();
    switch_timer_test_results[1] = switch_test_initialize();
    switch_timer_test_results[2] = switch_test_bounds();
    switch_timer_test_results[3] = switch_test_expire();
    switch_timer_test_results[4] = switch_test_cancel();
    switch_timer_test_results[5] = switch_test_rearm();
    switch_timer_test_results[6] = switch_test_reentrant();
    switch_timer_test_results[7] = switch_test_arm_failure();
    switch_timer_test_results[8] = switch_test_early();
    switch_timer_test_results[9] = switch_test_wrap_deadline();
    switch_timer_test_results[10] = switch_test_queue_loss();
    switch_timer_test_done = 0x53574133;
}
