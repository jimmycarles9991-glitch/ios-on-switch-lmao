// SPDX-License-Identifier: BSD-2-Clause-Patent
int switch_replace_schedule(uint64_t anchor)
{
    swlan_chan_slot slot = {2437, 1048576};
    swlan_chan_seq sequence = {anchor, 1048576, 1, 2437, &slot};
    return switch_wlan_a3_radio_schedule(&sequence);
}

TEST_F(SwitchA3Radio, RunningScheduleReplacementPreservesCommittedResources)
{
    switch_ready();
    const auto before = switch_state();
    switch_firmware.controls.clear();
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().applied_generation, before.applied_generation);
    EXPECT_EQ(switch_send(), -EAGAIN);
    switch_drain();
    ASSERT_EQ(switch_firmware.controls.size(), 1u);
    EXPECT_EQ(switch_firmware.controls[0].operation, SWITCH_A3_INSTALL_SCHEDULE);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_GT(switch_state().applied_generation, before.applied_generation);
    EXPECT_EQ(switch_state().clock.epoch, before.clock.epoch);
    EXPECT_GE(switch_send(), 0);
}

TEST_F(SwitchA3Radio, BusyScheduleWaitsForOwnershipEventWithoutStopping)
{
    switch_ready();
    const auto before = switch_state();
    switch_firmware.controls.clear();
    switch_firmware.schedule_busy = 1;
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.controls.size(), 1u);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().applied_generation, before.applied_generation);
    EXPECT_EQ(switch_send(), -EAGAIN);
    switch_state_notice();
    switch_drain();
    ASSERT_EQ(switch_firmware.controls.size(), 2u);
    for (const auto &control : switch_firmware.controls)
        EXPECT_EQ(control.operation, SWITCH_A3_INSTALL_SCHEDULE);
    EXPECT_GT(switch_state().applied_generation, before.applied_generation);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, ScheduleOwnershipEventBeforeBusyReplyIsNotLost)
{
    switch_ready();
    switch_firmware.controls.clear();
    switch_firmware.schedule_busy = 1;
    bool signalled = false;
    switch_firmware.after_reply = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_INSTALL_SCHEDULE && !signalled) {
            signalled = true;
            switch_state_notice();
        }
    };
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    EXPECT_TRUE(signalled);
    ASSERT_EQ(switch_firmware.controls.size(), 2u);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_GT(switch_state().tx_credits, 0u);
}

TEST_F(SwitchA3Radio, IssuedScheduleCommitsBeforeCoalescedReplacement)
{
    switch_ready();
    switch_firmware.controls.clear();
    switch_firmware.schedules.clear();
    bool replaced = false;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_INSTALL_SCHEDULE && !replaced) {
            replaced = true;
            EXPECT_EQ(switch_replace_schedule(3000000), 0);
        }
    };
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.schedules.size(), 2u);
    EXPECT_EQ(switch_firmware.schedules[0].anchor_tsf, 2000000u);
    EXPECT_EQ(switch_firmware.schedules[1].anchor_tsf, 3000000u);
    EXPECT_EQ(switch_state().applied_generation, switch_firmware.controls.back().config_generation);
    EXPECT_TRUE(switch_state().ready);
}

void switch_schedule_fault_before_reply(bool issued)
{
    switch_ready();
    const uint32_t previous = switch_state().applied_generation;
    bool noticed = false, retired = false;
    switch_firmware.after_reply = [&](const switch_a3_control &request) {
        if (request.operation != SWITCH_A3_INSTALL_SCHEDULE || noticed)
            return;
        noticed = true;
        switch_firmware.state = SWITCH_A3_FAULTED;
        switch_firmware.fault = -1;
        switch_firmware.applied = issued ? request.config_generation : previous;
        switch_firmware.state_generation = switch_firmware.applied;
        switch_state_notice(-1);
        EXPECT_FALSE(switch_state().ready);
        EXPECT_EQ(switch_state().tx_credits, 0u);
        EXPECT_EQ(switch_send(), -EAGAIN);
    };
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (noticed && !retired) {
            EXPECT_FALSE(switch_state().ready);
            EXPECT_EQ(switch_state().tx_credits, 0u);
            EXPECT_EQ(switch_send(), -EAGAIN);
            retired = request.operation == SWITCH_A3_STOP;
        }
    };
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    EXPECT_TRUE(noticed);
    EXPECT_TRUE(retired);
    switch_firmware.after_reply = {};
    switch_firmware.during_control = {};
}

TEST_F(SwitchA3Radio, IssuedScheduleFaultBeforeReplyCannotRestoreReadiness)
{
    switch_schedule_fault_before_reply(true);
}

TEST_F(SwitchA3Radio, AppliedScheduleFaultBeforeReplyCannotRestoreReadiness)
{
    switch_schedule_fault_before_reply(false);
}

TEST_F(SwitchA3Radio, ScheduleReplacementKeepsSubmittedPacketOutcome)
{
    switch_ready();
    const int id = switch_send();
    ASSERT_GE(id, 0);
    switch_drain();
    const auto packet = switch_firmware.dma.back();
    switch_firmware.events.clear();
    switch_firmware.schedule_busy = 1;
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    for (const auto &event : switch_firmware.events)
        EXPECT_NE(event.type, SWLAN_AWDL_TXDONE);
    switch_complete(packet);
    switch_drain();
    unsigned completions = 0;
    for (const auto &event : switch_firmware.events)
        if (event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(id)) {
            ++completions;
            EXPECT_EQ(event.status, 0u);
        }
    EXPECT_EQ(completions, 1u);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_GT(switch_state().tx_credits, 0u);
}

TEST_F(SwitchA3Radio, InvalidLiveScheduleLeavesAppliedResourcesAndScheduleIntact)
{
    switch_ready();
    const auto before = switch_state();
    const int outstanding = switch_send();
    ASSERT_GT(outstanding, 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 1u);
    switch_firmware.controls.clear();
    switch_firmware.schedule_failure = -2;
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.controls.size(), 1u);
    EXPECT_EQ(switch_firmware.controls[0].operation, SWITCH_A3_INSTALL_SCHEDULE);
    EXPECT_EQ(switch_state().applied_generation, before.applied_generation);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().error, 0);
    EXPECT_EQ(switch_send(), -EAGAIN);
    switch_complete(switch_firmware.dma.front());
    switch_state_notice();
    switch_drain();
    unsigned completions = 0;
    for (const auto &event : switch_firmware.events)
        if (event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(outstanding)) {
            ++completions;
            EXPECT_EQ(event.status, 0u);
        }
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(switch_state().tx_credits, 0u);
    EXPECT_EQ(switch_send(), -EAGAIN);
    EXPECT_EQ(switch_firmware.controls.size(), 1u);
    switch_firmware.schedule_failure = 0;
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    EXPECT_EQ(switch_send(), -EAGAIN);
    switch_drain();
    EXPECT_GT(switch_state().applied_generation, before.applied_generation);
    EXPECT_GT(switch_state().tx_credits, 0u);
    EXPECT_GE(switch_send(), 0);
}

TEST_F(SwitchA3Radio, SynchronouslyRefusedScheduleSurvivesEarlierCommit)
{
    switch_ready();
    switch_firmware.controls.clear();
    bool refused = false;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        if (refused || control.operation != SWITCH_A3_INSTALL_SCHEDULE)
            return;
        const swlan_chan_slot slot = {2437, 0};
        const swlan_chan_seq invalid = {3000000, 1048576, 1, 2437, &slot};
        EXPECT_EQ(switch_wlan_a3_radio_schedule(&invalid), -EINVAL);
        refused = true;
    };
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    ASSERT_TRUE(refused);
    ASSERT_EQ(switch_firmware.controls.size(), 1u);
    const auto applied = switch_state().applied_generation;
    EXPECT_EQ(applied, switch_firmware.controls.back().config_generation);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().error, 0);
    EXPECT_EQ(switch_state().tx_credits, 0u);
    EXPECT_EQ(switch_send(), -EAGAIN);
    ASSERT_EQ(switch_replace_schedule(2000000), 0);
    switch_drain();
    EXPECT_GT(switch_state().applied_generation, applied);
    EXPECT_GT(switch_send(), 0);
}

TEST_F(SwitchA3Radio, StationParkAndResumeKeepTheCommittedSession)
{
    switch_ready();
    const auto before = switch_state();
    switch_firmware.controls.clear();
    switch_firmware.schedules.clear();
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    ASSERT_EQ(switch_firmware.schedules.size(), 1u);
    for (const auto &slot : switch_firmware.schedules[0].slots)
        EXPECT_EQ(slot.chanspec, 0u);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_send(), -EAGAIN);
    ASSERT_EQ(switch_replace_schedule(3000000), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.schedules.size(), 1u);
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    ASSERT_EQ(switch_firmware.schedules.size(), 2u);
    EXPECT_EQ(switch_firmware.schedules.back().anchor_tsf, 3000000u);
    for (const auto &control : switch_firmware.controls)
        EXPECT_EQ(control.operation, SWITCH_A3_INSTALL_SCHEDULE);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().generation, before.generation);
    EXPECT_EQ(switch_state().clock.epoch, before.clock.epoch);
    EXPECT_GE(switch_send(), 0);
}

TEST_F(SwitchA3Radio, SafelyRetiredAwdlFaultDoesNotPreventStationChanges)
{
    switch_ready();
    switch_firmware.state = SWITCH_A3_FAULTED;
    switch_firmware.fault = -1;
    switch_state_notice(-1);
    switch_drain();
    ASSERT_FALSE(switch_state().ready);
    ASSERT_EQ(switch_state().error, -EIO);
    ASSERT_EQ(switch_a3_host::gFirmwareOwnerGeneration, 0u);
    ASSERT_GT(switch_firmware.transport_idle_calls, 0u);
    switch_firmware.controls.clear();
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    EXPECT_EQ(switch_state().error, -EIO);
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_state().error, -EIO);
    EXPECT_TRUE(switch_firmware.controls.empty());
}

TEST_F(SwitchA3Radio, ScheduleChangeDuringRefusedDmaRetiresUnsubmittedWork)
{
    switch_ready();
    const int id = switch_send();
    ASSERT_GE(id, 0);
    bool replaced = false;
    switch_firmware.during_dma = [&] {
        if (!replaced) {
            replaced = true;
            EXPECT_EQ(switch_replace_schedule(2000000), 0);
        }
    };
    switch_drain();
    EXPECT_TRUE(replaced);
    EXPECT_TRUE(switch_firmware.dma.empty());
    unsigned cancelled = 0;
    for (const auto &event : switch_firmware.events)
        cancelled += event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(id) &&
                     event.status == ECANCELED;
    EXPECT_EQ(cancelled, 1u);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().tx_credits, 8u);
}

TEST_F(SwitchA3Radio, FailedInitialClockBurstWaitsForAnotherRequest)
{
    switch_attach();
    switch_firmware.delay_ns = 3000000;
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    ASSERT_EQ(switch_firmware.controls.size(), 3u);
    for (const auto &control : switch_firmware.controls)
        EXPECT_EQ(control.operation, SWITCH_A3_GET_CLOCK);
    switch_firmware.delay_ns = 100000;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, FaultStateDiagnosticRetainsFirstCauseWhenResourcesAreDown)
{
    switch_ready();
    for (unsigned index = 0; index < 7; ++index) {
        switch_test_now_ns += 30000000000ULL;
        ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
        switch_drain();
    }
    EXPECT_EQ(switch_a3_host::gDiagnostics.requests, 7u);
    switch_firmware.diagnostic_modify = [](struct switch_a3_diagnostic &snapshot) {
        snapshot.first_fault.sequence = 1;
        snapshot.first_fault.reason = SWITCH_A3_FAULT_TIMER_OWNER;
        snapshot.first_fault.session = snapshot.session;
        snapshot.first_fault.epoch = snapshot.clock_epoch;
        snapshot.latest_fault = snapshot.first_fault;
    };
    switch_firmware.state = SWITCH_A3_FAULTED;
    switch_firmware.fault = -1;
    switch_test_log.clear();
    switch_state_notice(-1);
    switch_drain();
    unsigned causes = 0;
    for (const auto &line : switch_test_log)
        causes += line.find("A3 diagnostic cause first:") != std::string::npos;
    EXPECT_EQ(causes, 1u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.requests, 8u);
    EXPECT_FALSE(switch_state().ready);
}
