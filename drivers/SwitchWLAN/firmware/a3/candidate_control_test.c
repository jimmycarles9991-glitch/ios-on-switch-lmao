/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "control.h"
#include "diagnostic.h"
#include "rx.h"
#include "timer.h"
#include "tx.h"
#include "excursion.h"

static uint32_t switch_wlc[384], switch_band[8], switch_pi;
static uint64_t switch_now, switch_input[32];
static uint16_t switch_channel;
static int switch_ack_failure, switch_rx_failure, switch_tx_failure, switch_binding_valid;
static int switch_clock_failure, switch_notice_failure, switch_cancel_failure;
static int switch_timer_failure, switch_allow_away, switch_accept_excursion;
static uint32_t switch_ack, switch_rx, switch_tx, switch_tx_generation, switch_rx_generation, switch_tx_configures;
static uint32_t switch_ack_configures, switch_ack_disables, switch_clock_reads, switch_excursion_active,
    switch_timer_arms;
static uint16_t switch_physical;
static uint32_t switch_pending, switch_pending_rx;
static uint32_t switch_bsscfg[128];
static struct switch_a3_reply switch_reply;
static struct switch_a3_reply switch_notice;
static uint32_t switch_notices;
static uint32_t switch_ack_reconciles;
static switch_a3_excursion_callback switch_excursion_callback;
static void *switch_excursion_context;
static struct switch_a3_excursion_request switch_excursion_request;
__attribute__((used)) volatile uint32_t switch_control_test_results[39];
volatile uint32_t switch_control_test_done;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static void switch_copy(void *to, const void *from, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index)
        ((uint8_t *)to)[index] = ((const uint8_t *)from)[index];
}

__attribute__((used)) void *memcpy(void *to, const void *from, size_t length)
{
    switch_copy(to, from, length);
    return to;
}

__attribute__((used)) void *memset(void *to, int value, size_t length)
{
    for (size_t index = 0; index < length; ++index)
        ((uint8_t *)to)[index] = (uint8_t)value;
    return to;
}

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    ++switch_clock_reads;
    if (wlc != switch_wlc)
        return -2;
    *tsf = switch_now;
    if (switch_clock_failure) {
        *tsf = 0;
        return switch_clock_failure;
    }
    return 0;
}

uint16_t switch_a3_tx_phy_chanspec(void *pi)
{
    return pi == &switch_pi ? (switch_physical ? switch_physical : switch_channel) : 0;
}

void *switch_a3_station_bsscfg(void *wlc)
{
    return wlc == switch_wlc ? switch_bsscfg : 0;
}

uint16_t switch_a3_channel_control(uint32_t chanspec)
{
    return chanspec;
}

int switch_a3_channel_usable(void *wlc, uint16_t chanspec)
{
    return wlc == switch_wlc && (chanspec == 0x1006 || (switch_allow_away && chanspec == 0xd02c));
}

uint16_t switch_a3_channel_socials(void *wlc)
{
    return switch_a3_channel_usable(wlc, 0x1006) ? SWITCH_A3_SOCIAL_6 : 0;
}

uint16_t switch_a3_channel_home(void *wlc)
{
    return wlc == switch_wlc ? switch_channel : 0;
}

int switch_a3_beacon_bound(void *wlc, uint16_t home, uint64_t now, void **bsscfg, uint64_t *until)
{
    (void)wlc;
    (void)home;
    (void)now;
    (void)bsscfg;
    (void)until;
    return -16;
}

#if DEVELOPMENT || DEBUG
const struct switch_a3_beacon_anchor *switch_a3_beacon_anchor(void *wlc)
{
    (void)wlc;
    return 0;
}
#endif

void switch_a3_beacon_reset(void *wlc)
{
    (void)wlc;
}

#if DEVELOPMENT || DEBUG
void switch_a3_excursion_rx_ready(void *wlc)
{
    (void)wlc;
}
#endif

uint32_t switch_a3_excursion_pending(void)
{
    return switch_excursion_active;
}

int switch_a3_excursion_cancel(void *wlc, uint32_t session, uint32_t epoch, uint32_t generation)
{
    (void)wlc;
    (void)session;
    (void)epoch;
    (void)generation;
    return 0;
}

int switch_a3_excursion_start(const struct switch_a3_excursion_request *request, switch_a3_excursion_callback callback,
                              void *context)
{
    if (switch_accept_excursion) {
        switch_excursion_request = *request;
        switch_excursion_callback = callback;
        switch_excursion_context = context;
        switch_excursion_active = 1;
        return 0;
    }
    return -16;
}

void switch_a3_tx_wake(void *wlc)
{
    (void)wlc;
}

int switch_a3_rx_event(void *wlc, const void *data, uint32_t length)
{
    (void)wlc;
    if (switch_notice_failure)
        return switch_notice_failure;
    if (length == sizeof switch_notice) {
        switch_copy(&switch_notice, data, length);
        ++switch_notices;
    }
    return length == sizeof(struct switch_a3_reply) ? 0 : -2;
}

void switch_a3_ack_detached(void)
{}

void switch_a3_ack_disable(void)
{
    ++switch_ack_disables;
    switch_ack = 0;
}

int32_t switch_a3_ack_prepare(void *wlc)
{
    return wlc == switch_wlc ? switch_ack_failure : -2;
}

int32_t switch_a3_ack_reconcile(void *wlc)
{
    ++switch_ack_reconciles;
    return switch_a3_ack_prepare(wlc);
}

int32_t switch_a3_ack_configure(void *wlc, const uint8_t address[6], int enabled)
{
    (void)wlc;
    (void)address;
    ++switch_ack_configures;
    switch_ack = enabled && !switch_ack_failure;
    return enabled ? switch_ack_failure : 0;
}

int switch_a3_rx_configure(void *wlc, const uint8_t address[6], int enabled, uint32_t session, uint32_t epoch,
                           uint32_t generation)
{
    (void)wlc;
    (void)address;
    (void)session;
    (void)epoch;
    switch_rx = enabled && !switch_rx_failure;
    switch_rx_generation = generation;
    return enabled ? switch_rx_failure : 0;
}

uint32_t switch_a3_rx_pending(void)
{
    return switch_pending_rx;
}

int switch_a3_timer_cancel(void *wlc)
{
    (void)wlc;
    return 0;
}

int switch_a3_timer_initialize(void *wlc)
{
    (void)wlc;
    return 0;
}

int switch_a3_timer_initialize_owned(struct switch_a3_timer *timer, void *wlc)
{
    timer->wlc = wlc;
    return 0;
}

int switch_a3_timer_cancel_owned(struct switch_a3_timer *timer, void *wlc)
{
    (void)wlc;
    timer->snapshot.armed = 0;
    return 0;
}

int switch_a3_timer_arm_owned(struct switch_a3_timer *timer, void *wlc, uint64_t deadline, uint32_t session,
                              uint32_t epoch, switch_a3_timer_callback callback, void *context)
{
    (void)wlc;
    (void)callback;
    (void)context;
    if (switch_timer_failure) {
        timer->snapshot.status = switch_timer_failure;
        timer->fault = SWITCH_A3_FAULT_CLOCK_UNSTABLE;
        return switch_timer_failure;
    }
    ++switch_timer_arms;
    timer->snapshot.requested_tsf = deadline;
    timer->snapshot.session_generation = session;
    timer->snapshot.clock_generation = epoch;
    timer->observed_tsf = switch_now;
    timer->snapshot.armed = 1;
    return 0;
}

int switch_a3_timer_defer_owned(struct switch_a3_timer *timer, void *wlc, uint32_t session, uint32_t epoch,
                                switch_a3_timer_callback callback, void *context)
{
    return switch_a3_timer_arm_owned(timer, wlc, switch_now + 1, session, epoch, callback, context);
}

int switch_a3_timer_arm(void *wlc, uint64_t deadline, uint32_t session, uint32_t epoch,
                        switch_a3_timer_callback callback, void *context)
{
    (void)wlc;
    (void)deadline;
    (void)session;
    (void)epoch;
    (void)callback;
    (void)context;
    return 0;
}

void switch_a3_timer_snapshot(struct switch_a3_deadline_snapshot *snapshot)
{
    snapshot->armed = 0;
}

int switch_a3_tx_binding_valid(const struct switch_a3_ring_binding *rings)
{
    (void)rings;
    return switch_binding_valid;
}

uint32_t switch_a3_tx_credits(void)
{
    return switch_tx ? 5 : 0;
}

uint32_t switch_a3_tx_pending(void)
{
    return switch_pending;
}

int switch_a3_tx_cancel(void *wlc)
{
    (void)wlc;
    switch_tx = 0;
    return switch_cancel_failure;
}

int switch_a3_tx_configure(void *wlc, const struct switch_a3_configuration *configuration,
                           const struct switch_a3_ring_binding *rings, uint32_t session, uint32_t epoch,
                           uint32_t generation, uint32_t firmware)
{
    (void)wlc;
    (void)configuration;
    (void)rings;
    (void)session;
    (void)epoch;
    (void)firmware;
    switch_tx_generation = generation;
    ++switch_tx_configures;
    switch_tx = !switch_tx_failure;
    return switch_tx_failure;
}

static void switch_reset(void)
{
    switch_now = 10000;
    switch_channel = 0x1006;
    switch_ack_failure = switch_rx_failure = switch_tx_failure = 0;
    switch_ack = switch_rx = switch_tx = 0;
    switch_tx_configures = switch_ack_configures = switch_ack_disables = switch_clock_reads = 0;
    switch_excursion_active = switch_timer_arms = 0;
    switch_physical = 0;
    switch_pending = 0;
    switch_pending_rx = 0;
    switch_clock_failure = switch_notice_failure = switch_cancel_failure = 0;
    switch_timer_failure = switch_allow_away = switch_accept_excursion = 0;
    switch_ack_reconciles = 0;
    switch_excursion_callback = 0;
    switch_excursion_context = 0;
    switch_notices = 0;
    switch_binding_valid = 1;
    *((uint8_t *)switch_wlc + 25) = 1;
    switch_wlc[8] = (uint32_t)switch_band;
    switch_band[4] = (uint32_t)&switch_pi;
    switch_a3_control_initialize(switch_wlc);
}

static int32_t switch_request(uint32_t operation, uint32_t generation, uint32_t epoch, const void *payload,
                              uint32_t bytes)
{
    struct switch_a3_control *request = (void *)switch_input;
    *request = (struct switch_a3_control){
        SWITCH_A3_CONTROL_MAGIC,
        SWITCH_A3_WIRE_VERSION,
        sizeof *request,
        sizeof *request + bytes,
        17,
        7,
        epoch,
        generation,
        operation,
    };
    if (bytes)
        switch_copy((uint8_t *)switch_input + sizeof *request, payload, bytes);
    const int32_t result =
        switch_a3_control_request(switch_input, sizeof *request + bytes, &switch_reply, sizeof switch_reply);
    return result ? result : switch_reply.status;
}

static int32_t switch_configure(void)
{
    const struct switch_a3_configuration configuration = {
        {2, 3, 4, 5, 6, 7}, {0, 0x25, 0, 0xff, 0x94, 0x73}, 0, 0x1006, SWITCH_A3_CONFIG_RX | SWITCH_A3_CONFIG_ACK,
    };
    return switch_request(SWITCH_A3_CONFIGURE, 9, 1, &configuration, sizeof configuration);
}

static struct switch_a3_schedule switch_schedule(void)
{
    struct switch_a3_schedule schedule = {0};
    schedule.anchor_tsf = switch_now;
    schedule.period_us = 65536;
    schedule.infra_chanspec = 0x1006;
    schedule.slot_count = 2;
    schedule.slots[0] = (struct switch_a3_slot){0x1006, 0, 32768};
    schedule.slots[1] = (struct switch_a3_slot){0, 0, 32768};
    return schedule;
}

static int32_t switch_install(struct switch_a3_schedule schedule)
{
    return switch_request(SWITCH_A3_INSTALL_SCHEDULE, 10, 1, &schedule, sizeof schedule);
}

static int32_t switch_bind(void)
{
    const struct switch_a3_ring_binding rings = {3, 4, 1800, 0, 4, 0};
    return switch_request(SWITCH_A3_BIND_RINGS, 10, 1, &rings, sizeof rings);
}

static uint32_t switch_test(uint32_t scenario)
{
    switch_reset();
    if (scenario == 0) {
        SWITCH_REQUIRE(switch_request(SWITCH_A3_START, 0, 1, 0, 0) == -25);
        SWITCH_REQUIRE(switch_reply.features == SWITCH_A3_FEATURE_REQUIRED && !switch_reply.tx_credits &&
                       switch_reply.state_revision && !switch_reply.state_generation &&
                       switch_reply.legal_social_channels == SWITCH_A3_SOCIAL_6 &&
                       switch_reply.home_chanspec == 0x1006);
        return 0;
    }
    SWITCH_REQUIRE(!switch_configure());
    if (scenario == 1) {
        SWITCH_REQUIRE(switch_bind() == -25);
        SWITCH_REQUIRE(switch_request(SWITCH_A3_START, 9, 1, 0, 0) == -25);
        return 0;
    }
    struct switch_a3_schedule schedule = switch_schedule();
    if (scenario == 21) {
        schedule.slots[0].chanspec = 0;
        SWITCH_REQUIRE(switch_install(schedule) == -23);
        SWITCH_REQUIRE(!switch_ack && !switch_rx && !switch_tx && switch_reply.state != SWITCH_A3_RUNNING);
        return 0;
    }
    if (scenario == 2)
        schedule.slots[0].chanspec = 0x102c;
    if (scenario == 3)
        schedule.slots[0].dwell_us--;
    if (scenario == 4)
        schedule.slots[2].chanspec = 0x1006;
    if (scenario == 5)
        schedule.anchor_tsf = UINT64_MAX;
    if (scenario >= 2 && scenario <= 5) {
        SWITCH_REQUIRE(switch_install(schedule) == (scenario == 2 ? -23 : -2));
        SWITCH_REQUIRE(!switch_ack && !switch_rx && !switch_tx);
        SWITCH_REQUIRE(switch_reply.state != SWITCH_A3_RUNNING);
        return 0;
    }
    SWITCH_REQUIRE(!switch_install(schedule));
    SWITCH_REQUIRE(switch_reply.applied_generation == 10 && !switch_reply.tx_credits);
    if (scenario == 6) {
        SWITCH_REQUIRE(switch_request(SWITCH_A3_START, 10, 1, 0, 0) == -25);
        return 0;
    }
    if (scenario == 7) {
        switch_binding_valid = 0;
        SWITCH_REQUIRE(switch_bind() == -2);
        SWITCH_REQUIRE(switch_request(SWITCH_A3_START, 10, 1, 0, 0) == -25);
        return 0;
    }
    SWITCH_REQUIRE(!switch_bind());
    if (scenario == 8)
        switch_ack_failure = -27;
    if (scenario == 9)
        switch_rx_failure = -27;
    if (scenario == 10)
        switch_tx_failure = -27;
    if (scenario == 11)
        switch_channel = 0x100b;
    const int32_t started = switch_request(SWITCH_A3_START, 10, 1, 0, 0);
    if (scenario >= 8 && scenario <= 11) {
        SWITCH_REQUIRE(started == (scenario == 11 ? -25 : -27));
        SWITCH_REQUIRE(!switch_ack && !switch_rx && !switch_tx && !switch_reply.applied_generation);
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_FAULTED && !switch_reply.tx_credits);
        return 0;
    }
    SWITCH_REQUIRE(!started && switch_reply.state == SWITCH_A3_RUNNING);
    SWITCH_REQUIRE(switch_ack && switch_rx && switch_tx && switch_reply.tx_credits == 5);
    SWITCH_REQUIRE(switch_reply.features == SWITCH_A3_FEATURE_REQUIRED && switch_rx_generation == 10 &&
                   switch_tx_generation == 10);
    if (scenario == 16) {
        switch_tx_failure = -25;
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_START, 10, 1, 0, 0));
        SWITCH_REQUIRE(switch_tx_configures == 1 && switch_ack && switch_rx && switch_tx);
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_RUNNING && switch_reply.tx_credits == 5);
        return 0;
    }
    if (scenario == 17) {
        switch_pending = 1;
        SWITCH_REQUIRE(switch_request(SWITCH_A3_STOP, 10, 0, 0, 0) == -16);
        SWITCH_REQUIRE(!switch_ack && !switch_rx && !switch_tx && !switch_reply.applied_generation);
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_FAULTED && switch_reply.fault_status == -16 &&
                       switch_reply.state_generation == 10);
        SWITCH_REQUIRE(switch_configure() == -16);
        const uint32_t busy_revision = switch_reply.state_revision;
        switch_pending = 0;
        switch_a3_control_retired();
        SWITCH_REQUIRE(
            switch_notices == 1 && switch_notice.magic == SWITCH_A3_STATE_MAGIC && !switch_notice.request_id &&
            switch_notice.state == SWITCH_A3_STOPPED && !switch_notice.applied_generation &&
            switch_notice.session == 7 && switch_notice.clock_epoch == 1 && switch_notice.state_generation == 10 &&
            switch_notice.state_revision > busy_revision && !switch_notice.status && !switch_notice.fault_status);
        switch_a3_control_retired();
        SWITCH_REQUIRE(switch_notices == 1);
        SWITCH_REQUIRE(!switch_configure());
        SWITCH_REQUIRE(switch_reply.state_revision > switch_notice.state_revision &&
                       switch_reply.state_generation == 9);
        return 0;
    }
    if (scenario == 18) {
        switch_notice_failure = -27;
        switch_a3_control_fault(-29);
        SWITCH_REQUIRE(!switch_notices);
        switch_clock_failure = -35;
        switch_pending_rx = 2;
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 10, 0, 0, 0));
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_FAULTED && switch_reply.fault_status == -29 &&
                       switch_reply.state_generation == 10 && !switch_reply.applied_generation &&
                       switch_reply.clock_epoch == 1 && !switch_reply.tsf && switch_reply.pending_rx == 2);
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_STOP, 10, 0, 0, 0));
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_STOPPED && switch_reply.fault_status == -29 &&
                       switch_reply.state_generation == 10 && switch_reply.clock_epoch == 1 && !switch_reply.tsf);
        return 0;
    }
    if (scenario == 19) {
        switch_pending = 1;
        switch_a3_control_fault(-29);
        SWITCH_REQUIRE(switch_notices == 1 && switch_notice.state == SWITCH_A3_FAULTED && !switch_notice.status &&
                       switch_notice.fault_status == -29 && switch_notice.state_generation == 10);
        const uint32_t fault_revision = switch_notice.state_revision;
        switch_pending = 0;
        switch_a3_control_retired();
        SWITCH_REQUIRE(switch_notices == 2 && switch_notice.state == SWITCH_A3_STOPPED &&
                       switch_notice.fault_status == -29 && switch_notice.state_generation == 10 &&
                       switch_notice.state_revision > fault_revision && !switch_notice.applied_generation);
        return 0;
    }
    if (scenario == 20) {
        switch_cancel_failure = -1;
        SWITCH_REQUIRE(switch_request(SWITCH_A3_STOP, 10, 0, 0, 0) == -1);
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_FAULTED && switch_reply.fault_status == -1 &&
                       switch_reply.state_generation == 10 && !switch_reply.applied_generation);
        return 0;
    }
    if (scenario == 22) {
        const uint32_t running_revision = switch_reply.state_revision;
        schedule.slots[0].chanspec = 0;
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_INSTALL_SCHEDULE, 11, 1, &schedule, sizeof schedule));
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_RUNNING && !switch_reply.fault_status &&
                       switch_reply.state_generation == 11 && switch_reply.applied_generation == 11 &&
                       switch_reply.state_revision > running_revision && switch_ack && switch_rx && switch_tx);
        return 0;
    }
    if (scenario == 12) {
        SWITCH_REQUIRE(switch_request(SWITCH_A3_START, 10, 2, 0, 0) == -35);
        return 0;
    }
    if (scenario == 13) {
        switch_a3_control_down();
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 10, 0, 0, 0));
        SWITCH_REQUIRE(switch_reply.clock_epoch == 2);
    } else if (scenario == 14) {
        --switch_now;
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_CLOCK, 10, 0, 0, 0));
        SWITCH_REQUIRE(switch_reply.clock_epoch == 2);
    } else {
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_STOP, 10, 0, 0, 0));
    }
    SWITCH_REQUIRE(!switch_ack && !switch_rx && !switch_tx);
    SWITCH_REQUIRE(!switch_reply.applied_generation && !switch_reply.tx_credits);
    SWITCH_REQUIRE(switch_reply.state != SWITCH_A3_RUNNING);
    return 0;
}

static uint32_t switch_test_diagnostic(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    const struct switch_a3_configuration configuration =
        *(const struct switch_a3_configuration *)((const uint8_t *)switch_input + sizeof(struct switch_a3_control));
    SWITCH_A3_DIAG(switch_a3_diagnostic.counters[SWITCH_A3_DIAG_TX_NATIVE] = 73);
    SWITCH_REQUIRE(!switch_configure());
    struct switch_a3_control request = {
        SWITCH_A3_CONTROL_MAGIC, SWITCH_A3_WIRE_VERSION, sizeof request, sizeof request,
        91, 7, 1, 9, SWITCH_A3_GET_CLOCK,
    };
    uint64_t storage[48];
    uint8_t *output = (uint8_t *)storage;
    const size_t base = sizeof(struct switch_a3_reply), full = base + sizeof(struct switch_a3_diagnostic);
    for (unsigned variant = 0; variant < 6; ++variant) {
        for (size_t index = 0; index < sizeof storage; ++index)
            output[index] = 0xa5;
        const size_t length = variant == 0 ? base : variant == 1 ? full - 1 : full;
        request.operation = variant == 3 ? SWITCH_A3_GET_STATE : SWITCH_A3_GET_CLOCK;
        request.session = variant == 4 ? 8 : 7;
        switch_clock_failure = variant == 5 ? -35 : 0;
        SWITCH_REQUIRE(!switch_a3_control_request(&request, sizeof request, output, length));
        const struct switch_a3_reply *reply = (const void *)output;
        SWITCH_REQUIRE(reply->total_bytes == base && reply->request_id == 91 &&
                       reply->status == (variant == 4 ? -2 : variant == 5 ? -35 : 0));
        size_t written = base;
#if DEVELOPMENT || DEBUG
        if (variant >= 2) {
            const struct switch_a3_diagnostic *snapshot = (const void *)(output + base);
            SWITCH_REQUIRE(snapshot->magic == SWITCH_A3_DIAGNOSTIC_MAGIC && snapshot->bytes == sizeof *snapshot &&
                           snapshot->version == SWITCH_A3_DIAGNOSTIC_VERSION && snapshot->session == 7 &&
                           snapshot->clock_epoch == 1 && snapshot->request_id == reply->request_id &&
                           snapshot->state_revision == reply->state_revision &&
                           snapshot->counters[SWITCH_A3_DIAG_TX_NATIVE] == 73);
            written = full;
        }
#endif
        for (size_t index = written; index < sizeof storage; ++index)
            SWITCH_REQUIRE(output[index] == 0xa5);
    }
    switch_clock_failure = 0;
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_CONFIGURE, 11, 1, &configuration, sizeof configuration));
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.counters[SWITCH_A3_DIAG_TX_NATIVE] == 73));
    return 0;
}

static uint32_t switch_test_transaction(unsigned variant)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    struct switch_a3_schedule schedule = switch_schedule();
    SWITCH_REQUIRE(!switch_install(schedule) && !switch_bind());
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_START, 10, 1, 0, 0));
    const uint32_t ack_calls = switch_ack_configures;
    if (variant == 0) {
        --schedule.period_us;
        SWITCH_REQUIRE(switch_request(SWITCH_A3_INSTALL_SCHEDULE, 11, 1, &schedule, sizeof schedule) == -2);
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_RUNNING && switch_reply.applied_generation == 10 &&
                       !switch_reply.fault_status && switch_reply.clock_epoch == 1 && switch_ack && switch_rx &&
                       switch_tx && switch_ack_configures == ack_calls && switch_tx_configures == 1);
        return 0;
    }
    if (variant == 1 || variant >= 4)
        switch_pending = 1;
    if (variant == 2) {
        switch_excursion_active = 1;
        switch_physical = 0xd02c;
    }
    if (variant == 1 || variant == 2 || variant >= 4) {
        SWITCH_REQUIRE(switch_request(SWITCH_A3_INSTALL_SCHEDULE, 11, 1, &schedule, sizeof schedule) == -16);
        SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_RUNNING && switch_reply.applied_generation == 10 &&
                       switch_reply.state_generation == 10 && !switch_reply.fault_status &&
                       switch_reply.clock_epoch == 1 && switch_ack && switch_rx && !switch_tx &&
                       switch_ack_configures == ack_calls && switch_tx_configures == 1);
        const uint32_t notices = switch_notices;
        if (variant >= 4) {
            --schedule.period_us;
            SWITCH_REQUIRE(switch_request(SWITCH_A3_INSTALL_SCHEDULE, 12, 1, &schedule, sizeof schedule) == -2);
            switch_pending = 0;
            if (variant == 5)
                switch_channel = 0x100b;
            switch_a3_control_retired();
            SWITCH_REQUIRE(switch_notices == notices + 1 && switch_notice.clock_epoch == 1);
            if (variant == 4)
                SWITCH_REQUIRE(switch_notice.state == SWITCH_A3_RUNNING && switch_notice.applied_generation == 10 &&
                               !switch_notice.fault_status && switch_ack && switch_rx && switch_tx);
            else
                SWITCH_REQUIRE(switch_notice.state == SWITCH_A3_FAULTED && !switch_notice.applied_generation &&
                               switch_notice.fault_status == -25 && !switch_ack && !switch_rx && !switch_tx);
            return 0;
        }
        switch_pending = switch_excursion_active = 0;
        switch_physical = 0;
        switch_a3_control_retired();
        SWITCH_REQUIRE(switch_notices == notices + 1 && switch_notice.state == SWITCH_A3_RUNNING &&
                       switch_notice.applied_generation == 10 && !switch_notice.fault_status);
    }
    if (variant == 3)
        schedule.slots[0].chanspec = 0;
    const uint32_t arms = switch_timer_arms;
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_INSTALL_SCHEDULE, 11, 1, &schedule, sizeof schedule));
    SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_RUNNING && switch_reply.applied_generation == 11 &&
                   switch_reply.state_generation == 11 && !switch_reply.fault_status &&
                   switch_reply.clock_epoch == 1 && switch_ack && switch_rx && switch_tx &&
                   switch_ack_configures == ack_calls && switch_tx_configures == 2 &&
                   switch_rx_generation == 11 && switch_tx_generation == 11);
    if (variant == 3)
        SWITCH_REQUIRE(switch_timer_arms == arms);
    return 0;
}

static uint32_t switch_test_fault_scope(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    struct switch_a3_configuration configuration =
        *(const struct switch_a3_configuration *)((const uint8_t *)switch_input + sizeof(struct switch_a3_control));
    struct switch_a3_timer timer = {0};
    timer.snapshot.session_generation = 7;
    timer.snapshot.clock_generation = 1;
    timer.fault = SWITCH_A3_FAULT_TIMER_EARLY;
    timer.observed_tsf = 10000;
    timer.snapshot.requested_tsf = 10030;
    timer.snapshot.actual_tsf = 10010;
    switch_a3_control_timer_fault(-30, &timer, 0);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 1 && switch_reply.fault_status == -30);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.first_fault.sequence == 1 &&
                       switch_a3_diagnostic.first_fault.reason == SWITCH_A3_FAULT_TIMER_EARLY &&
                       switch_a3_diagnostic.first_fault.observed_tsf == 10000 &&
                       switch_a3_diagnostic.first_fault.actual_tsf == 10010));
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_CONFIGURE, 11, 1, &configuration, sizeof configuration));
    timer.fault = SWITCH_A3_FAULT_CLOCK_BACKWARD;
    timer.snapshot.actual_tsf = 9999;
    switch_a3_control_timer_fault(-35, &timer, 0);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2 && switch_reply.fault_status == -35);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_CONFIGURE, 12, 2, &configuration, sizeof configuration));
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.first_fault.sequence == 1 &&
                       switch_a3_diagnostic.first_fault.reason == SWITCH_A3_FAULT_TIMER_EARLY &&
                       switch_a3_diagnostic.latest_fault.sequence == 2 &&
                       switch_a3_diagnostic.latest_fault.reason == SWITCH_A3_FAULT_CLOCK_BACKWARD &&
                       switch_a3_diagnostic.latest_fault.session == 7 &&
                       switch_a3_diagnostic.latest_fault.epoch == 1 &&
                       switch_a3_diagnostic.latest_fault.config == 11));
    SWITCH_A3_DIAG(
        const uint64_t actual = 3;
        switch_a3_control_record(SWITCH_A3_FAULT_NONE, INT32_MAX, 0, 0, 0, 0);
        SWITCH_REQUIRE(switch_a3_diagnostic.latest_fault.sequence == 2);
        switch_a3_diagnostic.latest_fault.sequence = UINT32_MAX;
        switch_a3_control_record(SWITCH_A3_FAULT_RESOURCE, INT32_MAX, 0, 0, &actual, 4u | (5u << 16));
        SWITCH_REQUIRE(switch_a3_diagnostic.latest_fault.sequence == 1 &&
                       switch_a3_diagnostic.latest_fault.native_error == INT16_MAX &&
                       switch_a3_diagnostic.latest_fault.actual_tsf == actual &&
                       switch_a3_diagnostic.latest_fault.target_chanspec == 4 &&
                       switch_a3_diagnostic.latest_fault.actual_chanspec == 5 &&
                       switch_a3_diagnostic.first_fault.reason == SWITCH_A3_FAULT_TIMER_EARLY &&
                       switch_a3_diagnostic.first_fault.observed_tsf == 10000);
        switch_a3_control_record(SWITCH_A3_FAULT_RESOURCE, INT32_MIN, 0, 0, &actual, 4u | (5u << 16));
        SWITCH_REQUIRE(switch_a3_diagnostic.latest_fault.sequence == 2 &&
                       switch_a3_diagnostic.latest_fault.native_error == INT16_MIN));
    return 0;
}

static uint32_t switch_test_loss_epoch(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    switch_a3_control_down();
    switch_a3_control_down();
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2);
    switch_a3_control_down();
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2);
    SWITCH_REQUIRE(switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 0, 0) == -25);
    switch_a3_control_down();
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0) && switch_reply.clock_epoch == 2);
    switch_a3_control_up();
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 0, 0));
    switch_a3_control_down();
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 3);
    return 0;
}

static uint32_t switch_test_down_no_d11(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    struct switch_a3_schedule schedule = {
        .anchor_tsf = 1000,
        .period_us = 1048576,
        .slot_count = 1,
        .infra_chanspec = 0x1006,
        .slots = {{.chanspec = 0x1006, .dwell_us = 1048576}},
    };
    SWITCH_REQUIRE(!switch_install(schedule));
    SWITCH_REQUIRE(!switch_bind());
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_START, 10, 1, 0, 0));
    SWITCH_REQUIRE(switch_ack && switch_reply.state == SWITCH_A3_RUNNING);
    const uint32_t reads = switch_clock_reads, ack_configures = switch_ack_configures;
    switch_a3_control_down();
    SWITCH_REQUIRE(!switch_ack && switch_ack_disables == 1 && switch_ack_configures == ack_configures &&
                   switch_clock_reads == reads && !switch_notice.tsf);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_clock_reads == reads && !switch_reply.tsf);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_STOP, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_clock_reads == reads && switch_ack_configures == ack_configures && switch_ack_disables == 2 &&
                   !switch_reply.tsf);
    return 0;
}

#include "candidate_contract_cases.h"

void switch_control_test_run(void)
{
    for (uint32_t index = 0; index < 23; ++index)
        switch_control_test_results[index] = switch_test(index);
    switch_control_test_results[23] = switch_test_diagnostic();
    for (unsigned index = 0; index < 6; ++index)
        switch_control_test_results[24 + index] = switch_test_transaction(index);
    switch_control_test_results[30] = switch_test_fault_scope();
    switch_control_test_results[31] = switch_test_loss_epoch();
    switch_control_test_results[32] = switch_test_retired_excursion_identity(0);
    switch_control_test_results[33] = switch_test_retired_excursion_identity(1);
    switch_control_test_results[34] = switch_test_new_epoch_arm_identity();
    switch_control_test_results[35] = switch_test_live_ack_loss(0);
    switch_control_test_results[36] = switch_test_live_ack_loss(1);
    switch_control_test_results[37] = switch_test_transient_clock_sample();
    switch_control_test_results[38] = switch_test_down_no_d11();
    switch_control_test_done = 0x53574133;
}
