/* SPDX-License-Identifier: BSD-2-Clause-Patent */
static uint32_t switch_test_transient_clock_sample(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    SWITCH_REQUIRE(!switch_install(switch_schedule()) && !switch_bind());
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_START, 10, 1, 0, 0));
    const uint32_t ack_configures = switch_ack_configures, tx_configures = switch_tx_configures;
    const uint32_t arms = switch_timer_arms;
    switch_clock_failure = -35;
    SWITCH_REQUIRE(switch_request(SWITCH_A3_GET_CLOCK, 10, 1, 0, 0) == -35);
    SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_RUNNING && switch_reply.clock_epoch == 1 &&
                   switch_reply.applied_generation == 10 && !switch_reply.fault_status &&
                   switch_ack && switch_rx && switch_tx && switch_rx_generation == 10 && switch_tx_generation == 10);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(!switch_a3_diagnostic.first_fault.sequence && !switch_a3_diagnostic.latest_fault.sequence));
    switch_clock_failure = 0;
    switch_now += 10;
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_CLOCK, 10, 1, 0, 0));
    SWITCH_REQUIRE(switch_reply.state == SWITCH_A3_RUNNING && switch_reply.clock_epoch == 1 &&
                   switch_reply.applied_generation == 10 && !switch_reply.fault_status &&
                   switch_reply.tsf == switch_now && switch_ack_configures == ack_configures &&
                   switch_tx_configures == tx_configures && switch_timer_arms == arms);
    return 0;
}

static uint32_t switch_test_retired_excursion_identity(unsigned backward)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    switch_allow_away = switch_accept_excursion = 1;
    struct switch_a3_schedule schedule = switch_schedule();
    schedule.slots[0].chanspec = 0xd02c;
    SWITCH_REQUIRE(!switch_install(schedule) && !switch_bind());
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_START, 10, 1, 0, 0));
    SWITCH_REQUIRE(switch_excursion_active && switch_excursion_callback);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_RESET_CLOCK, 10, 1, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2 && !switch_reply.applied_generation);
    if (backward)
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_CLOCK, 0, 0, 0, 0));
    const struct switch_a3_excursion_result result = {
        .returned_tsf = switch_now + 37,
        .session = switch_excursion_request.session,
        .clock_epoch = switch_excursion_request.clock_epoch,
        .generation = switch_excursion_request.generation,
        .reason = backward ? SWITCH_A3_EXCURSION_CLOCK_BACKWARD : SWITCH_A3_EXCURSION_RETURN_FAILED,
        .native_status = 4,
    };
    switch_excursion_active = 0;
    switch_excursion_callback(switch_excursion_context, SWITCH_A3_EXCURSION_COMPLETED, &result);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_GET_STATE, 0, 0, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2 && switch_reply.state == SWITCH_A3_FAULTED);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.first_fault.reason == SWITCH_A3_FAULT_CLOCK_RESET &&
                       switch_a3_diagnostic.latest_fault.reason ==
                           (backward ? SWITCH_A3_FAULT_CLOCK_BACKWARD : SWITCH_A3_FAULT_CHANNEL_RETURN) &&
                       switch_a3_diagnostic.latest_fault.session == 7 &&
                       switch_a3_diagnostic.latest_fault.epoch == 1 &&
                       switch_a3_diagnostic.latest_fault.config == 10));
    return 0;
}

static uint32_t switch_test_new_epoch_arm_identity(void)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    const struct switch_a3_configuration configuration =
        *(const struct switch_a3_configuration *)((const uint8_t *)switch_input + sizeof(struct switch_a3_control));
    struct switch_a3_schedule schedule = switch_schedule();
    SWITCH_REQUIRE(!switch_install(schedule) && !switch_bind());
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_START, 10, 1, 0, 0));
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_RESET_CLOCK, 10, 1, 0, 0));
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2);
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_CONFIGURE, 11, 2, &configuration, sizeof configuration));
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_INSTALL_SCHEDULE, 12, 2, &schedule, sizeof schedule));
    const struct switch_a3_ring_binding rings = {3, 4, 1800, 0, 4, 0};
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_BIND_RINGS, 12, 2, &rings, sizeof rings));
    switch_timer_failure = -1;
    SWITCH_REQUIRE(switch_request(SWITCH_A3_START, 12, 2, 0, 0) == -1);
    SWITCH_REQUIRE(switch_reply.clock_epoch == 2 && switch_reply.state == SWITCH_A3_FAULTED &&
                   !switch_reply.applied_generation && !switch_rx && !switch_tx);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.first_fault.reason == SWITCH_A3_FAULT_CLOCK_RESET &&
                       switch_a3_diagnostic.latest_fault.reason == SWITCH_A3_FAULT_CLOCK_UNSTABLE &&
                       switch_a3_diagnostic.latest_fault.session == 7 &&
                       switch_a3_diagnostic.latest_fault.epoch == 2 &&
                       switch_a3_diagnostic.latest_fault.config == 12 &&
                       !switch_a3_diagnostic.latest_fault.observed_tsf &&
                       !switch_a3_diagnostic.latest_fault.deadline_tsf &&
                       !switch_a3_diagnostic.latest_fault.actual_tsf));
    return 0;
}

static uint32_t switch_test_live_ack_loss(unsigned resume)
{
    switch_reset();
    SWITCH_REQUIRE(!switch_configure());
    struct switch_a3_schedule schedule = switch_schedule();
    SWITCH_REQUIRE(!switch_install(schedule) && !switch_bind());
    SWITCH_REQUIRE(!switch_request(SWITCH_A3_START, 10, 1, 0, 0));
    uint32_t generation = 11;
    schedule.slots[0].chanspec = 0;
    if (resume) {
        const uint32_t reconciles = switch_ack_reconciles;
        SWITCH_REQUIRE(!switch_request(SWITCH_A3_INSTALL_SCHEDULE, generation++, 1, &schedule, sizeof schedule));
        SWITCH_REQUIRE(switch_ack_reconciles == reconciles + 1 && switch_ack && switch_rx && switch_tx);
        schedule.slots[0].chanspec = 0x1006;
    }
    const uint32_t configured = switch_tx_configures;
    switch_ack_failure = -1;
    SWITCH_REQUIRE(switch_request(SWITCH_A3_INSTALL_SCHEDULE, generation, 1, &schedule, sizeof schedule) == -1);
    SWITCH_REQUIRE(switch_reply.clock_epoch == 1 && switch_reply.state == SWITCH_A3_FAULTED &&
                   !switch_reply.applied_generation && !switch_reply.tx_credits && !switch_ack && !switch_rx &&
                   !switch_tx && switch_tx_configures == configured);
    SWITCH_A3_DIAG(
        SWITCH_REQUIRE(switch_a3_diagnostic.first_fault.reason == SWITCH_A3_FAULT_ACK &&
                       switch_a3_diagnostic.first_fault.session == 7 &&
                       switch_a3_diagnostic.first_fault.epoch == 1 &&
                       switch_a3_diagnostic.first_fault.config == generation));
    return 0;
}
