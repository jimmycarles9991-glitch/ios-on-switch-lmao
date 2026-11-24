/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "control.h"
#include "ack.h"
#include "clock.h"
#include "rx.h"
#include "timer.h"
#include "wire.h"

uint32_t switch_a3_control_features(void)
{
    return 0;
}

void switch_a3_control_retired(void)
{}

void switch_a3_control_fault(int error)
{
    (void)error;
}

static struct {
    void *wlc;
    struct switch_a3_configuration configuration;
    uint64_t last_tsf;
    uint32_t session, clock_epoch, applied_generation, state;
    uint32_t state_generation, state_revision;
    int32_t fault_status;
    uint8_t clock_valid;
} switch_a3_control_state;

static void switch_a3_control_copy(void *output, const void *input, size_t length)
{
    uint8_t *out = output;
    const uint8_t *in = input;
    for (size_t index = 0; index < length; ++index)
        out[index] = in[index];
}

void switch_a3_control_initialize(void *wlc)
{
    switch_a3_control_state.wlc = wlc;
    switch_a3_control_state.session = 0;
    switch_a3_control_state.clock_epoch = 1;
    switch_a3_control_state.applied_generation = 0;
    switch_a3_control_state.state_generation = 0;
    switch_a3_control_state.state_revision = 0;
    switch_a3_control_state.fault_status = 0;
    switch_a3_control_state.state = SWITCH_A3_STOPPED;
    switch_a3_control_state.clock_valid = 0;
    switch_a3_ack_detached();
}

static int32_t switch_a3_control_stop(void)
{
    void *wlc = switch_a3_control_state.wlc;
    switch_a3_rx_configure(wlc, 0, 0, 0, 0, 0);
    const int32_t timer_status = switch_a3_timer_cancel(wlc);
    const int32_t ack_status = switch_a3_ack_configure(wlc, 0, 0);
    const int32_t result = ack_status && ack_status != -11 ? ack_status : timer_status;
    switch_a3_control_state.applied_generation = 0;
    switch_a3_control_state.state = result && result != -11 ? SWITCH_A3_FAULTED : SWITCH_A3_STOPPED;
    if (result && result != -11 && !switch_a3_control_state.fault_status)
        switch_a3_control_state.fault_status = result;
    return result == -11 ? 0 : result;
}

void switch_a3_control_down(void)
{
    switch_a3_control_stop();
    if (!++switch_a3_control_state.clock_epoch)
        ++switch_a3_control_state.clock_epoch;
    switch_a3_control_state.applied_generation = 0;
    switch_a3_control_state.clock_valid = 0;
}

void switch_a3_control_up(void)
{
    if (switch_a3_control_state.applied_generation &&
        (switch_a3_control_state.configuration.flags & SWITCH_A3_CONFIG_ACK) &&
        switch_a3_ack_reconcile(switch_a3_control_state.wlc)) {
        switch_a3_control_stop();
        switch_a3_control_state.state = SWITCH_A3_FAULTED;
        switch_a3_control_state.applied_generation = 0;
    }
}

static int32_t switch_a3_control_clock(uint64_t *tsf)
{
    const int32_t result = switch_a3_clock_read(switch_a3_control_state.wlc, tsf);
    if (result) {
        switch_a3_control_state.fault_status = result;
        switch_a3_control_down();
        switch_a3_control_state.state = SWITCH_A3_FAULTED;
        return result;
    }
    if (switch_a3_control_state.clock_valid && *tsf < switch_a3_control_state.last_tsf) {
        switch_a3_control_state.fault_status = -35;
        switch_a3_control_stop();
        if (!++switch_a3_control_state.clock_epoch)
            ++switch_a3_control_state.clock_epoch;
        switch_a3_control_state.applied_generation = 0;
        switch_a3_control_state.state = SWITCH_A3_FAULTED;
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
    if (configuration.flags & SWITCH_A3_CONFIG_ACK) {
        result = switch_a3_ack_prepare(switch_a3_control_state.wlc);
        if (result)
            return result;
    }
    switch_a3_control_copy(&switch_a3_control_state.configuration, &configuration, sizeof configuration);
    switch_a3_control_state.session = request->session;
    switch_a3_control_state.applied_generation = request->config_generation;
    switch_a3_control_state.state_generation = request->config_generation;
    switch_a3_control_state.fault_status = 0;
    return 0;
}

#if DEVELOPMENT || DEBUG
static void switch_a3_control_deadline_fired(void *context, uint64_t requested, uint64_t actual, uint32_t session,
                                             uint32_t epoch)
{
    (void)context;
    (void)requested;
    (void)actual;
    if (session != switch_a3_control_state.session || epoch != switch_a3_control_state.clock_epoch)
        switch_a3_control_state.state = SWITCH_A3_FAULTED;
}

static int32_t switch_a3_control_receive(uint32_t enabled)
{
    if (enabled > 1)
        return -2;
    if (!enabled)
        return switch_a3_control_stop();
    void *wlc = switch_a3_control_state.wlc;
    const struct switch_a3_configuration *configuration = &switch_a3_control_state.configuration;
    if (!(configuration->flags & SWITCH_A3_CONFIG_RX))
        return -25;
    if (configuration->flags & SWITCH_A3_CONFIG_ACK) {
        const int32_t result = switch_a3_ack_configure(wlc, configuration->address, 1);
        if (result)
            return result;
    }
    const int32_t result =
        switch_a3_rx_configure(wlc, configuration->address, 1, switch_a3_control_state.session,
                               switch_a3_control_state.clock_epoch, switch_a3_control_state.applied_generation);
    if (result) {
        switch_a3_control_stop();
        return result;
    }
    switch_a3_control_state.state = SWITCH_A3_CONFIGURING;
    return 0;
}
#endif

static int32_t switch_a3_control_apply(const struct switch_a3_control *request, const uint8_t *payload, uint32_t length)
{
    if (switch_a3_control_state.session && request->session != switch_a3_control_state.session)
        return -2;
    if (request->operation == SWITCH_A3_GET_CLOCK || request->operation == SWITCH_A3_GET_STATE)
        return length ? -2 : 0;
    if (request->operation == SWITCH_A3_RESET_CLOCK) {
        if (length)
            return -2;
        switch_a3_control_down();
        return 0;
    }
    if (request->operation == SWITCH_A3_STOP)
        return length ? -2 : switch_a3_control_stop();
    if (request->clock_epoch != switch_a3_control_state.clock_epoch)
        return -35;
    if (request->operation == SWITCH_A3_CONFIGURE)
        return switch_a3_control_configure(request, payload, length);
    if (!switch_a3_control_state.session || request->config_generation != switch_a3_control_state.applied_generation ||
        !switch_a3_control_state.applied_generation)
        return -25;
    switch (request->operation) {
    case SWITCH_A3_START:
    case SWITCH_A3_INSTALL_SCHEDULE:
    case SWITCH_A3_BIND_RINGS:
        return -23;
#if DEVELOPMENT || DEBUG
    case SWITCH_A3_TEST_RECEIVE: {
        uint32_t enabled;
        if (length != sizeof enabled)
            return -2;
        switch_a3_control_copy(&enabled, payload, sizeof enabled);
        return switch_a3_control_receive(enabled);
    }
    case SWITCH_A3_TEST_DEADLINE: {
        uint64_t deadline;
        if (length != sizeof deadline)
            return -2;
        switch_a3_control_copy(&deadline, payload, sizeof deadline);
        const int32_t result = switch_a3_timer_initialize(switch_a3_control_state.wlc);
        if (result)
            return result;
        return switch_a3_timer_arm(switch_a3_control_state.wlc, deadline, switch_a3_control_state.session,
                                   switch_a3_control_state.clock_epoch, switch_a3_control_deadline_fired, 0);
    }
#endif
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
    reply.magic = SWITCH_A3_REPLY_MAGIC;
    reply.version = SWITCH_A3_WIRE_VERSION;
    reply.header_bytes = sizeof reply;
    reply.total_bytes = sizeof reply;
    reply.request_id = request.request_id;
    reply.features = 0;
    reply.tx_credits = 0;
    reply.tsf = 0;
    reply.firmware_generation = 1;
    reply.reserved = 0;
    if (request.operation == SWITCH_A3_GET_STATE || request.operation == SWITCH_A3_STOP ||
        request.operation == SWITCH_A3_RESET_CLOCK) {
        reply.status = switch_a3_control_apply(&request, (const uint8_t *)input + sizeof request,
                                               request.total_bytes - sizeof request);
        switch_a3_clock_read(switch_a3_control_state.wlc, &reply.tsf);
    } else {
        reply.status = switch_a3_control_clock(&reply.tsf);
        if (!reply.status)
            reply.status = switch_a3_control_apply(&request, (const uint8_t *)input + sizeof request,
                                                   request.total_bytes - sizeof request);
    }
    reply.session = switch_a3_control_state.session ? switch_a3_control_state.session : request.session;
    reply.clock_epoch = switch_a3_control_state.clock_epoch;
    reply.applied_generation = switch_a3_control_state.applied_generation;
    reply.state = switch_a3_control_state.state;
    reply.pending_rx = switch_a3_rx_pending();
    if (!++switch_a3_control_state.state_revision)
        ++switch_a3_control_state.state_revision;
    reply.state_generation = switch_a3_control_state.state_generation;
    reply.state_revision = switch_a3_control_state.state_revision;
    reply.fault_status = switch_a3_control_state.fault_status;
    reply.legal_social_channels = 0;
    reply.home_chanspec = 0;
    switch_a3_control_copy(output, &reply, sizeof reply);
    return 0;
}

int32_t switch_a3_control_deadline(void *output, uint32_t output_length)
{
#if DEVELOPMENT || DEBUG
    if (!output)
        return -2;
    if (output_length < sizeof(struct switch_a3_deadline_snapshot))
        return -14;
    struct switch_a3_deadline_snapshot snapshot;
    switch_a3_timer_snapshot(&snapshot);
    switch_a3_control_copy(output, &snapshot, sizeof snapshot);
    return 0;
#else
    (void)output;
    (void)output_length;
    return -23;
#endif
}
