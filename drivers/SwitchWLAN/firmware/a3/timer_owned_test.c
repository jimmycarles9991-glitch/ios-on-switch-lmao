/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "timer.h"

static uint32_t switch_wlc[512];
static uint8_t switch_queue[56], switch_other_queue[56];
static struct switch_a3_timer switch_timers[2];

static struct {
    void (*callback)(void *);
    void *argument;
    uint32_t armed, delay;
} switch_native[2];

static uint64_t switch_now;
static uint32_t switch_allocations, switch_calls, switch_rearm;
static int switch_clock_error;
uint32_t switch_control_test_results[13];
volatile uint32_t switch_control_test_done;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != switch_wlc)
        return -2;
    *tsf = switch_now;
    return switch_clock_error;
}

void *switch_a3_timeout_allocate(void *queue)
{
    return queue == switch_queue && switch_allocations < 2 ? &switch_native[switch_allocations++] : 0;
}

int switch_a3_timeout_arm(void *timeout, uint32_t delay, void (*callback)(void *), void *argument)
{
    for (unsigned index = 0; index < 2; ++index)
        if (timeout == &switch_native[index] && !switch_native[index].armed && delay) {
            switch_native[index].armed = 1;
            switch_native[index].delay = delay;
            switch_native[index].callback = callback;
            switch_native[index].argument = argument;
            return 1;
        }
    return 0;
}

void switch_a3_timeout_cancel(void *timeout)
{
    for (unsigned index = 0; index < 2; ++index)
        if (timeout == &switch_native[index])
            switch_native[index].armed = 0;
}

static void switch_callback(void *context, uint64_t requested, uint64_t actual, uint32_t session, uint32_t epoch)
{
    (void)requested;
    if (context == &switch_timers[0] && session == 2 && epoch == 3)
        ++switch_calls;
    if (switch_rearm) {
        switch_rearm = 0;
        if (switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, actual + 20, 2, 3, switch_callback,
                                      &switch_timers[0]))
            switch_calls += 100;
    }
}

static void switch_fire(unsigned index, uint64_t now)
{
    switch_now = now;
    switch_native[index].armed = 0;
    switch_native[index].callback(switch_native[index].argument);
}

static uint32_t switch_test(unsigned scenario)
{
    for (unsigned index = 0; index < sizeof switch_timers; ++index)
        ((uint8_t *)switch_timers)[index] = 0;
    for (unsigned index = 0; index < sizeof switch_native; ++index)
        ((uint8_t *)switch_native)[index] = 0;
    switch_now = 100;
    switch_allocations = switch_calls = switch_rearm = switch_clock_error = 0;
    switch_wlc[0x5f8 / 4] = (uint32_t)switch_queue;
    switch_queue[52] = switch_other_queue[52] = 1;
    SWITCH_REQUIRE(!switch_a3_timer_initialize_owned(&switch_timers[0], switch_wlc));
    SWITCH_REQUIRE(!switch_a3_timer_initialize_owned(&switch_timers[1], switch_wlc));
    SWITCH_REQUIRE(switch_allocations == 2 && switch_timers[0].timeout != switch_timers[1].timeout);
    if (scenario == 3) {
        SWITCH_REQUIRE(switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, 100, 2, 3, switch_callback,
                                                 &switch_timers[0]) == SWITCH_A3_TIMER_ELAPSED);
        SWITCH_REQUIRE(switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, UINT64_C(0x80000064), 2, 3,
                                                 switch_callback, &switch_timers[0]) == -29);
        return 0;
    }
    SWITCH_REQUIRE(
        !switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, 120, 2, 3, switch_callback, &switch_timers[0]));
    SWITCH_REQUIRE(
        !switch_a3_timer_arm_owned(&switch_timers[1], switch_wlc, 140, 2, 3, switch_callback, &switch_timers[0]));
    if (scenario >= 8) {
        switch_fire(0, scenario == 10 || scenario == 12 ? 90 : 110);
        if (scenario == 10 || scenario == 12) {
            SWITCH_REQUIRE(switch_calls == 1 && switch_timers[0].snapshot.status == -35);
        } else {
            SWITCH_REQUIRE(!switch_calls && switch_timers[0].snapshot.armed && switch_native[0].delay == 10);
            switch_fire(0, scenario == 8 ? 120 : 115);
            SWITCH_REQUIRE(switch_calls == 1 && !switch_timers[0].snapshot.armed);
            SWITCH_REQUIRE(scenario == 8 ? !switch_timers[0].snapshot.status : switch_timers[0].snapshot.status != 0);
        }
        if (scenario >= 11) {
            switch_clock_error = -35;
            SWITCH_REQUIRE(switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, 150, 2, 3,
                                                     switch_callback, &switch_timers[0]) == -35);
            SWITCH_REQUIRE(switch_timers[0].fault == SWITCH_A3_FAULT_CLOCK_UNSTABLE);
        }
        return 0;
    }
    if (scenario == 1)
        switch_clock_error = -35;
    if (scenario == 2 || scenario == 5)
        switch_wlc[0x5f8 / 4] = (uint32_t)switch_other_queue;
    if (scenario == 5) {
        SWITCH_REQUIRE(switch_a3_timer_initialize_owned(&switch_timers[0], switch_wlc) == -1);
        SWITCH_REQUIRE(switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, 120, 2, 3, switch_callback,
                                                 &switch_timers[0]) == -1);
        return 0;
    }
    if (scenario == 4)
        switch_rearm = 1;
    if (scenario == 7) {
        SWITCH_REQUIRE(!switch_a3_timer_cancel_owned(&switch_timers[0], switch_wlc));
        switch_fire(0, 120);
        SWITCH_REQUIRE(!switch_calls && switch_timers[1].snapshot.armed);
        switch_fire(1, 140);
        SWITCH_REQUIRE(switch_calls == 1);
        return 0;
    }
    switch_fire(0, 120);
    SWITCH_REQUIRE(switch_calls == 1 && switch_timers[1].snapshot.armed);
    if (scenario == 1 || scenario == 2) {
        SWITCH_REQUIRE(switch_timers[0].snapshot.status == (scenario == 1 ? -35 : -1));
        return 0;
    }
    if (scenario == 4) {
        SWITCH_REQUIRE(switch_timers[0].snapshot.armed && switch_native[0].delay == 20);
        switch_fire(0, 140);
        SWITCH_REQUIRE(switch_calls == 2);
    } else if (scenario == 6) {
        SWITCH_REQUIRE(!switch_a3_timer_cancel_owned(&switch_timers[0], switch_wlc));
        SWITCH_REQUIRE(!switch_a3_timer_cancel_owned(&switch_timers[0], switch_wlc));
        SWITCH_REQUIRE(
            !switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, 150, 2, 3, switch_callback, &switch_timers[0]));
        switch_fire(0, 150);
        SWITCH_REQUIRE(switch_calls == 2);
    }
    SWITCH_REQUIRE(!switch_a3_timer_cancel_owned(&switch_timers[1], switch_wlc));
    const uint32_t calls = switch_calls;
    switch_fire(1, 160);
    SWITCH_REQUIRE(switch_calls == calls);
    return 0;
}

void switch_control_test_run(void)
{
    for (unsigned index = 0; index < 13; ++index)
        switch_control_test_results[index] = switch_test(index);
    switch_control_test_done = 0x53574133;
}
