// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraHost1x.cpp, SwitchHost1xChannels.cpp and SwitchHost1xFence.cpp on the host with a memory register file: where
// waits sleep and callbacks run.
// The memory does not clear status bits written one, so each step sets the registers the interrupt would see.
#include "switch_host1x_fence_stubs.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../TegraHost1x.cpp"
#include "../SwitchHost1xChannels.cpp"
#include "../SwitchHost1xFence.cpp"

namespace {
alignas(16384) uint8_t gHostRegisters[kHost1xSize];
alignas(4096) uint8_t gMcRegisters[0x1000];
alignas(4096) uint8_t gChannelBuffers[2 * TEGRA_HOST1X_CHANNELS][SWITCH_HOST1X_ARENA_WORDS * 4];
uint32_t gChannelAllocated;

uint32_t &reg(uint32_t offset)
{
    return *reinterpret_cast<uint32_t *>(gHostRegisters + kSyncOffset + offset);
}

uint32_t &value(uint32_t id)
{
    return reg(0xf80 + 4 * id);
}

uint32_t &threshold(uint32_t id)
{
    return reg(0x1380 + 4 * id);
}

// What the threshold interrupt's line would carry for id: its status bit, and then the action on the work loop.
void interrupt(uint32_t id)
{
    reg(0xe80 + 4 * (id / 32)) = 1u << (id % 32);
    switch_host1x_intr_handle();
    reg(0xe80 + 4 * (id / 32)) = 0;
}

struct Signal {
    int calls = 0, status = 1;
    bool lockHeld = false;
};

std::vector<Signal> gSignals(8);
std::vector<std::string> gOrder;

void signaled(switch_host1x_fence *fence, int status)
{
    auto &signal = gSignals[fence->threshold % gSignals.size()];
    ++signal.calls;
    signal.status = status;
    signal.lockHeld = gLock->held;
    gOrder.push_back("callback " + std::to_string(fence->threshold));
}

// A filter waiter's fn: in the filter, under its simple lock, never under the list's IOLock, with preemption off.
void filtered(void *ctx)
{
    gOrder.push_back("filter " + std::to_string(reinterpret_cast<uintptr_t>(ctx)));
    if (!gFilterLock->held || gFilterLock->disabled || gLock->held || !switch_test_preemption_off)
        gOrder.push_back("wrong context");
}

void *tag(uintptr_t n)
{
    return reinterpret_cast<void *>(n);
}

uint32_t &enabled(uint32_t id)
{
    return reg(0xf20 + 4 * (id / 32));
}

uint32_t &disabled(uint32_t id)
{
    return reg(0xf00 + 4 * (id / 32));
}
}  // namespace

extern "C" uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    return reinterpret_cast<uintptr_t>(pa == kHost1xPA ? gHostRegisters : gMcRegisters);
}

extern "C" bool switch_car_clock_enabled(unsigned)
{
    return true;
}

extern "C" bool switch_car_reset_asserted(unsigned)
{
    return false;
}

extern "C" uint32_t switch_car_read(uint32_t)
{
    return SOURCE_PLLP | HOST1X_DIV_2_5;
}

extern "C" void switch_car_write(uint32_t, uint32_t)
{}

extern "C" void switch_car_clock_enable(unsigned)
{}

extern "C" void switch_car_reset_deassert(unsigned)
{}

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    if (gChannelAllocated == 2 * TEGRA_HOST1X_CHANNELS || size > sizeof(gChannelBuffers[0]))
        return false;
    out->va = gChannelBuffers[gChannelAllocated++];
    out->pa = 0x80000000u + gChannelAllocated * 0x10000u;
    out->size = size;
    return true;
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return &nub;
}

void switch_interrupt_describe(uint32_t, char *buffer, size_t size)
{
    snprintf(buffer, size, "host test");
}

extern "C" void switch_emc_gpu_submit(void)
{}

namespace {
struct Host1xFence : testing::Test {
    void SetUp() override
    {
        ASSERT_EQ(tegra_host1x_start(), TEGRA_HOST1X_OK);
        switch_test_sleep = nullptr;
        switch_test_kicks = switch_test_wakeups = 0;
        switch_test_now = 1000;
        switch_test_bounds_at = 0;
        gBoundsAt = 0;
        gSignals.assign(8, {});
        gOrder.clear();
        switch_test_preemption_off = 0;
    }
};

TEST_F(Host1xFence, AWaitOnAReachedSyncpointDoesNotSleep)
{
    value(165) = 7;
    EXPECT_EQ(tegra_host1x_syncpt_wait(165, 7, 1000), TEGRA_HOST1X_OK);
}

// NVENC waits from two places on one syncpoint: a waiter's fence leaves the others armed.
TEST_F(Host1xFence, AWaiterEndsByTheInterruptAndLeavesTheOtherFencesOfItsSyncpointArmed)
{
    value(166) = 0;
    switch_host1x_fence later;
    switch_host1x_fence_init(&later, 166, 5, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&later, 0), TEGRA_HOST1X_OK);
    int sleeps = 0;
    switch_test_sleep = [&](uint64_t) {
        ++sleeps;
        value(166) = 3;
        interrupt(166);
        return THREAD_AWAKENED;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait(166, 3, 1000), TEGRA_HOST1X_OK);
    EXPECT_EQ(sleeps, 1);
    EXPECT_EQ(threshold(166), 5u);
    EXPECT_EQ(later.state, SWITCH_HOST1X_FENCE_QUEUED);
    EXPECT_EQ(gSignals[5].calls, 0);
    value(166) = 5;
    interrupt(166);
    EXPECT_EQ(gSignals[5].calls, 1);
    EXPECT_EQ(gSignals[5].status, 0);
    EXPECT_FALSE(gSignals[5].lockHeld);
}

TEST_F(Host1xFence, AWaitTheEngineNeverFinishesTimesOutAndDisarms)
{
    value(167) = 0;
    switch_test_sleep = [](uint64_t) {
        return THREAD_TIMED_OUT;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait(167, 1, 1000), TEGRA_HOST1X_ETIMEDOUT);
    EXPECT_EQ(reg(0xf00 + 4 * (167 / 32)) & (1u << (167 % 32)), 1u << (167 % 32));
}

TEST_F(Host1xFence, AReachedWaitTheInterruptMissedIsLostAfterOneGrace)
{
    value(168) = 0;
    int sleeps = 0;
    switch_test_sleep = [&](uint64_t) {
        ++sleeps;
        value(168) = 1;
        return THREAD_TIMED_OUT;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait(168, 1, 1000), TEGRA_HOST1X_ELOST);
    EXPECT_EQ(sleeps, 2);
}

TEST_F(Host1xFence, OnceStrictALostInterruptPanicsWithTheSyncpointAndTheGic)
{
    tegra_host1x_set_strict();
    value(169) = 0;
    switch_test_sleep = [&](uint64_t) {
        value(169) = 1;
        return THREAD_TIMED_OUT;
    };
    try {
        tegra_host1x_syncpt_wait(169, 1, 1000);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find("syncpoint 169 threshold interrupt lost (INTID 97)"), std::string::npos)
            << e.what();
        EXPECT_NE(std::string(e.what()).find("host test"), std::string::npos);
    }
    // The panic left the waiter's fence, on the gone stack, in the list.
    __atomic_store_n(&gStrict, false, __ATOMIC_RELEASE);
    gLock->held = false;
    gList = SwitchHost1xFenceList<Registers>{};
}

TEST_F(Host1xFence, AFenceAlreadyReachedIsHandedToTheWorkLoopNotCalledInline)
{
    value(170) = 9;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 170, 2, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    EXPECT_EQ(gSignals[2].calls, 0);
    EXPECT_EQ(fence.state, SWITCH_HOST1X_FENCE_HANDED_OFF);
    EXPECT_EQ(switch_test_kicks, 1u);
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[2].calls, 1);
    EXPECT_EQ(gSignals[2].status, 0);
    EXPECT_EQ(fence.state, SWITCH_HOST1X_FENCE_IDLE);
}

TEST_F(Host1xFence, AHandedOffFenceRemovedBeforeTheWorkLoopRunsIsNeverCalled)
{
    value(171) = 4;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 171, 3, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    EXPECT_TRUE(switch_host1x_fence_remove(&fence));
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[3].calls, 0);
    EXPECT_FALSE(switch_host1x_fence_remove(&fence));
}

TEST_F(Host1xFence, ABoundOutlivedFailsTheFenceOnTheWorkLoop)
{
    value(172) = 0;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 172, 4, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 2000), TEGRA_HOST1X_OK);
    EXPECT_EQ(switch_test_bounds_at, 1000u + 2000u * 1000u);
    switch_test_now = switch_test_bounds_at;
    switch_test_bounds(nullptr, nullptr);
    EXPECT_EQ(gSignals[4].calls, 0);
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[4].calls, 1);
    EXPECT_EQ(gSignals[4].status, TEGRA_HOST1X_ETIMEDOUT);
}

// The bound of a job behind a GPU producer starts when the producer is done (D8): watch moves the deadline.
TEST_F(Host1xFence, WatchStartsTheBoundOfAnUnboundedFence)
{
    value(173) = 0;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 173, 6, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    EXPECT_EQ(switch_test_bounds_at, 0u);
    switch_test_now = 5000;
    switch_host1x_fence_watch(&fence, 10);
    EXPECT_EQ(switch_test_bounds_at, 5000u + 10u * 1000u);
    EXPECT_TRUE(switch_host1x_fence_remove(&fence));
}

TEST_F(Host1xFence, AcrossSleepAddsFailAndPendingFencesAreArmedAgain)
{
    value(174) = 0;
    switch_host1x_fence fence, refused;
    switch_host1x_fence_init(&fence, 174, 1, signaled);
    switch_host1x_fence_init(&refused, 174, 7, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    tegra_host1x_suspend();
    EXPECT_EQ(switch_host1x_fence_add(&refused, 0), TEGRA_HOST1X_ENODEV);
    EXPECT_EQ(tegra_host1x_syncpt_wait(174, 1, 1000), TEGRA_HOST1X_ENODEV);
    threshold(174) = 0;
    tegra_host1x_resume();
    EXPECT_EQ(threshold(174), 1u);
    EXPECT_EQ(fence.state, SWITCH_HOST1X_FENCE_QUEUED);
    value(174) = 1;
    interrupt(174);
    EXPECT_EQ(gSignals[1].calls, 1);
}

// A submitter that finds the arena held by a job in flight sleeps until that job's fence interrupt, on host1x's work
// loop, gives the room back; nothing polls.
TEST_F(Host1xFence, ASubmitWithoutArenaRoomSleepsUntilTheOldestJobsFenceInterrupt)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_VIC, half = SWITCH_HOST1X_ARENA_WORDS / 2;
    value(175) = 0;
    uint32_t *words = nullptr;
    for (const uint32_t fence : {1u, 2u}) {
        ASSERT_EQ(switch_host1x_job_reserve(channel, half, &words), TEGRA_HOST1X_OK);
        ASSERT_EQ(switch_host1x_job_submit(channel, words, half, 175, fence), TEGRA_HOST1X_OK);
    }
    ASSERT_EQ(switch_host1x_job_reserve(channel, half, &words), TEGRA_HOST1X_EAGAIN);
    int sleeps = 0;
    switch_test_sleep = [&](uint64_t) {
        ++sleeps;
        EXPECT_EQ(threshold(175), 1u) << "armed at the oldest job's fence";
        value(175) = 1;
        interrupt(175);
        return THREAD_AWAKENED;
    };
    ASSERT_EQ(switch_host1x_job_begin(channel, half, &words), TEGRA_HOST1X_OK);
    EXPECT_EQ(sleeps, 1);
    switch_host1x_job_cancel(channel);
    switch_test_sleep = nullptr;
    value(175) = 2;
    ASSERT_EQ(switch_host1x_job_reserve(channel, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK);
    switch_host1x_job_cancel(channel);
}

// The display's VIC flip: its fn in the filter, then the pass, which runs the ordinary fence's callback and retires the
// waiter, so a later interrupt's filter never runs it again.
TEST_F(Host1xFence, AFilterWaiterRunsInTheFilterAheadOfThePassAndOnce)
{
    value(176) = 0;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, 176, 3, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    switch_host1x_filter_handle handle = 0;
    ASSERT_EQ(switch_host1x_filter_wait(176, 3, filtered, tag(1), &handle), TEGRA_HOST1X_OK);
    EXPECT_EQ(threshold(176), 3u);
    switch_host1x_intr_filter();
    EXPECT_TRUE(gOrder.empty()) << "not there yet";
    value(176) = 3;
    switch_host1x_intr_filter();
    interrupt(176);
    switch_host1x_intr_filter();
    const std::vector<std::string> want = {"filter 1", "callback 3"};
    EXPECT_EQ(gOrder, want);
    EXPECT_FALSE(switch_host1x_filter_cancel(handle));
}

TEST_F(Host1xFence, AThresholdAlreadyPassedIsRefusedAndItsWaiterNeverRuns)
{
    value(177) = 5;
    switch_host1x_filter_handle handle = 0;
    EXPECT_EQ(switch_host1x_filter_wait(177, 5, filtered, tag(2), &handle), SWITCH_HOST1X_FILTER_PASSED);
    switch_host1x_intr_filter();
    EXPECT_TRUE(gOrder.empty());
}

TEST_F(Host1xFence, APassThatBeatsTheFilterRetiresTheWaiterUnrun)
{
    value(178) = 0;
    switch_host1x_filter_handle handle = 0;
    ASSERT_EQ(switch_host1x_filter_wait(178, 2, filtered, tag(3), &handle), TEGRA_HOST1X_OK);
    value(178) = 2;
    interrupt(178);
    switch_host1x_intr_filter();
    EXPECT_TRUE(gOrder.empty());
    EXPECT_FALSE(switch_host1x_filter_cancel(handle)) << "gone with its pass";
}

TEST_F(Host1xFence, ACancelledWaiterNeverRunsAndItsThresholdIsDisarmed)
{
    value(179) = 0;
    switch_host1x_filter_handle handle = 0;
    ASSERT_EQ(switch_host1x_filter_wait(179, 4, filtered, tag(4), &handle), TEGRA_HOST1X_OK);
    EXPECT_EQ(enabled(179) & (1u << (179 % 32)), 1u << (179 % 32));
    disabled(179) = 0;
    EXPECT_TRUE(switch_host1x_filter_cancel(handle));
    EXPECT_EQ(disabled(179) & (1u << (179 % 32)), 1u << (179 % 32));
    value(179) = 4;
    switch_host1x_intr_filter();
    EXPECT_TRUE(gOrder.empty());
    EXPECT_FALSE(switch_host1x_filter_cancel(handle)) << "a second cancel";
}

TEST_F(Host1xFence, ACancelAfterTheFilterRanSaysItRan)
{
    value(180) = 0;
    switch_host1x_filter_handle handle = 0;
    ASSERT_EQ(switch_host1x_filter_wait(180, 1, filtered, tag(5), &handle), TEGRA_HOST1X_OK);
    value(180) = 1;
    switch_host1x_intr_filter();
    EXPECT_FALSE(switch_host1x_filter_cancel(handle));
    interrupt(180);
    switch_host1x_intr_filter();
    const std::vector<std::string> want = {"filter 5"};
    EXPECT_EQ(gOrder, want);
}

// SC7 with waiters out: the filter reads nothing of the unclocked sync unit; one whose syncpoint got there across the
// sleep is expired by the resume's re-arm and never runs, one still short runs from the filter after the resume.
TEST_F(Host1xFence, AcrossSleepTheFilterReadsNothingAndAWaiterStillShortRunsAfterTheResume)
{
    value(181) = value(182) = 0;
    switch_host1x_filter_handle reached = 0, shortOf = 0;
    ASSERT_EQ(switch_host1x_filter_wait(181, 3, filtered, tag(6), &reached), TEGRA_HOST1X_OK);
    ASSERT_EQ(switch_host1x_filter_wait(182, 3, filtered, tag(7), &shortOf), TEGRA_HOST1X_OK);
    tegra_host1x_suspend();
    value(181) = 3;
    switch_host1x_intr_filter();
    EXPECT_TRUE(gOrder.empty());
    tegra_host1x_resume();
    switch_host1x_intr_handle();
    switch_host1x_intr_filter();
    EXPECT_TRUE(gOrder.empty());
    value(182) = 3;
    switch_host1x_intr_filter();
    const std::vector<std::string> want = {"filter 7"};
    EXPECT_EQ(gOrder, want);
    interrupt(182);
}

TEST_F(Host1xFence, EveryWaiterOutRefusesTheNextUntilAPassFreesThem)
{
    value(183) = 0;
    switch_host1x_filter_handle handle = 0;
    for (uint32_t i = 1; i <= kFilterWaiters; ++i)
        ASSERT_EQ(switch_host1x_filter_wait(183, i, filtered, tag(10 + i), &handle), TEGRA_HOST1X_OK) << i;
    EXPECT_EQ(switch_host1x_filter_wait(183, 9, filtered, tag(9), &handle), TEGRA_HOST1X_ENOMEM);
    value(183) = kFilterWaiters;
    interrupt(183);
    EXPECT_EQ(switch_host1x_filter_wait(183, 20, filtered, tag(20), &handle), TEGRA_HOST1X_OK);
    EXPECT_TRUE(switch_host1x_filter_cancel(handle));
    EXPECT_TRUE(gOrder.empty());
}

// The filter takes no blocking lock: with a waiter ready, a fn that tried one would throw, and the stubs throw for any
// IOLock taken under the filter's simple lock.
TEST_F(Host1xFence, TheFilterRunsWithPreemptionOffAndNothingThatBlocks)
{
    value(184) = 0;
    switch_host1x_filter_handle handle = 0;
    ASSERT_EQ(switch_host1x_filter_wait(184, 1, filtered, tag(30), &handle), TEGRA_HOST1X_OK);
    value(184) = 1;
    ASSERT_NO_THROW(switch_host1x_intr_filter());
    EXPECT_EQ(gOrder, std::vector<std::string>{"filter 30"});
    EXPECT_EQ(switch_test_preemption_off, 0);
    IOSimpleLock held{};
    IOSimpleLockLock(&held);
    EXPECT_THROW(IOLockLock(gLock), std::runtime_error);
    IOSimpleLockUnlock(&held);
    interrupt(184);
}
}  // namespace
