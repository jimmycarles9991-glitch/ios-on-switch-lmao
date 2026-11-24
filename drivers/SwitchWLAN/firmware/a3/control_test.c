/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "control.h"
#include "rx.h"
#include "timer.h"
#include "wire.h"

static uint64_t switch_now;
static int32_t switch_clock_status, switch_ack_status, switch_timer_status;
static uint32_t switch_prepared, switch_ack_enabled, switch_rx_enabled, switch_cancelled, switch_pending;
static uint32_t switch_rx_session, switch_rx_epoch, switch_rx_generation;
static struct switch_a3_deadline_snapshot switch_deadline;
static struct switch_a3_reply switch_reply;
static uint64_t switch_input[32], switch_output[12];
static int32_t switch_transport;
uint32_t switch_control_test_results[14];
volatile uint32_t switch_control_test_done;

#define SWITCH_REQUIRE(condition) \
    do {                          \
        if (!(condition))         \
            return __LINE__;      \
    } while (0)

static void switch_copy(void *output, const void *input, uint32_t length)
{
    uint8_t *out = output;
    const uint8_t *in = input;
    for (uint32_t index = 0; index < length; ++index)
        out[index] = in[index];
}

int switch_a3_clock_read(void *wlc, uint64_t *tsf)
{
    if (wlc != (void *)0x120000)
        return -2;
    *tsf = switch_now;
    return switch_clock_status;
}

void switch_a3_ack_detached(void)
{}

int32_t switch_a3_ack_prepare(void *wlc)
{
    (void)wlc;
    ++switch_prepared;
    return switch_ack_status;
}

int32_t switch_a3_ack_configure(void *wlc, const uint8_t address[6], int enabled)
{
    (void)wlc;
    (void)address;
    switch_ack_enabled = enabled;
    return switch_ack_status;
}

int32_t switch_a3_ack_reconcile(void *wlc)
{
    (void)wlc;
    return switch_ack_status;
}

int switch_a3_rx_configure(void *wlc, const uint8_t address[6], int enabled, uint32_t session, uint32_t epoch,
                           uint32_t generation)
{
    (void)wlc;
    (void)address;
    switch_rx_enabled = enabled;
    switch_rx_session = session;
    switch_rx_epoch = epoch;
    switch_rx_generation = generation;
    return 0;
}

uint32_t switch_a3_rx_pending(void)
{
    return switch_pending;
}

int switch_a3_timer_cancel(void *wlc)
{
    (void)wlc;
    ++switch_cancelled;
    return switch_timer_status;
}

int switch_a3_timer_initialize(void *wlc)
{
    (void)wlc;
    return switch_timer_status;
}

int switch_a3_timer_arm(void *wlc, uint64_t deadline, uint32_t session, uint32_t epoch,
                        switch_a3_timer_callback callback, void *context)
{
    (void)wlc;
    (void)callback;
    (void)context;
    switch_deadline.requested_tsf = deadline;
    switch_deadline.session_generation = session;
    switch_deadline.clock_generation = epoch;
    switch_deadline.armed = 1;
    return 0;
}

void switch_a3_timer_snapshot(struct switch_a3_deadline_snapshot *snapshot)
{
    *snapshot = switch_deadline;
}

static void switch_reset(void)
{
    switch_clock_status = switch_ack_status = switch_timer_status = 0;
    switch_prepared = switch_ack_enabled = switch_rx_enabled = switch_cancelled = switch_pending = 0;
    switch_now = 1000;
    switch_a3_control_initialize((void *)0x120000);
}

static struct switch_a3_configuration switch_configuration(void)
{
    static const struct switch_a3_configuration configuration = {
        {2, 3, 4, 5, 6, 7}, {0, 0x25, 0, 0xff, 0x94, 0x73}, 0, 0x1006, SWITCH_A3_CONFIG_RX | SWITCH_A3_CONFIG_ACK,
    };
    return configuration;
}

static void switch_request(uint32_t operation, const void *payload, uint32_t length, uint32_t session, uint32_t epoch,
                           uint32_t generation)
{
    struct switch_a3_control *request = (void *)switch_input;
    request->magic = SWITCH_A3_CONTROL_MAGIC;
    request->version = SWITCH_A3_WIRE_VERSION;
    request->header_bytes = sizeof *request;
    request->total_bytes = sizeof *request + length;
    request->request_id = 71;
    request->session = session;
    request->clock_epoch = epoch;
    request->config_generation = generation;
    request->operation = operation;
    if (length)
        switch_copy((uint8_t *)switch_input + sizeof *request, payload, length);
    switch_output[0] = switch_output[1 + sizeof switch_reply / sizeof *switch_output] = UINT64_C(0xa5a5a5a5a5a5a5a5);
    switch_transport =
        switch_a3_control_request(switch_input, sizeof *request + length, switch_output + 1, sizeof switch_reply);
    switch_copy(&switch_reply, switch_output + 1, sizeof switch_reply);
}

static void switch_configure(void)
{
    struct switch_a3_configuration configuration = switch_configuration();
    switch_request(SWITCH_A3_CONFIGURE, &configuration, sizeof configuration, 7, 1, 9);
}

static uint32_t switch_test_initial(void)
{
    switch_reset();
    switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 0, 0, 0);
    SWITCH_REQUIRE(!switch_transport && !switch_reply.status && !switch_reply.session);
    SWITCH_REQUIRE(switch_reply.clock_epoch == 1 && switch_reply.firmware_generation == 1 && switch_reply.tsf == 1000);
    SWITCH_REQUIRE(!switch_reply.features && !switch_reply.tx_credits && !switch_reply.reserved);
    SWITCH_REQUIRE(switch_reply.magic == SWITCH_A3_REPLY_MAGIC && switch_reply.request_id == 71);
    SWITCH_REQUIRE(switch_reply.header_bytes == sizeof switch_reply && switch_reply.total_bytes == sizeof switch_reply);
    switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 42, 0, 0);
    SWITCH_REQUIRE(!switch_reply.status && switch_reply.session == 42);
    SWITCH_REQUIRE(!switch_a3_control_request(switch_input, sizeof switch_reply, switch_output, sizeof switch_reply));
    SWITCH_REQUIRE(((struct switch_a3_reply *)switch_output)->session == 42);
    switch_request(SWITCH_A3_GET_STATE, 0, 0, 43, 0, 0);
    SWITCH_REQUIRE(!switch_reply.status && switch_reply.session == 43 && !switch_reply.applied_generation);
    return 0;
}

static uint32_t switch_test_configure(void)
{
    switch_reset();
    switch_configure();
    SWITCH_REQUIRE(!switch_transport && !switch_reply.status && switch_prepared == 1);
    SWITCH_REQUIRE(switch_reply.session == 7 && switch_reply.applied_generation == 9);
    SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_STOPPED && !switch_reply.features);
    SWITCH_REQUIRE(!switch_ack_enabled && !switch_rx_enabled);
    SWITCH_REQUIRE(switch_output[0] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    SWITCH_REQUIRE(switch_output[1 + sizeof switch_reply / sizeof *switch_output] == UINT64_C(0xa5a5a5a5a5a5a5a5));
    return 0;
}

static uint32_t switch_test_invalid_configuration(void)
{
    for (uint32_t variant = 0; variant < 6; ++variant) {
        switch_reset();
        struct switch_a3_configuration configuration = switch_configuration();
        if (variant == 0)
            configuration.address[0] = 1;
        if (variant == 1)
            configuration.bssid[0] = 1;
        if (variant == 2)
            configuration.flags = 8;
        if (variant == 3)
            configuration.chanspec = 0;
        if (variant == 4)
            configuration.ethertype = 0x86dd;
        if (variant == 5)
            for (uint32_t index = 0; index < 6; ++index)
                configuration.address[index] = 0;
        switch_request(SWITCH_A3_CONFIGURE, &configuration, sizeof configuration, 7, 1, 9);
        SWITCH_REQUIRE(switch_reply.status && !switch_prepared && !switch_reply.applied_generation);
    }
    return 0;
}

static uint32_t switch_test_generation(void)
{
    switch_reset();
    switch_configure();
    switch_configure();
    SWITCH_REQUIRE(!switch_reply.status && switch_prepared == 1);
    struct switch_a3_configuration configuration = switch_configuration();
    configuration.address[5] = 99;
    switch_request(SWITCH_A3_CONFIGURE, &configuration, sizeof configuration, 7, 1, 9);
    SWITCH_REQUIRE(switch_reply.status == -2 && switch_prepared == 1);
    switch_request(SWITCH_A3_CONFIGURE, &configuration, sizeof configuration, 7, 1, 10);
    SWITCH_REQUIRE(!switch_reply.status && switch_reply.applied_generation == 10 && switch_prepared == 2);
    return 0;
}

static uint32_t switch_test_stale(void)
{
    switch_reset();
    switch_configure();
    switch_request(SWITCH_A3_GET_STATE, 0, 0, 6, 0, 9);
    SWITCH_REQUIRE(switch_reply.status == -2 && switch_reply.session == 7);
    switch_request(SWITCH_A3_START, 0, 0, 7, 2, 9);
    SWITCH_REQUIRE(switch_reply.status == -35);
    switch_request(SWITCH_A3_START, 0, 0, 7, 1, 8);
    SWITCH_REQUIRE(switch_reply.status == -25);
    return 0;
}

static uint32_t switch_test_unsupported(void)
{
    switch_reset();
    switch_configure();
    for (uint32_t operation = SWITCH_A3_INSTALL_SCHEDULE; operation <= SWITCH_A3_START; ++operation) {
        switch_request(operation, 0, 0, 7, 1, 9);
        SWITCH_REQUIRE(switch_reply.status == -23 && !switch_reply.features && switch_reply.state == SWITCH_A3_STOPPED);
    }
    return 0;
}

static uint32_t switch_test_receive(void)
{
    switch_reset();
    switch_configure();
    uint32_t enabled = 1;
    switch_request(SWITCH_A3_TEST_RECEIVE, &enabled, sizeof enabled, 7, 1, 9);
#if DEVELOPMENT || DEBUG
    SWITCH_REQUIRE(!switch_reply.status && switch_ack_enabled && switch_rx_enabled);
    SWITCH_REQUIRE(switch_rx_session == 7 && switch_rx_epoch == 1 && switch_rx_generation == 9);
    SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_CONFIGURING && !switch_reply.features);
#else
    SWITCH_REQUIRE(switch_reply.status == -23 && !switch_ack_enabled && !switch_rx_enabled);
#endif
    switch_pending = 2;
    switch_request(SWITCH_A3_STOP, 0, 0, 7, 0, 9);
    SWITCH_REQUIRE(!switch_reply.status && !switch_ack_enabled && !switch_rx_enabled);
    SWITCH_REQUIRE(switch_reply.pending_rx == 2 && switch_reply.state == SWITCH_A3_STOPPED);
    return 0;
}

static uint32_t switch_test_deadline(void)
{
    switch_reset();
    switch_configure();
    const uint64_t deadline = 5000;
    switch_request(SWITCH_A3_TEST_DEADLINE, &deadline, sizeof deadline, 7, 1, 9);
#if DEVELOPMENT || DEBUG
    SWITCH_REQUIRE(!switch_reply.status && switch_deadline.requested_tsf == deadline);
    SWITCH_REQUIRE(switch_deadline.session_generation == 7 && switch_deadline.clock_generation == 1);
    SWITCH_REQUIRE(!switch_a3_control_deadline(switch_output, 40));
    SWITCH_REQUIRE(switch_a3_control_deadline(switch_output, 39) == -14);
#else
    SWITCH_REQUIRE(switch_reply.status == -23);
    SWITCH_REQUIRE(switch_a3_control_deadline(switch_output, 40) == -23);
#endif
    return 0;
}

static uint32_t switch_test_reset_clock(void)
{
    switch_reset();
    switch_configure();
    switch_request(SWITCH_A3_RESET_CLOCK, 0, 0, 7, 0, 9);
    SWITCH_REQUIRE(!switch_reply.status && switch_reply.clock_epoch == 2 && !switch_reply.applied_generation);
    switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 7, 0, 0);
    SWITCH_REQUIRE(!switch_reply.status && switch_reply.clock_epoch == 2 && switch_reply.session == 7);
    switch_request(SWITCH_A3_START, 0, 0, 7, 1, 9);
    SWITCH_REQUIRE(switch_reply.status == -35);
    return 0;
}

static uint32_t switch_test_lifecycle(void)
{
    switch_reset();
    switch_configure();
    switch_a3_control_down();
    switch_a3_control_up();
    switch_request(SWITCH_A3_GET_STATE, 0, 0, 7, 0, 0);
    SWITCH_REQUIRE(!switch_reply.status && switch_reply.clock_epoch == 2 && !switch_reply.applied_generation);
    SWITCH_REQUIRE(!switch_rx_enabled && !switch_ack_enabled);
    switch_request(SWITCH_A3_STOP, 0, 0, 7, 0, 0);
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2);
    return 0;
}

static uint32_t switch_test_clock_fault(void)
{
    switch_reset();
    switch_configure();
    switch_now = 900;
    switch_request(SWITCH_A3_GET_STATE, 0, 0, 7, 0, 0);
    SWITCH_REQUIRE(switch_reply.clock_epoch == 1 && switch_reply.applied_generation == 9);
    switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 7, 0, 0);
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2 && switch_reply.state == SWITCH_A3_FAULTED);
    SWITCH_REQUIRE(!switch_reply.applied_generation && !switch_rx_enabled && !switch_ack_enabled);
    switch_clock_status = -35;
    switch_request(SWITCH_A3_STOP, 0, 0, 7, 0, 0);
    SWITCH_REQUIRE(!switch_reply.status && switch_reply.fault_status == -35 && !switch_rx_enabled &&
                   !switch_ack_enabled);
    return 0;
}

static uint32_t switch_test_failures(void)
{
    switch_reset();
    switch_ack_status = -27;
    switch_configure();
    SWITCH_REQUIRE(switch_reply.status == -27 && !switch_reply.applied_generation);
    switch_ack_status = 0;
    switch_configure();
    switch_timer_status = -1;
    switch_request(SWITCH_A3_STOP, 0, 0, 7, 0, 0);
    SWITCH_REQUIRE(switch_reply.status == -1 && switch_reply.state == SWITCH_A3_FAULTED);
    return 0;
}

static uint32_t switch_test_bounds(void)
{
    switch_reset();
    switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 0, 0, 0);
    SWITCH_REQUIRE(switch_a3_control_request(0, 32, switch_output, 64) == -2);
    SWITCH_REQUIRE(switch_a3_control_request(switch_input, 31, switch_output, sizeof switch_reply) == -14);
    SWITCH_REQUIRE(switch_a3_control_request(switch_input, 32, switch_output, sizeof switch_reply - 1) == -14);
    for (uint32_t variant = 0; variant < 5; ++variant) {
        switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 0, 0, 0);
        struct switch_a3_control *request = (void *)switch_input;
        if (variant == 0)
            request->magic ^= 1;
        if (variant == 1)
            request->version = SWITCH_A3_WIRE_VERSION + 1;
        if (variant == 2)
            request->header_bytes = 31;
        if (variant == 3)
            request->total_bytes = 33;
        if (variant == 4)
            request->request_id = 0;
        SWITCH_REQUIRE(switch_a3_control_request(switch_input, 32, switch_output, sizeof switch_reply) == -2);
    }
    return 0;
}

static uint32_t switch_test_alias(void)
{
    switch_reset();
    switch_configure();
    SWITCH_REQUIRE(!switch_a3_control_request(switch_input, sizeof switch_reply, switch_input, sizeof switch_reply));
    struct switch_a3_reply *reply = (void *)switch_input;
    SWITCH_REQUIRE(!reply->status && reply->request_id == 71 && reply->applied_generation == 9);
    return 0;
}

void switch_control_test_run(void)
{
    switch_control_test_results[0] = switch_test_initial();
    switch_control_test_results[1] = switch_test_configure();
    switch_control_test_results[2] = switch_test_invalid_configuration();
    switch_control_test_results[3] = switch_test_generation();
    switch_control_test_results[4] = switch_test_stale();
    switch_control_test_results[5] = switch_test_unsupported();
    switch_control_test_results[6] = switch_test_receive();
    switch_control_test_results[7] = switch_test_deadline();
    switch_control_test_results[8] = switch_test_reset_clock();
    switch_control_test_results[9] = switch_test_lifecycle();
    switch_control_test_results[10] = switch_test_clock_fault();
    switch_control_test_results[11] = switch_test_failures();
    switch_control_test_results[12] = switch_test_bounds();
    switch_control_test_results[13] = switch_test_alias();
    switch_control_test_done = 0x53574133;
}
