// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_wlan_a3_radio_stubs.h"
#include "../SwitchWLANA3Probe.h"
#include "../SwitchWLANA3Radio.h"
#include "../SwitchWLANA3TxProbe.h"
#include "../SwitchWLANA3Wire.h"
#include "../awdl/awdl.h"
#include <gtest/gtest.h>
#include "bazel/testing/fresh_process.h"

namespace {
struct Firmware {
    uint32_t session = 0, epoch = 1, generation = 0, state = SWITCH_A3_STOPPED;
    uint32_t revision = 0;
    uint32_t failOperation = 0;
    uint16_t channel = 0xc024;
    int stopFailure = 0, transmitFailure = 0;
    unsigned outstanding = 0, closes = 0, recoveries = 0, snapshotReads = 0, snapshotFault = 0;
    bool badGeneration = false, badSnapshot = false, mutateAuthorization = false;
    bool ringsClosed = true, autoClose = true, foreignOpen = false, halted = false;
    switch_wlan_a3_probe_lease lease;
    switch_a3_deadline_snapshot snapshot = {};
    switch_a3_ring_binding binding = {};
    switch_a3_configuration configuration = {};
    switch_a3_schedule schedule = {};
    std::vector<switch_a3_control> controls;
    std::vector<uint8_t> envelope;
} switch_firmware;

void drain()
{
    unsigned turns = 0;
    while (switch_test_run_one())
        ASSERT_LT(++turns, 100u);
}

void prepare(bool stageOne = false)
{
    const uint8_t address[6] = {2, 3, 4, 5, 6, 7};
    switch_a3_caps caps = {};
    caps.magic = SWITCH_A3_CAPS_MAGIC;
    caps.version = SWITCH_A3_CAPS_VERSION;
    caps.size = sizeof caps;
    caps.status = SWITCH_A3_ATTACHED;
    switch_wlan_a3_radio_attach(&caps, address, 0, 3, 4);
    ASSERT_EQ(switch_wlan_a3_radio_clock(), 0);
    drain();
    switch_firmware.controls.clear();
    switch_wlan_a3_tx_probe_prepare(address, 0, 3, 4);
    if (stageOne)
        switch_wlan_a3_probe(address, 0);
    else
        switch_wlan_a3_tx_probe_start();
    drain();
}

unsigned operations(uint32_t operation)
{
    unsigned count = 0;
    for (const auto &request : switch_firmware.controls)
        count += request.operation == operation;
    return count;
}

unsigned logCount(const char *needle)
{
    unsigned count = 0;
    for (const auto &line : switch_test_log)
        count += line.find(needle) != std::string::npos;
    return count;
}

switch_a3_tx_status completion(int result = SWITCH_A3_TX_SENT)
{
    switch_a3_tx_header tx = {};
    memcpy(&tx, switch_firmware.envelope.data() + 14, sizeof tx);
    switch_a3_tx_status status = {};
    status.magic = SWITCH_A3_TX_STATUS_MAGIC;
    status.version = SWITCH_A3_WIRE_VERSION;
    status.header_bytes = sizeof status;
    status.total_bytes = sizeof status;
    status.session = tx.session;
    status.clock_epoch = tx.clock_epoch;
    status.schedule_generation = tx.schedule_generation;
    status.request_id = tx.request_id;
    status.status = result;
    status.credits = 1;
    status.tsf = 12345;
    return status;
}

void observe(const switch_a3_tx_status &status)
{
    switch_wlan_a3_tx_probe_observe(reinterpret_cast<const uint8_t *>(&status), sizeof status);
    EXPECT_TRUE(switch_wlan_a3_radio_frame(reinterpret_cast<const uint8_t *>(&status), sizeof status));
    drain();
}

void retireDma()
{
    switch_firmware.outstanding = 0;
    switch_wlan_a3_tx_probe_dma(0, switch_firmware.ringsClosed);
    drain();
}

void remainsUnavailable()
{
    swlan_awdl_state state = {};
    ASSERT_EQ(switch_wlan_a3_radio_state(&state), 0);
    EXPECT_FALSE(state.ready);
    swlan_awdl_caps caps = {};
    ASSERT_TRUE(switch_wlan_a3_radio_caps(&caps));
    EXPECT_FALSE(caps.can_monitor || caps.can_inject || caps.can_scheduled_hops || caps.can_stay_assoc);
}

TEST(SwitchA3TxProbe, OnePsfUsesCurrentChannelActualRingsAndCorrelatedRadioStatus)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_firmware.mutateAuthorization = true;
        prepare(true);
        EXPECT_TRUE(switch_firmware.envelope.empty());
        switch_test_now_ns += 200000000;
        drain();
        ASSERT_FALSE(switch_firmware.envelope.empty());
        switch_a3_tx_header header = {};
        swlan_awdl_state state = {};
        ASSERT_EQ(switch_wlan_a3_radio_state(&state), 0);
        ASSERT_TRUE(switch_a3_tx_decode(switch_firmware.envelope.data(), switch_firmware.envelope.size(),
                                        state.generation, state.clock.epoch, &header));
        EXPECT_EQ(header.chanspec, switch_firmware.channel);
        EXPECT_EQ(header.schedule_generation, 2u);
        EXPECT_EQ(header.flags, SWITCH_A3_TX_NO_ACK | SWITCH_A3_TX_ON_CHANNEL_ONLY);
        EXPECT_EQ(header.rate, 12u);
        EXPECT_EQ(header.expiry_tsf - header.earliest_tsf, 500000u);
        EXPECT_EQ(switch_firmware.binding.first_ring, 3u);
        EXPECT_EQ(switch_firmware.binding.ring_count, 4u);
        EXPECT_EQ(switch_firmware.binding.max_frame, 1800u);
        EXPECT_EQ(switch_firmware.binding.credits, 1u);
        EXPECT_EQ(switch_firmware.schedule.infra_chanspec, switch_firmware.channel);
        EXPECT_EQ(switch_firmware.schedule.slot_count, 1u);
        EXPECT_EQ(switch_firmware.schedule.slots[0].chanspec, switch_firmware.channel);
        EXPECT_EQ(switch_firmware.schedule.slots[0].dwell_us, 500000u);
        awdl_af frame = {};
        ASSERT_EQ(awdl_af_parse(switch_firmware.envelope.data() + 14 + sizeof header, header.frame_bytes, &frame), 0);
        EXPECT_EQ(frame.subtype, AWDL_SUBTYPE_PSF);
        EXPECT_EQ(frame.sync.tx_channel, 36u);
        EXPECT_EQ(operations(SWITCH_A3_START), 1u);
        remainsUnavailable();
        observe(completion());
        EXPECT_EQ(switch_firmware.closes, 1u);
        EXPECT_EQ(logCount("DMA retired 1"), 0u);
        EXPECT_EQ(switch_firmware.recoveries, 0u);
        retireDma();
        EXPECT_EQ(logCount("result 0 status 0 flags 0 TSF unavailable, DMA retired 1 rings closed 1, recovery 0"), 1u);
        EXPECT_EQ(operations(SWITCH_A3_STOP), 2u);
        switch_wlan_a3_tx_probe_start();
        observe(completion());
        switch_test_now_ns += 2000000000;
        drain();
        EXPECT_EQ(operations(SWITCH_A3_START), 1u);
        EXPECT_EQ(operations(SWITCH_A3_STOP), 2u);
        remainsUnavailable();
    });
}

TEST(SwitchA3TxProbe, StaleCompletionsCannotCompleteTheOneRequest)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        prepare();
        ASSERT_FALSE(switch_firmware.envelope.empty());
        for (unsigned field = 0; field < 4; ++field) {
            auto status = completion();
            if (field == 0)
                ++status.session;
            else if (field == 1)
                ++status.clock_epoch;
            else if (field == 2)
                ++status.schedule_generation;
            else
                ++status.request_id;
            observe(status);
        }
        EXPECT_EQ(switch_firmware.closes, 0u);
        observe(completion(SWITCH_A3_TX_FAILED));
        retireDma();
        EXPECT_EQ(logCount("result -5 status 3"), 1u);
        EXPECT_EQ(switch_firmware.recoveries, 0u);
    });
}

TEST(SwitchA3TxProbe, DmaRetirementWithoutRadioStatusTimesOutAndRecoversOnce)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        prepare();
        ASSERT_FALSE(switch_firmware.envelope.empty());
        retireDma();
        EXPECT_EQ(switch_firmware.closes, 0u);
        switch_test_now_ns += 500000000;
        drain();
        EXPECT_EQ(switch_firmware.closes, 1u);
        EXPECT_EQ(switch_firmware.recoveries, 1u);
        EXPECT_EQ(logCount("result -60 status -1"), 1u);
        observe(completion());
        switch_test_now_ns += 2000000000;
        drain();
        EXPECT_EQ(switch_firmware.recoveries, 1u);
        EXPECT_EQ(operations(SWITCH_A3_STOP), 2u);
        remainsUnavailable();
    });
}

TEST(SwitchA3TxProbe, RadioCompletionDoesNotHideMissingDmaRetirement)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        prepare();
        observe(completion());
        switch_test_now_ns += 500000000;
        drain();
        EXPECT_EQ(switch_firmware.recoveries, 1u);
        EXPECT_EQ(logCount("DMA retired 0 rings closed 1, recovery 1"), 1u);
    });
}

TEST(SwitchA3TxProbe, MissingRingDeleteCompletionIsNotDmaRetirement)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        prepare();
        switch_firmware.autoClose = false;
        retireDma();
        observe(completion());
        EXPECT_EQ(switch_firmware.recoveries, 0u);
        switch_test_now_ns += 500000000;
        drain();
        EXPECT_EQ(switch_firmware.recoveries, 1u);
        EXPECT_EQ(logCount("DMA retired 1 rings closed 0, recovery 1"), 1u);
    });
}

TEST(SwitchA3TxProbe, SameSessionClockChangeDiscardsOldCleanup)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        prepare();
        switch_wlan_a3_radio_power(false);
        ++switch_firmware.epoch;
        switch_wlan_a3_radio_power(true);
        drain();
        EXPECT_TRUE(switch_firmware.lease.claim(switch_firmware.session, switch_firmware.epoch, false));
        const auto stops = operations(SWITCH_A3_STOP);
        observe(completion());
        EXPECT_EQ(switch_firmware.closes, 0u);
        EXPECT_EQ(switch_firmware.recoveries, 0u);
        EXPECT_EQ(operations(SWITCH_A3_STOP), stops);
        EXPECT_EQ(logCount("old cleanup discarded"), 1u);
        EXPECT_TRUE(switch_firmware.lease.current(switch_firmware.session, switch_firmware.epoch));
    });
}

TEST(SwitchA3TxProbe, DmaRefusalCancelsWithoutWaitingForACompletion)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_firmware.transmitFailure = -ENOBUFS;
        prepare();
        EXPECT_TRUE(switch_firmware.envelope.empty());
        EXPECT_EQ(switch_firmware.closes, 1u);
        EXPECT_EQ(switch_firmware.recoveries, 0u);
        EXPECT_EQ(logCount("status -1 flags 0 TSF unavailable, DMA retired 1 rings closed 1, recovery 0"), 1u);
    });
}

TEST(SwitchA3TxProbe, StopFailureRequiresRecoveryAfterRealCompletion)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        prepare();
        switch_firmware.stopFailure = -16;
        retireDma();
        observe(completion());
        EXPECT_EQ(switch_firmware.recoveries, 1u);
        EXPECT_EQ(operations(SWITCH_A3_STOP), 2u);
        EXPECT_EQ(logCount("DMA retired 1 rings closed 1, recovery 1"), 1u);
        bool open = false;
        const switch_wlan_a3_probe_driver_state pendingRecovery = {
            switch_firmware.session, switch_firmware.epoch, true, true, false, switch_firmware.halted, false};
        EXPECT_FALSE(switch_wlan_a3_probe_mark_open(switch_firmware.lease, pendingRecovery, open));
        EXPECT_FALSE(open);
        EXPECT_TRUE(switch_firmware.lease.current(switch_firmware.session, switch_firmware.epoch));
        auto nextLifetime = pendingRecovery;
        ++nextLifetime.session;
        nextLifetime.halted = false;
        EXPECT_TRUE(switch_wlan_a3_probe_mark_open(switch_firmware.lease, nextLifetime, open));
    });
}

TEST(SwitchA3TxProbe, FailedConfigurationAndBindingNeverSubmit)
{
    for (const auto operation : {SWITCH_A3_CONFIGURE, SWITCH_A3_BIND_RINGS}) {
        switch_ios::test::ExpectPassesInFreshProcess([operation] {
            switch_firmware.failOperation = operation;
            prepare();
            EXPECT_TRUE(switch_firmware.envelope.empty());
            EXPECT_EQ(operations(SWITCH_A3_START), 0u);
            EXPECT_EQ(switch_firmware.closes, 1u);
            EXPECT_EQ(switch_firmware.recoveries, 0u);
            switch_test_now_ns += 2000000000;
            drain();
            EXPECT_EQ(operations(SWITCH_A3_STOP), 2u);
            EXPECT_EQ(switch_firmware.lease.session, 0u);
        });
    }
}

TEST(SwitchA3TxProbe, StaleControlGenerationNeverBindsOrSubmits)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_firmware.badGeneration = true;
        prepare();
        EXPECT_EQ(operations(SWITCH_A3_BIND_RINGS), 0u);
        EXPECT_EQ(operations(SWITCH_A3_START), 0u);
        EXPECT_TRUE(switch_firmware.envelope.empty());
    });
}

TEST(SwitchA3TxProbe, InvalidStageOneDeadlineDoesNotStartCandidateOperations)
{
    for (unsigned fault = 1; fault <= 7; ++fault) {
        switch_ios::test::ExpectPassesInFreshProcess([fault] {
            switch_firmware.snapshotFault = fault;
            prepare(true);
            switch_test_now_ns += 200000000;
            drain();
            EXPECT_EQ(operations(SWITCH_A3_INSTALL_SCHEDULE), 0u);
            EXPECT_EQ(operations(SWITCH_A3_BIND_RINGS), 0u);
            EXPECT_EQ(operations(SWITCH_A3_START), 0u);
            EXPECT_TRUE(switch_firmware.envelope.empty());
        });
    }
}

TEST(SwitchA3TxProbe, ForeignOpenCannotBeMutatedAndCurrentLeaseBlocksAConcurrentOpen)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_firmware.foreignOpen = true;
        prepare();
        EXPECT_EQ(operations(SWITCH_A3_STOP), 0u);
        EXPECT_EQ(operations(SWITCH_A3_CONFIGURE), 0u);
        EXPECT_EQ(switch_firmware.closes, 0u);
        EXPECT_TRUE(switch_firmware.envelope.empty());
    });
    switch_ios::test::ExpectPassesInFreshProcess([] {
        prepare();
        EXPECT_TRUE(switch_firmware.lease.current(switch_firmware.session, switch_firmware.epoch));
        EXPECT_FALSE(switch_firmware.lease.claim(switch_firmware.session, switch_firmware.epoch, false));
        retireDma();
        observe(completion());
        EXPECT_FALSE(switch_firmware.lease.current(switch_firmware.session, switch_firmware.epoch));
    });
}

TEST(SwitchA3TxProbe, OpenRechecksPowerAndRecoveryAtTheActualMark)
{
    switch_wlan_a3_probe_lease lease;
    const switch_wlan_a3_probe_driver_state initial = {1, 1, true, true, false, false, false};
    const bool earlierUp = initial.up && initial.power;
    ASSERT_TRUE(earlierUp);
    for (unsigned fault = 0; fault < 4; ++fault) {
        auto changed = initial;
        if (fault == 0)
            changed.up = false;
        else if (fault == 1)
            changed.power = false;
        else if (fault == 2)
            changed.gone = true;
        else
            changed.halted = true;
        bool open = false;
        EXPECT_FALSE(switch_wlan_a3_probe_mark_open(lease, changed, open));
        EXPECT_FALSE(open);
    }
}

TEST(SwitchA3TxProbe, QueuedControlCannotOutliveItsLeaseAtSubmission)
{
    switch_wlan_a3_probe_lease lease;
    ASSERT_TRUE(lease.claim(1, 1, false));
    switch_a3_control stop = {};
    stop.operation = SWITCH_A3_STOP;
    stop.session = stop.clock_epoch = 1;
    const switch_wlan_a3_probe_driver_state before = {1, 1, true, true, false, false, false};
    ASSERT_TRUE(switch_wlan_a3_probe_control_allowed(lease, before, stop));
    auto after = before;
    ++after.epoch;
    EXPECT_FALSE(switch_wlan_a3_probe_control_allowed(lease, after, stop));
    lease = {};
    EXPECT_FALSE(switch_wlan_a3_probe_control_allowed(lease, before, stop));
    ASSERT_TRUE(lease.claim(after.session, after.epoch, false));
    EXPECT_FALSE(switch_wlan_a3_probe_control_allowed(lease, after, stop));
    stop.clock_epoch = after.epoch;
    EXPECT_TRUE(switch_wlan_a3_probe_control_allowed(lease, after, stop));
    after.halted = true;
    EXPECT_FALSE(switch_wlan_a3_probe_control_allowed(lease, after, stop));
}
}

int switch_wlan_a3_control(const void *data, size_t length, void *out, size_t capacity, size_t *returned)
{
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    switch_a3_control request = {};
    memcpy(&request, data, sizeof request);
    EXPECT_EQ(length, request.total_bytes);
    if ((request.request_id & 0xf0000000u) == 0x90000000u) {
        swlan_awdl_state state = {};
        switch_wlan_a3_radio_state(&state);
        const switch_wlan_a3_probe_driver_state radio = {
            state.generation,       state.clock.epoch,          true, true, false,
            switch_firmware.halted, switch_firmware.foreignOpen};
        if (!switch_wlan_a3_probe_control_allowed(switch_firmware.lease, radio, request))
            return -ESTALE;
    }
    switch_firmware.controls.push_back(request);
    switch_a3_reply reply = {};
    reply.magic = SWITCH_A3_REPLY_MAGIC;
    reply.version = SWITCH_A3_WIRE_VERSION;
    reply.header_bytes = sizeof reply;
    reply.total_bytes = sizeof reply;
    reply.request_id = request.request_id;
    reply.session = switch_firmware.session ? switch_firmware.session : request.session;
    reply.clock_epoch = switch_firmware.epoch;
    reply.firmware_generation = 1;
    reply.state_revision = ++switch_firmware.revision;
    reply.legal_social_channels = 7;
    const uint8_t *payload = static_cast<const uint8_t *>(data) + sizeof request;
    if (request.operation == switch_firmware.failOperation)
        reply.status = -2;
    else if (request.operation == SWITCH_A3_CONFIGURE) {
        memcpy(&switch_firmware.configuration, payload, sizeof switch_firmware.configuration);
        switch_firmware.session = request.session;
        switch_firmware.generation = request.config_generation;
    } else if (request.operation == SWITCH_A3_INSTALL_SCHEDULE) {
        memcpy(&switch_firmware.schedule, payload, sizeof switch_firmware.schedule);
        switch_firmware.generation = request.config_generation;
    } else if (request.operation == SWITCH_A3_BIND_RINGS)
        memcpy(&switch_firmware.binding, payload, sizeof switch_firmware.binding);
    else if (request.operation == SWITCH_A3_START)
        switch_firmware.state = SWITCH_A3_RUNNING;
    else if (request.operation == SWITCH_A3_STOP) {
        reply.status = switch_firmware.stopFailure;
        if (!reply.status) {
            switch_firmware.state = SWITCH_A3_STOPPED;
            switch_firmware.generation = 0;
        }
    } else if (request.operation == SWITCH_A3_TEST_DEADLINE) {
        memcpy(&switch_firmware.snapshot.requested_tsf, payload, sizeof(uint64_t));
        switch_firmware.snapshot.actual_tsf = switch_firmware.snapshot.requested_tsf + 6;
        switch_firmware.snapshot.session_generation = request.session;
        switch_firmware.snapshot.clock_generation = request.clock_epoch;
        switch_firmware.snapshot.fired = 1;
    }
    reply.applied_generation = switch_firmware.generation;
    reply.state_generation = switch_firmware.generation;
    if (switch_firmware.badGeneration && request.operation == SWITCH_A3_INSTALL_SCHEDULE)
        --reply.applied_generation;
    reply.state = switch_firmware.state;
    reply.tx_credits = reply.state == SWITCH_A3_RUNNING ? 1 : 0;
    reply.tsf = switch_test_now_ns / 1000 + 1000000;
    switch_test_now_ns += 100000;
    EXPECT_GE(capacity, sizeof reply);
    memcpy(out, &reply, sizeof reply);
    *returned = sizeof reply;
    return 0;
}

int swlan_awdl_var(int, uint8_t, const char *name, const void *, size_t, void *out, size_t capacity, size_t *returned)
{
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    if (!strcmp(name, "chanspec")) {
        EXPECT_GE(capacity, 4u);
        const uint32_t channel = switch_firmware.channel;
        memcpy(out, &channel, sizeof channel);
        *returned = sizeof channel;
        return 0;
    }
    EXPECT_STREQ(name, SWITCH_A3_DEADLINE_IOVAR);
    auto snapshot = switch_firmware.snapshot;
    if (switch_firmware.badSnapshot || switch_firmware.snapshotFault == 6)
        snapshot.actual_tsf = snapshot.requested_tsf - 1;
    if (switch_firmware.snapshotFault == 1)
        ++snapshot.session_generation;
    if (switch_firmware.snapshotFault == 2)
        ++snapshot.clock_generation;
    if (switch_firmware.snapshotFault == 3)
        snapshot.armed = 1;
    if (switch_firmware.snapshotFault == 4)
        snapshot.fired = 0;
    if (switch_firmware.snapshotFault == 5)
        snapshot.status = -1;
    EXPECT_GE(capacity, sizeof snapshot);
    memcpy(out, &snapshot, sizeof snapshot);
    *returned = sizeof snapshot;
    if (switch_firmware.snapshotFault == 7)
        --*returned;
    ++switch_firmware.snapshotReads;
    return 0;
}

int switch_wlan_a3_tx_probe_claim(uint32_t session, uint32_t epoch)
{
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    return switch_firmware.lease.claim(session, epoch, switch_firmware.foreignOpen) ? 0 : -EBUSY;
}

int switch_wlan_a3_tx_probe_transmit(const uint8_t *envelope, size_t length)
{
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    if (switch_firmware.transmitFailure)
        return switch_firmware.transmitFailure;
    if (switch_firmware.mutateAuthorization) {
        std::vector<uint8_t> changed(envelope, envelope + length);
        changed.back() ^= 1;
        EXPECT_FALSE(switch_wlan_a3_tx_probe_authorize(changed.data(), changed.size()));
    }
    if (!switch_wlan_a3_tx_probe_authorize(envelope, length))
        return -EPERM;
    EXPECT_FALSE(switch_wlan_a3_tx_probe_authorize(envelope, length));
    EXPECT_TRUE(switch_firmware.envelope.empty());
    switch_firmware.envelope.assign(envelope, envelope + length);
    switch_firmware.outstanding = 1;
    switch_firmware.ringsClosed = false;
    return 0;
}

int switch_wlan_a3_tx_probe_cleanup(uint32_t session, uint32_t epoch, unsigned flags,
                                    switch_wlan_a3_probe_ownership *ownership)
{
    EXPECT_EQ(switch_test_adapter_locks, 0u);
    swlan_awdl_state state = {};
    if (switch_wlan_a3_radio_state(&state) || session != state.generation || epoch != state.clock.epoch ||
        !switch_firmware.lease.current(session, epoch))
        return -ESTALE;
    switch_firmware.closes += !!(flags & SWITCH_A3_PROBE_CLOSE);
    if ((flags & SWITCH_A3_PROBE_CLOSE) && switch_firmware.autoClose)
        switch_firmware.ringsClosed = true;
    switch_firmware.recoveries += !!(flags & SWITCH_A3_PROBE_RECOVER);
    if (flags & SWITCH_A3_PROBE_RECOVER)
        switch_firmware.halted = true;
    else if ((flags & SWITCH_A3_PROBE_RELEASE) && !switch_firmware.outstanding && switch_firmware.ringsClosed)
        switch_firmware.lease = {};
    *ownership = {switch_firmware.outstanding, switch_firmware.ringsClosed};
    return 0;
}

int switch_wlan_a3_transport_idle(void)
{
    return 1;
}

int switch_wlan_a3_dma_tx(const uint8_t *, size_t)
{
    ADD_FAILURE() << "qualification entered the operational TX path";
    return -ENXIO;
}

void switch_wlan_a3_notify(const swlan_awdl_event *)
{}

uint16_t swlan_awdl_infra_freq()
{
    return 5180;
}
