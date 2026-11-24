/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "timer.h"

extern void switch_a3_native_dispatch(void *queue);
extern int switch_a3_native_initialize(void *, uintptr_t, uintptr_t, uintptr_t, void *, uint32_t);

static uint32_t switch_wlc[512], switch_regs[128], switch_queue[14], switch_timeouts[2][6];
static struct switch_a3_timer switch_timers[2];
static uint64_t switch_now;
static unsigned switch_allocations, switch_calls, switch_rearm;
uint32_t switch_control_test_results[4];
volatile uint32_t switch_control_test_done;

#define SWITCH_REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (0)

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != switch_wlc)
        return -2;
    *tsf = switch_now;
    return 0;
}

void *switch_a3_timeout_allocate(void *queue)
{
    if (queue != switch_queue || switch_allocations == 2)
        return 0;
    uint32_t *timeout = switch_timeouts[switch_allocations++];
    timeout[1] = (uint32_t)queue;
    return timeout;
}

static void switch_callback(void *context, uint64_t requested, uint64_t actual, uint32_t session, uint32_t epoch)
{
    if (context != &switch_timers[0] || requested > actual || session != 2 || epoch != 3)
        switch_calls += 100;
    ++switch_calls;
    if (switch_rearm) {
        switch_rearm = 0;
        if (switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, actual + 30, 2, 3,
                                      switch_callback, &switch_timers[0]))
            switch_calls += 100;
    }
}

static void switch_fire(uint64_t now)
{
    switch_now = now;
    switch_regs[0x180 / 4] = (uint32_t)now;
    switch_a3_native_dispatch(switch_queue);
}

static uint32_t switch_test(unsigned scenario)
{
    for (unsigned index = 0; index < sizeof switch_timers; ++index)
        ((uint8_t *)switch_timers)[index] = 0;
    for (unsigned index = 0; index < sizeof switch_timeouts; ++index)
        ((uint8_t *)switch_timeouts)[index] = 0;
    for (unsigned index = 0; index < sizeof switch_queue; ++index)
        ((uint8_t *)switch_queue)[index] = 0;
    switch_calls = switch_allocations = switch_rearm = 0;
    switch_now = scenario == 3 ? UINT64_C(0xfffffff0) : 100;
    switch_regs[0x180 / 4] = (uint32_t)switch_now;
    switch_wlc[3] = (uint32_t)switch_regs;
    switch_wlc[0x5f8 / 4] = (uint32_t)switch_queue;
    SWITCH_REQUIRE(!switch_a3_native_initialize(switch_queue, 0x54455, 0x5445d, 0x5444d,
                                                switch_wlc, UINT32_MAX));
    SWITCH_REQUIRE(!switch_a3_timer_initialize_owned(&switch_timers[0], switch_wlc));
    SWITCH_REQUIRE(!switch_a3_timer_initialize_owned(&switch_timers[1], switch_wlc));
    const uint64_t deadline = switch_now + (scenario == 3 ? 32 : 20);
    SWITCH_REQUIRE(!switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, deadline, 2, 3,
                                              switch_callback, &switch_timers[0]));
    SWITCH_REQUIRE(switch_regs[0x18 / 4] == (scenario == 3 ? 32u : 20u));
    if (scenario == 0) {
        SWITCH_REQUIRE(!switch_a3_timer_cancel_owned(&switch_timers[0], switch_wlc));
        SWITCH_REQUIRE(!switch_a3_timer_arm_owned(&switch_timers[0], switch_wlc, 150, 2, 3,
                                                  switch_callback, &switch_timers[0]));
        switch_fire(120);
        SWITCH_REQUIRE(!switch_calls && switch_timers[0].snapshot.armed && switch_regs[0x18 / 4] == 30);
        switch_fire(150);
    } else if (scenario == 1) {
        SWITCH_REQUIRE(!switch_a3_timer_arm_owned(&switch_timers[1], switch_wlc, 140, 2, 3,
                                                  switch_callback, &switch_timers[0]));
        SWITCH_REQUIRE(!switch_a3_timer_cancel_owned(&switch_timers[0], switch_wlc));
        switch_fire(120);
        SWITCH_REQUIRE(!switch_calls && switch_timers[1].snapshot.armed && switch_regs[0x18 / 4] == 20);
        switch_fire(140);
    } else if (scenario == 2) {
        switch_rearm = 1;
        switch_fire(120);
        SWITCH_REQUIRE(switch_calls == 1 && switch_timers[0].snapshot.armed && switch_regs[0x18 / 4] == 30);
        switch_fire(150);
    } else {
        switch_fire(deadline);
    }
    SWITCH_REQUIRE(switch_calls == (scenario == 2 ? 2u : 1u));
    return 0;
}

void switch_control_test_run(void)
{
    for (unsigned index = 0; index < 4; ++index)
        switch_control_test_results[index] = switch_test(index);
    switch_control_test_done = 0x53574133;
}
