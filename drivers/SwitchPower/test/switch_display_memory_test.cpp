// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include "../SwitchDisplayMemoryState.h"
using namespace switch_display;

namespace {
switch_display_memory_head ui(uint64_t bytes = 312000000)
{
    switch_display_memory_head result{};
    result.pixel_clock_hz = 78000000;
    result.active_width = 720;
    result.active_height = 1280;
    result.total_width = 800;
    result.total_height = 1320;
    result.peak_bytes_per_second = bytes;
    EXPECT_TRUE(memoryWindow(result.windows[0], bytes, 720, 1280, 720, 1280, false, false, false));
    return result;
}

TEST(DisplayMemory, BlankFillBandwidthUsesPixelDuration)
{
    EXPECT_EQ(memoryBlankBandwidth(100000, 100000000, 100000), 100u);
    EXPECT_EQ(memoryBlankBandwidth(100000, 200000000, 100000), 200u);
    EXPECT_EQ(memoryBlankBandwidth(100000, 100000000, 10000), 1000u);
    EXPECT_EQ(memoryBlankBandwidth(100001, 100000000, 100000), 101u);
}

TEST(DisplayMemory, MergedBandwidthBoundCoversEveryRelativeThreeWindowDemand)
{
    for (unsigned count = 2; count <= 3; ++count)
        for (uint64_t own = 1; own <= 128; ++own)
            for (uint64_t total = own; total <= own * count; ++total)
                for (uint32_t duration : {1u, 2000u, 1000000u}) {
                    const long double share = 1.1L * total / count;
                    const long double others = total - own;
                    const long double fill = 1.1L * total * (count - 1) / count - others;
                    const long double spool = own > share ? (own - share) * duration * others / fill / 1000 : 0;
                    EXPECT_LE(std::ceil(spool), memorySpoolEnvelope(own, duration));
                }
}

TEST(DisplayMemory, ProfileTakesT210AndB01WithLpddr4)
{
    EXPECT_TRUE(memoryProfile(0x2120, 1));
    EXPECT_TRUE(memoryProfile(0x2110, 1));
    EXPECT_FALSE(memoryProfile(0x2100, 1));
    EXPECT_FALSE(memoryProfile(0x2130, 1));
    EXPECT_FALSE(memoryProfile(0x2120, 2));
    EXPECT_FALSE(memoryProfile(0x1920, 1));
}

TEST(DisplayMemory, LayoutBufferingMatchesT21xLineStorage)
{
    switch_display_memory_window w{};
    ASSERT_TRUE(memoryWindow(w, 1, 1920, 1080, 960, 540, true, false, false));
    EXPECT_EQ(w.required_buffering_bytes, 1920u * 3 * 3);
    EXPECT_EQ(w.latency_buffering_bytes, 0u);
    ASSERT_TRUE(memoryWindow(w, 1, 1280, 720, 1280, 720, false, false, true));
    EXPECT_EQ(w.required_buffering_bytes, 720u * 4 * 4);
    EXPECT_EQ(w.latency_buffering_bytes, 720u * 4);
    ASSERT_TRUE(memoryWindow(w, 1, 1920, 1080, 960, 540, true, true, false));
    EXPECT_EQ(w.required_buffering_bytes, 0u);
    EXPECT_EQ(w.latency_buffering_bytes, 0u);
    EXPECT_FALSE(memoryWindow(w, 1, UINT32_MAX, 1080, 960, 540, true, false, false));
}

TEST(DisplayMemory, MergingQueuedViewsRetainsWorstBufferAndLatencyDemand)
{
    switch_display_memory_window old{300, 9000, 3000, 1}, next{200, 16000, 0, 1};
    mergeMemoryWindow(old, next);
    EXPECT_EQ(old.peak_bytes_per_second, 300u);
    EXPECT_EQ(old.required_buffering_bytes, 16000u);
    EXPECT_EQ(old.latency_buffering_bytes, 0u);
    mergeMemoryWindow(old, {});
    EXPECT_EQ(old.required_buffering_bytes, 16000u);
}

TEST(DisplayMemory, SourcePolicyUsesRootClockDividedByTwo)
{
    auto head = ui();
    MemoryPlan low, high;
    ASSERT_TRUE(memoryPlan(head, 204000, low));
    ASSERT_TRUE(memoryPlan(head, 1600000, high));
    EXPECT_EQ(low.ptsa_rate, 16u);
    EXPECT_EQ(low.allowance[0], 255u);
    EXPECT_EQ(low.minimum[0], 117u);
    EXPECT_EQ(high.minimum[0], 24u);
    EXPECT_EQ(low.ptsa_rate, high.ptsa_rate);
}

TEST(DisplayMemory, ExcessiveBandwidthOrBufferingRefusesAdmission)
{
    auto head = ui(64000000000ULL);
    MemoryPlan plan;
    EXPECT_FALSE(memoryPlan(head, 1600000, plan));
    head = ui();
    head.windows[0].required_buffering_bytes = 200000;
    EXPECT_FALSE(memoryPlan(head, 1600000, plan));
    head = ui();
    head.windows[0].peak_bytes_per_second++;
    EXPECT_FALSE(memoryPlan(head, 1600000, plan));
    head = ui();
    head.total_width = head.active_width;
    head.total_height = head.active_height;
    EXPECT_FALSE(memoryPlan(head, 1600000, plan));
}

TEST(DisplayMemory, DvfsBufferRequirementIsPartOfAdmission)
{
    auto head = ui();
    MemoryPlan plan;
    EXPECT_TRUE(memoryPlan(head, 1600000, plan, 2000));
    EXPECT_FALSE(memoryPlan(head, 1600000, plan, 1000000));
}

TEST(DisplayMemory, PerHeadWindowFifoCapacityIsDifferent)
{
    auto first = ui(2000000000), second = first;
    second.head = 1;
    MemoryPlan a, b;
    ASSERT_TRUE(memoryPlan(first, 1600000, a));
    ASSERT_TRUE(memoryPlan(second, 1600000, b));
    EXPECT_GT(b.allowance[0], a.allowance[0]);
}

TEST(DisplayMemory, RegisterProgrammingPreservesOtherClientsAndUnusedHead)
{
    std::array<uint32_t, 1024> regs{};
    regs.fill(0xabcd0000);
    regs[0x434 / 4] = 20;
    regs[0x494 / 4] = 4;
    regs[0x4a0 / 4] = 50;
    regs[0x44c / 4] = 100;
    regs[0x50c / 4] = 10;
    regs[0x428 / 4] = 12;
    const auto before = regs;
    MemoryPlan plans[2];
    ASSERT_TRUE(memoryPlan(ui(), 1600000, plans[0]));
    const bool owned[2] = {true, false};
    ASSERT_TRUE(memoryProgram(regs, plans, owned));
    EXPECT_EQ(regs[0x47c / 4], (16u + 12 + 20 + 4 + 50) / 2 + 100 + 10);
    EXPECT_EQ(regs[0x2e8 / 4], (before[0x2e8 / 4] & ~0x00ff00ffu) | 0x00ff00ffu);
    EXPECT_EQ(regs[0x690 / 4], (before[0x690 / 4] & ~0x00ff00ffu) | 0x00ff00ffu);
    for (unsigned offset : {0x434, 0x438, 0x43c, 0x494, 0x498, 0x49c, 0x4a0, 0x4a4, 0x4a8, 0x44c, 0x450,
                            0x454, 0x50c, 0x510, 0x514, 0x428, 0x42c, 0x430, 0x2f4, 0x694, 0x960})
        EXPECT_EQ(regs[offset / 4], before[offset / 4]) << offset;
}

TEST(DisplayMemory, ArbitrationCapacityFailureMakesNoRegisterWrite)
{
    std::array<uint32_t, 1024> regs{};
    regs.fill(4095);
    const auto before = regs;
    MemoryPlan plans[2];
    ASSERT_TRUE(memoryPlan(ui(), 1600000, plans[0]));
    const bool owned[2] = {true, false};
    EXPECT_FALSE(memoryProgram(regs, plans, owned));
    EXPECT_EQ(regs, before);
}

TEST(DisplayMemory, RetainedDemandSelectsLegalRateAndRepeatsWithoutChangingIdentity)
{
    MemoryState state;
    state.heads[0] = ui(2000000000);
    state.owned[0] = true;
    const uint32_t rates[] = {204000, 1331200, 1600000};
    EXPECT_EQ(state.floor(rates, 3, 0, 1600000), 1u);
    EXPECT_TRUE(state.same(state.heads[0]));
    auto changed = state.heads[0];
    changed.windows[0].required_buffering_bytes++;
    EXPECT_FALSE(state.same(changed));
    MemoryPlan before, after, combined;
    ASSERT_TRUE(memoryPlan(state.heads[0], 1331200, before));
    ASSERT_TRUE(memoryPlan(state.heads[0], 1600000, after));
    ASSERT_TRUE(memoryTransition(before, after, combined));
    EXPECT_LE(combined.allowance[0], before.allowance[0]);
    EXPECT_LE(combined.allowance[0], after.allowance[0]);
    EXPECT_GE(combined.allowance[0], before.minimum[0]);
}
}  // namespace
