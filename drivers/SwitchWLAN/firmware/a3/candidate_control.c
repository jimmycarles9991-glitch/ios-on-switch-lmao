/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "control.h"
#include "ack.h"
#include "channel.h"
#include "clock.h"
#include "rx.h"
#include "timer.h"
#include "wire.h"
#include "scheduler.h"
#include "tx.h"
#include "beacon.h"
#include "excursion.h"
#include "diagnostic.h"

static struct {
    uint8_t clock_valid, stopping, quiescing, native_up;
    void *wlc;

    union {
        struct switch_a3_owner owner;

        struct {
            uint32_t session, clock_epoch, state_generation;
        };
    };

    uint32_t applied_generation, state;
    uint32_t address_generation, schedule_generation, bound_generation;
    uint32_t state_revision;
    int32_t stop_status, fault_status;
    uint64_t last_tsf;
    struct switch_a3_configuration configuration;
    struct switch_a3_ring_binding rings;
} switch_a3_control_state;

static void switch_a3_control_copy(void *output, const void *input, size_t length);

#if DEVELOPMENT || DEBUG
__attribute__((noinline, noclone)) void switch_a3_control_record(uint16_t reason, int error,
                                                               const struct switch_a3_owner *owner,
                                                               const struct switch_a3_timer *timer, const uint64_t *actual,
                                                               uint32_t channels)
{
    if (!reason)
        return;
    struct switch_a3_fault_record *record = &switch_a3_diagnostic.latest_fault;
    if (!owner)
        owner = &switch_a3_control_state.owner;
    const int timed = timer && timer->snapshot.session_generation == owner->session &&
                      timer->snapshot.clock_generation == owner->epoch && reason != SWITCH_A3_FAULT_CLOCK_UNSTABLE;
    record->observed_tsf = timer ? (timed ? timer->observed_tsf : 0) : switch_a3_control_state.last_tsf;
    record->deadline_tsf = timed ? timer->snapshot.requested_tsf : 0;
    record->actual_tsf = (timer && !timed) || !actual ? 0 : *actual;
    if (!++record->sequence)
        record->sequence = 1;
    record->session = owner->session;
    record->epoch = owner->epoch;
    record->config = owner->generation;
    record->reason = reason;
    record->native_error = (int16_t)__builtin_arm_ssat(error, 16);
    record->target_chanspec = (uint16_t)channels;
    record->actual_chanspec = (uint16_t)(channels >> 16);
    if (!switch_a3_diagnostic.first_fault.sequence)
        switch_a3_control_copy(&switch_a3_diagnostic.first_fault, record, sizeof *record);
}
#endif

uint32_t switch_a3_control_features(void)
{
    return SWITCH_A3_FEATURE_RX | SWITCH_A3_FEATURE_ACK | SWITCH_A3_FEATURE_TX | SWITCH_A3_FEATURE_SCHEDULE |
           SWITCH_A3_FEATURE_COEX | SWITCH_A3_FEATURE_CLOCK;
}

static void switch_a3_control_report(struct switch_a3_reply *reply)
{
    reply->clock_epoch = switch_a3_control_state.clock_epoch;
    reply->applied_generation = switch_a3_control_state.applied_generation;
    reply->state = switch_a3_control_state.state;
    reply->features = switch_a3_control_features();
    reply->tx_credits = reply->state == SWITCH_A3_RUNNING ? switch_a3_tx_credits() : 0;
    reply->pending_rx = switch_a3_rx_pending();
    if (!++switch_a3_control_state.state_revision)
        ++switch_a3_control_state.state_revision;
    reply->state_generation = switch_a3_control_state.state_generation;
    reply->state_revision = switch_a3_control_state.state_revision;
    reply->fault_status = switch_a3_control_state.fault_status ? switch_a3_control_state.fault_status
                                                               : switch_a3_control_state.stop_status;
    reply->legal_social_channels = switch_a3_channel_socials(switch_a3_control_state.wlc);
    reply->home_chanspec = switch_a3_channel_home(switch_a3_control_state.wlc);
}

static __attribute__((noinline)) void switch_a3_control_zero(void *output, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)output;
    for (size_t index = 0; index < length; ++index)
        bytes[index] = 0;
}

static __attribute__((noinline)) void switch_a3_reply_init(struct switch_a3_reply *reply, uint32_t magic)
{
    switch_a3_control_zero(reply, sizeof *reply);
    reply->magic = magic;
    reply->version = SWITCH_A3_WIRE_VERSION;
    reply->header_bytes = reply->total_bytes = sizeof *reply;
    reply->firmware_generation = 1;
}

static void switch_a3_control_notice(void)
{
    if (!switch_a3_control_state.session)
        return;
    struct switch_a3_reply reply;
    switch_a3_reply_init(&reply, SWITCH_A3_STATE_MAGIC);
    reply.session = switch_a3_control_state.session;
    switch_a3_control_report(&reply);
    if (switch_a3_control_state.native_up)
        switch_a3_clock_read(switch_a3_control_state.wlc, &reply.tsf);
    switch_a3_rx_event(switch_a3_control_state.wlc, &reply, sizeof reply);
}

static __attribute__((noinline)) void switch_a3_control_copy(void *output, const void *input, size_t length)
{
    uint8_t *out = output;
    const uint8_t *in = input;
    for (size_t index = 0; index < length; ++index)
        out[index] = in[index];
}

void switch_a3_control_initialize(void *wlc)
{
    switch_a3_control_zero(&switch_a3_control_state, sizeof switch_a3_control_state);
    switch_a3_control_state.wlc = wlc;
    switch_a3_control_state.clock_epoch = 1;
    switch_a3_control_state.native_up = wlc && *((const uint8_t *)wlc + 25);
    SWITCH_A3_DIAG(switch_a3_control_zero(
        &switch_a3_diagnostic.session, sizeof switch_a3_diagnostic - offsetof(struct switch_a3_diagnostic, session)));
    switch_a3_scheduler_stop();
    switch_a3_beacon_reset(wlc);
    switch_a3_ack_detached();
}

static int32_t switch_a3_control_stop(void)
{
    void *wlc = switch_a3_control_state.wlc;
    switch_a3_control_state.stopping = 1;
    switch_a3_control_state.quiescing = 0;
    const int32_t tx_status = switch_a3_tx_cancel(wlc);
    const int32_t scheduler_status = switch_a3_scheduler_stop();
    switch_a3_control_state.applied_generation = 0;
    switch_a3_control_state.address_generation = 0;
    switch_a3_control_state.schedule_generation = 0;
    switch_a3_control_state.bound_generation = 0;
    switch_a3_rx_configure(wlc, 0, 0, 0, 0, 0);
    /* wlc_down suspends the D11 core before calling switch_a3_control_down while wlc+25 is still set. */
    int32_t ack_status = 0;
    if (switch_a3_control_state.native_up)
        ack_status = switch_a3_ack_configure(wlc, 0, 0);
    else
        switch_a3_ack_disable();
    switch_a3_beacon_reset(wlc);
    int32_t result = scheduler_status;
    if (ack_status && ack_status != -11)
        result = ack_status;
    else if (tx_status)
        result = tx_status;
    if (!result && switch_a3_tx_pending())
        result = -16;
    switch_a3_control_state.state = result ? SWITCH_A3_FAULTED : SWITCH_A3_STOPPED;
    switch_a3_control_state.stop_status = result;
    if (result && result != -16 && !switch_a3_control_state.fault_status)
        switch_a3_control_state.fault_status = result;
    switch_a3_control_state.stopping = 0;
    return result;
}

static __attribute__((noinline, noclone)) void switch_a3_control_failed(
    int error, uint16_t reason, const struct switch_a3_owner *owner)
{
    if (!switch_a3_control_state.fault_status)
        switch_a3_control_state.fault_status = error;
    switch_a3_control_stop();
    switch_a3_control_state.state = SWITCH_A3_FAULTED;
    if (reason == SWITCH_A3_FAULT_CLOCK_BACKWARD && switch_a3_control_state.clock_valid &&
        (!owner || (owner->session == switch_a3_control_state.session &&
                    owner->epoch == switch_a3_control_state.clock_epoch))) {
        if (!++switch_a3_control_state.clock_epoch)
            ++switch_a3_control_state.clock_epoch;
        switch_a3_control_state.clock_valid = 0;
    }
}

static void switch_a3_control_scheduler_changed(void *context, int error)
{
    (void)context;
    if (switch_a3_control_state.stopping)
        return;
    if (error) {
        switch_a3_control_failed(error, switch_a3_scheduler_fault_reason(), switch_a3_scheduler_owner());
    } else if (switch_a3_control_state.stop_status == -16 && !switch_a3_tx_pending() &&
               !switch_a3_excursion_pending()) {
        switch_a3_control_state.stop_status = 0;
        switch_a3_control_state.state = SWITCH_A3_STOPPED;
    }
    switch_a3_control_notice();
}

static int32_t switch_a3_control_activate(uint32_t generation, int initial);

void switch_a3_control_retired(void)
{
    if (switch_a3_control_state.quiescing && !switch_a3_tx_pending() && !switch_a3_excursion_pending() &&
        (switch_a3_control_state.quiescing == 2 || switch_a3_scheduler_home_ready(switch_a3_control_state.wlc))) {
        if (switch_a3_control_state.quiescing == 2)
            switch_a3_control_activate(switch_a3_control_state.applied_generation, 0);
        switch_a3_control_notice();
        return;
    }
    if (!switch_a3_control_state.stopping && switch_a3_control_state.stop_status == -16 && !switch_a3_tx_pending() &&
        !switch_a3_excursion_pending()) {
        switch_a3_control_state.stop_status = 0;
        switch_a3_control_state.state = SWITCH_A3_STOPPED;
        switch_a3_control_notice();
    }
}

static __attribute__((noinline)) void switch_a3_control_resource_fault(int error, uint16_t reason)
{
    (void)reason;
    if (error) {
        switch_a3_control_record(reason, error, 0, 0, &switch_a3_control_state.last_tsf, 0);
        switch_a3_control_failed(error, SWITCH_A3_FAULT_NONE, 0);
        switch_a3_control_notice();
    }
}

void switch_a3_control_fault(int error)
{
    switch_a3_control_resource_fault(error, SWITCH_A3_FAULT_RESOURCE);
}

void switch_a3_control_timer_fault(int error, const struct switch_a3_timer *timer, const struct switch_a3_owner *owner)
{
    (void)owner;
    switch_a3_control_record(timer->fault, error, owner, timer, &timer->snapshot.actual_tsf, 0);
    switch_a3_control_failed(error, timer->fault, owner);
    switch_a3_control_notice();
}

static void switch_a3_control_invalidate(void)
{
    switch_a3_control_stop();
    if (switch_a3_control_state.clock_valid && !++switch_a3_control_state.clock_epoch)
        ++switch_a3_control_state.clock_epoch;
    switch_a3_control_state.applied_generation = 0;
    switch_a3_control_state.clock_valid = 0;
    if (!switch_a3_control_state.fault_status)
        switch_a3_control_state.fault_status = -25;
    switch_a3_control_notice();
}

void switch_a3_control_down(void)
{
    switch_a3_control_state.native_up = 0;
    switch_a3_control_record(SWITCH_A3_FAULT_RADIO_DOWN, -25, 0, 0, 0, 0);
    switch_a3_control_invalidate();
}

void switch_a3_control_up(void)
{
    switch_a3_control_state.native_up = 1;
    if (switch_a3_control_state.fault_status == -25)
        switch_a3_control_state.fault_status = 0;
    if (switch_a3_control_state.applied_generation &&
        (switch_a3_control_state.configuration.flags & SWITCH_A3_CONFIG_ACK)) {
        const int result = switch_a3_ack_reconcile(switch_a3_control_state.wlc);
        if (result) {
            switch_a3_control_resource_fault(result, SWITCH_A3_FAULT_ACK);
            return;
        }
    }
    switch_a3_control_notice();
}

static int32_t switch_a3_control_clock(uint64_t *tsf)
{
    if (!switch_a3_control_state.native_up)
        return -25;
    const int32_t result = switch_a3_clock_read(switch_a3_control_state.wlc, tsf);
    if (result)
        return result;
    if (switch_a3_control_state.clock_valid && *tsf < switch_a3_control_state.last_tsf) {
        switch_a3_control_record(SWITCH_A3_FAULT_CLOCK_BACKWARD, -35, 0, 0, tsf, 0);
        switch_a3_control_failed(-35, SWITCH_A3_FAULT_CLOCK_BACKWARD, 0);
        switch_a3_control_state.last_tsf = *tsf;
        switch_a3_control_state.clock_valid = 0;
        return 0;
    }
    switch_a3_control_state.last_tsf = *tsf;
    switch_a3_control_state.clock_valid = 1;
    return 0;
}

static int32_t switch_a3_control_configure(const struct switch_a3_control *request, const uint8_t *payload,
                                           uint32_t length)
{
    static const uint8_t bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
    struct switch_a3_configuration configuration;
    if (length != sizeof configuration || !request->session || !request->config_generation)
        return -2;
    switch_a3_control_copy(&configuration, payload, sizeof configuration);
    if (!configuration.chanspec || configuration.flags & ~(SWITCH_A3_CONFIG_RX | SWITCH_A3_CONFIG_ACK) ||
        (configuration.address[0] & 1u))
        return -2;
    if (configuration.ethertype)
        return -23;
    uint8_t nonzero = 0;
    for (size_t index = 0; index < 6; ++index) {
        if (configuration.bssid[index] != bssid[index])
            return -2;
        nonzero |= configuration.address[index];
    }
    if (!nonzero || request->config_generation < switch_a3_control_state.applied_generation)
        return -2;
    if (request->config_generation == switch_a3_control_state.applied_generation) {
        const uint8_t *old = (const uint8_t *)&switch_a3_control_state.configuration;
        for (size_t index = 0; index < sizeof configuration; ++index)
            if (old[index] != payload[index])
                return -2;
        return 0;
    }
    int32_t result = switch_a3_control_stop();
    if (result)
        return result;
    switch_a3_control_state.session = request->session;
    switch_a3_control_state.state_generation = request->config_generation;
    switch_a3_control_state.fault_status = 0;
    if (configuration.flags & SWITCH_A3_CONFIG_ACK) {
        result = switch_a3_ack_prepare(switch_a3_control_state.wlc);
        if (result) {
            switch_a3_control_state.fault_status = result;
            switch_a3_control_state.state = SWITCH_A3_FAULTED;
            return result;
        }
    }
    switch_a3_control_copy(&switch_a3_control_state.configuration, &configuration, sizeof configuration);
    switch_a3_control_state.applied_generation = request->config_generation;
    switch_a3_control_state.address_generation = request->config_generation;
    return 0;
}

static __attribute__((noinline)) int32_t switch_a3_control_activate(uint32_t generation, int initial)
{
    void *wlc = switch_a3_control_state.wlc;
    struct switch_a3_configuration *configuration = &switch_a3_control_state.configuration;
    uint16_t channel;
    int32_t result = switch_a3_scheduler_start(wlc, &channel);
    if (!result) {
        configuration->chanspec = channel;
        if (initial)
            result = switch_a3_ack_configure(wlc, configuration->address, 1);
        else
            result = switch_a3_ack_reconcile(wlc);
        if (result)
            switch_a3_control_resource_fault(result, SWITCH_A3_FAULT_ACK);
    }
    if (!result)
        result = switch_a3_rx_configure(wlc, configuration->address, 1, switch_a3_control_state.session,
                                         switch_a3_control_state.clock_epoch, generation);
    if (!result)
        result = switch_a3_tx_configure(wlc, configuration, &switch_a3_control_state.rings,
                                         switch_a3_control_state.session, switch_a3_control_state.clock_epoch,
                                         generation, 1);
    switch_a3_control_state.quiescing = 0;
    if (!result) {
        switch_a3_control_state.state = SWITCH_A3_RUNNING;
        result = switch_a3_scheduler_run(wlc, switch_a3_control_state.session,
                                          switch_a3_control_state.clock_epoch, generation,
                                          switch_a3_control_scheduler_changed, 0);
    }
    if (result && switch_a3_control_state.state != SWITCH_A3_FAULTED)
        switch_a3_control_failed(result, SWITCH_A3_FAULT_NONE, 0);
    return result;
}

static int32_t switch_a3_control_schedule(const struct switch_a3_control *request, const uint8_t *payload,
                                          uint32_t length)
{
    int32_t result = -2;
    if (length != sizeof(struct switch_a3_schedule) ||
        request->config_generation <= switch_a3_control_state.applied_generation)
        goto rejected;
    struct switch_a3_schedule schedule;
    switch_a3_control_copy(&schedule, payload, sizeof schedule);
    void *wlc = switch_a3_control_state.wlc;
    const int running = switch_a3_control_state.state == SWITCH_A3_RUNNING;
    result = switch_a3_scheduler_validate(wlc, &schedule, running);
    if (result)
        goto rejected;
    if (running) {
        if (switch_a3_control_state.quiescing == 2)
            switch_a3_control_state.quiescing = 1;
        if (!switch_a3_control_state.quiescing) {
            switch_a3_control_state.quiescing = 1;
            const int tx = switch_a3_tx_cancel(wlc);
            result = switch_a3_scheduler_quiesce();
            if (tx || (result && result != -16)) {
                switch_a3_control_fault(tx ? tx : result);
                return tx ? tx : result;
            }
        }
        if (switch_a3_tx_pending() || switch_a3_excursion_pending())
            return -16;
        if (!switch_a3_scheduler_home_ready(wlc)) {
            switch_a3_control_fault(-25);
            return -25;
        }
    }
    result = switch_a3_scheduler_install(switch_a3_control_state.wlc, &schedule);
    if (result)
        goto rejected;
    switch_a3_control_state.applied_generation = request->config_generation;
    switch_a3_control_state.schedule_generation = request->config_generation;
    switch_a3_control_state.state_generation = request->config_generation;
    if (running) {
        switch_a3_control_state.bound_generation = request->config_generation;
        result = switch_a3_control_activate(request->config_generation, 0);
    }
    return result;
rejected:
    if (switch_a3_control_state.quiescing) {
        switch_a3_control_state.quiescing = 2;
        switch_a3_control_retired();
    }
    return result;
}

static int32_t switch_a3_control_bind(const uint8_t *payload, uint32_t length)
{
    struct switch_a3_ring_binding rings;
    if (length != sizeof rings)
        return -2;
    switch_a3_control_copy(&rings, payload, sizeof rings);
    if (!switch_a3_tx_binding_valid(&rings))
        return -2;
    if (switch_a3_control_state.state == SWITCH_A3_RUNNING ||
        switch_a3_control_state.schedule_generation != switch_a3_control_state.applied_generation)
        return -25;
    switch_a3_control_copy(&switch_a3_control_state.rings, &rings, sizeof rings);
    switch_a3_control_state.bound_generation = switch_a3_control_state.applied_generation;
    return 0;
}

static int32_t switch_a3_control_start(void)
{
    const uint32_t generation = switch_a3_control_state.applied_generation;
    struct switch_a3_configuration *configuration = &switch_a3_control_state.configuration;
    if (!generation || switch_a3_control_state.bound_generation != generation ||
        switch_a3_control_state.schedule_generation != generation ||
        configuration->flags != (SWITCH_A3_CONFIG_RX | SWITCH_A3_CONFIG_ACK))
        return -25;
    void *wlc = switch_a3_control_state.wlc;
    if (switch_a3_control_state.state == SWITCH_A3_RUNNING) {
        const int32_t result = switch_a3_ack_reconcile(wlc);
        if (result)
            switch_a3_control_resource_fault(result, SWITCH_A3_FAULT_ACK);
        return result;
    }
    return switch_a3_control_activate(generation, 1);
}



static int32_t switch_a3_control_apply(const struct switch_a3_control *request, const uint8_t *payload, uint32_t length)
{
    if (switch_a3_control_state.session && request->session != switch_a3_control_state.session)
        return -2;
    if (request->operation == SWITCH_A3_GET_CLOCK || request->operation == SWITCH_A3_GET_STATE)
        return length ? -2 : 0;
    if (request->operation == SWITCH_A3_RESET_CLOCK) {
        if (length)
            return -2;
        switch_a3_control_record(SWITCH_A3_FAULT_CLOCK_RESET, 0, 0, 0, 0, 0);
        switch_a3_control_invalidate();
        return 0;
    }
    if (request->operation == SWITCH_A3_STOP)
        return length ? -2 : switch_a3_control_stop();
    if (request->clock_epoch != switch_a3_control_state.clock_epoch)
        return -35;
    if (request->operation == SWITCH_A3_CONFIGURE)
        return switch_a3_control_configure(request, payload, length);
    if (!switch_a3_control_state.session || !switch_a3_control_state.address_generation)
        return -25;
    if (request->operation == SWITCH_A3_INSTALL_SCHEDULE)
        return switch_a3_control_schedule(request, payload, length);
    if (request->config_generation != switch_a3_control_state.applied_generation)
        return -25;
    switch (request->operation) {
    case SWITCH_A3_START:
        return length ? -2 : switch_a3_control_start();
    case SWITCH_A3_BIND_RINGS:
        return switch_a3_control_bind(payload, length);

    default:
        return -23;
    }
}

int32_t switch_a3_control_request(const void *input, uint32_t input_length, void *output, uint32_t output_length)
{
    if (!input || !output)
        return -2;
    if (input_length < sizeof(struct switch_a3_control) || output_length < sizeof(struct switch_a3_reply))
        return -14;
    struct switch_a3_control request;
    switch_a3_control_copy(&request, input, sizeof request);
    /* Broadcom GET pads the input to the reply capacity. */
    if (request.magic != SWITCH_A3_CONTROL_MAGIC || request.version != SWITCH_A3_WIRE_VERSION ||
        request.header_bytes != sizeof request || request.total_bytes < sizeof request ||
        request.total_bytes > input_length || !request.request_id)
        return -2;
    struct switch_a3_reply reply;
    switch_a3_reply_init(&reply, SWITCH_A3_REPLY_MAGIC);
    reply.request_id = request.request_id;
    if (request.operation == SWITCH_A3_GET_STATE || request.operation == SWITCH_A3_STOP ||
        request.operation == SWITCH_A3_RESET_CLOCK) {
        reply.status = switch_a3_control_apply(&request, (const uint8_t *)input + sizeof request,
                                               request.total_bytes - sizeof request);
        if (switch_a3_control_state.native_up)
            switch_a3_clock_read(switch_a3_control_state.wlc, &reply.tsf);
    } else {
        reply.status = switch_a3_control_clock(&reply.tsf);
        if (!reply.status)
            reply.status = switch_a3_control_apply(&request, (const uint8_t *)input + sizeof request,
                                                   request.total_bytes - sizeof request);
    }
    reply.session = switch_a3_control_state.session ? switch_a3_control_state.session : request.session;
    switch_a3_control_report(&reply);
    switch_a3_control_copy(output, &reply, sizeof reply);
#if DEVELOPMENT || DEBUG
    if ((request.operation == SWITCH_A3_GET_CLOCK || request.operation == SWITCH_A3_GET_STATE) &&
        output_length >= sizeof reply + sizeof switch_a3_diagnostic) {
        switch_a3_diagnostic.session = reply.session;
        switch_a3_diagnostic.clock_epoch = reply.clock_epoch;
        switch_a3_diagnostic.request_id = reply.request_id;
        switch_a3_diagnostic.state_revision = reply.state_revision;
        const struct switch_a3_beacon_anchor *anchor = switch_a3_beacon_anchor(switch_a3_control_state.wlc);
        if (anchor)
            switch_a3_control_copy(&switch_a3_diagnostic.beacon, anchor, sizeof *anchor);
        else
            switch_a3_diagnostic.beacon.chanspec = 0;
        switch_a3_control_copy((uint8_t *)output + sizeof reply, &switch_a3_diagnostic,
                              sizeof switch_a3_diagnostic);
    }
#endif
    return 0;
}

