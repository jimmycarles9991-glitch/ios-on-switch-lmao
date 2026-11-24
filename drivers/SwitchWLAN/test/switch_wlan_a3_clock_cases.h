/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_WLAN_A3_CLOCK_CASES_H
#define SWITCH_WLAN_A3_CLOCK_CASES_H

static unsigned switch_clock_resets()
{
    unsigned resets = 0;
    for (const auto &request : switch_firmware.controls)
        resets += request.operation == SWITCH_A3_RESET_CLOCK;
    return resets;
}

TEST_F(SwitchA3Radio, SingleFastClockOutlierDoesNotReplaceTheAnchorOrResetFirmware)
{
    switch_ready();
    const auto previous = switch_state().clock;
    const uint64_t offset = switch_firmware.tsf_offset;
    switch_firmware.controls.clear();
    unsigned samples = 0;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_GET_CLOCK) {
            const bool outlier = ++samples == 1;
            switch_firmware.tsf_offset = offset + (outlier ? 10000 : 0);
            switch_firmware.delay_ns = outlier ? 50000 : 100000;
        }
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(samples, 3u);
    EXPECT_EQ(switch_clock_resets(), 0u);
    const auto current = switch_state();
    EXPECT_TRUE(current.ready);
    EXPECT_EQ(current.clock.epoch, previous.epoch);
    EXPECT_EQ(current.clock.tsf_us - current.clock.host_ns / 1000, offset);
}

TEST_F(SwitchA3Radio, TwoCoherentClockObservationsConfirmAJumpAfterANormalSample)
{
    switch_ready();
    const auto previous = switch_state().clock;
    const uint64_t offset = switch_firmware.tsf_offset;
    switch_firmware.controls.clear();
    unsigned samples = 0;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_GET_CLOCK)
            switch_firmware.tsf_offset = offset + (++samples == 1 ? 0 : 10000);
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_clock_resets(), 1u);
    EXPECT_EQ(switch_state().clock.epoch, previous.epoch + 1);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, ContradictoryClockOutliersKeepTheValidAnchor)
{
    switch_ready();
    const auto previous = switch_state().clock;
    const uint64_t offset = switch_firmware.tsf_offset;
    switch_firmware.controls.clear();
    unsigned samples = 0;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation != SWITCH_A3_GET_CLOCK)
            return;
        ++samples;
        switch_firmware.tsf_offset = samples == 2 ? offset - 10000 : offset + samples * 10000;
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(samples, 3u);
    EXPECT_EQ(switch_clock_resets(), 0u);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().clock.host_ns, previous.host_ns);
}

TEST_F(SwitchA3Radio, OneOutlierAndTwoUnusableRoundTripsCannotResetTheClock)
{
    switch_ready();
    const auto previous = switch_state().clock;
    switch_firmware.controls.clear();
    switch_firmware.tsf_offset += 10000;
    unsigned samples = 0;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_GET_CLOCK)
            switch_firmware.delay_ns = ++samples == 1 ? 100000 : 3000000;
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(samples, 3u);
    EXPECT_EQ(switch_clock_resets(), 0u);
    EXPECT_EQ(switch_state().clock.host_ns, previous.host_ns);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, ClockComparisonIncludesOldAnchorAgeAndBothUncertainties)
{
    switch_ready();
    const auto previous = switch_state().clock;
    switch_firmware.controls.clear();
    switch_test_now_ns += 5000000000ULL;
    switch_firmware.delay_ns = 1800000;
    switch_firmware.tsf_offset += 3200;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_clock_resets(), 0u);
    EXPECT_EQ(switch_state().clock.epoch, previous.epoch);
    EXPECT_GT(switch_state().clock.host_ns, previous.host_ns);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, ConfirmedClockJumpCannotResetAfterItsRevisionWasSuperseded)
{
    switch_ready();
    const auto previous = switch_state().clock;
    switch_firmware.controls.clear();
    switch_firmware.tsf_offset += 10000;
    SwitchWLANA3Job job = {};
    job.control = {SWITCH_A3_CONTROL_MAGIC, SWITCH_A3_WIRE_VERSION, sizeof(switch_a3_control),
                   sizeof(switch_a3_control), 100000, switch_state().generation, 0,
                   switch_state().applied_generation, SWITCH_A3_GET_CLOCK};
    swlan_awdl_clock_sample sample = {};
    switch_a3_reply reply = {};
    const auto current = [](uint32_t, uint32_t, uint32_t revision) { return revision == 0; };
    EXPECT_EQ(switch_wlan_a3_calibrate(job, previous, sample, reply, current), -ESTALE);
    EXPECT_EQ(switch_clock_resets(), 0u);
    EXPECT_EQ(switch_state().clock.epoch, previous.epoch);
}

#endif
