// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "../SwitchDisplayMemoryEnvelope.h"
#include "../SwitchDisplayMemoryTiming.h"

namespace {
switch_display_memory_head mode()
{
    switch_display_memory_head result{};
    result.pixel_clock_hz = 78000000;
    result.active_width = 720;
    result.active_height = 1280;
    result.total_width = 1000;
    result.total_height = 1300;
    return result;
}

class DisplayMemoryEnvelope : public testing::Test {
protected:
    switch_display::MemoryEnvelope envelope;
    std::vector<switch_display_memory_head> requests;
    switch_display_memory_window updated[4] = {}, active[2] = {};
    int error = 0;

    void SetUp() override
    {
        ASSERT_TRUE(envelope.configure(mode(), 720, 1280, false));
    }

    int apply(const switch_display_memory_head *head)
    {
        requests.push_back(*head);
        return error;
    }

    bool admit(uint64_t peak)
    {
        return envelope.admit(updated, active, peak, [this](auto *h) {
            return apply(h);
        });
    }

    bool arm(uint64_t peak)
    {
        return envelope.arm(active, peak, [this](auto *h) {
            return apply(h);
        });
    }

    bool retire(uint64_t peak)
    {
        return envelope.retire(active, peak, [this](auto *h) {
            return apply(h);
        });
    }
};

TEST_F(DisplayMemoryEnvelope, UiAloneOnlyWhileNothingIsHeldSinceTheModeWasConfigured)
{
    ASSERT_TRUE(envelope.applyUiAlone([this](auto *h) {
        return apply(h);
    }));
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].peak_bytes_per_second, 312000000u);
    EXPECT_FALSE(requests[0].windows[1].active || requests[0].windows[2].active);
    EXPECT_TRUE(envelope.holding());
    ASSERT_TRUE(envelope.applyUiAlone([this](auto *h) {
        return apply(h);
    }));
    EXPECT_EQ(requests.size(), 1u);
    ASSERT_TRUE(envelope.configure(mode(), 720, 1280, false));
    EXPECT_FALSE(envelope.holding());
    ASSERT_TRUE(envelope.applyUiAlone([this](auto *h) {
        return apply(h);
    }));
    EXPECT_EQ(requests.size(), 2u);
}

TEST_F(DisplayMemoryEnvelope, EitherBankCanTakeAnyAcceptedLayerWithoutNewAdmission)
{
    updated[0] = {600000000, 20000, 8000, 1};
    updated[1] = {900000000, 12000, 4000, 1};
    ASSERT_TRUE(admit(1812000000));
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].bandwidth_envelope, 1u);
    for (unsigned i = 1; i != 3; ++i) {
        EXPECT_EQ(requests[0].windows[i].peak_bytes_per_second, 900000000u);
        EXPECT_EQ(requests[0].windows[i].required_buffering_bytes, 20000u);
        EXPECT_EQ(requests[0].windows[i].latency_buffering_bytes, 4000u);
    }
    active[0] = updated[1];
    active[1] = updated[0];
    ASSERT_TRUE(arm(1812000000));
    EXPECT_EQ(requests.size(), 1u);
}

TEST_F(DisplayMemoryEnvelope, InheritedAndPendingLayersSurviveLaterPartialUpdate)
{
    updated[0] = {700000000, 20000, 0, 1};
    ASSERT_TRUE(admit(1012000000));
    updated[0] = {};
    updated[2] = {800000000, 24000, 5000, 1};
    ASSERT_TRUE(admit(1812000000));
    EXPECT_EQ(requests.back().windows[1].required_buffering_bytes, 24000u);
    EXPECT_EQ(requests.back().windows[1].latency_buffering_bytes, 0u);
    EXPECT_EQ(requests.back().peak_bytes_per_second, 1812000000u);
}

TEST_F(DisplayMemoryEnvelope, RefusedGeometryDoesNotCommitQueueEnvelope)
{
    updated[0] = {900000000, 30000, 0, 1};
    error = -28;
    EXPECT_FALSE(admit(1212000000));
    EXPECT_EQ(envelope.reservation().peak_bytes_per_second, 0u);
    updated[0] = {400000000, 1000, 0, 1};
    error = 0;
    ASSERT_TRUE(admit(712000000));
    EXPECT_EQ(envelope.reservation().windows[1].required_buffering_bytes, 1000u);
}

TEST_F(DisplayMemoryEnvelope, OldDemandRetainedUntilExplicitLatchRetirement)
{
    updated[0] = {900000000, 30000, 0, 1};
    ASSERT_TRUE(admit(1212000000));
    ASSERT_TRUE(arm(312000000));
    EXPECT_EQ(requests.size(), 1u);
    error = -5;
    EXPECT_FALSE(retire(312000000));
    EXPECT_EQ(envelope.reservation().windows[1].active, 1u);
    error = 0;
    ASSERT_TRUE(retire(312000000));
    EXPECT_EQ(envelope.reservation().windows[1].active, 0u);
    EXPECT_EQ(envelope.reservation().peak_bytes_per_second, 312000000u);
}

TEST_F(DisplayMemoryEnvelope, UiReservationCoversPitchAndNativeBlockStorage)
{
    ASSERT_TRUE(arm(312000000));
    const auto &ui = requests.back().windows[0];
    EXPECT_EQ(ui.required_buffering_bytes, 720u * 4);
    EXPECT_EQ(ui.latency_buffering_bytes, 0u);
    EXPECT_EQ(ui.peak_bytes_per_second, 312000000u);
}

TEST_F(DisplayMemoryEnvelope, ResumeReappliesModeWithoutDroppingQueuedDemand)
{
    updated[0] = {900000000, 30000, 0, 1};
    ASSERT_TRUE(admit(1212000000));
    auto changed = mode();
    changed.total_height = 1310;
    ASSERT_TRUE(envelope.configure(changed, 720, 1280, false));
    ASSERT_TRUE(arm(312000000));
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests.back().total_height, 1310u);
    EXPECT_EQ(requests.back().windows[1].active, 1u);
    EXPECT_EQ(requests.back().peak_bytes_per_second, 1212000000u);
}

class TimingRegisters {
public:
    uint32_t access = 4;
    std::array<uint32_t, 0x500> active{}, programmed{};

    struct Register {
        TimingRegisters &regs;
        unsigned index;

        operator uint32_t() const
        {
            return index == 0x40 ? regs.access : (regs.access & 1 ? regs.active : regs.programmed)[index];
        }

        void operator=(uint32_t value)
        {
            ASSERT_EQ(index, 0x40u);
            regs.access = value;
        }
    };

    Register operator[](unsigned index)
    {
        return {*this, index};
    }
};

TEST(DisplayMemoryTiming, CapturesActiveTimingsAndRestoresRegisterBank)
{
    TimingRegisters regs;
    regs.active[0x409] = (1280u << 16) | 720;
    regs.active[0x407] = (1u << 16) | 72;
    regs.active[0x408] = (9u << 16) | 72;
    regs.active[0x40a] = (10u << 16) | 136;
    regs.programmed = regs.active;
    regs.programmed[0x409] = (800u << 16) | 600;
    switch_display_memory_head out{};
    ASSERT_TRUE(switch_display::memoryTiming(regs, 78000000, out));
    EXPECT_EQ(regs.access, 4u);
    EXPECT_EQ(out.total_width, 1000u);
    EXPECT_EQ(out.total_height, 1300u);
    EXPECT_EQ(out.pixel_clock_hz, 78000000u);
    regs.active[0x409] = 0;
    EXPECT_FALSE(switch_display::memoryTiming(regs, 78000000, out));
    EXPECT_EQ(regs.access, 4u);
    EXPECT_EQ(out.active_width, 720u);
}

TEST(DisplayMemoryTiming, ResumeCanReserveProgrammedModeBeforeItsGeneralCommit)
{
    TimingRegisters regs;
    regs.access = 5;
    regs.programmed[0x409] = (1280u << 16) | 720;
    regs.programmed[0x407] = (1u << 16) | 72;
    regs.programmed[0x408] = (9u << 16) | 72;
    regs.programmed[0x40a] = (10u << 16) | 136;
    switch_display_memory_head out{};
    ASSERT_TRUE(switch_display::memoryTiming(regs, 78000000, out, switch_display::TimingBank::Programmed));
    EXPECT_EQ(regs.access, 5u);
    EXPECT_EQ(out.total_width, 1000u);
    EXPECT_EQ(out.total_height, 1300u);
}
}
