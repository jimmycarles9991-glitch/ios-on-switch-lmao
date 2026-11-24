/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "excursion.h"
#include "channel.h"
#include "clock.h"
#include "timer.h"
#include "diagnostic.h"
#include "control.h"
#include <stddef.h>

extern int switch_a3_excursion_scan_request(void *, int, const uint8_t *, int, void *, int, int, int, int, int,
                                            const uint16_t *, int, int, int, void (*)(void *, uint32_t, void *), void *,
                                            int, int, void *, void (*)(void *, void *, uint32_t *), void *);

static struct {
    uint8_t active, submitting, completed, entered;
    switch_a3_excursion_callback callback;
    void *context;
    uint32_t token;
    uint64_t station_return_tsf;
    struct switch_a3_excursion_result result;
    struct switch_a3_timer timer;
} switch_a3_excursion;

static uint32_t switch_a3_excursion_serial;

static uint16_t switch_a3_excursion_channel(void *wlc)
{
    return *(uint16_t *)((uint8_t *)wlc + 0x468);
}

static int switch_a3_excursion_matches(void *token)
{
    return switch_a3_excursion.active && (uintptr_t)token == switch_a3_excursion.token;
}

static void switch_a3_excursion_notify(uint32_t event)
{
    struct switch_a3_excursion_result result = switch_a3_excursion.result;
    switch_a3_excursion.callback(switch_a3_excursion.context, event, &result);
}

static void switch_a3_excursion_finish(void)
{
    if (switch_a3_excursion.submitting || !switch_a3_excursion.completed)
        return;
    switch_a3_excursion.active = 0;
    switch_a3_excursion_notify(SWITCH_A3_EXCURSION_COMPLETED);
}

static void switch_a3_excursion_completed(void *token, uint32_t status, void *bsscfg)
{
    if (!switch_a3_excursion_matches(token) || switch_a3_excursion.completed)
        return;
    const struct switch_a3_channel_binding *request = switch_a3_channel_current();
    struct switch_a3_excursion_result *result = &switch_a3_excursion.result;
    const int timer_status = switch_a3_timer_cancel_owned(&switch_a3_excursion.timer, request->wlc);
    uint16_t actual = 0;
    result->native_status = (int32_t)status;
    result->channel_result = switch_a3_channel_complete();
    if (timer_status || bsscfg != request->bsscfg || result->reason == SWITCH_A3_EXCURSION_OWNERSHIP_LOST ||
        result->channel_result == SWITCH_A3_CHANNEL_OWNERSHIP_LOST)
        result->reason = SWITCH_A3_EXCURSION_OWNERSHIP_LOST;
    else if (result->channel_result == SWITCH_A3_CHANNEL_CLOCK_FAILED)
        result->reason = SWITCH_A3_EXCURSION_CLOCK_FAILED;
    else if (result->channel_result == SWITCH_A3_CHANNEL_STATION_CHANGED)
        result->reason = SWITCH_A3_EXCURSION_RETURN_FAILED;
    else if (switch_a3_clock_read(request->wlc, &result->returned_tsf))
        result->reason = SWITCH_A3_EXCURSION_CLOCK_FAILED;
    else if ((actual = switch_a3_excursion_channel(request->wlc)) != request->home_chanspec ||
             (switch_a3_excursion.station_return_tsf && result->returned_tsf > switch_a3_excursion.station_return_tsf))
        result->reason = SWITCH_A3_EXCURSION_RETURN_FAILED;
    else if (!result->reason && (status || !switch_a3_excursion.entered))
        result->reason = status ? SWITCH_A3_EXCURSION_NATIVE_FAILED : SWITCH_A3_EXCURSION_ENTRY_FAILED;
    SWITCH_A3_DIAG_COUNT(EXCURSION_RETURN);
    if (result->reason == SWITCH_A3_EXCURSION_RETURN_FAILED ||
        result->reason == SWITCH_A3_EXCURSION_OWNERSHIP_LOST)
        switch_a3_control_record(result->reason == SWITCH_A3_EXCURSION_RETURN_FAILED ? SWITCH_A3_FAULT_CHANNEL_RETURN
                                                                                   : SWITCH_A3_FAULT_EXCURSION_OWNER,
                                   result->native_status, &result->owner, &switch_a3_excursion.timer, &result->returned_tsf,
                                   request->home_chanspec | ((uint32_t)actual << 16));
    switch_a3_excursion.completed = 1;
    switch_a3_excursion_finish();
}

#if DEVELOPMENT || DEBUG
void switch_a3_excursion_rx_ready(void *wlc)
{
    switch_a3_clock_read(wlc, &switch_a3_excursion.result.rx_ready_tsf);
}

static void switch_a3_excursion_rx_end(const struct switch_a3_channel_binding *request)
{
    const struct switch_a3_excursion_result *result = &switch_a3_excursion.result;
    uint64_t now;
    if (result->rx_ready_tsf && switch_a3_channel_owned() &&
        switch_a3_excursion_channel(request->wlc) == request->target_chanspec &&
        !switch_a3_clock_read(request->wlc, &now) && now > result->rx_ready_tsf) {
        switch_a3_diagnostic.dwell_entered = result->rx_ready_tsf;
        switch_a3_diagnostic.dwell_returned = now;
        switch_a3_diagnostic.dwell_owner = result->owner;
        switch_a3_diagnostic.dwell_channel = request->target_chanspec;
    }
}
#endif

static int switch_a3_excursion_stop(uint32_t reason)
{
    const struct switch_a3_channel_binding *request = switch_a3_channel_current();
    SWITCH_A3_DIAG(if (!switch_a3_excursion.result.reason) switch_a3_excursion_rx_end(request));
    switch_a3_excursion.result.reason = reason;
    switch_a3_timer_cancel_owned(&switch_a3_excursion.timer, request->wlc);
    int status = switch_a3_channel_abort();
    if (status && switch_a3_excursion.active) {
        switch_a3_excursion.result.reason = SWITCH_A3_EXCURSION_OWNERSHIP_LOST;
        switch_a3_excursion_completed((void *)(uintptr_t)switch_a3_excursion.token, 4,
                                      switch_a3_channel_current()->bsscfg);
    }
    return status;
}

static void switch_a3_excursion_expired(void *token, uint64_t requested, uint64_t actual, uint32_t session,
                                        uint32_t epoch)
{
    (void)actual;
    const struct switch_a3_channel_binding *request = switch_a3_channel_current();
    if (!switch_a3_excursion_matches(token) || switch_a3_excursion.completed || requested != request->expiry_tsf ||
        session != request->session || epoch != request->clock_epoch)
        return;
    const int status = switch_a3_excursion.timer.snapshot.status;
    uint32_t reason = SWITCH_A3_EXCURSION_EXPIRED;
    if (status == -1)
        reason = SWITCH_A3_EXCURSION_OWNERSHIP_LOST;
    else if (switch_a3_excursion.timer.fault == SWITCH_A3_FAULT_CLOCK_BACKWARD)
        reason = SWITCH_A3_EXCURSION_CLOCK_BACKWARD;
    else if (switch_a3_excursion.timer.fault == SWITCH_A3_FAULT_TIMER_EARLY ||
             switch_a3_excursion.timer.fault == SWITCH_A3_FAULT_TIMER_ARM)
        reason = SWITCH_A3_EXCURSION_TIMER_FAILED;
    else if (status)
        reason = SWITCH_A3_EXCURSION_CLOCK_FAILED;
    switch_a3_excursion_stop(reason);
}

static void switch_a3_excursion_entered(void *wlc, void *token, uint32_t *dwell)
{
    if (!switch_a3_excursion_matches(token) || switch_a3_excursion.completed || !dwell)
        return;
    const struct switch_a3_channel_binding *request = switch_a3_channel_current();
    uint64_t now;
    uint16_t actual;
    if (wlc != request->wlc || switch_a3_excursion.entered || !switch_a3_channel_owned() ||
        switch_a3_clock_read(wlc, &now) || now >= request->expiry_tsf ||
        (actual = switch_a3_excursion_channel(wlc)) != request->target_chanspec) {
        *dwell = 0;
        switch_a3_excursion_stop(SWITCH_A3_EXCURSION_ENTRY_FAILED);
        return;
    }
    *dwell = ((uint32_t)(request->expiry_tsf - now) + 999u) / 1000u;
    switch_a3_excursion.entered = 1;
    SWITCH_A3_DIAG_COUNT(EXCURSION_ENTER);
    switch_a3_excursion_notify(SWITCH_A3_EXCURSION_ENTERED);
}

int switch_a3_excursion_start(const struct switch_a3_excursion_request *request, switch_a3_excursion_callback callback,
                              void *context)
{
    if (!request || !request->wlc || !request->bsscfg || !callback || !request->session || !request->clock_epoch ||
        !request->generation || !request->home_chanspec || !request->target_chanspec ||
        !*((uint8_t *)request->wlc + 25) || switch_a3_excursion_channel(request->wlc) != request->home_chanspec)
        return -2;
    if (switch_a3_excursion.active)
        return -16;
    if (switch_a3_excursion_serial == UINT32_MAX)
        return -27;
    uint64_t now;
    int status = switch_a3_clock_read(request->wlc, &now);
    if (status)
        return status;
    if (request->expiry_tsf <= now || request->expiry_tsf - now > 65535u * 1000u)
        return -29;
    if (*((uint8_t *)request->bsscfg + 8) && request->target_chanspec != request->home_chanspec &&
        (!request->station_bound_verified || !request->station_return_tsf ||
         request->expiry_tsf > request->station_return_tsf))
        return -23;
    if (switch_a3_excursion.timer.snapshot.armed)
        return -16;
    status = switch_a3_timer_initialize_owned(&switch_a3_excursion.timer, request->wlc);
    if (status)
        return status;
    void *token = (void *)(uintptr_t)++switch_a3_excursion_serial;
    struct switch_a3_channel_binding binding = {
        .wlc = request->wlc,
        .bsscfg = request->bsscfg,
        .completion = (uintptr_t)switch_a3_excursion_completed,
        .entered = (uintptr_t)switch_a3_excursion_entered,
        .completion_context = token,
        .entered_context = token,
        .expiry_tsf = request->expiry_tsf,
        .session = request->session,
        .clock_epoch = request->clock_epoch,
        .generation = request->generation,
        .home_chanspec = request->home_chanspec,
        .target_chanspec = request->target_chanspec,
    };
    status = switch_a3_channel_claim(&binding);
    if (status)
        return status;
    switch_a3_excursion.station_return_tsf = request->station_return_tsf;
    const struct switch_a3_channel_binding *bound = switch_a3_channel_current();
    for (size_t index = 0; index < sizeof switch_a3_excursion.result; ++index)
        ((uint8_t *)&switch_a3_excursion.result)[index] = 0;
    switch_a3_excursion.result.session = bound->session;
    switch_a3_excursion.result.clock_epoch = bound->clock_epoch;
    switch_a3_excursion.result.generation = bound->generation;
    switch_a3_excursion.callback = callback;
    switch_a3_excursion.context = context;
    switch_a3_excursion.token = (uintptr_t)token;
    switch_a3_excursion.active = switch_a3_excursion.submitting = 1;
    switch_a3_excursion.completed = switch_a3_excursion.entered = 0;
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    uint32_t ssid[9];
    for (size_t index = 0; index < 9; ++index)
        ssid[index] = 0;
    status = switch_a3_timer_arm_owned(&switch_a3_excursion.timer, bound->wlc, bound->expiry_tsf, bound->session,
                                       bound->clock_epoch, switch_a3_excursion_expired, token);
    if (!status) {
        status = switch_a3_clock_read(bound->wlc, &now);
        if (!status && now >= bound->expiry_tsf)
            status = SWITCH_A3_TIMER_ELAPSED;
        if (status) {
            const int cancelled = switch_a3_timer_cancel_owned(&switch_a3_excursion.timer, bound->wlc);
            if (cancelled)
                status = cancelled;
        }
    }
    if (status) {
        switch_a3_channel_complete();
        switch_a3_excursion.active = switch_a3_excursion.submitting = 0;
        return status == SWITCH_A3_TIMER_ELAPSED ? -29 : status;
    }
    const uint32_t dwell = ((uint32_t)(bound->expiry_tsf - now) + 999u) / 1000u;
    SWITCH_A3_DIAG_COUNT(EXCURSION_START);
    status = switch_a3_excursion_scan_request(bound->wlc, 2, broadcast, 1, ssid, 1, 0, dwell, dwell, -1,
                                              &bound->target_chanspec, 1, 0, 0, switch_a3_excursion_completed, token,
                                              11, 0, bound->bsscfg, switch_a3_excursion_entered, token);
    if (status && !switch_a3_excursion.completed)
        switch_a3_excursion_completed(token, (uint32_t)status, bound->bsscfg);
    const int retired = switch_a3_excursion.completed;
    switch_a3_excursion.submitting = 0;
    switch_a3_excursion_finish();
    return status && retired ? -16 : status;
}

int switch_a3_excursion_cancel(void *wlc, uint32_t session, uint32_t epoch, uint32_t generation)
{
    if (!switch_a3_excursion.active)
        return 0;
    const struct switch_a3_channel_binding *request = switch_a3_channel_current();
    if (wlc != request->wlc || session != request->session || epoch != request->clock_epoch ||
        generation != request->generation)
        return -25;
    return switch_a3_excursion_stop(SWITCH_A3_EXCURSION_CANCELLED);
}

uint32_t switch_a3_excursion_pending(void)
{
    return switch_a3_excursion.active;
}
