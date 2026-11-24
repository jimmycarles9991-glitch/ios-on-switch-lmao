/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "scheduler.h"
#include "beacon.h"
#include "channel.h"
#include "clock.h"
#include "excursion.h"
#include "timer.h"
#include "tx.h"
#include "ack.h"
#include "diagnostic.h"
#include "control.h"

extern uint16_t switch_a3_tx_phy_chanspec(void *pi);

static struct {
    uint8_t installed, running, allowed, stepping, parked, clock_valid, quiescing;
    uint16_t home, target, reason;
    void *wlc, *context;
    void (*changed)(void *, int);
    int fault;

    union {
        struct switch_a3_owner owner;

        struct {
            uint32_t session, epoch, generation;
        };
    };

    uint64_t until, window_end, observed_tsf;
    struct switch_a3_timer timer;
    struct switch_a3_schedule schedule;
} switch_a3_scheduler;

static uint16_t switch_a3_scheduler_channel(void *wlc)
{
    if (!wlc || !*((const uint8_t *)wlc + 25))
        return 0;
    void *band = *(void **)((uint8_t *)wlc + 32);
    void *pi = band ? *(void **)((uint8_t *)band + 16) : 0;
    return pi ? switch_a3_tx_phy_chanspec(pi) : 0;
}

static void switch_a3_scheduler_step(void);

static void switch_a3_scheduler_fault(int error, uint16_t reason)
{
    SWITCH_A3_DIAG_COUNT(SCHED_FAULT);
    switch_a3_scheduler.running = switch_a3_scheduler.allowed = 0;
    switch_a3_scheduler.fault = error;
    switch_a3_scheduler.reason = reason;
    switch_a3_control_record(reason, error, &switch_a3_scheduler.owner,
                              switch_a3_scheduler.timer.fault == reason ? &switch_a3_scheduler.timer : 0,
                              switch_a3_scheduler.timer.fault == reason ? &switch_a3_scheduler.timer.snapshot.actual_tsf : 0,
                              switch_a3_scheduler.target |
                                  ((uint32_t)switch_a3_scheduler_channel(switch_a3_scheduler.wlc) << 16));
    switch_a3_timer_cancel_owned(&switch_a3_scheduler.timer, switch_a3_scheduler.wlc);
    if (switch_a3_scheduler.changed)
        switch_a3_scheduler.changed(switch_a3_scheduler.context, error);
}

static void switch_a3_scheduler_due(void *context, uint64_t requested, uint64_t actual, uint32_t session,
                                    uint32_t epoch)
{
    (void)context;
    (void)requested;
    (void)actual;
    if (switch_a3_scheduler.running && session == switch_a3_scheduler.session && epoch == switch_a3_scheduler.epoch) {
        if (switch_a3_scheduler.timer.snapshot.status)
            switch_a3_scheduler_fault(switch_a3_scheduler.timer.snapshot.status, switch_a3_scheduler.timer.fault);
        else
            switch_a3_scheduler_step();
    }
}

static void switch_a3_scheduler_defer(void)
{
    const int status =
        switch_a3_timer_defer_owned(&switch_a3_scheduler.timer, switch_a3_scheduler.wlc, switch_a3_scheduler.session,
                                    switch_a3_scheduler.epoch, switch_a3_scheduler_due, 0);
    if (status)
        switch_a3_scheduler_fault(status, switch_a3_scheduler.timer.fault);
}

static void switch_a3_scheduler_arm(uint64_t deadline)
{
    const int result =
        switch_a3_timer_arm_owned(&switch_a3_scheduler.timer, switch_a3_scheduler.wlc, deadline,
                                  switch_a3_scheduler.session, switch_a3_scheduler.epoch, switch_a3_scheduler_due, 0);
    if (result == SWITCH_A3_TIMER_ELAPSED) {
        switch_a3_scheduler.allowed = 0;
        switch_a3_scheduler_defer();
    } else if (result) {
        switch_a3_scheduler_fault(result, switch_a3_scheduler.timer.fault);
    }
}

static __attribute__((noinline)) uint32_t switch_a3_scheduler_phase(uint64_t elapsed, uint32_t period)
{
    uint32_t remainder = 0;
    for (unsigned bit = 0; bit < 64; ++bit) {
        remainder = (remainder << 1) | (uint32_t)(elapsed >> 63);
        elapsed <<= 1;
        if (remainder >= period)
            remainder -= period;
    }
    return remainder;
}

static uint16_t switch_a3_scheduler_window(uint64_t now, uint64_t *boundary, int *allowed)
{
    const struct switch_a3_schedule *schedule = &switch_a3_scheduler.schedule;
    if (now < schedule->anchor_tsf) {
        *boundary = schedule->anchor_tsf;
        *allowed = 0;
        return switch_a3_scheduler.home;
    }
    const uint32_t phase = switch_a3_scheduler_phase(now - schedule->anchor_tsf, schedule->period_us);
    uint32_t end = 0, slot = 0;
    for (; slot < schedule->slot_count; ++slot) {
        end += schedule->slots[slot].dwell_us;
        if (phase < end)
            break;
    }
    const uint16_t requested = schedule->slots[slot].chanspec;
    *allowed = requested != 0;
    uint32_t remaining = end - phase;
    for (unsigned count = 1; count < schedule->slot_count; ++count) {
        slot = (slot + 1u) % schedule->slot_count;
        if (schedule->slots[slot].chanspec != requested)
            break;
        remaining += schedule->slots[slot].dwell_us;
    }
    *boundary = now + remaining;
    return !requested || requested == switch_a3_channel_control(switch_a3_scheduler.home) ? switch_a3_scheduler.home
                                                                                          : requested;
}

static void switch_a3_scheduler_excursion(void *context, uint32_t event,
                                          const struct switch_a3_excursion_result *result)
{
    (void)context;
    if (result->session != switch_a3_scheduler.session || result->clock_epoch != switch_a3_scheduler.epoch ||
        result->generation != switch_a3_scheduler.generation)
        return;
    if (event == SWITCH_A3_EXCURSION_ENTERED) {
        const int status = switch_a3_ack_reconcile(switch_a3_scheduler.wlc);
        if (status) {
            switch_a3_scheduler_fault(status, SWITCH_A3_FAULT_ACK);
            return;
        }
        switch_a3_scheduler.allowed = switch_a3_scheduler.running;
        if (switch_a3_scheduler.allowed) {
            SWITCH_A3_DIAG(switch_a3_excursion_rx_ready(switch_a3_scheduler.wlc));
            switch_a3_tx_wake(switch_a3_scheduler.wlc);
        }
        return;
    }
    switch_a3_scheduler.allowed = 0;
    switch_a3_scheduler.quiescing = 0;
    if (result->reason == SWITCH_A3_EXCURSION_RETURN_FAILED || result->reason == SWITCH_A3_EXCURSION_OWNERSHIP_LOST ||
        result->reason == SWITCH_A3_EXCURSION_CLOCK_FAILED || result->reason == SWITCH_A3_EXCURSION_CLOCK_BACKWARD ||
        result->reason == SWITCH_A3_EXCURSION_TIMER_FAILED) {
        const uint16_t reason = result->reason == SWITCH_A3_EXCURSION_CLOCK_BACKWARD ? SWITCH_A3_FAULT_CLOCK_BACKWARD
                                : result->reason == SWITCH_A3_EXCURSION_CLOCK_FAILED ? SWITCH_A3_FAULT_CLOCK_UNSTABLE
                                : result->reason == SWITCH_A3_EXCURSION_TIMER_FAILED ? SWITCH_A3_FAULT_TIMER_EARLY
                                : result->reason == SWITCH_A3_EXCURSION_OWNERSHIP_LOST ? SWITCH_A3_FAULT_EXCURSION_OWNER
                                                                                    : SWITCH_A3_FAULT_CHANNEL_RETURN;
        switch_a3_scheduler_fault(reason == SWITCH_A3_FAULT_CLOCK_BACKWARD ? -35 : -1, reason);
    } else if (switch_a3_scheduler.running && !switch_a3_scheduler.stepping) {
        if (result->reason == SWITCH_A3_EXCURSION_OK) {
            switch_a3_scheduler_step();
        } else {
            uint64_t next = switch_a3_scheduler.window_end;
            if (result->reason == SWITCH_A3_EXCURSION_EXPIRED && switch_a3_scheduler.until < next &&
                next - switch_a3_scheduler.until > 10240u + 5000u + 1000u)
                next = switch_a3_scheduler.until + 10240u + 5000u;
            else if (result->reason == SWITCH_A3_EXCURSION_ENTRY_FAILED &&
                     result->channel_result == SWITCH_A3_CHANNEL_NO_PM_ACK && result->returned_tsf &&
                     next > result->returned_tsf && next - result->returned_tsf > 7000u)
                next = result->returned_tsf + 5000u;
            switch_a3_scheduler_arm(next);
        }
    } else if (!switch_a3_scheduler.running && switch_a3_scheduler.changed) {
        switch_a3_scheduler.changed(switch_a3_scheduler.context, 0);
    }
}

static void switch_a3_scheduler_step(void)
{
    if (!switch_a3_scheduler.running || switch_a3_scheduler.stepping || switch_a3_scheduler.parked)
        return;
    uint64_t now, boundary;
    if (switch_a3_clock_read(switch_a3_scheduler.wlc, &now)) {
        switch_a3_scheduler_fault(-1, SWITCH_A3_FAULT_CLOCK_UNSTABLE);
        return;
    }
    if (switch_a3_scheduler.clock_valid && now < switch_a3_scheduler.observed_tsf) {
        switch_a3_scheduler.timer.fault = SWITCH_A3_FAULT_CLOCK_BACKWARD;
        switch_a3_scheduler.timer.snapshot.actual_tsf = now;
        switch_a3_scheduler_fault(-35, SWITCH_A3_FAULT_CLOCK_BACKWARD);
        return;
    }
    switch_a3_scheduler.observed_tsf = now;
    switch_a3_scheduler.clock_valid = 1;
    int allowed;
    const uint16_t target = switch_a3_scheduler_window(now, &boundary, &allowed);
    switch_a3_scheduler.allowed = 0;
    if (boundary <= now) {
        switch_a3_scheduler_fault(-29, SWITCH_A3_FAULT_TIMER_ARM);
        return;
    }
    if (switch_a3_excursion_pending())
        return;
    if (switch_a3_scheduler_channel(switch_a3_scheduler.wlc) != switch_a3_scheduler.home) {
        switch_a3_scheduler.allowed = 0;
        switch_a3_scheduler.parked = 2;
        switch_a3_scheduler.reason = SWITCH_A3_FAULT_HOME_CHANGED;
        switch_a3_timer_cancel_owned(&switch_a3_scheduler.timer, switch_a3_scheduler.wlc);
        switch_a3_control_record(SWITCH_A3_FAULT_HOME_CHANGED, -25, &switch_a3_scheduler.owner, 0, &now,
                                    switch_a3_scheduler.home |
                                        ((uint32_t)switch_a3_scheduler_channel(switch_a3_scheduler.wlc) << 16));
        if (switch_a3_scheduler.changed)
            switch_a3_scheduler.changed(switch_a3_scheduler.context, 0);
        return;
    }
    if (allowed && !switch_a3_channel_usable(switch_a3_scheduler.wlc, switch_a3_channel_control(target))) {
        SWITCH_A3_DIAG_COUNT(SCHED_BLOCK);
        switch_a3_scheduler_arm(boundary);
        return;
    }
    switch_a3_scheduler.target = target;
    switch_a3_scheduler.window_end = boundary;
    switch_a3_scheduler.until = boundary;
    if (target == switch_a3_scheduler.home) {
        const int status = switch_a3_ack_reconcile(switch_a3_scheduler.wlc);
        if (status) {
            switch_a3_scheduler_fault(status, SWITCH_A3_FAULT_ACK);
            return;
        }
        switch_a3_scheduler.allowed = allowed;
        switch_a3_scheduler_arm(boundary);
        if (switch_a3_scheduler.running && allowed)
            switch_a3_tx_wake(switch_a3_scheduler.wlc);
        return;
    }
    void *bsscfg = switch_a3_station_bsscfg(switch_a3_scheduler.wlc);
    uint64_t station_return = 0;
    if (!bsscfg) {
        switch_a3_scheduler_fault(-25, SWITCH_A3_FAULT_HOME_CHANGED);
        return;
    }
    if (*((const uint8_t *)bsscfg + 8)) {
        if (switch_a3_beacon_bound(switch_a3_scheduler.wlc, switch_a3_scheduler.home, now, &bsscfg, &station_return)) {
            SWITCH_A3_DIAG_COUNT(SCHED_BLOCK);
            switch_a3_scheduler_arm(boundary);
            return;
        }
        /* Keep the native ten-TU TBTT guard without skipping an imminent beacon; 5 ms covers 1 Mbps 2.4 GHz beacons. */
        if (station_return <= now || station_return - now <= 10240u + 1000u) {
            SWITCH_A3_DIAG_COUNT(SCHED_BLOCK);
            const uint64_t retry = station_return + 5000u;
            switch_a3_scheduler_arm(station_return > now && retry + 1000u < boundary ? retry : boundary);
            return;
        }
        const uint64_t guarded = station_return - 10240u;
        if (guarded < switch_a3_scheduler.until)
            switch_a3_scheduler.until = guarded;
    }
    if (switch_a3_scheduler.until <= now || switch_a3_scheduler.until - now < 1000u) {
        SWITCH_A3_DIAG_COUNT(SCHED_BLOCK);
        switch_a3_scheduler_arm(boundary);
        return;
    }
    const struct switch_a3_excursion_request request = {
        .wlc = switch_a3_scheduler.wlc,
        .bsscfg = bsscfg,
        .expiry_tsf = switch_a3_scheduler.until,
        .station_return_tsf = station_return,
        .session = switch_a3_scheduler.session,
        .clock_epoch = switch_a3_scheduler.epoch,
        .generation = switch_a3_scheduler.generation,
        .home_chanspec = switch_a3_scheduler.home,
        .target_chanspec = target,
        .station_bound_verified = station_return != 0,
    };
    switch_a3_scheduler.stepping = 1;
    const int status = switch_a3_excursion_start(&request, switch_a3_scheduler_excursion, 0);
    switch_a3_scheduler.stepping = 0;
    if (!switch_a3_excursion_pending()) {
        switch_a3_scheduler.allowed = 0;
        if (!switch_a3_scheduler.running)
            return;
        if (!status || status == -16 || status == -29)
            switch_a3_scheduler_arm(boundary);
        else
            switch_a3_scheduler_fault(status, SWITCH_A3_FAULT_EXCURSION_OWNER);
    } else if (status) {
        switch_a3_scheduler_fault(status, SWITCH_A3_FAULT_EXCURSION_OWNER);
    }
}

int switch_a3_scheduler_validate(void *wlc, const struct switch_a3_schedule *schedule, int allow_empty)
{
    if (!schedule || !schedule->slot_count || schedule->slot_count > SWITCH_A3_MAX_SLOTS || !schedule->period_us ||
        schedule->period_us > 0x7fffffffu || schedule->anchor_tsf > UINT64_MAX - schedule->period_us)
        return -2;
    const uint16_t current = switch_a3_scheduler.installed && switch_a3_scheduler.wlc == wlc &&
                                     switch_a3_scheduler.parked != 1
                                 ? switch_a3_scheduler.home : switch_a3_channel_home(wlc);
    if (!current || !switch_a3_station_bsscfg(wlc))
        return -25;
    const uint16_t home_control = switch_a3_channel_control(current);
    if (schedule->infra_chanspec && schedule->infra_chanspec != home_control)
        return SWITCH_A3_CHANNEL_CONTEXT_CHANGED;
    uint64_t total = 0;
    int active = 0;
    for (uint32_t index = 0; index < SWITCH_A3_MAX_SLOTS; ++index) {
        const struct switch_a3_slot *slot = &schedule->slots[index];
        if (slot->reserved || (index >= schedule->slot_count && (slot->chanspec || slot->dwell_us)))
            return -2;
        if (index >= schedule->slot_count)
            continue;
        if (!slot->dwell_us)
            return -2;
        if (slot->chanspec && !switch_a3_channel_usable(wlc, slot->chanspec))
            return -23;
        active |= slot->chanspec != 0;
        total += slot->dwell_us;
    }
    if (total != schedule->period_us)
        return -2;
    if (!active && !allow_empty)
        return -23;
    return 0;
}

int switch_a3_scheduler_home_ready(void *wlc)
{
    const uint16_t native = switch_a3_channel_home(wlc);
    const uint16_t home = switch_a3_scheduler.installed && switch_a3_scheduler.wlc == wlc &&
                                  switch_a3_scheduler.parked != 1 ? switch_a3_scheduler.home : native;
    return !switch_a3_excursion_pending() && home && native == home && switch_a3_scheduler_channel(wlc) == home;
}

int switch_a3_scheduler_quiesce(void)
{
    switch_a3_scheduler.running = switch_a3_scheduler.allowed = 0;
    switch_a3_scheduler.quiescing = 1;
    const int status = switch_a3_timer_cancel_owned(&switch_a3_scheduler.timer, switch_a3_scheduler.wlc);
    if (switch_a3_excursion_pending()) {
        const int result = switch_a3_excursion_cancel(switch_a3_scheduler.wlc, switch_a3_scheduler.session,
                                                      switch_a3_scheduler.epoch, switch_a3_scheduler.generation);
        if (result)
            return result;
    }
    return switch_a3_excursion_pending() ? -16 : status;
}

uint16_t switch_a3_scheduler_fault_reason(void)
{
    return switch_a3_scheduler.reason;
}

const struct switch_a3_owner *switch_a3_scheduler_owner(void)
{
    return &switch_a3_scheduler.owner;
}

int switch_a3_scheduler_install(void *wlc, const struct switch_a3_schedule *schedule)
{
    const int status = switch_a3_scheduler_validate(wlc, schedule, switch_a3_scheduler.installed);
    if (status)
        return status;
    if (!switch_a3_scheduler_home_ready(wlc))
        return -16;
    const uint8_t *source = (const uint8_t *)schedule;
    for (size_t index = 0; index < sizeof *schedule; ++index)
        ((uint8_t *)&switch_a3_scheduler.schedule)[index] = source[index];
    switch_a3_scheduler.wlc = wlc;
    if (!switch_a3_scheduler.installed || switch_a3_scheduler.parked == 1) {
        switch_a3_scheduler.home = switch_a3_channel_home(wlc);
        switch_a3_beacon_reset(wlc);
    }
    switch_a3_scheduler.installed = 1;
    switch_a3_scheduler.fault = 0;
    switch_a3_scheduler.reason = SWITCH_A3_FAULT_NONE;
    switch_a3_scheduler.parked = switch_a3_scheduler.clock_valid = switch_a3_scheduler.quiescing = 0;
    unsigned active = 0;
    for (unsigned index = 0; index < schedule->slot_count; ++index)
        active |= schedule->slots[index].chanspec;
    switch_a3_scheduler.parked = !active;
    return 0;
}

int switch_a3_scheduler_start(void *wlc, uint16_t *chanspec)
{
    if (!chanspec || !switch_a3_scheduler.installed || switch_a3_scheduler.wlc != wlc)
        return -25;
    if (switch_a3_scheduler_channel(wlc) != switch_a3_scheduler.home)
        return SWITCH_A3_CHANNEL_CONTEXT_CHANGED;
    const int status = switch_a3_timer_initialize_owned(&switch_a3_scheduler.timer, wlc);
    if (status)
        return status;
    *chanspec = switch_a3_scheduler.home;
    return 0;
}

int switch_a3_scheduler_run(void *wlc, uint32_t session, uint32_t epoch, uint32_t generation,
                            void (*changed)(void *, int), void *context)
{
    if (!switch_a3_scheduler.installed || switch_a3_scheduler.wlc != wlc || !session || !epoch || !generation)
        return -2;
    switch_a3_scheduler.session = session;
    switch_a3_scheduler.epoch = epoch;
    switch_a3_scheduler.generation = generation;
    switch_a3_scheduler.changed = changed;
    switch_a3_scheduler.context = context;
    switch_a3_scheduler.running = 1;
    switch_a3_scheduler_step();
    return switch_a3_scheduler.fault;
}

int switch_a3_scheduler_stop(void)
{
    switch_a3_scheduler.installed = 0;
    return switch_a3_scheduler_quiesce();
}

int switch_a3_scheduler_admit(void *wlc, uint16_t chanspec, uint64_t *until)
{
    uint64_t now;
    uint16_t actual;
    if (!until || !switch_a3_scheduler.running || !switch_a3_scheduler.allowed || switch_a3_scheduler.wlc != wlc ||
        switch_a3_channel_control(actual = switch_a3_scheduler_channel(wlc)) != chanspec || switch_a3_clock_read(wlc, &now) ||
        now >= switch_a3_scheduler.until || !switch_a3_channel_usable(wlc, chanspec))
        return -35;
    (void)actual;
    *until = switch_a3_scheduler.until;
    return 0;
}

void switch_a3_scheduler_observed(void *wlc)
{
    if (switch_a3_scheduler.wlc != wlc || switch_a3_excursion_pending())
        return;
    if (!switch_a3_scheduler.running) {
        if (switch_a3_scheduler.installed && switch_a3_scheduler.quiescing && switch_a3_scheduler_home_ready(wlc)) {
            switch_a3_scheduler.quiescing = 0;
            if (switch_a3_scheduler.changed)
                switch_a3_scheduler.changed(switch_a3_scheduler.context, 0);
        }
        return;
    }
    if (switch_a3_scheduler.parked) {
        const int active = switch_a3_scheduler.parked != 1;
        const uint16_t home = switch_a3_channel_home(wlc);
        if (active && home && home != switch_a3_scheduler.home) {
            switch_a3_scheduler_fault(-25, SWITCH_A3_FAULT_HOME_CHANGED);
            return;
        }
        if (!switch_a3_scheduler_home_ready(wlc) || (!active && switch_a3_tx_pending()))
            return;
        if (home != switch_a3_scheduler.home && switch_a3_scheduler.changed) {
            switch_a3_scheduler.home = home;
            switch_a3_scheduler.changed(switch_a3_scheduler.context, 0);
        }
        if (!active)
            return;
        switch_a3_scheduler.home = home;
        switch_a3_scheduler.parked = 0;
        if (switch_a3_scheduler.changed)
            switch_a3_scheduler.changed(switch_a3_scheduler.context, 0);
    }
    switch_a3_scheduler_defer();
}
