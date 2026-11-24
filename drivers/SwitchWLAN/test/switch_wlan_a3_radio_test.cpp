/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "switch_wlan_a3_radio_stubs.h"
#include "../SwitchWLANA3Radio.h"
#include "../SwitchWLANA3Probe.h"
#include "../SwitchWLANA3TxProbe.h"
#include "../SwitchWLANA3Wire.h"
#include "../SwitchWLANA3State.h"
#include "../awdl/awdl.h"
#include "../awdl/awdl_radio.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>
#include "bazel/testing/fresh_process.h"

namespace {
struct Firmware {
    uint32_t epoch = 1, applied = 0, state = SWITCH_A3_STOPPED, credits = 8;
    uint32_t revision = 0, state_generation = 0;
    uint32_t features = SWITCH_A3_FEATURE_REQUIRED;
    uint16_t legal_social_channels = 7;
    int fault = 0;
    int failure = 0, bind_failure = 0, receive_failure = 0, stop_failure = 0, dma_refusals = 0;
    int transport_idle = 1, schedule_busy = 0, schedule_failure = 0, clock_failure = 0;
    uint32_t bound_generation = 0;
    uint64_t delay_ns = 100000, tsf_offset = 1000000;
    unsigned clock_calls = 0, received = 0, transport_idle_calls = 0;
    unsigned diagnostic_calls = 0;
    size_t diagnostic_bytes = sizeof(struct switch_a3_diagnostic);
    std::function<void(struct switch_a3_diagnostic &)> diagnostic_modify;
    std::vector<uint8_t> received_frame;
    swlan_rx_meta received_meta = {};
    bool signal_dma_inside = false;
    bool bad_snapshot = false;
    unsigned deadline_reads = 0;
    switch_a3_deadline_snapshot snapshot = {};
    std::function<void(const switch_a3_control &)> during_control;
    std::function<void(const switch_a3_control &)> after_reply;
    std::function<void()> during_dma;
    std::vector<switch_a3_control> controls;
    std::vector<switch_a3_ring_binding> bindings;
    std::vector<switch_a3_schedule> schedules;
    std::vector<std::vector<uint8_t>> dma;
    std::vector<swlan_awdl_event> events;
} switch_firmware;

switch_a3_caps switch_caps()
{
    switch_a3_caps caps = {};
    caps.magic = SWITCH_A3_CAPS_MAGIC;
    caps.version = SWITCH_A3_CAPS_VERSION;
    caps.size = sizeof caps;
    caps.status = SWITCH_A3_ATTACHED;
    caps.features = switch_firmware.features;
    return caps;
}

void switch_attach(unsigned ring_count = 4)
{
    const auto caps = switch_caps();
    const uint8_t address[6] = {2, 3, 4, 5, 6, 7};
    switch_wlan_a3_radio_attach(&caps, address, 1, 3, ring_count);
}

void switch_drain()
{
    unsigned turns = 0;
    while (switch_test_run_one())
        ASSERT_LT(++turns, 100u);
}

swlan_awdl_state switch_state()
{
    swlan_awdl_state state = {};
    EXPECT_EQ(switch_wlan_a3_radio_state(&state), 0);
    return state;
}

int switch_filter()
{
    const uint8_t address[6] = {2, 10, 11, 12, 13, 14};
    return switch_wlan_a3_radio_filter(address, awdl_bssid, 0);
}

int switch_refilter()
{
    const uint8_t address[6] = {2, 10, 11, 12, 13, 15};
    return switch_wlan_a3_radio_filter(address, awdl_bssid, 0);
}

int switch_schedule(bool empty = false, uint64_t anchor = 1000000)
{
    swlan_chan_slot slots[AWDL_SEQ_SLOTS];
    for (auto &slot : slots)
        slot = {static_cast<uint16_t>(empty ? 0 : 2437), 65536};
    const swlan_chan_seq seq = {anchor, 1048576, AWDL_SEQ_SLOTS, 2437, slots};
    return switch_wlan_a3_radio_schedule(&seq);
}

void switch_ready()
{
    switch_attach();
    switch_drain();
    const bool was_ready = switch_state().ready;
    switch_test_core_lock = true;
    EXPECT_EQ(switch_filter(), 0);
    EXPECT_EQ(switch_schedule(), 0);
    EXPECT_EQ(switch_state().ready != 0, was_ready);
    switch_test_core_lock = false;
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
}

int switch_send(bool ack = false)
{
    uint8_t frame[64] = {0x08, 0};
    frame[4] = ack ? 2 : 1;
    memcpy(frame + 16, awdl_bssid, 6);
    const auto state = switch_state();
    swlan_tx_req request = {};
    request.frame = frame;
    request.len = sizeof frame;
    request.freq_mhz = 2437;
    request.rate_mcs = 0xff;
    request.flags = ack ? 0 : SWLAN_TX_NO_ACK;
    request.deadline_tsf = state.clock.tsf_us + 65536;
    request.generation = state.generation;
    request.clock_epoch = state.clock.epoch;
    return switch_wlan_a3_radio_tx(&request);
}

void switch_complete(const std::vector<uint8_t> &packet, uint32_t flags = 0, int32_t outcome = SWITCH_A3_TX_SENT)
{
    switch_a3_tx_header tx;
    memcpy(&tx, packet.data() + 14, sizeof tx);
    switch_a3_tx_status status = {};
    status.magic = SWITCH_A3_TX_STATUS_MAGIC;
    status.version = SWITCH_A3_WIRE_VERSION;
    status.header_bytes = sizeof status;
    status.total_bytes = sizeof status;
    status.session = tx.session;
    status.clock_epoch = tx.clock_epoch;
    status.schedule_generation = tx.schedule_generation;
    status.request_id = tx.request_id;
    status.status = outcome;
    status.flags = flags;
    EXPECT_TRUE(switch_wlan_a3_radio_frame(reinterpret_cast<const uint8_t *>(&status), sizeof status));
}

void switch_receive(void *, const uint8_t *frame, size_t length, const swlan_rx_meta *meta)
{
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    ++switch_firmware.received;
    switch_firmware.received_frame.assign(frame, frame + length);
    switch_firmware.received_meta = *meta;
}

void switch_state_notice(int status = 0, uint32_t session = 0, uint32_t epoch = 0, uint32_t applied = 0,
                         uint32_t revision = 0)
{
    switch_a3_reply reply = {};
    reply.magic = SWITCH_A3_STATE_MAGIC;
    reply.version = SWITCH_A3_WIRE_VERSION;
    reply.header_bytes = sizeof reply;
    reply.total_bytes = sizeof reply;
    reply.session = session ? session : switch_state().generation;
    reply.clock_epoch = epoch ? epoch : switch_firmware.epoch;
    reply.applied_generation = applied ? applied : switch_firmware.applied;
    reply.state_generation = reply.applied_generation ? reply.applied_generation : switch_firmware.state_generation;
    reply.state_revision = revision ? revision : ++switch_firmware.revision;
    reply.state = switch_firmware.state;
    reply.fault_status = status;
    reply.features = switch_firmware.features;
    reply.tx_credits = switch_firmware.credits;
    reply.firmware_generation = 1;
    reply.legal_social_channels = switch_firmware.legal_social_channels;
    EXPECT_TRUE(switch_wlan_a3_radio_frame(reinterpret_cast<const uint8_t *>(&reply), sizeof reply));
}

class SwitchA3Radio : public ::testing::Test {
    void SetUp() override
    {
        switch_firmware.during_control = {};
        switch_firmware.after_reply = {};
        switch_firmware.during_dma = {};
        switch_test_sleep_action = {};
        switch_wlan_a3_radio_close();
        switch_drain();
        switch_wlan_a3_radio_lost();
        switch_drain();
        switch_firmware = {};
        switch_test_sleep_action = {};
        switch_test_now_ns += 1000000000ULL;
        switch_test_core_lock = false;
    }
};

TEST_F(SwitchA3Radio, ClockPrecedesConfigureAndCoreCallsNeverEnterDriverGate)
{
    switch_attach();
    switch_test_core_lock = true;
    EXPECT_EQ(switch_filter(), 0);
    EXPECT_EQ(switch_wlan_a3_radio_clock(), 0);
    EXPECT_EQ(switch_schedule(true), 0);
    switch_test_core_lock = false;
    switch_drain();
    unsigned clocks = 0;
    for (const auto &control : switch_firmware.controls) {
        clocks += control.operation == SWITCH_A3_GET_CLOCK;
        if (control.operation == SWITCH_A3_CONFIGURE)
            EXPECT_EQ(clocks, 3u);
    }
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().clock.epoch, 1u);
}

TEST_F(SwitchA3Radio, OldControlReplyCannotMutateReattachedState)
{
    switch_attach();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    const auto old = switch_state().generation;
    bool replaced = false;
    switch_firmware.during_control = [&](const switch_a3_control &) {
        if (replaced)
            return;
        replaced = true;
        switch_wlan_a3_radio_lost();
        switch_attach();
        EXPECT_EQ(switch_wlan_a3_radio_clock(), 0);
    };
    switch_drain();
    const auto state = switch_state();
    EXPECT_NE(state.generation, old);
    EXPECT_FALSE(state.error);
    EXPECT_EQ(state.clock.epoch, 1u);
    for (const auto &event : switch_firmware.events)
        if (event.type == SWLAN_AWDL_CLOCK_CHANGED)
            EXPECT_EQ(event.generation, state.generation);
}

TEST_F(SwitchA3Radio, FailedConfigureCannotStartAndFailureSurvivesClockQueries)
{
    switch_attach();
    switch_drain();
    switch_firmware.failure = -2;
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_state().error, -EINVAL);
    for (const auto &control : switch_firmware.controls)
        EXPECT_NE(control.operation, SWITCH_A3_START);
    switch_firmware.failure = 0;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_state().error, -EINVAL);
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, ScheduleBindingAndStartAreAtomicAtQueueLimit)
{
    switch_attach();
    switch_drain();
    for (unsigned i = 0; i < 20; ++i) {
        ASSERT_EQ(switch_filter(), 0);
        ASSERT_EQ(switch_schedule(i % 2), 0);
    }
    switch_drain();
    ASSERT_EQ(switch_firmware.schedules.size(), 1u);
    EXPECT_EQ(switch_firmware.schedules[0].slots[0].chanspec, 0u);
    EXPECT_TRUE(switch_state().ready);
    ASSERT_EQ(switch_firmware.bindings.size(), 1u);
    const auto &commands = switch_firmware.controls;
    EXPECT_EQ(commands[commands.size() - 3].operation, SWITCH_A3_INSTALL_SCHEDULE);
    EXPECT_EQ(commands[commands.size() - 2].operation, SWITCH_A3_BIND_RINGS);
    EXPECT_EQ(commands.back().operation, SWITCH_A3_START);
}

TEST_F(SwitchA3Radio, BindingUsesActualRingRangeBeforeStart)
{
    switch_ready();
    ASSERT_EQ(switch_firmware.bindings.size(), 1u);
    const auto &binding = switch_firmware.bindings[0];
    EXPECT_EQ(binding.first_ring, 3u);
    EXPECT_EQ(binding.ring_count, 4u);
    EXPECT_EQ(binding.max_frame, 1800u);
    EXPECT_EQ(binding.credits, 8u);
    EXPECT_EQ(binding.reserved, 0u);
    EXPECT_EQ(binding.reserved2, 0u);
    const auto &requests = switch_firmware.controls;
    ASSERT_GE(requests.size(), 3u);
    const auto &install = requests[requests.size() - 3];
    const auto &bind = requests[requests.size() - 2];
    const auto &start = requests.back();
    EXPECT_EQ(install.operation, SWITCH_A3_INSTALL_SCHEDULE);
    EXPECT_EQ(bind.operation, SWITCH_A3_BIND_RINGS);
    EXPECT_EQ(start.operation, SWITCH_A3_START);
    EXPECT_EQ(install.config_generation, bind.config_generation);
    EXPECT_EQ(bind.config_generation, start.config_generation);
    EXPECT_EQ(bind.session, start.session);
}

TEST_F(SwitchA3Radio, BindingFailureBlocksStartAndOperationalReadiness)
{
    switch_ready();
    switch_firmware.controls.clear();
    switch_firmware.bindings.clear();
    switch_firmware.bind_failure = -2;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_BIND_RINGS)
            EXPECT_FALSE(switch_state().ready);
    };
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.bindings.size(), 1u);
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_state().error, -EINVAL);
    for (const auto &request : switch_firmware.controls)
        EXPECT_NE(request.operation, SWITCH_A3_START);
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_state().error, -EINVAL);
}

TEST_F(SwitchA3Radio, NoAllocatedRingsRefusesScheduleWithoutFirmwareCommands)
{
    switch_attach(0);
    switch_drain();
    ASSERT_EQ(switch_filter(), 0);
    switch_drain();
    const auto commands = switch_firmware.controls.size();
    EXPECT_EQ(switch_schedule(), -EOPNOTSUPP);
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.size(), commands);
    EXPECT_TRUE(switch_firmware.bindings.empty());
    EXPECT_FALSE(switch_state().ready);
}

TEST_F(SwitchA3Radio, DmaBackpressureAndDuplicateCompletionsPreserveCredits)
{
    switch_firmware.credits = 1;
    switch_ready();
    switch_firmware.dma_refusals = 1;
    switch_test_core_lock = true;
    const int id = switch_send();
    ASSERT_GT(id, 0);
    EXPECT_EQ(switch_send(), -ENOBUFS);
    EXPECT_TRUE(switch_firmware.dma.empty());
    switch_test_core_lock = false;
    switch_drain();
    EXPECT_TRUE(switch_firmware.dma.empty());
    EXPECT_EQ(switch_state().tx_credits, 0u);
    switch_wlan_a3_radio_dma_ready();
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 1u);
    const auto packet = switch_firmware.dma[0];
    switch_complete(packet);
    EXPECT_EQ(switch_state().tx_credits, 1u);
    const auto events = switch_firmware.events.size();
    switch_complete(packet);
    EXPECT_EQ(switch_firmware.events.size(), events);
    EXPECT_EQ(switch_state().tx_credits, 1u);
}

TEST_F(SwitchA3Radio, NativeRateAndFiniteDeadlineMatchFirmwareSubmissionContract)
{
    switch_ready();
    uint8_t frame[64] = {0x08};
    frame[4] = 1;
    const auto state = switch_state();
    swlan_tx_req request = {};
    request.frame = frame;
    request.len = sizeof frame;
    request.freq_mhz = 2437;
    request.rate_mcs = 0xff;
    request.flags = SWLAN_TX_NO_ACK;
    request.generation = state.generation;
    request.clock_epoch = state.clock.epoch;
    ASSERT_GT(switch_wlan_a3_radio_tx(&request), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 1u);
    switch_a3_tx_header header = {};
    ASSERT_TRUE(switch_a3_tx_decode(switch_firmware.dma[0].data(), switch_firmware.dma[0].size(), state.generation,
                                    state.clock.epoch, &header));
    EXPECT_EQ(header.rate, 12u);
    EXPECT_TRUE(header.flags & SWITCH_A3_TX_ON_CHANNEL_ONLY);
    EXPECT_GT(header.expiry_tsf, state.clock.tsf_us);
    request.rate_mcs = 0;
    EXPECT_EQ(switch_wlan_a3_radio_tx(&request), -EOPNOTSUPP);
    request.rate_mcs = 0xff;
    frame[4] = 2;
    EXPECT_EQ(switch_wlan_a3_radio_tx(&request), -EOPNOTSUPP);
}

TEST_F(SwitchA3Radio, DmaReadyDuringRefusalIsNotLost)
{
    switch_ready();
    switch_firmware.dma_refusals = 1;
    switch_firmware.signal_dma_inside = true;
    ASSERT_GT(switch_send(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.dma.size(), 1u);
}

TEST_F(SwitchA3Radio, MissingTxStatusExpiresOnceAndReplaysAfterProvenRetirement)
{
    switch_firmware.credits = 1;
    switch_ready();
    const int id = switch_send();
    ASSERT_GT(id, 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 1u);
    EXPECT_EQ(switch_state().tx_credits, 0u);
    switch_test_now_ns += 100000000;
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
    switch_test_now_ns += 1100000000;
    switch_drain();
    EXPECT_EQ(switch_state().tx_credits, 1u);
    EXPECT_TRUE(switch_state().ready);
    unsigned expired = 0;
    for (const auto &event : switch_firmware.events) {
        EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
        expired += event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(id) && event.status == ETIME;
    }
    EXPECT_EQ(expired, 1u);
    const auto events = switch_firmware.events.size();
    switch_complete(switch_firmware.dma[0]);
    switch_test_now_ns += 2000000000;
    switch_drain();
    EXPECT_EQ(switch_firmware.events.size(), events);
    EXPECT_EQ(switch_firmware.dma.size(), 1u);
}

TEST_F(SwitchA3Radio, QueuedDmaCannotTransmitAfterItsDeadline)
{
    switch_ready();
    switch_firmware.dma_refusals = 100;
    const int id = switch_send();
    ASSERT_GT(id, 0);
    switch_drain();
    EXPECT_TRUE(switch_firmware.dma.empty());
    switch_test_now_ns += 100000000;
    switch_drain();
    switch_firmware.dma_refusals = 0;
    switch_wlan_a3_radio_dma_ready();
    switch_drain();
    EXPECT_TRUE(switch_firmware.dma.empty());
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().tx_credits, 8u);
    unsigned expired = 0;
    for (const auto &event : switch_firmware.events)
        expired +=
            event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(id) && event.status == ETIMEDOUT;
    EXPECT_EQ(expired, 1u);
}

TEST_F(SwitchA3Radio, FailedTimeoutCleanupRequiresFreshFirmwareInsteadOfReopenLoop)
{
    switch_ready();
    ASSERT_GT(switch_send(), 0);
    switch_drain();
    switch_firmware.stop_failure = -2;
    switch_test_now_ns += 1200000000;
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_state().tx_credits, 0u);
    EXPECT_EQ(switch_wlan_a3_radio_clock(), -ENXIO);
    EXPECT_EQ(switch_filter(), -ENXIO);
    unsigned recovery = 0;
    for (const auto &event : switch_firmware.events)
        recovery += event.type == SWLAN_AWDL_RECOVERY_REQUIRED;
    EXPECT_EQ(recovery, 1u);
    const auto events = switch_firmware.events.size();
    switch_test_now_ns += 2000000000;
    switch_drain();
    EXPECT_EQ(switch_firmware.events.size(), events);
    switch_firmware.stop_failure = 0;
    switch_wlan_a3_radio_lost();
    switch_ready();
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, UnicastCompletionRequiresActualAcknowledgement)
{
    switch_ready();
    ASSERT_GT(switch_send(true), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 1u);
    switch_complete(switch_firmware.dma[0]);
    ASSERT_GE(switch_firmware.events.size(), 2u);
    EXPECT_EQ(switch_firmware.events[switch_firmware.events.size() - 2].status, EIO);
    ASSERT_GT(switch_send(true), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 2u);
    switch_complete(switch_firmware.dma[1], SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED);
    EXPECT_EQ(switch_firmware.events[switch_firmware.events.size() - 2].status, 0u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.tx_records, 2u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.tx_sent, 2u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.tx_ack_missing, 1u);
    switch_complete(switch_firmware.dma[1]);
    EXPECT_EQ(switch_a3_host::gDiagnostics.tx_rejected, 1u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.tx_sent, 2u);
}

TEST_F(SwitchA3Radio, StopIsAcceptedEvenWhenControlQueueIsFull)
{
    switch_ready();
    for (unsigned i = 0; i < 16; ++i)
        ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_wlan_a3_radio_close(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_STOP);
    EXPECT_FALSE(switch_state().ready);
}

TEST_F(SwitchA3Radio, FailedOrdinaryStopRequiresRecoveryAndCannotReopen)
{
    switch_ready();
    switch_firmware.stop_failure = -2;
    EXPECT_EQ(switch_wlan_a3_radio_close(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    unsigned recovery = 0;
    for (const auto &event : switch_firmware.events)
        recovery += event.type == SWLAN_AWDL_RECOVERY_REQUIRED;
    EXPECT_EQ(recovery, 1u);
    const size_t controls = switch_firmware.controls.size();
    switch_test_now_ns += 2000000000;
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.size(), controls);
    EXPECT_LT(switch_filter(), 0);
}

TEST_F(SwitchA3Radio, StopAndAddressChangeCancelQueuedRequestsOnce)
{
    switch_ready();
    const int first = switch_send();
    ASSERT_GT(first, 0);
    ASSERT_EQ(switch_wlan_a3_radio_close(), 0);
    switch_drain();
    EXPECT_TRUE(switch_firmware.dma.empty());
    unsigned cancelled = 0;
    for (const auto &event : switch_firmware.events)
        cancelled +=
            event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(first) && event.status == ECANCELED;
    EXPECT_EQ(cancelled, 1u);
    EXPECT_FALSE(switch_state().ready);
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    const int second = switch_send();
    ASSERT_GT(second, 0);
    const uint8_t changed[6] = {2, 20, 21, 22, 23, 24};
    ASSERT_EQ(switch_wlan_a3_radio_filter(changed, awdl_bssid, 0), 0);
    switch_drain();
    cancelled = 0;
    for (const auto &event : switch_firmware.events)
        cancelled +=
            event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(second) && event.status == ECANCELED;
    EXPECT_EQ(cancelled, 1u);
    EXPECT_TRUE(switch_firmware.dma.empty());
}

TEST_F(SwitchA3Radio, CancelReportsTransferredRequestsAsPossiblySent)
{
    switch_ready();
    const int transferred = switch_send();
    ASSERT_GT(transferred, 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 1u);
    const int queued = switch_send();
    ASSERT_GT(queued, 0);
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.dma.size(), 1u);
    std::vector<std::pair<int, uint16_t>> reports;
    for (const auto &event : switch_firmware.events)
        if (event.type == SWLAN_AWDL_TXDONE)
            reports.emplace_back(static_cast<int>(event.id), event.status);
    std::sort(reports.begin(), reports.end());
    const std::vector<std::pair<int, uint16_t>> expected = {{transferred, ETIME}, {queued, ECANCELED}};
    EXPECT_EQ(reports, expected);
}

TEST_F(SwitchA3Radio, ClockEpochChangesDiscardEarlierBurstAndInvalidateConfiguration)
{
    switch_ready();
    unsigned samples = 0;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_GET_CLOCK && ++samples == 2) {
            switch_firmware.epoch = 2;
            switch_firmware.delay_ns = 200000;
        }
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_state().clock.epoch, 2u);
    EXPECT_TRUE(switch_state().ready);
    switch_firmware.during_control = {};
    switch_firmware.tsf_offset += 10000;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_state().clock.epoch, 3u);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, CalibrationKeepsNewerFaultEvenWhenTheEarliestSampleWasBest)
{
    switch_ready();
    unsigned samples = 0;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        if (control.operation == SWITCH_A3_GET_CLOCK && ++samples == 2) {
            switch_firmware.delay_ns += 100000;
            switch_firmware.state = SWITCH_A3_FAULTED;
            switch_firmware.applied = 0;
            switch_firmware.fault = -1;
        }
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(samples, 3u);
    EXPECT_EQ(switch_state().error, -EIO);
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_STOP);
}

TEST_F(SwitchA3Radio, CalibrationCannotPublishAnEpochOlderThanAnInterleavedNotice)
{
    switch_ready();
    const auto before = switch_state();
    unsigned samples = 0;
    switch_firmware.after_reply = [&](const switch_a3_control &control) {
        if (control.operation == SWITCH_A3_GET_CLOCK && ++samples == 3) {
            ++switch_firmware.epoch;
            switch_firmware.state = SWITCH_A3_STOPPED;
            switch_firmware.applied = 0;
            switch_state_notice(-25);
        }
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    unsigned stale = 0;
    for (const auto &event : switch_firmware.events)
        stale += event.type == SWLAN_AWDL_CLOCK_CHANGED && event.status == ESTALE;
    EXPECT_EQ(stale, 1u);
    EXPECT_EQ(switch_state().clock.epoch, before.clock.epoch + 1);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, RefreshAdoptsExternalConfigurationWithoutClaimingReady)
{
    switch_attach();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    switch_firmware.applied = 5;
    switch_firmware.features = 0;
    ASSERT_EQ(switch_wlan_a3_radio_refresh(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    ASSERT_EQ(switch_filter(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.back().config_generation, 6u);
}

TEST_F(SwitchA3Radio, OrdinaryPowerCycleKeepsSessionAndRejectsOldWork)
{
    switch_ready();
    const auto before = switch_state();
    bool power_cycled = false;
    switch_firmware.during_control = [&](const switch_a3_control &) {
        if (power_cycled)
            return;
        power_cycled = true;
        switch_wlan_a3_radio_power(false);
        EXPECT_FALSE(switch_state().ready);
        EXPECT_EQ(switch_send(), -EAGAIN);
        ++switch_firmware.epoch;
        switch_firmware.applied = 0;
        switch_firmware.state = SWITCH_A3_STOPPED;
        switch_wlan_a3_radio_power(true);
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    const auto after = switch_state();
    EXPECT_EQ(before.generation, after.generation);
    EXPECT_EQ(after.clock.epoch, before.clock.epoch + 1);
    EXPECT_TRUE(after.ready);
    EXPECT_FALSE(after.error);
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
    unsigned down = 0, up = 0;
    for (const auto &event : switch_firmware.events) {
        down += event.type == SWLAN_AWDL_RADIO_DOWN;
        up += event.type == SWLAN_AWDL_RADIO_UP;
    }
    EXPECT_EQ(down, 1u);
    EXPECT_EQ(up, 1u);
}

TEST_F(SwitchA3Radio, ReconfigurationWaitsForRealRfAndDmaRetirement)
{
    switch_ready();
    const int id = switch_send();
    ASSERT_GT(id, 0);
    switch_drain();
    const auto packet = switch_firmware.dma[0];
    const auto configurations = switch_firmware.schedules.size();
    switch_firmware.stop_failure = -16;
    switch_firmware.transport_idle = 0;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_firmware.schedules.size(), configurations);
    switch_complete(packet);
    switch_drain();
    EXPECT_EQ(switch_firmware.schedules.size(), configurations);
    switch_firmware.stop_failure = 0;
    switch_firmware.fault = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    switch_state_notice();
    switch_drain();
    EXPECT_EQ(switch_firmware.schedules.size(), configurations);
    switch_firmware.transport_idle = 1;
    switch_wlan_a3_radio_dma_ready();
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_firmware.schedules.size(), configurations + 1);
    unsigned completed = 0;
    for (const auto &event : switch_firmware.events)
        completed += event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(id);
    EXPECT_EQ(completed, 1u);
    const auto events = switch_firmware.events.size();
    switch_complete(packet);
    EXPECT_EQ(switch_firmware.events.size(), events);
}

TEST_F(SwitchA3Radio, MissingCanceledStatusDoesNotBlockProvenRfAndDmaRetirement)
{
    switch_ready();
    const int id = switch_send();
    ASSERT_GT(id, 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.dma.size(), 1u);
    const auto packet = switch_firmware.dma[0];
    switch_firmware.stop_failure = -16;
    switch_firmware.transport_idle = 0;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();

    switch_firmware.stop_failure = switch_firmware.fault = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    switch_state_notice();
    switch_drain();
    EXPECT_EQ(switch_firmware.transport_idle_calls, 1u);
    EXPECT_EQ(switch_send(), -EAGAIN);
    EXPECT_EQ(switch_firmware.schedules.size(), 1u);
    EXPECT_EQ(switch_firmware.dma.size(), 1u);

    switch_firmware.transport_idle = 1;
    switch_wlan_a3_radio_dma_ready();
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().tx_credits, 8u);
    EXPECT_EQ(switch_firmware.schedules.size(), 2u);
    unsigned canceled = 0;
    for (const auto &event : switch_firmware.events)
        canceled += event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(id) &&
                    event.status == ETIME;
    EXPECT_EQ(canceled, 1u);
    const auto events = switch_firmware.events.size();
    switch_complete(packet);
    EXPECT_EQ(switch_firmware.events.size(), events);
}

TEST_F(SwitchA3Radio, StateNoticesRejectForeignEpochAndNeverInventReadiness)
{
    switch_firmware.epoch = 2;
    switch_ready();
    const auto before = switch_state();
    switch_firmware.state = SWITCH_A3_FAULTED;
    switch_state_notice(-1, before.generation + 1);
    EXPECT_TRUE(switch_state().ready);
    switch_state_notice(-1, before.generation, before.clock.epoch - 1);
    EXPECT_TRUE(switch_state().ready);
    switch_firmware.fault = -1;
    switch_state_notice(-1, before.generation, before.clock.epoch);
    EXPECT_FALSE(switch_state().ready);
    switch_drain();
    EXPECT_EQ(switch_state().error, -EIO);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_STOP);
    unsigned recovery = 0, fault = 0;
    for (const auto &event : switch_firmware.events) {
        recovery += event.type == SWLAN_AWDL_RECOVERY_REQUIRED;
        fault += event.type == SWLAN_AWDL_CONFIG_CHANGED && event.status == EIO;
    }
    EXPECT_EQ(recovery, 0u);
    EXPECT_EQ(fault, 1u);
    switch_firmware.state = SWITCH_A3_RUNNING;
    switch_state_notice();
    EXPECT_FALSE(switch_state().ready);
}

TEST_F(SwitchA3Radio, SupersededRunningNoticeDoesNotWithdrawCurrentReadiness)
{
    switch_ready();
    const uint32_t previous = switch_firmware.applied;
    ASSERT_EQ(switch_schedule(true), 0);
    switch_drain();
    ASSERT_TRUE(switch_state().ready);
    ASSERT_GT(switch_firmware.applied, previous);
    switch_state_notice(0, 0, 0, previous);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_GT(switch_state().tx_credits, 0u);
}

TEST_F(SwitchA3Radio, LateFaultNoticeCannotReplaceAppliedConfiguration)
{
    switch_ready();
    const auto previous = switch_firmware.revision;
    ASSERT_EQ(switch_schedule(true), 0);
    switch_drain();
    const auto before = switch_state();
    ASSERT_TRUE(before.ready);
    switch_state_notice(-1, before.generation, before.clock.epoch, 0, previous);
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_state().applied_generation, before.applied_generation);
}

TEST_F(SwitchA3Radio, NoLegalSocialChannelsPreventsStart)
{
    switch_firmware.legal_social_channels = 0;
    switch_attach();
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    EXPECT_TRUE(switch_state().social_channels_valid);
    EXPECT_EQ(switch_state().social_channels, 0u);
    EXPECT_FALSE(switch_state().ready);
    for (const auto &control : switch_firmware.controls)
        EXPECT_NE(control.operation, SWITCH_A3_START);
}

TEST_F(SwitchA3Radio, ParkedRegulatorySnapshotRefreshesOnceOnStationResume)
{
    switch_firmware.legal_social_channels = 0;
    switch_attach();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    ASSERT_TRUE(switch_state().social_channels_valid);
    ASSERT_EQ(switch_state().social_channels, 0u);
    const auto commands = switch_firmware.controls.size();
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    switch_firmware.legal_social_channels = SWITCH_A3_SOCIAL_6;
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    ASSERT_EQ(switch_firmware.controls.size(), commands + 1);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_GET_STATE);
    EXPECT_EQ(switch_state().social_channels, SWITCH_A3_SOCIAL_6);
    EXPECT_FALSE(switch_state().ready);
}

TEST_F(SwitchA3Radio, CachedClockExpiryRefusesTxUntilANewBoundedSample)
{
    switch_ready();
    switch_test_now_ns += 2000000000ULL;
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_send(), -EAGAIN);
    EXPECT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
    EXPECT_GT(switch_send(), 0);
}

TEST_F(SwitchA3Radio, FailedEarlyRenewalPreservesCurrentAppliedRadioUntilExpiry)
{
    switch_ready();
    const auto before = switch_state();
    const auto commands = switch_firmware.controls.size();
    switch_firmware.delay_ns = 3000000;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    const auto retained = switch_state();
    EXPECT_TRUE(retained.ready);
    EXPECT_EQ(retained.error, 0);
    EXPECT_EQ(retained.applied_generation, before.applied_generation);
    EXPECT_EQ(retained.clock.host_ns, before.clock.host_ns);
    EXPECT_EQ(retained.clock.epoch, before.clock.epoch);
    ASSERT_EQ(switch_firmware.controls.size(), commands + 3);
    for (size_t index = commands; index < switch_firmware.controls.size(); ++index)
        EXPECT_EQ(switch_firmware.controls[index].operation, SWITCH_A3_GET_CLOCK);
    ASSERT_FALSE(switch_firmware.events.empty());
    EXPECT_EQ(switch_firmware.events.back().type, SWLAN_AWDL_CLOCK_CHANGED);
    EXPECT_EQ(switch_firmware.events.back().status, EAGAIN);
    switch_test_now_ns = before.clock.host_ns + 2000000000ULL;
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_send(), -EAGAIN);
    switch_firmware.delay_ns = 100000;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    const auto renewed = switch_state();
    EXPECT_TRUE(renewed.ready);
    EXPECT_EQ(renewed.applied_generation, before.applied_generation);
    EXPECT_EQ(renewed.clock.epoch, before.clock.epoch);
    EXPECT_GT(renewed.clock.host_ns, before.clock.host_ns);
    ASSERT_EQ(switch_firmware.controls.size(), commands + 6);
    for (size_t index = commands + 3; index < switch_firmware.controls.size(); ++index)
        EXPECT_EQ(switch_firmware.controls[index].operation, SWITCH_A3_GET_CLOCK);
}

TEST_F(SwitchA3Radio, TwentyEarlyRenewalsKeepTheSameAppliedRadioReady)
{
    switch_ready();
    const auto initial = switch_state();
    const auto commands = switch_firmware.controls.size();
    constexpr unsigned renewals = 20;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        EXPECT_EQ(control.operation, SWITCH_A3_GET_CLOCK);
        EXPECT_TRUE(switch_state().ready);
        EXPECT_EQ(switch_state().applied_generation, initial.applied_generation);
    };
    for (unsigned remaining = renewals; remaining; --remaining) {
        const auto before = switch_state();
        struct switch_awdl_radio_clock clock = {};
        ASSERT_EQ(switch_awdl_radio_clock_update(&clock, before.clock.tsf_us, before.clock.host_ns,
                                                before.clock.uncertainty_ns, before.clock.epoch,
                                                switch_test_now_ns), 1);
        const uint64_t deadline = switch_awdl_radio_clock_deadline(&clock);
        ASSERT_GT(deadline, switch_test_now_ns);
        switch_test_now_ns = deadline;
        ASSERT_TRUE(switch_awdl_radio_clock_try_renew(&clock, switch_test_now_ns));
        ASSERT_TRUE(switch_state().ready);
        ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
        switch_drain();
        const auto after = switch_state();
        ASSERT_TRUE(after.ready);
        ASSERT_EQ(after.applied_generation, initial.applied_generation);
        ASSERT_EQ(after.clock.epoch, initial.clock.epoch);
        ASSERT_GT(after.clock.host_ns, before.clock.host_ns);
    }
    EXPECT_EQ(switch_firmware.controls.size(), commands + renewals * 3);
}

TEST_F(SwitchA3Radio, FailedRenewalStillRetiresAConfirmedFirmwareFault)
{
    switch_ready();
    switch_firmware.delay_ns = 3000000;
    switch_firmware.state = SWITCH_A3_FAULTED;
    switch_firmware.applied = 0;
    switch_firmware.fault = -1;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_state().error, -EIO);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_STOP);
}

TEST_F(SwitchA3Radio, StateRefreshCannotOverwriteHostReservationsWithCredits)
{
    switch_ready();
    for (unsigned i = 0; i < 8; ++i)
        ASSERT_GT(switch_send(), 0);
    ASSERT_EQ(switch_wlan_a3_radio_refresh(), 0);
    switch_drain();
    EXPECT_EQ(switch_state().tx_credits, 0u);
    EXPECT_EQ(switch_send(), -ENOBUFS);
    ASSERT_EQ(switch_firmware.dma.size(), 8u);
    switch_complete(switch_firmware.dma[0]);
    EXPECT_EQ(switch_state().tx_credits, 1u);
}

TEST_F(SwitchA3Radio, StationMutationParksAndResumesDesiredSchedule)
{
    switch_ready();
    const auto schedules = switch_firmware.schedules.size();
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_INSTALL_SCHEDULE);
    EXPECT_EQ(switch_firmware.schedules.size(), schedules + 1);
    switch_drain();
    EXPECT_EQ(switch_firmware.schedules.size(), schedules + 1);
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_firmware.schedules.size(), schedules + 2);
}

TEST_F(SwitchA3Radio, DormantAttachStationScanAndPowerCycleDoNotSendControls)
{
    switch_attach();
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    switch_wlan_a3_radio_station_resume();
    switch_wlan_a3_radio_power(false);
    switch_wlan_a3_radio_power(true);
    switch_drain();
    EXPECT_TRUE(switch_firmware.controls.empty());
    EXPECT_EQ(switch_firmware.transport_idle_calls, 0u);
    EXPECT_FALSE(switch_state().error);
}

TEST_F(SwitchA3Radio, StationScanDefersQueuedConfigurationWithoutStop)
{
    switch_attach();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    switch_firmware.controls.clear();
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    switch_drain();
    EXPECT_TRUE(switch_firmware.controls.empty());
    EXPECT_EQ(switch_firmware.transport_idle_calls, 0u);
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    ASSERT_EQ(switch_firmware.controls.size(), 4u);
    EXPECT_EQ(switch_firmware.controls.front().operation, SWITCH_A3_CONFIGURE);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, ReadOnlyControlInFlightDoesNotBlockStationPause)
{
    switch_attach();
    bool paused = false;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        if (!paused && control.operation == SWITCH_A3_GET_CLOCK) {
            ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
            paused = true;
        }
    };
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_TRUE(paused);
    ASSERT_EQ(switch_firmware.controls.size(), 3u);
    for (const auto &control : switch_firmware.controls)
        EXPECT_EQ(control.operation, SWITCH_A3_GET_CLOCK);
    EXPECT_EQ(switch_firmware.transport_idle_calls, 0u);
    switch_wlan_a3_radio_station_resume();
}

TEST_F(SwitchA3Radio, SupersededFirstConfigureStillRequiresStopAndDmaRetirement)
{
    switch_attach();
    bool superseded = false;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        if (!superseded && control.operation == SWITCH_A3_CONFIGURE) {
            superseded = true;
            ASSERT_EQ(switch_schedule(true), 0);
        }
    };
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    ASSERT_TRUE(superseded);
    ASSERT_EQ(switch_firmware.controls.size(), 9u);
    EXPECT_EQ(switch_firmware.controls[3].operation, SWITCH_A3_CONFIGURE);
    EXPECT_EQ(switch_firmware.controls[4].operation, SWITCH_A3_STOP);
    EXPECT_EQ(switch_firmware.controls[5].operation, SWITCH_A3_CONFIGURE);
    EXPECT_EQ(switch_firmware.transport_idle_calls, 1u);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, FailedFirstConfigureStillBlocksStationUntilStop)
{
    switch_attach();
    switch_firmware.failure = -2;
    ASSERT_EQ(switch_filter(), 0);
    switch_drain();
    ASSERT_EQ(switch_state().error, -EINVAL);
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_STOP);
    EXPECT_EQ(switch_firmware.transport_idle_calls, 1u);
    switch_firmware.failure = 0;
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_CONFIGURE);
}

TEST_F(SwitchA3Radio, FirstConfigureInFlightKeepsStationPausedUntilFirmwareLoss)
{
    switch_attach();
    bool waited = false;
    const uint32_t old = switch_state().generation;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        if (control.operation != SWITCH_A3_CONFIGURE || control.session != old)
            return;
        switch_test_sleep_action = [&] {
            waited = true;
            switch_wlan_a3_radio_lost();
            switch_attach();
        };
        EXPECT_EQ(switch_wlan_a3_radio_station_prepare(), -ENXIO);
    };
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    EXPECT_TRUE(waited);
    EXPECT_NE(switch_state().generation, old);
    for (const auto &control : switch_firmware.controls)
        EXPECT_NE(control.operation, SWITCH_A3_STOP);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, PendingControlCanCompleteAfterTheFormerPauseDeadline)
{
    switch_ready();
    switch_firmware.during_control = [](const switch_a3_control &control) {
        if (control.operation != SWITCH_A3_STOP)
            return;
        switch_test_now_ns += 1600000000ULL;
        while (switch_test_run_one()) {
        }
        for (const auto &event : switch_firmware.events)
            EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
    };
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    EXPECT_FALSE(switch_state().error);
    switch_firmware.during_control = {};
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, CalibrationAfterStationParkDoesNotSpendAnOwnershipDeadline)
{
    switch_ready();
    ASSERT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    bool advanced = false;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        if (control.operation != SWITCH_A3_GET_CLOCK || advanced)
            return;
        advanced = true;
        switch_test_now_ns += 1600000000ULL;
        while (switch_test_run_one()) {
        }
        for (const auto &event : switch_firmware.events)
            EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
    };
    switch_test_now_ns += 2000000000ULL;
    switch_wlan_a3_radio_station_resume();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_TRUE(advanced);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, StationMutationWaitsForNativeBusyRetirementNotice)
{
    switch_ready();
    ASSERT_GT(switch_send(), 0);
    switch_drain();
    const auto packet = switch_firmware.dma[0];
    switch_firmware.schedule_busy = 1;
    switch_firmware.during_control = [&](const switch_a3_control &control) {
        if (control.operation == SWITCH_A3_INSTALL_SCHEDULE && switch_firmware.schedule_busy) {
            switch_test_sleep_action = [&] {
                switch_complete(packet);
                switch_state_notice();
            };
        }
    };
    EXPECT_EQ(switch_wlan_a3_radio_station_prepare(), 0);
    EXPECT_TRUE(switch_state().ready);
    switch_firmware.during_control = {};
    switch_wlan_a3_radio_station_resume();
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, StationMutationFailsClosedOnUnretiredOwnership)
{
    switch_ready();
    ASSERT_GT(switch_send(), 0);
    switch_drain();
    switch_firmware.schedule_busy = 1;
    switch_firmware.stop_failure = -16;
    EXPECT_EQ(switch_wlan_a3_radio_station_prepare(), -ETIMEDOUT);
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_filter(), -ENXIO);
}

TEST_F(SwitchA3Radio, StationPrepareTimesOutAndRequiresRecoveryWhenFirmwareStallsWithoutTx)
{
    switch_ready();
    switch_firmware.schedule_busy = 1;
    EXPECT_EQ(switch_wlan_a3_radio_station_prepare(), -ETIMEDOUT);
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_state().error, -ETIMEDOUT);
    EXPECT_EQ(switch_filter(), -ENXIO);
    unsigned recovery = 0;
    for (const auto &event : switch_firmware.events)
        recovery += event.type == SWLAN_AWDL_RECOVERY_REQUIRED;
    EXPECT_EQ(recovery, 1u);
}

TEST_F(SwitchA3Radio, UnicastDataUsesAdaptiveOfdmRatesAndActionStaysAt6Mbps)
{
    switch_ready();
    auto sendFrame = [&](uint8_t fc0, uint8_t dst0, uint32_t flags) -> uint32_t {
        uint8_t frame[64] = {fc0, 0};
        frame[4] = dst0;
        memcpy(frame + 16, awdl_bssid, 6);
        const auto state = switch_state();
        swlan_tx_req req = {};
        req.frame = frame;
        req.len = sizeof frame;
        req.freq_mhz = 2437;
        req.rate_mcs = 0xff;
        req.flags = flags;
        req.deadline_tsf = state.clock.tsf_us + 65536;
        req.generation = state.generation;
        req.clock_epoch = state.clock.epoch;
        const size_t before = switch_firmware.dma.size();
        EXPECT_GT(switch_wlan_a3_radio_tx(&req), 0);
        switch_drain();
        EXPECT_EQ(switch_firmware.dma.size(), before + 1);
        switch_a3_tx_header hdr = {};
        EXPECT_TRUE(switch_a3_tx_decode(switch_firmware.dma.back().data(), switch_firmware.dma.back().size(),
                                        state.generation, state.clock.epoch, &hdr));
        return hdr.rate;
    };

    EXPECT_EQ(sendFrame(0xd0, 1, SWLAN_TX_NO_ACK), 12u);
    switch_complete(switch_firmware.dma.back());
    EXPECT_EQ(sendFrame(0x08, 1, SWLAN_TX_NO_ACK), 12u);
    switch_complete(switch_firmware.dma.back());

    EXPECT_EQ(sendFrame(0x08, 2, 0), 108u);
    switch_complete(switch_firmware.dma.back());

    EXPECT_EQ(sendFrame(0x08, 2, 0), 72u);
    const auto first72 = switch_firmware.dma.back();
    EXPECT_EQ(sendFrame(0x08, 2, 0), 72u);
    const auto second72 = switch_firmware.dma.back();
    switch_complete(first72);
    switch_complete(second72);

    EXPECT_EQ(sendFrame(0x08, 2, 0), 48u);
    switch_complete(switch_firmware.dma.back(), 0, SWITCH_A3_TX_EXPIRED);
    EXPECT_EQ(sendFrame(0x08, 2, 0), 48u);
    switch_complete(switch_firmware.dma.back(), 0, SWITCH_A3_TX_CANCELLED);
    EXPECT_EQ(sendFrame(0x08, 2, 0), 48u);
    switch_complete(switch_firmware.dma.back(), 0, SWITCH_A3_TX_FAILED);

    for (unsigned i = 0; i < switch_a3_host::kDataRateStepUpAcks; ++i) {
        EXPECT_EQ(sendFrame(0x08, 2, 0), 24u);
        switch_complete(switch_firmware.dma.back(), SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED);
    }
    EXPECT_EQ(sendFrame(0x08, 2, 0), 48u);
    switch_complete(switch_firmware.dma.back(), SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED);

    switch_wlan_a3_radio_power(false);
    switch_wlan_a3_radio_power(true);
    switch_drain();
    EXPECT_EQ(sendFrame(0x08, 2, 0), 108u);
    switch_complete(switch_firmware.dma.back(), SWITCH_A3_TX_ACK_VALID | SWITCH_A3_TX_ACKED);
}

TEST_F(SwitchA3Radio, DesiredSessionIsRestoredAutomaticallyAfterFirmwareRecovery)
{
    switch_ready();
    const auto old = switch_state();
    switch_wlan_a3_radio_lost();
    switch_firmware.applied = switch_firmware.bound_generation = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    switch_attach();
    switch_drain();
    EXPECT_NE(switch_state().generation, old.generation);
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, OrdinaryFirmwareDownNoticeDoesNotLatchChipRecovery)
{
    switch_ready();
    const auto before = switch_state();
    switch_wlan_a3_radio_power(false);
    ++switch_firmware.epoch;
    switch_firmware.applied = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    switch_state_notice(-25);
    switch_wlan_a3_radio_power(true);
    switch_drain();
    EXPECT_EQ(switch_state().generation, before.generation);
    EXPECT_EQ(switch_state().clock.epoch, before.clock.epoch + 1);
    EXPECT_TRUE(switch_state().ready);
    for (const auto &event : switch_firmware.events)
        EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
}

TEST_F(SwitchA3Radio, MissingNativeStopRetirementHasOneBoundedRecoveryDeadline)
{
    switch_ready();
    switch_firmware.stop_failure = -16;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    const auto commands = switch_firmware.controls.size();
    switch_test_now_ns += 1000000000ULL;
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.size(), commands);
    switch_test_now_ns += 600000000ULL;
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    unsigned recovery = 0;
    for (const auto &event : switch_firmware.events)
        recovery += event.type == SWLAN_AWDL_RECOVERY_REQUIRED;
    EXPECT_EQ(recovery, 1u);
    switch_test_now_ns += 2000000000ULL;
    switch_drain();
    ASSERT_EQ(switch_firmware.controls.size(), commands + 1);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_GET_STATE);
}

TEST_F(SwitchA3Radio, DroppedStoppedNoticeReconcilesAtDeadlineWithoutReset)
{
    switch_ready();
    switch_firmware.stop_failure = -16;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    const auto commands = switch_firmware.controls.size();
    switch_firmware.stop_failure = switch_firmware.fault = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    switch_test_now_ns += 1600000000ULL;
    switch_drain();
    ASSERT_GT(switch_firmware.controls.size(), commands);
    EXPECT_EQ(switch_firmware.controls[commands].operation, SWITCH_A3_GET_STATE);
    unsigned checks = 0;
    for (const auto &control : switch_firmware.controls)
        checks += control.operation == SWITCH_A3_GET_STATE;
    EXPECT_EQ(checks, 1u);
    EXPECT_TRUE(switch_state().ready);
    EXPECT_EQ(switch_firmware.schedules.size(), 2u);
    for (const auto &event : switch_firmware.events)
        EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
}

TEST_F(SwitchA3Radio, StoppedSnapshotFromOlderOwnerCannotReleaseCurrentFirmware)
{
    switch_ready();
    switch_firmware.stop_failure = -16;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    const auto commands = switch_firmware.controls.size();
    switch_firmware.stop_failure = switch_firmware.fault = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    ASSERT_GT(switch_firmware.state_generation, 1u);
    --switch_firmware.state_generation;
    switch_test_now_ns += 1600000000ULL;
    switch_drain();
    EXPECT_EQ(switch_state().error, -ETIMEDOUT);
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_firmware.transport_idle_calls, 0u);
    ASSERT_EQ(switch_firmware.controls.size(), commands + 1);
    EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_GET_STATE);
    EXPECT_EQ(switch_firmware.schedules.size(), 1u);
}

TEST_F(SwitchA3Radio, NewDesiredScheduleWaitsForTheOwnerBeingChecked)
{
    switch_ready();
    switch_firmware.stop_failure = -16;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    switch_firmware.stop_failure = switch_firmware.fault = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    unsigned checks = 0;
    switch_firmware.during_control = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_GET_STATE) {
            ++checks;
            EXPECT_EQ(switch_firmware.transport_idle_calls, 0u);
            EXPECT_EQ(switch_schedule(), 0);
        } else if (request.operation == SWITCH_A3_CONFIGURE || request.operation == SWITCH_A3_START) {
            EXPECT_EQ(switch_firmware.transport_idle_calls, 1u);
        }
    };
    switch_test_now_ns += 1600000000ULL;
    switch_drain();
    EXPECT_EQ(checks, 1u);
    EXPECT_EQ(switch_firmware.transport_idle_calls, 1u);
    EXPECT_TRUE(switch_state().ready);
    ASSERT_EQ(switch_firmware.schedules.size(), 2u);
    EXPECT_NE(switch_firmware.schedules.back().slots[0].chanspec, 0u);
    for (const auto &event : switch_firmware.events)
        EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
}

TEST_F(SwitchA3Radio, NewerStoppedNoticeDuringFinalCheckRetiresCurrentOwner)
{
    switch_ready();
    switch_firmware.stop_failure = -16;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    switch_firmware.stop_failure = switch_firmware.fault = 0;
    switch_firmware.state = SWITCH_A3_STOPPED;
    unsigned checks = 0;
    switch_firmware.after_reply = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_GET_STATE) {
            ++checks;
            switch_state_notice();
        }
    };
    switch_test_now_ns += 1600000000ULL;
    switch_drain();
    EXPECT_EQ(checks, 1u);
    EXPECT_EQ(switch_firmware.transport_idle_calls, 1u);
    EXPECT_TRUE(switch_state().ready);
    for (const auto &event : switch_firmware.events)
        EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
}

TEST_F(SwitchA3Radio, StopRetirementNoticeCanPrecedeItsBusyReply)
{
    switch_ready();
    switch_firmware.stop_failure = -16;
    bool notified = false;
    switch_firmware.after_reply = [&](const switch_a3_control &control) {
        if (!notified && control.operation == SWITCH_A3_STOP) {
            notified = true;
            switch_firmware.stop_failure = 0;
            switch_firmware.state = SWITCH_A3_STOPPED;
            switch_firmware.fault = -1;
            switch_state_notice(-1);
        }
    };
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    EXPECT_TRUE(notified);
    EXPECT_TRUE(switch_state().ready);
    for (const auto &event : switch_firmware.events)
        EXPECT_NE(event.type, SWLAN_AWDL_RECOVERY_REQUIRED);
}

TEST_F(SwitchA3Radio, RawRxRequiresAppliedGenerationAndStripsRetainedFcs)
{
    switch_ready();
    ASSERT_EQ(switch_wlan_a3_radio_rx(switch_receive, nullptr), 0);
    switch_a3_raw_rx rx = {};
    rx.magic = SWITCH_A3_RAW_RX_MAGIC;
    rx.version = SWITCH_A3_RAW_RX_VERSION;
    rx.header_bytes = sizeof rx;
    rx.frame_bytes = 28;
    rx.flags = SWITCH_A3_RX_FCS_PRESENT;
    rx.session = switch_state().generation;
    rx.clock_epoch = switch_state().clock.epoch;
    rx.config_generation = switch_firmware.applied;
    rx.chanspec = switch_firmware.schedules.back().slots[0].chanspec;
    rx.tsf = switch_state().clock.tsf_us;
    std::vector<uint8_t> packet(sizeof rx + rx.frame_bytes, 0xab);
    memcpy(packet.data(), &rx, sizeof rx);
    switch_test_now_ns += 30000000000ULL;
    ASSERT_TRUE(switch_state().ready);
    ASSERT_EQ(switch_state().tx_credits, 0u);
    ASSERT_EQ(switch_send(), -EAGAIN);
    EXPECT_TRUE(switch_wlan_a3_radio_frame(packet.data(), packet.size()));
    ASSERT_EQ(switch_firmware.received, 1u);
    EXPECT_EQ(switch_firmware.received_frame.size(), 24u);
    EXPECT_EQ(switch_firmware.received_meta.tsf, rx.tsf);
    EXPECT_EQ(switch_firmware.received_meta.generation, rx.session);
    EXPECT_EQ(switch_firmware.received_meta.clock_epoch, rx.clock_epoch);
    EXPECT_EQ(switch_firmware.received_meta.applied_generation, rx.config_generation);
    EXPECT_EQ(switch_firmware.received_meta.freq_mhz, 2437u);
    ++rx.config_generation;
    memcpy(packet.data(), &rx, sizeof rx);
    EXPECT_TRUE(switch_wlan_a3_radio_frame(packet.data(), packet.size()));
    EXPECT_EQ(switch_firmware.received, 1u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.rx_stale, 1u);
    --rx.config_generation;
    rx.reserved = 1;
    memcpy(packet.data(), &rx, sizeof rx);
    EXPECT_TRUE(switch_wlan_a3_radio_frame(packet.data(), packet.size()));
    EXPECT_EQ(switch_a3_host::gDiagnostics.rx_invalid, 1u);
    rx.reserved = 0;
    memcpy(packet.data(), &rx, sizeof rx);
    ASSERT_EQ(switch_wlan_a3_radio_close(), 0);
    EXPECT_TRUE(switch_wlan_a3_radio_frame(packet.data(), packet.size()));
    EXPECT_EQ(switch_firmware.received, 1u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.rx_records, 4u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.rx_delivered, 1u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.rx_unready, 1u);
}

TEST_F(SwitchA3Radio, DiagnosticClockSuffixRepeatsEveryThirtySecondsWithoutExtraControls)
{
    switch_ready();
    const size_t initial = switch_firmware.controls.size();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.size() - initial, 3u);
    EXPECT_EQ(switch_firmware.diagnostic_calls, 1u);
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.diagnostic_calls, 1u);
    for (unsigned index = 0; index < 10; ++index) {
        switch_test_now_ns += 30000000000ULL;
        ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
        switch_drain();
        EXPECT_TRUE(switch_state().ready);
    }
    EXPECT_EQ(switch_firmware.diagnostic_calls, 11u);
    EXPECT_EQ(switch_a3_host::gDiagnostics.requests, 11u);
    switch_wlan_a3_discovery_trial();
    switch_wlan_a3_discovery_trial();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.diagnostic_calls, 12u);
    switch_wlan_a3_discovery_trial();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.diagnostic_calls, 12u);
    switch_test_now_ns += 30000000000ULL;
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.diagnostic_calls, 13u);
    const char *blocked = nullptr;
    EXPECT_FALSE(switch_wlan_a3_trial_applied(44, &blocked));
    EXPECT_STREQ(blocked, "firmware home channel differs");
    ASSERT_TRUE(switch_wlan_a3_trial_applied(6, &blocked));
    EXPECT_EQ(blocked, nullptr);
    switch_wlan_a3_trial_boundary(true, 6);
    switch_wlan_a3_trial_boundary(true, 6);
    switch_drain();
    EXPECT_EQ(switch_firmware.diagnostic_calls, 14u);
    const auto startGeneration = switch_state().applied_generation;
    ASSERT_EQ(switch_schedule(false, 2000000), 0);
    switch_drain();
    ASSERT_GT(switch_state().applied_generation, startGeneration);
    ASSERT_TRUE(switch_wlan_a3_trial_applied(6));
    switch_wlan_a3_trial_boundary(false, 6);
    switch_wlan_a3_trial_boundary(false, 6);
    ASSERT_EQ(switch_schedule(true), 0);
    EXPECT_FALSE(switch_wlan_a3_trial_applied(6, &blocked));
    EXPECT_STREQ(blocked, "firmware schedule commit pending");
    switch_drain();
    EXPECT_FALSE(switch_wlan_a3_trial_applied(6, &blocked));
    EXPECT_STREQ(blocked, "firmware has an away or inactive slot");
    EXPECT_EQ(switch_firmware.diagnostic_calls, 15u);
    unsigned matched = 0;
    for (const auto &line : switch_test_log)
        matched += line.find("snapshot matched") != std::string::npos;
    EXPECT_EQ(matched, 2u);
    SwitchWLANA3Diagnostics budget = {};
    budget.requests = 1;
    budget.requested_ns = switch_test_now_ns;
    EXPECT_FALSE(switch_a3_diagnostic_due(budget, switch_test_now_ns - 1));
    EXPECT_FALSE(switch_a3_diagnostic_due(budget, switch_test_now_ns + 29999999999ULL));
    EXPECT_TRUE(switch_a3_diagnostic_due(budget, switch_test_now_ns + 30000000000ULL));
}

TEST_F(SwitchA3Radio, MeasuredEchoUsesOneExistingExchangeAndExpiresOnAddressChange)
{
    switch_ready();
    switch_a3_host::gDiagnostics.requests = 1;
    switch_a3_host::gDiagnostics.requested_ns = switch_test_now_ns;
    const auto state = switch_state();
    const auto initial = switch_firmware.controls.size();
    switch_wlan_a3_echo_snapshot(state.generation + 1, state.clock.epoch, 9, 3);
    switch_wlan_a3_echo_snapshot(state.generation, state.clock.epoch + 1, 9, 3);
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    ASSERT_EQ(switch_firmware.diagnostic_calls, 0u);

    switch_wlan_a3_echo_snapshot(state.generation, state.clock.epoch, 0, 0);
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.size(), initial + 6);
    EXPECT_EQ(switch_firmware.diagnostic_calls, 1u);
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.diagnostic_calls, 1u);

    switch_wlan_a3_echo_snapshot(state.generation, state.clock.epoch, 13, 5);
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.diagnostic_calls, 1u);
    unsigned matched = 0;
    for (const auto &line : switch_test_log)
        matched += line.find("A3 echo snapshot id 0 seq 0 matched") != std::string::npos;
    EXPECT_EQ(matched, 1u);
}

TEST_F(SwitchA3Radio, DiagnosticStatusCountsDecodedOutcomes)
{
    switch_ready();
    const uint16_t reported[] = {0, ETIMEDOUT, ECANCELED, EPROTO};
    for (int32_t status = SWITCH_A3_TX_SENT; status <= SWITCH_A3_TX_FAILED; ++status) {
        const int id = switch_send();
        ASSERT_GT(id, 0);
        switch_drain();
        switch_complete(switch_firmware.dma.back(), 0, status);
        unsigned done = 0;
        for (const auto &event : switch_firmware.events)
            done += event.type == SWLAN_AWDL_TXDONE && event.id == static_cast<uint32_t>(id) &&
                    event.status == reported[status - SWITCH_A3_TX_SENT];
        EXPECT_EQ(done, 1u) << status;
    }
    const auto &diagnostic = switch_a3_host::gDiagnostics;
    EXPECT_EQ(diagnostic.tx_records, 4u);
    EXPECT_EQ(diagnostic.tx_rejected, 0u);
    EXPECT_EQ(diagnostic.tx_sent, 1u);
    EXPECT_EQ(diagnostic.tx_expired, 1u);
    EXPECT_EQ(diagnostic.tx_cancelled, 1u);
    EXPECT_EQ(diagnostic.tx_failed, 1u);
    EXPECT_EQ(diagnostic.tx_last_status, static_cast<uint32_t>(SWITCH_A3_TX_FAILED));
}

TEST_F(SwitchA3Radio, InvalidDiagnosticSuffixDoesNotInvalidateClock)
{
    switch_ready();
    for (unsigned fault = 0; fault < 7; ++fault) {
        SCOPED_TRACE(fault);
        switch_a3_host::gDiagnostics = {};
        switch_firmware.diagnostic_bytes = fault == 0 ? 0 : fault == 1 ? 5 : sizeof(struct switch_a3_diagnostic);
        switch_firmware.diagnostic_modify = [fault](struct switch_a3_diagnostic &snapshot) {
            if (fault == 2)
                ++snapshot.version;
            if (fault == 3)
                ++snapshot.session;
            if (fault == 4)
                ++snapshot.clock_epoch;
            if (fault == 5)
                ++snapshot.request_id;
            if (fault == 6)
                ++snapshot.state_revision;
        };
        switch_test_log.clear();
        ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
        switch_drain();
        EXPECT_TRUE(switch_state().ready);
        unsigned missing = 0, summaries = 0;
        for (const auto &line : switch_test_log) {
            missing += line.find("A3 diagnostic unavailable:") != std::string::npos;
            summaries += line.find("A3 diagnostic TX:") != std::string::npos;
        }
        EXPECT_EQ(missing, 1u);
        EXPECT_EQ(summaries, 0u);
        switch_test_now_ns += 30000000000ULL;
    }
}

TEST_F(SwitchA3Radio, FailedClockReplyStillExportsCausalDiagnostic)
{
    switch_ready();
    switch_firmware.clock_failure = -1;
    switch_firmware.diagnostic_modify = [](struct switch_a3_diagnostic &snapshot) {
        snapshot.first_fault.sequence = 1;
        snapshot.first_fault.reason = SWITCH_A3_FAULT_CLOCK_UNSTABLE;
        snapshot.first_fault.native_error = -1;
        snapshot.pm_status = 0x80;
        snapshot.pm_reason = SWITCH_A3_PM_STATUS_REJECTED;
    };
    switch_test_log.clear();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    unsigned causes = 0, pm = 0;
    for (const auto &line : switch_test_log) {
        causes += line.find("A3 diagnostic cause first:") != std::string::npos;
        pm += line.find("status 0x80 reason 3") != std::string::npos;
    }
    EXPECT_EQ(causes, 1u);
    EXPECT_EQ(pm, 1u);
}

TEST_F(SwitchA3Radio, StaleDiagnosticReplyDoesNotConsumeNewSessionBudget)
{
    switch_ready();
    bool replaced = false;
    switch_firmware.after_reply = [&](const switch_a3_control &request) {
        if (request.operation == SWITCH_A3_GET_CLOCK && !replaced) {
            replaced = true;
            switch_wlan_a3_radio_lost();
            switch_firmware.applied = switch_firmware.bound_generation = 0;
            switch_firmware.state = SWITCH_A3_STOPPED;
            switch_attach();
        }
    };
    switch_test_log.clear();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_TRUE(replaced);
    EXPECT_EQ(switch_a3_host::gDiagnostics.requests, 0u);
    for (const auto &line : switch_test_log)
        EXPECT_EQ(line.find("A3 diagnostic TX:"), std::string::npos);
    switch_firmware.after_reply = {};
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    ASSERT_TRUE(switch_state().ready);
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_a3_host::gDiagnostics.requests, 1u);
}

TEST_F(SwitchA3Radio, UnavailableClockCoalescesRequestsAndCanBeRetried)
{
    switch_firmware.delay_ns = 3000000;
    switch_attach();
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    const auto commands = switch_firmware.controls.size();
    EXPECT_EQ(switch_state().error, 0);
    EXPECT_FALSE(switch_state().ready);
    for (unsigned i = 0; i < 10; ++i)
        EXPECT_EQ(switch_wlan_a3_radio_clock(), 0);
    switch_drain();
    EXPECT_EQ(switch_firmware.controls.size(), commands + 3);
    switch_firmware.delay_ns = 100000;
    ASSERT_EQ(switch_filter(), 0);
    ASSERT_EQ(switch_schedule(), 0);
    switch_drain();
    EXPECT_TRUE(switch_state().ready);
}

TEST_F(SwitchA3Radio, TerminalTransportResourceFailureRequestsRecovery)
{
    switch_ready();
    switch_firmware.transport_idle = -EIO;
    ASSERT_EQ(switch_refilter(), 0);
    switch_drain();
    EXPECT_FALSE(switch_state().ready);
    EXPECT_EQ(switch_filter(), -ENXIO);
    unsigned recovery = 0;
    for (const auto &event : switch_firmware.events)
        recovery += event.type == SWLAN_AWDL_RECOVERY_REQUIRED;
    EXPECT_EQ(recovery, 1u);
}

TEST_F(SwitchA3Radio, DevelopmentQualificationIsBoundedAndDoesNotAdvertiseFeatures)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_firmware.features = 0;
        switch_attach();
        ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
        switch_drain();
        switch_firmware.controls.clear();
        const uint8_t mac[6] = {2, 3, 4, 5, 6, 7};
        switch_wlan_a3_probe(mac, 1);
        switch_drain();
        ASSERT_EQ(switch_firmware.controls.size(), 6u);
        for (const auto &request : switch_firmware.controls)
            EXPECT_NE(request.operation, SWITCH_A3_START);
        EXPECT_EQ(switch_firmware.deadline_reads, 0u);
        EXPECT_FALSE(switch_state().ready);
        swlan_awdl_caps caps = {};
        ASSERT_TRUE(switch_wlan_a3_radio_caps(&caps));
        EXPECT_FALSE(caps.can_monitor || caps.can_inject || caps.can_scheduled_hops || caps.can_stay_assoc);
        switch_test_now_ns += 200000000;
        switch_drain();
        EXPECT_EQ(switch_firmware.deadline_reads, 1u);
        switch_test_now_ns += 2000000000;
        switch_drain();
        EXPECT_EQ(switch_firmware.deadline_reads, 1u);
        unsigned fired = 0;
        for (const auto &line : switch_test_log)
            fired += line.find("deadline fired 1") != std::string::npos;
        EXPECT_EQ(fired, 1u);
        std::vector<uint8_t> raw(sizeof(switch_a3_raw_rx) + 28);
        switch_a3_raw_rx header = {};
        header.magic = SWITCH_A3_RAW_RX_MAGIC;
        header.version = SWITCH_A3_RAW_RX_VERSION;
        header.header_bytes = sizeof header;
        header.frame_bytes = 28;
        header.flags = SWITCH_A3_RX_FCS_PRESENT;
        header.session = switch_state().generation + 1;
        header.clock_epoch = switch_state().clock.epoch;
        header.config_generation = 1;
        header.tsf = switch_state().clock.tsf_us;
        memcpy(raw.data(), &header, sizeof header);
        switch_wlan_a3_probe_rx(raw.data(), raw.size());
        --header.session;
        memcpy(raw.data(), &header, sizeof header);
        for (unsigned i = 0; i < 8; ++i)
            switch_wlan_a3_probe_rx(raw.data(), raw.size());
        unsigned received = 0;
        for (const auto &line : switch_test_log)
            received += line.find("qualification: raw RX") != std::string::npos;
        EXPECT_EQ(received, 4u);
    });
}

TEST_F(SwitchA3Radio, DevelopmentQualificationStopsAfterReceiveFailure)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_attach();
        ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
        switch_drain();
        switch_firmware.controls.clear();
        switch_firmware.receive_failure = -2;
        const uint8_t mac[6] = {2, 3, 4, 5, 6, 7};
        switch_wlan_a3_probe(mac, 1);
        switch_drain();
        ASSERT_EQ(switch_firmware.controls.size(), 4u);
        EXPECT_EQ(switch_firmware.controls.back().operation, SWITCH_A3_STOP);
        switch_test_now_ns += 1000000000;
        switch_drain();
        EXPECT_EQ(switch_firmware.deadline_reads, 0u);
    });
}

TEST_F(SwitchA3Radio, DevelopmentQualificationRejectsStaleDeadlineSnapshot)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_attach();
        ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
        switch_drain();
        const uint8_t mac[6] = {2, 3, 4, 5, 6, 7};
        switch_wlan_a3_probe(mac, 1);
        switch_drain();
        switch_firmware.bad_snapshot = true;
        switch_test_now_ns += 200000000;
        switch_drain();
        unsigned rejected = 0;
        for (const auto &line : switch_test_log)
            rejected += line.find("deadline snapshot rejected") != std::string::npos;
        EXPECT_EQ(rejected, 1u);
    });
}
#include "switch_wlan_a3_schedule_cases.h"
#include "switch_wlan_a3_clock_cases.h"
}

uint16_t swlan_awdl_infra_freq(void)
{
    return 2437;
}

int switch_wlan_a3_control(const void *data, size_t length, void *out, size_t capacity, size_t *returned)
{
    EXPECT_FALSE(switch_test_core_lock);
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    switch_a3_control request;
    memcpy(&request, data, sizeof request);
    EXPECT_EQ(length, request.total_bytes);
    switch_firmware.controls.push_back(request);
    if (switch_firmware.during_control)
        switch_firmware.during_control(request);
    if (request.operation == SWITCH_A3_CONFIGURE && request.session != switch_state().generation)
        return -ENXIO;
    switch_a3_reply reply = {};
    reply.magic = SWITCH_A3_REPLY_MAGIC;
    reply.version = SWITCH_A3_WIRE_VERSION;
    reply.header_bytes = sizeof reply;
    reply.total_bytes = sizeof reply;
    reply.request_id = request.request_id;
    reply.session = request.session;
    if (request.operation == SWITCH_A3_GET_CLOCK && switch_firmware.clock_failure)
        reply.status = switch_firmware.clock_failure;
    else if (request.operation == SWITCH_A3_CONFIGURE && switch_firmware.failure)
        reply.status = switch_firmware.failure;
    else if (request.operation == SWITCH_A3_TEST_RECEIVE)
        reply.status = switch_firmware.receive_failure;
    else if (request.operation == SWITCH_A3_STOP && switch_firmware.stop_failure) {
        reply.status = switch_firmware.stop_failure;
        switch_firmware.applied = 0;
        switch_firmware.state = SWITCH_A3_FAULTED;
        switch_firmware.fault = switch_firmware.stop_failure;
    } else if (request.operation == SWITCH_A3_INSTALL_SCHEDULE && switch_firmware.schedule_failure) {
        reply.status = switch_firmware.schedule_failure;
    } else if (request.operation == SWITCH_A3_INSTALL_SCHEDULE && switch_firmware.schedule_busy) {
        --switch_firmware.schedule_busy;
        reply.status = -16;
    } else if (request.operation == SWITCH_A3_CONFIGURE || request.operation == SWITCH_A3_INSTALL_SCHEDULE) {
        switch_firmware.applied = request.config_generation;
        switch_firmware.state_generation = request.config_generation;
        switch_firmware.bound_generation = switch_firmware.state == SWITCH_A3_RUNNING
                                               ? request.config_generation : 0;
        switch_firmware.fault = 0;
        if (request.operation == SWITCH_A3_INSTALL_SCHEDULE) {
            switch_a3_schedule schedule = {};
            memcpy(&schedule, static_cast<const uint8_t *>(data) + sizeof request, sizeof schedule);
            switch_firmware.schedules.push_back(schedule);
        }
    } else if (request.operation == SWITCH_A3_BIND_RINGS) {
        switch_a3_ring_binding binding = {};
        if (length != sizeof request + sizeof binding)
            reply.status = -2;
        else {
            memcpy(&binding, static_cast<const uint8_t *>(data) + sizeof request, sizeof binding);
            switch_firmware.bindings.push_back(binding);
            reply.status = switch_firmware.bind_failure;
            if (!reply.status && binding.first_ring && binding.ring_count && binding.max_frame == 1800 &&
                binding.credits && !binding.reserved && !binding.reserved2 &&
                request.config_generation == switch_firmware.applied)
                switch_firmware.bound_generation = request.config_generation;
            else if (!reply.status)
                reply.status = -2;
        }
    } else if (request.operation == SWITCH_A3_START) {
        if (!switch_firmware.bound_generation || switch_firmware.bound_generation != request.config_generation)
            reply.status = -25;
        else
            switch_firmware.state = SWITCH_A3_RUNNING;
    } else if (request.operation == SWITCH_A3_STOP) {
        switch_firmware.state = SWITCH_A3_STOPPED;
        switch_firmware.applied = 0;
        switch_firmware.bound_generation = 0;
        switch_firmware.fault = 0;
    } else if (request.operation == SWITCH_A3_RESET_CLOCK) {
        ++switch_firmware.epoch;
        switch_firmware.applied = 0;
        switch_firmware.state = SWITCH_A3_STOPPED;
    }
    reply.clock_epoch = switch_firmware.epoch;
    reply.applied_generation = switch_firmware.applied;
    reply.state_generation = reply.applied_generation ? reply.applied_generation : switch_firmware.state_generation;
    reply.state_revision = ++switch_firmware.revision;
    reply.fault_status = switch_firmware.fault;
    reply.legal_social_channels = switch_firmware.legal_social_channels;
    reply.state = switch_firmware.state;
    reply.features = switch_firmware.features;
    reply.tx_credits = switch_firmware.credits;
    reply.firmware_generation = 1;
    if (request.operation == SWITCH_A3_TEST_DEADLINE) {
        memcpy(&switch_firmware.snapshot.requested_tsf, static_cast<const uint8_t *>(data) + sizeof request, 8);
        switch_firmware.snapshot.actual_tsf = switch_firmware.snapshot.requested_tsf + 5;
        switch_firmware.snapshot.session_generation = request.session;
        switch_firmware.snapshot.clock_generation = reply.clock_epoch;
        switch_firmware.snapshot.fired = 1;
    }
    switch_test_now_ns += switch_firmware.delay_ns / 2;
    reply.tsf = switch_test_now_ns / 1000 + switch_firmware.tsf_offset;
    switch_test_now_ns += switch_firmware.delay_ns / 2;
    EXPECT_GE(capacity, sizeof reply);
    memcpy(out, &reply, sizeof reply);
    *returned = sizeof reply;
    if ((request.operation == SWITCH_A3_GET_CLOCK || request.operation == SWITCH_A3_GET_STATE) &&
        capacity >= sizeof reply + sizeof(struct switch_a3_diagnostic)) {
        ++switch_firmware.diagnostic_calls;
        struct switch_a3_diagnostic snapshot = {};
        snapshot.magic = SWITCH_A3_DIAGNOSTIC_MAGIC;
        snapshot.version = SWITCH_A3_DIAGNOSTIC_VERSION;
        snapshot.bytes = sizeof snapshot;
        snapshot.session = reply.session;
        snapshot.clock_epoch = reply.clock_epoch;
        snapshot.request_id = reply.request_id;
        snapshot.state_revision = reply.state_revision;
        if (switch_firmware.diagnostic_modify)
            switch_firmware.diagnostic_modify(snapshot);
        const size_t bytes = switch_firmware.diagnostic_bytes < sizeof snapshot
                                 ? switch_firmware.diagnostic_bytes : sizeof snapshot;
        memcpy(static_cast<uint8_t *>(out) + sizeof reply, &snapshot, bytes);
        *returned += bytes;
    }
    if (switch_firmware.after_reply)
        switch_firmware.after_reply(request);
    return 0;
}

int switch_wlan_a3_dma_tx(const uint8_t *packet, size_t length)
{
    EXPECT_FALSE(switch_test_core_lock);
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    if (switch_firmware.during_dma)
        switch_firmware.during_dma();
    const auto state = switch_state();
    switch_a3_tx_header tx = {};
    if (!state.ready || !switch_a3_tx_decode(packet, length, state.generation, state.clock.epoch, &tx) ||
        !switch_wlan_a3_radio_tx_allowed(packet, length))
        return -EAGAIN;
    if (switch_firmware.dma_refusals) {
        --switch_firmware.dma_refusals;
        if (switch_firmware.signal_dma_inside)
            switch_wlan_a3_radio_dma_ready();
        return -ENOBUFS;
    }
    switch_firmware.dma.emplace_back(packet, packet + length);
    return 0;
}

int switch_wlan_a3_transport_idle(void)
{
    EXPECT_FALSE(switch_test_core_lock);
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    ++switch_firmware.transport_idle_calls;
    return switch_firmware.transport_idle;
}

void switch_wlan_a3_notify(const swlan_awdl_event *event)
{
    EXPECT_FALSE(switch_test_core_lock);
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    switch_firmware.events.push_back(*event);
}

int switch_wlan_a3_tx_probe_claim(uint32_t, uint32_t)
{
    ADD_FAILURE() << "TX qualification was not prepared";
    return -ENXIO;
}

int switch_wlan_a3_tx_probe_transmit(const uint8_t *, size_t)
{
    ADD_FAILURE() << "TX qualification was not prepared";
    return -ENXIO;
}

int switch_wlan_a3_tx_probe_cleanup(uint32_t, uint32_t, unsigned, switch_wlan_a3_probe_ownership *)
{
    ADD_FAILURE() << "TX qualification was not prepared";
    return -ENXIO;
}

int swlan_awdl_var(int, uint8_t, const char *name, const void *, size_t, void *out, size_t capacity, size_t *returned)
{
    EXPECT_STREQ(name, SWITCH_A3_DEADLINE_IOVAR);
    EXPECT_FALSE(switch_test_core_lock);
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    EXPECT_GE(capacity, sizeof(switch_a3_deadline_snapshot));
    auto snapshot = switch_firmware.snapshot;
    if (switch_firmware.bad_snapshot)
        ++snapshot.session_generation;
    memcpy(out, &snapshot, sizeof snapshot);
    *returned = sizeof snapshot;
    ++switch_firmware.deadline_reads;
    return 0;
}
