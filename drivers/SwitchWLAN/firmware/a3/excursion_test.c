/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "excursion.h"
#include "diagnostic.h"
#include "channel.h"
#include "timer.h"
#include "control.h"

union switch_test_memory {
    uint64_t alignment;
    uint8_t bytes[2304];
};
static union switch_test_memory switch_wlc, switch_bsscfg, switch_scan, switch_private;
static struct switch_a3_excursion_request switch_request;
static struct switch_a3_excursion_result switch_result;
static struct switch_a3_channel_binding switch_binding;
static void (*switch_complete)(void *, uint32_t, void *), (*switch_entered)(void *, void *, uint32_t *);
static void *switch_complete_context, *switch_entered_context;
static switch_a3_timer_callback switch_expiry;
static void *switch_expiry_context;
static uint64_t switch_now, switch_deadline;
static uint32_t switch_session, switch_epoch, switch_dwell, switch_native_dwell;
static uint32_t switch_submits, switch_aborts, switch_claims, switch_events, switch_finishes, switch_fault;
static uint32_t switch_gate_result, switch_timer_armed, switch_scan_depth, switch_reenter;
static int switch_clock_error, switch_initialize_error, switch_arm_error, switch_claim_error;
static int switch_rx_start_delay, switch_rx_sample_failure;
static int switch_submit_mode, switch_deferred, switch_expire_before_submit;
volatile uint32_t switch_excursion_test_results[49];
volatile uint32_t switch_excursion_test_done;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

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

static void switch_zero(void *address, size_t bytes)
{
    uint8_t *out = address;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = 0;
}

__attribute__((used)) void *memset(void *address, int value, size_t bytes)
{
    uint8_t *out = address;
    for (size_t index = 0; index < bytes; ++index)
        out[index] = value;
    return address;
}

static void switch_write_pointer(void *base, unsigned offset, const void *value)
{
    *(const void **)((uint8_t *)base + offset) = value;
}

static void switch_home(void)
{
    *(uint16_t *)(switch_wlc.bytes + 0x468) = switch_request.home_chanspec;
}

static void switch_native_finish(uint32_t status)
{
    void (*complete)(void *, uint32_t, void *) = switch_complete;
    void *context = switch_complete_context;
    switch_scan.bytes[6] = 0;
    switch_write_pointer(switch_private.bytes, 0x1fc, NULL);
    switch_write_pointer(switch_private.bytes, 0x200, NULL);
    switch_complete = NULL;
    if (complete)
        complete(context, status, switch_request.bsscfg);
}

static void switch_observe(void *context, uint32_t event, const struct switch_a3_excursion_result *result)
{
    if (context != switch_private.bytes + 1024)
        switch_fault = 1;
    switch_result = *result;
    if (event == SWITCH_A3_EXCURSION_COMPLETED) {
        if (switch_scan_depth)
            switch_fault = 2;
        ++switch_finishes;
        if (switch_reenter == 1) {
            switch_reenter = 0;
            switch_submit_mode = 0;
            ++switch_request.generation;
            if (switch_a3_excursion_start(&switch_request, switch_observe, context))
                switch_fault = 3;
        }
    } else {
        ++switch_events;
        switch_now += switch_rx_start_delay;
        switch_clock_error = switch_rx_sample_failure;
        switch_a3_excursion_rx_ready(switch_wlc.bytes);
        switch_clock_error = 0;
        if (switch_reenter == 2) {
            switch_reenter = 0;
            switch_home();
            switch_a3_excursion_cancel(switch_request.wlc, 2, 3, switch_request.generation);
        }
    }
}

static void switch_reset(void)
{
    SWITCH_A3_DIAG(
        for (size_t index = 0; index < sizeof switch_a3_diagnostic; ++index)
            ((uint8_t *)&switch_a3_diagnostic)[index] = 0);
    switch_zero(&switch_wlc, sizeof switch_wlc);
    switch_zero(&switch_bsscfg, sizeof switch_bsscfg);
    switch_zero(&switch_scan, sizeof switch_scan);
    switch_zero(&switch_private, sizeof switch_private);
    switch_request = (struct switch_a3_excursion_request){.wlc = switch_wlc.bytes,
                                                          .bsscfg = switch_bsscfg.bytes,
                                                          .expiry_tsf = 10000,
                                                          .session = 2,
                                                          .clock_epoch = 3,
                                                          .generation = 4,
                                                          .home_chanspec = 0xd024,
                                                          .target_chanspec = 0x1006};
    switch_wlc.bytes[25] = 1;
    switch_home();
    switch_write_pointer(switch_wlc.bytes, 0x168, switch_scan.bytes);
    switch_write_pointer(switch_scan.bytes, 0, switch_private.bytes);
    switch_write_pointer(switch_private.bytes, 8, switch_wlc.bytes);
    switch_write_pointer(switch_bsscfg.bytes, 0, switch_wlc.bytes);
    switch_bsscfg.bytes[16] = 1;
    switch_now = 1000;
    switch_clock_error = switch_initialize_error = switch_arm_error = switch_claim_error = 0;
    switch_rx_start_delay = switch_rx_sample_failure = 0;
    switch_submit_mode = switch_deferred = 0;
    switch_expire_before_submit = 0;
    switch_submits = switch_aborts = switch_claims = switch_events = switch_finishes = switch_fault = 0;
    switch_gate_result = switch_timer_armed = switch_scan_depth = switch_reenter = 0;
    switch_complete = NULL;
    switch_entered = NULL;
    switch_expiry = NULL;
    switch_zero(&switch_result, sizeof switch_result);
}

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != switch_wlc.bytes)
        switch_fault = 4;
    if (!switch_clock_error)
        *tsf = switch_now;
    return switch_clock_error;
}

int switch_a3_timer_initialize(void *wlc)
{
    if (wlc != switch_wlc.bytes)
        switch_fault = 5;
    return switch_initialize_error;
}

int switch_a3_timer_initialize_owned(struct switch_a3_timer *timer, void *wlc)
{
    timer->snapshot.armed = switch_timer_armed;
    return switch_a3_timer_initialize(wlc);
}

int switch_a3_timer_arm(void *wlc, uint64_t deadline, uint32_t session, uint32_t epoch,
                        switch_a3_timer_callback callback, void *context)
{
    if (wlc != switch_wlc.bytes || switch_scan_depth)
        switch_fault = 6;
    switch_deadline = deadline;
    switch_session = session;
    switch_epoch = epoch;
    switch_expiry = callback;
    switch_expiry_context = context;
    switch_timer_armed = !switch_arm_error;
    if (switch_expire_before_submit)
        switch_now = deadline;
    return switch_arm_error;
}

int switch_a3_timer_cancel(void *wlc)
{
    if (wlc != switch_wlc.bytes)
        switch_fault = 7;
    switch_timer_armed = 0;
    return 0;
}

int switch_a3_timer_cancel_owned(struct switch_a3_timer *timer, void *wlc)
{
    timer->snapshot.armed = 0;
    return switch_a3_timer_cancel(wlc);
}

int switch_a3_timer_arm_owned(struct switch_a3_timer *timer, void *wlc, uint64_t deadline, uint32_t session,
                              uint32_t epoch, switch_a3_timer_callback callback, void *context)
{
    const int result = switch_a3_timer_arm(wlc, deadline, session, epoch, callback, context);
    timer->snapshot.armed = switch_timer_armed;
    timer->snapshot.status = result;
    return result;
}

void switch_a3_timer_snapshot(struct switch_a3_deadline_snapshot *snapshot)
{
    switch_zero(snapshot, sizeof *snapshot);
    snapshot->armed = switch_timer_armed;
}

int switch_a3_channel_claim(const struct switch_a3_channel_binding *binding)
{
    ++switch_claims;
    switch_binding = *binding;
    if (binding->wlc != switch_request.wlc || binding->bsscfg != switch_request.bsscfg || binding->session != 2 ||
        binding->clock_epoch != 3 || binding->generation != switch_request.generation ||
        binding->expiry_tsf != switch_request.expiry_tsf || binding->home_chanspec != switch_request.home_chanspec ||
        binding->target_chanspec != switch_request.target_chanspec)
        switch_fault = 8;
    return switch_claim_error;
}

uint32_t switch_a3_channel_complete(void)
{
    return switch_gate_result;
}

const struct switch_a3_channel_binding *switch_a3_channel_current(void)
{
    return &switch_binding;
}

int switch_a3_channel_owned(void)
{
    return switch_scan.bytes[6] && *(void **)(switch_wlc.bytes + 0x168) == switch_scan.bytes &&
           *(void **)switch_scan.bytes == switch_private.bytes &&
           *(void **)(switch_private.bytes + 8) == switch_binding.wlc &&
           (uintptr_t)*(void **)(switch_private.bytes + 0x1fc) == switch_binding.completion &&
           *(void **)(switch_private.bytes + 0x200) == switch_binding.completion_context &&
           (uintptr_t)*(void **)(switch_private.bytes + 0x22c) == switch_binding.entered &&
           *(void **)(switch_private.bytes + 0x230) == switch_binding.entered_context;
}

int switch_a3_channel_abort(void)
{
    ++switch_aborts;
    if (*(void **)(switch_private.bytes + 0x200) != switch_binding.completion_context)
        return -1;
    switch_gate_result = SWITCH_A3_CHANNEL_CANCELLED;
    if (!switch_deferred) {
        switch_home();
        switch_native_finish(4);
    }
    return 0;
}

int switch_a3_excursion_scan_request(void *wlc, int type, const uint8_t *bssid, int ssids, void *ssid, int passive,
                                     int probes, int active_ms, int passive_ms, int home_ms, const uint16_t *channels,
                                     int count, int start, int save, void (*complete)(void *, uint32_t, void *),
                                     void *completion_context, int request_type, int flags, void *bsscfg,
                                     void (*entered)(void *, void *, uint32_t *), void *entered_context)
{
    ++switch_scan_depth;
    ++switch_submits;
    if (!switch_timer_armed || wlc != switch_wlc.bytes || type != 2 || ssids != 1 || passive != 1 || probes ||
        active_ms != passive_ms || active_ms < 1 || home_ms != -1 || *channels != switch_request.target_chanspec ||
        count != 1 || start || save || request_type != 11 || flags || bsscfg != switch_bsscfg.bytes ||
        (uintptr_t)complete != switch_binding.completion || (uintptr_t)entered != switch_binding.entered ||
        completion_context != switch_binding.completion_context || entered_context != switch_binding.entered_context)
        switch_fault = 9;
    for (unsigned index = 0; index < 6; ++index)
        if (bssid[index] != 255)
            switch_fault = 10;
    for (unsigned index = 0; index < 36; ++index)
        if (((uint8_t *)ssid)[index])
            switch_fault = 11;
    switch_native_dwell = active_ms;
    switch_complete = complete;
    switch_complete_context = completion_context;
    switch_entered = entered;
    switch_entered_context = entered_context;
    if (switch_submit_mode == 1) {
        switch_complete = NULL;
        --switch_scan_depth;
        return -4;
    }
    if (switch_submit_mode == 2) {
        complete(completion_context, 16, bsscfg);
        --switch_scan_depth;
        return -4;
    }
    switch_scan.bytes[6] = 1;
    switch_write_pointer(switch_private.bytes, 0x1fc, (void *)complete);
    switch_write_pointer(switch_private.bytes, 0x200, completion_context);
    switch_write_pointer(switch_private.bytes, 0x22c, (void *)entered);
    switch_write_pointer(switch_private.bytes, 0x230, entered_context);
    if (switch_submit_mode == 3) {
        switch_now = 2000;
        *(uint16_t *)(switch_wlc.bytes + 0x468) = *channels;
        switch_dwell = active_ms;
        entered(wlc, entered_context, &switch_dwell);
        switch_home();
        switch_now = 9000;
        switch_native_finish(0);
    }
    --switch_scan_depth;
    return 0;
}

static int switch_start(void)
{
    return switch_a3_excursion_start(&switch_request, switch_observe, switch_private.bytes + 1024);
}

static void switch_enter(void)
{
    switch_now = 2000;
    *(uint16_t *)(switch_wlc.bytes + 0x468) = switch_request.target_chanspec;
    switch_dwell = 99;
    switch_entered(switch_request.wlc, switch_entered_context, &switch_dwell);
}

static uint32_t switch_test_lifecycle(unsigned variant)
{
    switch_reset();
    switch_submit_mode = variant < 4 ? variant : 0;
    int status = switch_start();
    if (variant == 1 || variant == 2) {
        SWITCH_REQUIRE(status == -16 && switch_finishes == 1 && !switch_timer_armed &&
                       switch_result.reason == SWITCH_A3_EXCURSION_NATIVE_FAILED);
    } else if (variant == 3) {
        SWITCH_REQUIRE(!status && switch_events == 1 && switch_finishes == 1 && switch_dwell == 8 &&
                       switch_result.reason == SWITCH_A3_EXCURSION_OK);
    } else {
        SWITCH_REQUIRE(!status && switch_native_dwell == 9 && switch_finishes == 0);
        if (variant != 9)
            switch_enter();
        if (variant == 4 || variant == 5) {
            switch_deferred = variant == 5;
            SWITCH_REQUIRE(!switch_a3_excursion_cancel(switch_request.wlc, 2, 3, 4));
            if (variant == 5) {
                SWITCH_REQUIRE(!switch_finishes && switch_start() == -2);
                switch_home();
                switch_native_finish(4);
            }
        } else if (variant == 6) {
            switch_now = 10000;
            switch_expiry(switch_expiry_context, switch_deadline, switch_now, 2, 3);
        } else {
            switch_home();
            switch_now = 9000;
            if (variant == 7)
                *(uint16_t *)(switch_wlc.bytes + 0x468) = 0x100b;
            if (variant == 8)
                switch_clock_error = -35;
            switch_native_finish(0);
        }
        uint32_t reason = variant == 4 || variant == 5 ? SWITCH_A3_EXCURSION_CANCELLED
                          : variant == 6               ? SWITCH_A3_EXCURSION_EXPIRED
                          : variant == 7               ? SWITCH_A3_EXCURSION_RETURN_FAILED
                          : variant == 8               ? SWITCH_A3_EXCURSION_CLOCK_FAILED
                          : variant == 9               ? SWITCH_A3_EXCURSION_ENTRY_FAILED
                                                       : 0;
        SWITCH_REQUIRE(switch_finishes == 1 && switch_result.reason == reason && !switch_timer_armed);
    }
    SWITCH_REQUIRE(!switch_fault);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.counters[SWITCH_A3_DIAG_EXCURSION_START] == 1 &&
                       switch_a3_diagnostic.counters[SWITCH_A3_DIAG_EXCURSION_RETURN] == 1 &&
                       switch_a3_diagnostic.counters[SWITCH_A3_DIAG_EXCURSION_ENTER] == switch_events));
    return 0;
}

static uint32_t switch_test_preflight(unsigned variant)
{
    switch_reset();
    int wanted = -23;
    if (variant < 4) {
        switch_bsscfg.bytes[8] = 1;
        switch_request.station_bound_verified = variant != 0;
        switch_request.station_return_tsf = variant == 1 ? 0 : variant == 2 ? 9999 : 11000;
        if (variant == 3)
            wanted = 0;
    }
    if (variant == 4) {
        switch_timer_armed = 1;
        wanted = 0;
    }
    if (variant == 5) {
        switch_initialize_error = -27;
        wanted = -27;
    }
    if (variant == 6) {
        switch_arm_error = -35;
        wanted = -35;
    }
    if (variant == 7) {
        switch_claim_error = -16;
        wanted = -16;
    }
    if (variant == 8) {
        switch_request.expiry_tsf = 1000;
        wanted = -29;
    }
    if (variant == 9) {
        switch_request.expiry_tsf = 65536001;
        wanted = -29;
    }
    if (variant == 10) {
        switch_request.expiry_tsf = 65536000;
        wanted = 0;
    }
    if (variant >= 11) {
        switch_expire_before_submit = variant == 11;
        switch_arm_error = variant == 12 ? SWITCH_A3_TIMER_ELAPSED : 0;
        wanted = -29;
    }
    SWITCH_REQUIRE(switch_start() == wanted && !switch_fault && !switch_finishes);
    if (!wanted) {
        if (variant == 10)
            SWITCH_REQUIRE(switch_native_dwell == 65535);
        switch_home();
        switch_native_finish(4);
    } else {
        SWITCH_REQUIRE(!switch_submits);
        if (variant >= 11)
            SWITCH_REQUIRE(!switch_timer_armed && !switch_a3_excursion_pending());
    }
    return 0;
}

static uint32_t switch_test_callbacks(unsigned variant)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_start());
    void (*complete)(void *, uint32_t, void *) = switch_complete;
    void (*entered)(void *, void *, uint32_t *) = switch_entered;
    void *token = switch_complete_context;
    switch_a3_timer_callback expiry = switch_expiry;
    if (variant < 4) {
        switch_home();
        switch_native_finish(0);
        SWITCH_REQUIRE(!switch_start());
        uint32_t dwell = 77;
        if (variant == 0)
            complete(token, 0, switch_request.bsscfg);
        if (variant == 1)
            entered(switch_request.wlc, token, &dwell);
        if (variant == 2)
            expiry(token, switch_deadline, switch_deadline, 2, 3);
        if (variant == 3)
            SWITCH_REQUIRE(switch_a3_excursion_cancel(switch_request.wlc, 2, 3, 9) == -25);
        SWITCH_REQUIRE(switch_finishes == 1 && dwell == 77 && !switch_aborts);
        switch_home();
        switch_native_finish(4);
    } else if (variant == 4) {
        switch_request.station_return_tsf = 8000;
        switch_home();
        switch_native_finish(0);
    } else if (variant == 5) {
        complete(token, 0, switch_wlc.bytes);
        SWITCH_REQUIRE(switch_result.reason == SWITCH_A3_EXCURSION_OWNERSHIP_LOST);
    } else if (variant == 6) {
        switch_reenter = 1;
        switch_home();
        switch_native_finish(0);
        SWITCH_REQUIRE(switch_submits == 2 && switch_finishes == 1);
        switch_home();
        switch_native_finish(4);
    } else {
        switch_reenter = 2;
        switch_enter();
        SWITCH_REQUIRE(switch_finishes == 1 && switch_result.reason == SWITCH_A3_EXCURSION_CANCELLED);
    }
    SWITCH_REQUIRE(!switch_fault);
    return 0;
}

static uint32_t switch_test_entry_failure(unsigned variant)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_start());
    switch_now = variant == 0 ? 10000 : 2000;
    *(uint16_t *)(switch_wlc.bytes + 0x468) = variant == 1 ? 0x100b : switch_request.target_chanspec;
    if (variant == 2)
        switch_clock_error = -35;
    if (variant == 3)
        switch_write_pointer(switch_private.bytes, 0x200, NULL);
    switch_dwell = 9;
    switch_entered(switch_request.wlc, switch_entered_context, &switch_dwell);
    SWITCH_REQUIRE(!switch_fault && switch_dwell == 0 && switch_finishes == 1 && switch_aborts == 1);
    SWITCH_REQUIRE(switch_result.reason == (variant == 2   ? SWITCH_A3_EXCURSION_CLOCK_FAILED
                                            : variant == 3 ? SWITCH_A3_EXCURSION_OWNERSHIP_LOST
                                                           : SWITCH_A3_EXCURSION_ENTRY_FAILED));
    return 0;
}

static uint32_t switch_test_final(unsigned variant)
{
    switch_reset();
    switch_request.station_return_tsf = variant == 0 ? 8000 : 11000;
    SWITCH_REQUIRE(!switch_start());
    if (variant == 4) {
        switch_zero(&switch_request, sizeof switch_request);
        switch_request.station_return_tsf = 8000;
        switch_now = 2000;
        *(uint16_t *)(switch_wlc.bytes + 0x468) = switch_binding.target_chanspec;
        switch_dwell = 99;
        switch_entered(switch_wlc.bytes, switch_entered_context, &switch_dwell);
    } else
        switch_enter();
    *(uint16_t *)(switch_wlc.bytes + 0x468) = switch_binding.home_chanspec;
    switch_now = 9000;
    void (*complete)(void *, uint32_t, void *) = switch_complete;
    void *token = switch_complete_context;
    if (variant == 4) {
        switch_scan.bytes[6] = 0;
        complete(token, 0, switch_binding.bsscfg);
        SWITCH_REQUIRE(switch_dwell == 8 && switch_result.session == 2 && switch_result.clock_epoch == 3 &&
                       switch_result.generation == 4 && switch_result.rx_ready_tsf == 2000);
    } else
        switch_native_finish(0);
    if (variant == 1)
        complete(token, 0, switch_request.bsscfg);
    if (variant == 2)
        switch_expiry(token, switch_deadline + 1, switch_deadline + 1, 2, 3);
    if (variant == 3)
        SWITCH_REQUIRE(!switch_a3_excursion_cancel(switch_request.wlc, 2, 3, 4));
    SWITCH_REQUIRE(!switch_fault && switch_finishes == 1 &&
                   switch_result.reason == (variant == 0 ? SWITCH_A3_EXCURSION_RETURN_FAILED : SWITCH_A3_EXCURSION_OK));
    return 0;
}

static uint32_t switch_test_gate_loss(unsigned variant)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_start());
    switch_home();
    switch_now = 9000;
    switch_gate_result = variant ? SWITCH_A3_CHANNEL_CLOCK_FAILED : SWITCH_A3_CHANNEL_OWNERSHIP_LOST;
    switch_native_finish(0);
    SWITCH_REQUIRE(switch_finishes == 1 && !switch_fault && !switch_timer_armed &&
                   switch_result.reason == (variant ? SWITCH_A3_EXCURSION_CLOCK_FAILED
                                                    : SWITCH_A3_EXCURSION_OWNERSHIP_LOST));
    return 0;
}

static uint32_t switch_test_rx_dwell(unsigned variant)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_start());
    switch_rx_start_delay = 300;
    switch_rx_sample_failure = variant == 1;
    switch_enter();
    switch_now = variant == 2 ? 2100 : 8700;
    if (variant == 3)
        switch_home();
    if (variant == 4)
        switch_write_pointer(switch_private.bytes, 0x200, 0);
    if (variant == 5) {
        switch_home();
        switch_native_finish(0);
    } else {
        switch_deferred = variant == 6;
        switch_a3_excursion_cancel(switch_request.wlc, 2, 3, 4);
        if (switch_deferred) {
            switch_now = 9400;
            switch_a3_excursion_cancel(switch_request.wlc, 2, 3, 4);
            switch_home();
            switch_native_finish(4);
        }
    }
    if (variant && variant != 6)
        SWITCH_REQUIRE(!switch_a3_diagnostic.dwell_entered && !switch_a3_diagnostic.dwell_returned);
    else
        SWITCH_REQUIRE(switch_a3_diagnostic.dwell_entered == 2300 && switch_a3_diagnostic.dwell_returned == 8700 &&
                       switch_a3_diagnostic.dwell_channel == switch_request.target_chanspec &&
                       switch_a3_diagnostic.dwell_owner.session == 2 && switch_a3_diagnostic.dwell_owner.epoch == 3 &&
                       switch_a3_diagnostic.dwell_owner.generation == 4);
    return 0;
}

void switch_excursion_test_run(void)
{
    for (unsigned index = 0; index < 10; ++index)
        switch_excursion_test_results[index] = switch_test_lifecycle(index);
    for (unsigned index = 0; index < 11; ++index)
        switch_excursion_test_results[10 + index] = switch_test_preflight(index);
    for (unsigned index = 0; index < 8; ++index)
        switch_excursion_test_results[21 + index] = switch_test_callbacks(index);
    for (unsigned index = 0; index < 4; ++index)
        switch_excursion_test_results[29 + index] = switch_test_entry_failure(index);
    for (unsigned index = 0; index < 5; ++index)
        switch_excursion_test_results[33 + index] = switch_test_final(index);
    switch_excursion_test_results[38] = switch_test_preflight(11);
    switch_excursion_test_results[39] = switch_test_preflight(12);
    switch_excursion_test_results[40] = switch_test_gate_loss(0);
    switch_excursion_test_results[41] = switch_test_gate_loss(1);
    for (unsigned index = 0; index < 7; ++index)
        switch_excursion_test_results[42 + index] = switch_test_rx_dwell(index);
    switch_excursion_test_done = 0x53574133;
}
