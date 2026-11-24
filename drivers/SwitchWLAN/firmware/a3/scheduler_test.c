/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "scheduler.h"
#include "channel.h"
#include "excursion.h"
#include "timer.h"
#include "diagnostic.h"
#include "control.h"

static uint32_t switch_wlc[512], switch_band[8], switch_pi, switch_bsscfg[128];
static uint64_t switch_now, switch_tbtt, switch_timer_due;
static uint16_t switch_phy, switch_home, switch_restricted;
static uint32_t switch_pending_tx;
static int switch_clock_error, switch_beacon_error, switch_start_error, switch_cancel_deferred;
static int switch_ack_error;
static unsigned switch_clock_step, switch_native_arms;
static uint8_t switch_timer_queue[56], switch_timer_handle;
static void (*switch_timer_callback)(void *);
static void *switch_timer_argument;
static unsigned switch_starts, switch_changed, switch_wakes, switch_native_pending;
static int switch_last_error;
static struct switch_a3_timer *switch_due;
static struct switch_a3_excursion_request switch_request;
static switch_a3_excursion_callback switch_callback;
static void *switch_context;
uint32_t switch_control_test_results[39];
volatile uint32_t switch_control_test_done;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

void *memset(void *destination, int value, size_t bytes)
{
    for (size_t index = 0; index < bytes; ++index)
        ((uint8_t *)destination)[index] = value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t bytes)
{
    for (size_t index = 0; index < bytes; ++index)
        ((uint8_t *)destination)[index] = ((const uint8_t *)source)[index];
    return destination;
}

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != switch_wlc)
        return -2;
    *tsf = switch_now;
    switch_now += switch_clock_step;
    return switch_clock_error;
}

int switch_a3_ack_reconcile(void *wlc)
{
    return wlc == switch_wlc ? switch_ack_error : -2;
}

uint16_t switch_a3_tx_phy_chanspec(void *pi)
{
    return pi == &switch_pi ? switch_phy : 0;
}

void *switch_a3_station_bsscfg(void *wlc)
{
    return wlc == switch_wlc ? switch_bsscfg : 0;
}

uint16_t switch_a3_channel_home(void *wlc)
{
    return wlc == switch_wlc ? switch_home : 0;
}

uint16_t switch_a3_channel_control(uint32_t chanspec)
{
    return chanspec == 0xe02a ? 0xd024 : chanspec;
}

int switch_a3_channel_usable(void *wlc, uint16_t chanspec)
{
    return wlc == switch_wlc && chanspec != switch_restricted &&
           (chanspec == 0xd024 || chanspec == 0x1006 || chanspec == 0xd02c);
}

void switch_a3_beacon_reset(void *wlc)
{
    (void)wlc;
}

int switch_a3_beacon_bound(void *wlc, uint16_t home, uint64_t now, void **bsscfg, uint64_t *until)
{
    if (wlc != switch_wlc || home != switch_home || now > switch_now)
        return -2;
    *bsscfg = switch_bsscfg;
    *until = switch_tbtt;
    return switch_beacon_error;
}

void *switch_a3_timeout_allocate(void *queue)
{
    return queue == switch_timer_queue ? &switch_timer_handle : 0;
}

void switch_a3_timeout_cancel(void *timeout)
{
    if (timeout == &switch_timer_handle)
        switch_timer_callback = 0;
}

int switch_a3_timeout_arm(void *timeout, uint32_t delay, void (*callback)(void *), void *argument)
{
    if (timeout != &switch_timer_handle || !delay || switch_timer_callback || !switch_timer_queue[52])
        return 0;
    ++switch_native_arms;
    switch_due = argument;
    switch_timer_argument = argument;
    switch_timer_callback = callback;
    switch_timer_due = switch_now + delay;
    return 1;
}

uint32_t switch_a3_tx_pending(void)
{
    return switch_pending_tx;
}

void switch_a3_tx_wake(void *wlc)
{
    if (wlc == switch_wlc)
        ++switch_wakes;
}

int switch_a3_excursion_start(const struct switch_a3_excursion_request *request, switch_a3_excursion_callback callback,
                              void *context)
{
    ++switch_starts;
    switch_request = *request;
    switch_callback = callback;
    switch_context = context;
    switch_native_pending = !switch_start_error;
    return switch_start_error;
}

static void switch_complete_channel(uint32_t reason, uint32_t channel_result)
{
    struct switch_a3_excursion_result result = {
        .returned_tsf = switch_now,
        .session = switch_request.session,
        .clock_epoch = switch_request.clock_epoch,
        .generation = switch_request.generation,
        .reason = reason,
        .channel_result = channel_result,
    };
    switch_native_pending = 0;
    switch_phy = switch_home;
    switch_callback(switch_context, SWITCH_A3_EXCURSION_COMPLETED, &result);
}

static void switch_complete(uint32_t reason)
{
    switch_complete_channel(reason, 0);
}

int switch_a3_excursion_cancel(void *wlc, uint32_t session, uint32_t epoch, uint32_t generation)
{
    if (wlc != switch_wlc || session != 2 || epoch != 3 || generation != 4)
        return -2;
    if (!switch_cancel_deferred)
        switch_complete(SWITCH_A3_EXCURSION_CANCELLED);
    return 0;
}

void switch_a3_excursion_rx_ready(void *wlc)
{
    (void)wlc;
}

uint32_t switch_a3_excursion_pending(void)
{
    return switch_native_pending;
}

#if DEVELOPMENT || DEBUG
void switch_a3_control_record(uint16_t reason, int error, const struct switch_a3_owner *owner,
                               const struct switch_a3_timer *timer, const uint64_t *actual, uint32_t channels)
{
    (void)error;
    (void)owner;
    (void)timer;
    (void)channels;
    (void)actual;
    switch_a3_diagnostic.latest_fault.reason = reason;
}
#endif

static void switch_change(void *context, int error)
{
    if (context == &switch_pi)
        ++switch_changed;
    switch_last_error = error;
}

static void switch_fire(uint64_t now)
{
    switch_now = now;
    if (switch_timer_callback && now >= switch_timer_due) {
        void (*callback)(void *) = switch_timer_callback;
        switch_timer_callback = 0;
        callback(switch_timer_argument);
    }
}

static struct switch_a3_schedule switch_reset(void)
{
    switch_a3_scheduler_stop();
    switch_native_pending = 0;
    switch_now = 1000;
    switch_clock_step = switch_native_arms = 0;
    switch_tbtt = 100000;
    switch_phy = switch_home = 0xd024;
    switch_restricted = 0;
    switch_wlc[8] = (uint32_t)switch_band;
    switch_wlc[0x5f8 / 4] = (uint32_t)switch_timer_queue;
    switch_timer_queue[52] = 1;
    switch_band[4] = (uint32_t)&switch_pi;
    *((uint8_t *)switch_wlc + 25) = 1;
    *((uint8_t *)switch_bsscfg + 8) = 0;
    *((uint8_t *)switch_bsscfg + 16) = 1;
    switch_clock_error = switch_beacon_error = switch_start_error = switch_cancel_deferred = 0;
    switch_ack_error = 0;
    switch_starts = switch_changed = switch_wakes = switch_pending_tx = 0;
    switch_last_error = 0;
    struct switch_a3_schedule schedule = {
        .anchor_tsf = 1000,
        .period_us = 65536,
        .infra_chanspec = 0xd024,
        .slot_count = 2,
        .slots = {{0xd024, 0, 32768}, {0, 0, 32768}},
    };
    return schedule;
}

static int switch_start(const struct switch_a3_schedule *schedule)
{
    uint16_t channel;
    int result = switch_a3_scheduler_install(switch_wlc, schedule);
    if (!result)
        result = switch_a3_scheduler_start(switch_wlc, &channel);
    if (!result)
        result = switch_a3_scheduler_run(switch_wlc, 2, 3, 4, switch_change, &switch_pi);
    return result;
}

static uint32_t switch_test(unsigned scenario)
{
    struct switch_a3_schedule schedule = switch_reset();
    if (scenario < 8) {
        if (scenario == 0)
            schedule.slot_count = 0;
        if (scenario == 1)
            schedule.slot_count = 17;
        if (scenario == 2)
            schedule.period_us = 0;
        if (scenario == 3)
            schedule.period_us = 0x80000000u;
        if (scenario == 4)
            --schedule.slots[0].dwell_us;
        if (scenario == 5)
            schedule.slots[2].reserved = 1;
        if (scenario == 6)
            schedule.slots[0].chanspec = 0xd034;
        if (scenario == 7)
            schedule.anchor_tsf = UINT64_MAX;
        SWITCH_REQUIRE(switch_start(&schedule) == (scenario == 6 ? -23 : -2));
        return 0;
    }
    if (scenario == 8) {
        schedule.anchor_tsf = 2000;
        SWITCH_REQUIRE(!switch_start(&schedule));
        uint64_t until;
        SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) == -35);
        SWITCH_REQUIRE(switch_due->snapshot.requested_tsf == 2000);
        switch_fire(2000);
        SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) && until == 34768);
        return 0;
    }
    if (scenario == 9) {
        SWITCH_REQUIRE(!switch_start(&schedule));
        uint64_t until;
        SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) && until == 33768);
        switch_fire(33768);
        SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) == -35);
        switch_fire(66536);
        SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0xd024, &until));
        return 0;
    }
    if (scenario == 10) {
        switch_now = UINT64_C(0x3000083e8);
        SWITCH_REQUIRE(!switch_start(&schedule));
        uint64_t until;
        SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) == -35);
        return 0;
    }
    if (scenario == 11) {
        switch_phy = switch_home = 0xe02a;
        SWITCH_REQUIRE(!switch_start(&schedule));
        uint64_t until;
        SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0xd024, &until));
        return 0;
    }
    if (scenario >= 30) {
        uint64_t until;
        SWITCH_REQUIRE(!switch_start(&schedule));
        if (scenario == 31 || scenario == 32) {
            switch_phy = 0x1006;
            switch_fire(switch_timer_due);
            SWITCH_REQUIRE(switch_changed == 1 && !switch_last_error && !switch_timer_callback &&
                           switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) == -35);
            if (scenario == 32) {
                switch_home = 0x1006;
                switch_a3_scheduler_observed(switch_wlc);
                SWITCH_REQUIRE(switch_changed == 2 && switch_last_error == -25 &&
                               switch_a3_scheduler_fault_reason() == SWITCH_A3_FAULT_HOME_CHANGED);
            } else {
                switch_phy = switch_home;
                switch_a3_scheduler_observed(switch_wlc);
                SWITCH_REQUIRE(switch_timer_callback && !switch_last_error);
            }
            return 0;
        }
        SWITCH_REQUIRE(!switch_a3_scheduler_quiesce());
        if (scenario == 34) {
            switch_phy = 0x1006;
            switch_a3_scheduler_observed(switch_wlc);
            SWITCH_REQUIRE(!switch_changed);
            switch_phy = switch_home;
            switch_a3_scheduler_observed(switch_wlc);
            switch_a3_scheduler_observed(switch_wlc);
            SWITCH_REQUIRE(switch_changed == 1 && !switch_last_error && !switch_timer_callback);
            return 0;
        }
        schedule.slots[0].chanspec = 0;
        SWITCH_REQUIRE(!switch_a3_scheduler_install(switch_wlc, &schedule));
        SWITCH_REQUIRE(!switch_a3_scheduler_run(switch_wlc, 2, 3, 5, switch_change, &switch_pi));
        const unsigned arms = switch_native_arms;
        if (scenario == 33) {
            switch_phy = switch_home = 0x1006;
            switch_pending_tx = 1;
            switch_a3_scheduler_observed(switch_wlc);
            SWITCH_REQUIRE(!switch_changed && switch_native_arms == arms);
            switch_pending_tx = 0;
            switch_a3_scheduler_observed(switch_wlc);
            SWITCH_REQUIRE(switch_changed == 1 && !switch_last_error);
            schedule.infra_chanspec = schedule.slots[0].chanspec = 0x1006;
            SWITCH_REQUIRE(!switch_a3_scheduler_install(switch_wlc, &schedule));
            SWITCH_REQUIRE(!switch_a3_scheduler_run(switch_wlc, 2, 3, 6, switch_change, &switch_pi));
            SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0x1006, &until));
        } else {
            switch_a3_scheduler_observed(switch_wlc);
            switch_a3_scheduler_observed(switch_wlc);
            SWITCH_REQUIRE(!switch_changed && switch_native_arms == arms && !switch_timer_callback);
        }
        return 0;
    }
    if (scenario >= 24) {
        uint64_t until;
        if (scenario == 24) {
            schedule.anchor_tsf = switch_now + 1;
            switch_clock_step = 1;
            SWITCH_REQUIRE(!switch_start(&schedule) && !switch_last_error && switch_native_arms == 1);
            SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) == -35);
            switch_clock_step = 0;
            switch_fire(switch_timer_due);
            SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) && until == 33769);
            return 0;
        }
        if (scenario == 28)
            schedule.slots[1].chanspec = 0x1006;
        SWITCH_REQUIRE(!switch_start(&schedule) && switch_native_arms == 1);
        if (scenario == 25) {
            switch_a3_scheduler_observed(switch_wlc);
            switch_a3_scheduler_observed(switch_wlc);
            SWITCH_REQUIRE(switch_native_arms == 2);
            SWITCH_REQUIRE(!switch_a3_scheduler_stop() && !switch_timer_callback);
            switch_fire(switch_timer_due);
            SWITCH_REQUIRE(switch_native_arms == 2 && !switch_changed);
            return 0;
        }
        if (scenario == 27) {
            SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0xd024, &until));
            switch_restricted = 0xd024;
            SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) == -35);
            SWITCH_REQUIRE(!switch_changed && switch_native_arms == 1 && switch_due->snapshot.armed);
            return 0;
        }
        if (scenario == 28) {
            switch_restricted = 0x1006;
            switch_fire(33768);
            SWITCH_REQUIRE(!switch_starts && !switch_changed && switch_phy == switch_home);
            SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0x1006, &until) == -35);
            SWITCH_REQUIRE(switch_due->snapshot.requested_tsf == 66536);
            switch_fire(66536);
            SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0xd024, &until));
            return 0;
        }
        if (scenario == 26)
            switch_clock_error = -2;
        else
            switch_wlc[0x5f8 / 4] = 0;
        switch_fire(switch_timer_due);
        SWITCH_REQUIRE(switch_changed == 1 && switch_last_error == (scenario == 26 ? -2 : -1));
        SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0xd024, &until) == -35);
        return 0;
    }
    schedule.slots[0].chanspec = 0x1006;
    if (scenario == 12) {
        SWITCH_REQUIRE(!switch_start(&schedule) && switch_starts == 1 && switch_native_pending);
        SWITCH_REQUIRE(switch_request.target_chanspec == 0x1006 && switch_request.home_chanspec == 0xd024);
        SWITCH_REQUIRE(switch_request.expiry_tsf == 33768 && !switch_request.station_bound_verified);
        return 0;
    }
    *((uint8_t *)switch_bsscfg + 8) = 1;
    schedule.period_us = 200000;
    schedule.slots[0].dwell_us = 160000;
    schedule.slots[1].dwell_us = 40000;
    if (scenario == 13) {
        SWITCH_REQUIRE(!switch_start(&schedule) && switch_starts == 1);
        SWITCH_REQUIRE(switch_request.expiry_tsf == 89760 && switch_request.station_return_tsf == 100000);
        SWITCH_REQUIRE(switch_request.station_bound_verified);
        return 0;
    }
    if (scenario == 14) {
        switch_beacon_error = -16;
        SWITCH_REQUIRE(!switch_start(&schedule) && !switch_starts);
        switch_beacon_error = 0;
        switch_clock_step = 1;
        switch_a3_scheduler_observed(switch_wlc);
        SWITCH_REQUIRE(!switch_changed && !switch_starts && switch_native_arms == 2);
        switch_clock_step = 0;
        switch_fire(switch_timer_due);
        SWITCH_REQUIRE(switch_starts == 1 && switch_native_pending);
        return 0;
    }
    if (scenario == 15) {
        switch_tbtt = switch_now + 10240;
        SWITCH_REQUIRE(!switch_start(&schedule) && !switch_starts && switch_due->snapshot.armed);
        SWITCH_REQUIRE(switch_due->snapshot.requested_tsf == switch_tbtt + 5000);
        switch_tbtt += 102400;
        switch_fire(switch_due->snapshot.requested_tsf);
        SWITCH_REQUIRE(switch_starts == 1 && switch_native_pending && switch_request.expiry_tsf == switch_tbtt - 10240);
        return 0;
    }
    if (scenario == 16) {
        switch_start_error = -16;
        SWITCH_REQUIRE(!switch_start(&schedule) && switch_starts == 1 && !switch_native_pending);
        SWITCH_REQUIRE(switch_due->snapshot.requested_tsf == 161000);
        return 0;
    }
    SWITCH_REQUIRE(!switch_start(&schedule) && switch_native_pending);
    if (scenario == 17) {
        switch_cancel_deferred = 1;
        SWITCH_REQUIRE(switch_a3_scheduler_stop() == -16 && switch_native_pending);
        switch_complete(SWITCH_A3_EXCURSION_CANCELLED);
        SWITCH_REQUIRE(!switch_native_pending && switch_changed == 1 && !switch_last_error);
        return 0;
    }
    if (scenario == 18) {
        switch_complete(SWITCH_A3_EXCURSION_RETURN_FAILED);
        SWITCH_REQUIRE(switch_changed == 1 && switch_last_error == -1);
        return 0;
    }
    if (scenario == 19) {
        switch_complete(SWITCH_A3_EXCURSION_CLOCK_FAILED);
        SWITCH_REQUIRE(switch_changed == 1 && switch_last_error == -1 &&
                       switch_a3_scheduler_fault_reason() == SWITCH_A3_FAULT_CLOCK_UNSTABLE);
        return 0;
    }
    if (scenario == 20) {
        switch_complete(SWITCH_A3_EXCURSION_ENTRY_FAILED);
        SWITCH_REQUIRE(switch_starts == 1 && switch_due->snapshot.requested_tsf == 161000);
        return 0;
    }
    struct switch_a3_excursion_result result = {
        .session = 2,
        .clock_epoch = 3,
        .generation = scenario == 21 ? 5 : 4,
    };
    switch_phy = 0x1006;
    switch_callback(switch_context, SWITCH_A3_EXCURSION_ENTERED, &result);
    uint64_t until;
    if (scenario == 21) {
        SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0x1006, &until) == -35);
        return 0;
    }
    SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, 0x1006, &until) && until == 89760 && switch_wakes == 1);
    if (scenario == 22) {
        switch_now = until;
        SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0x1006, &until) == -35);
    } else {
        SWITCH_REQUIRE(!switch_a3_scheduler_stop() && !switch_native_pending);
        SWITCH_REQUIRE(switch_a3_scheduler_admit(switch_wlc, 0x1006, &until) == -35);
    }
    return 0;
}

static uint32_t switch_test_phase(void)
{
    static const uint32_t periods[] = {3, 65536, 200000, 0x7fffffff};
    static const uint64_t elapsed[] = {0, 1, 65535, UINT32_MAX, UINT64_C(0x100000000),
                                       UINT64_C(0x3000083e8), UINT64_C(0xfffffffe00000000)};
    for (unsigned p = 0; p < sizeof periods / sizeof periods[0]; ++p)
        for (unsigned e = 0; e < sizeof elapsed / sizeof elapsed[0]; ++e) {
            struct switch_a3_schedule schedule = switch_reset();
            schedule.period_us = periods[p];
            schedule.slots[0].dwell_us = periods[p] / 2;
            schedule.slots[1].dwell_us = periods[p] - schedule.slots[0].dwell_us;
            switch_now += elapsed[e];
            uint32_t phase = 0;
            for (unsigned bit = 64; bit; --bit) {
                phase = (phase << 1) | ((elapsed[e] >> (bit - 1)) & 1u);
                if (phase >= periods[p])
                    phase -= periods[p];
            }
            const uint32_t end = phase < schedule.slots[0].dwell_us ? schedule.slots[0].dwell_us : periods[p];
            SWITCH_REQUIRE(!switch_start(&schedule));
            SWITCH_REQUIRE(switch_due->snapshot.requested_tsf == switch_now + end - phase);
        }
    return 0;
}

static uint32_t switch_test_adjacent_windows(void)
{
    for (unsigned which = 0; which < 3; ++which) {
        struct switch_a3_schedule schedule = switch_reset();
        schedule.slots[0].chanspec = 0x1006;
        schedule.slots[1].chanspec = which == 1 ? switch_home : 0xd02c;
        if (which == 2) {
            *((uint8_t *)switch_bsscfg + 8) = 1;
            switch_tbtt = 30000;
        }
        SWITCH_REQUIRE(!switch_start(&schedule) && switch_starts == 1);
        const struct switch_a3_excursion_result entered = {
            .session = switch_request.session,
            .clock_epoch = switch_request.clock_epoch,
            .generation = switch_request.generation,
        };
        switch_phy = switch_request.target_chanspec;
        switch_callback(switch_context, SWITCH_A3_EXCURSION_ENTERED, &entered);
        switch_now = switch_request.expiry_tsf + 7;
        switch_complete(SWITCH_A3_EXCURSION_EXPIRED);
        SWITCH_REQUIRE(switch_due && switch_due->snapshot.armed);
        if (which == 2) {
            SWITCH_REQUIRE(switch_now < 30000 && switch_timer_due == 33768 && switch_starts == 1);
        } else {
            SWITCH_REQUIRE(switch_timer_due == switch_now + 1);
            switch_fire(switch_timer_due);
            uint64_t until;
            if (which == 1)
                SWITCH_REQUIRE(!switch_a3_scheduler_admit(switch_wlc, switch_home, &until) && until == 66536);
            else
                SWITCH_REQUIRE(switch_starts == 2 && switch_request.target_chanspec == 0xd02c);
        }
    }
    return 0;
}

static uint32_t switch_test_pm_retry(void)
{
    struct switch_a3_schedule schedule = switch_reset();
    schedule.slots[0].chanspec = 0x1006;
    schedule.period_us = 200000;
    schedule.slots[0].dwell_us = 160000;
    schedule.slots[1].dwell_us = 40000;
    *((uint8_t *)switch_bsscfg + 8) = 1;
    SWITCH_REQUIRE(!switch_start(&schedule) && switch_starts == 1 && switch_native_pending);
    switch_now = 4000;
    switch_complete_channel(SWITCH_A3_EXCURSION_ENTRY_FAILED, SWITCH_A3_CHANNEL_NO_PM_ACK);
    SWITCH_REQUIRE(switch_due && switch_due->snapshot.armed && switch_due->snapshot.requested_tsf == 9000);
    switch_fire(switch_timer_due);
    SWITCH_REQUIRE(switch_starts == 2 && switch_native_pending);
    switch_now = 154000;
    switch_complete_channel(SWITCH_A3_EXCURSION_ENTRY_FAILED, SWITCH_A3_CHANNEL_NO_PM_ACK);
    SWITCH_REQUIRE(switch_due && switch_due->snapshot.armed && switch_due->snapshot.requested_tsf == 161000);

    schedule = switch_reset();
    schedule.slots[0].chanspec = 0x1006;
    schedule.period_us = 200000;
    schedule.slots[0].dwell_us = 160000;
    schedule.slots[1].dwell_us = 40000;
    *((uint8_t *)switch_bsscfg + 8) = 1;
    SWITCH_REQUIRE(!switch_start(&schedule) && switch_starts == 1);
    switch_now = 4000;
    switch_complete_channel(SWITCH_A3_EXCURSION_ENTRY_FAILED, SWITCH_A3_CHANNEL_RESTRICTED);
    SWITCH_REQUIRE(switch_due && switch_due->snapshot.armed && switch_due->snapshot.requested_tsf == 161000);
    return 0;
}

static uint32_t switch_test_beacon_reentry(void)
{
    struct switch_a3_schedule schedule = switch_reset();
    schedule.slots[0].chanspec = 0x1006;
    schedule.period_us = 200000;
    schedule.slots[0].dwell_us = 160000;
    schedule.slots[1].dwell_us = 40000;
    *((uint8_t *)switch_bsscfg + 8) = 1;
    switch_tbtt = 30000;
    SWITCH_REQUIRE(!switch_start(&schedule) && switch_starts == 1 && switch_native_pending);
    SWITCH_REQUIRE(switch_request.expiry_tsf == 19760);
    switch_now = 19767;
    switch_complete(SWITCH_A3_EXCURSION_EXPIRED);
    SWITCH_REQUIRE(switch_due && switch_due->snapshot.armed && switch_due->snapshot.requested_tsf == 35000);
    switch_tbtt = 132400;
    switch_fire(switch_timer_due);
    SWITCH_REQUIRE(switch_starts == 2 && switch_native_pending && switch_request.expiry_tsf == 122160);
    return 0;
}

void switch_control_test_run(void)
{
    for (unsigned index = 0; index < 29; ++index)
        switch_control_test_results[index] = switch_test(index);
    for (unsigned index = 30; index < 35; ++index)
        switch_control_test_results[index - 1] = switch_test(index);
    switch_control_test_results[34] = switch_test_phase();
    switch_control_test_results[35] = switch_test_adjacent_windows();
    switch_control_test_results[36] = switch_test_pm_retry();
    switch_control_test_results[37] = switch_test_beacon_reentry();
    switch_control_test_results[38] = switch_test(29);
    switch_control_test_done = 0x53574133;
}
