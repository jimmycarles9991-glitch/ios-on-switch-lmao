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
#include "../SwitchHost1xErrors.cpp"

extern "C" bool switch_nvdec_host1x_cancelled(uint32_t, uint32_t)
{
    return false;
}

extern "C" bool switch_vic_host1x_cancelled(uint32_t, uint32_t)
{
    return false;
}

extern "C" void switch_gpu_bridge_gate_close(void)
{}

// No stream here waits on a GPU bridge.
extern "C" void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *, uint32_t *syncpoint, uint32_t *threshold)
{
    *syncpoint = *threshold = 0;
}

extern "C" int switch_gpu_bridge_status(const switch_gpu_bridge *)
{
    return 1;
}

extern "C" void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
    *bridge = nullptr;
}

extern "C" int switch_gpu_bridge_failed(const switch_gpu_bridge *)
{
    return 0;
}

extern "C" int switch_gpu_bridge_why(const switch_gpu_bridge *)
{
    return 0;
}

extern "C" int switch_gpu_bridge_source_failure(const switch_gpu_bridge *)
{
    return 0;
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *)
{
    return 1;
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *, int)
{
    return 0;
}

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

// A filter waiter's fn: in the filter, under its simple lock, never with interrupts re-enabled, with preemption off.
void filtered(void *ctx)
{
    gOrder.push_back("filter " + std::to_string(reinterpret_cast<uintptr_t>(ctx)));
    if (!gLock->held || gLock->disabled || !switch_test_preemption_off)
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

// SwitchHost1xSmmu.cpp's: the register file has no SMMU behind it.
bool switch_host1x_channels_confine(char *line, size_t size)
{
    snprintf(line, size, "no SMMU in the host test");
    return false;
}

bool switch_host1x_channels_confine_channel(const switch_dma_buffer *, const switch_dma_buffer *)
{
    return true;
}

extern "C" void tegra_mc_hot_reset(uint32_t, uint32_t, bool)
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
        static bool seeded = false;
        const bool first = !seeded;
        if (first) {
            seeded = true;
            for (uint32_t id = 0; id < kSyncpts; ++id)
                value(id) = 0x80088080u ^ (id + 1u);
        }
        ASSERT_EQ(tegra_host1x_start(), TEGRA_HOST1X_OK);
        if (first) {
            EXPECT_EQ(gPool.capacity(), 190u) << "non-zero power-on syncpoint SRAM stays in the pool";
            for (uint32_t id = 0; id < kSyncpts; ++id)
                value(id) = 0;
        }
        switch_test_sleep = nullptr;
        switch_test_kicks = switch_test_wakeups = switch_test_bounds_cancels = 0;
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

// An engine whose falcon parked on an error never reaches the threshold: the error line moves the engine's error word
// and kicks the syncpoint, which wakes its thread waiters; the wait ends at once with TEGRA_HOST1X_EFALCON, its fence
// taken off the list. A word unmoved since the caller's epoch ends nothing.
TEST_F(Host1xFence, AWaitEndsAtOnceWhenTheEnginesFalconErrorWordMovesAndItsSyncpointIsKicked)
{
    value(175) = 0;
    static uint32_t errors;
    static unsigned kicked;
    errors = 4;
    switch_test_sleep = [](uint64_t) {
        const unsigned before = switch_test_wakeups;
        __atomic_store_n(&errors, 5u, __ATOMIC_RELEASE);
        tegra_host1x_syncpt_kick(175);
        kicked = switch_test_wakeups - before;
        return THREAD_AWAKENED;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait_unless(175, 1, 1000, &errors, 4), TEGRA_HOST1X_EFALCON);
    EXPECT_EQ(kicked, 1u) << "the kick wakes the syncpoint's waiter";
    EXPECT_EQ(reg(0xf00 + 4 * (175 / 32)) & (1u << (175 % 32)), 1u << (175 % 32)) << "its threshold disarmed";
    switch_test_sleep = [](uint64_t) {
        return THREAD_TIMED_OUT;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait_unless(175, 1, 1000, &errors, 5), TEGRA_HOST1X_ETIMEDOUT);
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
    IOLock blocking{};
    IOSimpleLockLock(&held);
    EXPECT_THROW(IOLockLock(&blocking), std::runtime_error);
    IOSimpleLockUnlock(&held);
    interrupt(184);
}

TEST_F(Host1xFence, FilterHandlesPureWaitersWithoutWakingWorkLoopAndCancelsBoundsTimer)
{
    value(185) = 0;
    switch_host1x_filter_handle handle = 0;
    ASSERT_EQ(switch_host1x_filter_wait(185, 2, filtered, tag(40), &handle), TEGRA_HOST1X_OK);
    value(185) = 2;
    reg(0xe80 + 4 * (185 / 32)) = 1u << (185 % 32);
    EXPECT_FALSE(switch_host1x_intr_filter()) << "filter waiter retires directly in filter without work-loop bounce";
    reg(0xe80 + 4 * (185 / 32)) = 0;
    EXPECT_EQ(gOrder, std::vector<std::string>{"filter 40"});
    EXPECT_FALSE(switch_host1x_fence_names(185));

    value(186) = 0;
    switch_host1x_fence cb;
    switch_host1x_fence_init(&cb, 186, 3, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&cb, 5000), TEGRA_HOST1X_OK);
    EXPECT_NE(switch_test_bounds_at, 0u);
    const uint32_t cancelsBefore = switch_test_bounds_cancels;
    value(186) = 3;
    reg(0xe80 + 4 * (186 / 32)) = 1u << (186 % 32);
    EXPECT_TRUE(switch_host1x_intr_filter()) << "callback fence queues hand-off and wakes work loop";
    reg(0xe80 + 4 * (186 / 32)) = 0;
    EXPECT_EQ(switch_test_bounds_cancels, cancelsBefore + 1u)
        << "idle bounds timer cancelled when last bounded fence retires";
    EXPECT_EQ(switch_test_bounds_at, 0u);
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[3].calls, 1);
}
}  // namespace

// tegra_host1x_syncpt_free: an id goes back only when no fence names it and it was handed out; either mistake panics.
TEST_F(Host1xFence, AFreedSyncpointNamedByAFenceOrNotHandedOutPanics)
{
    uint32_t id = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&id), TEGRA_HOST1X_OK);
    value(id) = 0;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, id, 5, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    try {
        tegra_host1x_syncpt_free(id);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_EQ(std::string(e.what()),
                  "TegraHost1x: syncpoint " + std::to_string(id) + " freed while a fence or waiter still waits on it");
    }
    EXPECT_TRUE(switch_host1x_fence_remove(&fence));
    tegra_host1x_syncpt_free(id);
    try {
        tegra_host1x_syncpt_free(id);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_NE(std::string(e.what()).find("not handed out"), std::string::npos) << e.what();
    }
}

// Once the 32 ids every boot used before (160-191) are out, the pool hands out low ones. They wait, survive SC7 and
// release after a failure as those did.
TEST_F(Host1xFence, ALowSyncpointPastTheOldRangeWaitsSleepsAndReleasesAsAnyOther)
{
    std::vector<uint32_t> held;
    uint32_t low = 0;
    do {
        ASSERT_EQ(tegra_host1x_syncpt_alloc(&low), TEGRA_HOST1X_OK);
        held.push_back(low);
    } while (low >= 160);
    EXPECT_NE(low, 0u);
    EXPECT_NE(low, 9u);
    value(low) = 0;
    switch_host1x_fence fence;
    switch_host1x_fence_init(&fence, low, 1, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&fence, 0), TEGRA_HOST1X_OK);
    EXPECT_EQ(threshold(low), 1u);
    value(low) = 5;
    tegra_host1x_suspend();
    value(low) = 0;
    threshold(low) = 0;
    tegra_host1x_resume();
    EXPECT_EQ(value(low), 5u) << "SC7 restores its value";
    interrupt(low);
    EXPECT_EQ(gSignals[1].calls, 1);
    EXPECT_EQ(tegra_host1x_syncpt_release_failed(low, 9, 2), TEGRA_HOST1X_EINVAL) << "farther than the jobs allowed";
    for (const uint32_t id : held)
        tegra_host1x_syncpt_free(id);
    EXPECT_EQ(tegra_host1x_syncpt_release_failed(low, 6, 1), TEGRA_HOST1X_EINVAL) << "freed: not ours any more";
}

// Review-dma2 D7, debug kernels: a free before the syncpoint reached a threshold a channel's fenced job was given would
// let the job's late increment reach the next owner.
TEST_F(Host1xFence, AFreeWhileAChannelsJobStillOwesAnIncrementPanics)
{
    uint32_t id = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&id), TEGRA_HOST1X_OK);
    value(id) = 4;
    const uint32_t job[2] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
                             tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id)};
    ASSERT_EQ(switch_host1x_job_copy(TEGRA_HOST1X_CHANNEL_NVJPG, job, 2, id, 6), TEGRA_HOST1X_OK);
    try {
        tegra_host1x_syncpt_free(id);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_EQ(std::string(e.what()), "TegraHost1x: syncpoint " + std::to_string(id) +
                                             " freed at 4 while a job a channel ran still takes it to 6");
    }
    value(id) = 6;
    tegra_host1x_syncpt_free(id);
}

TEST_F(Host1xFence, AFreeAfterARestartDroppedTheOwingJobIsNoPanic)
{
    uint32_t id = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&id), TEGRA_HOST1X_OK);
    value(id) = 4;
    const uint32_t job[2] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
                             tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id)};
    ASSERT_EQ(switch_host1x_job_copy(TEGRA_HOST1X_CHANNEL_NVJPG, job, 2, id, 6), TEGRA_HOST1X_OK);
    tegra_host1x_channel_teardown(TEGRA_HOST1X_CHANNEL_NVJPG);
    tegra_host1x_channel_restart(TEGRA_HOST1X_CHANNEL_NVJPG);
    tegra_host1x_syncpt_free(id);
}

TEST_F(Host1xFence, AHighBitSyncpointFreesCleanlyAndStillChecksOwedThresholds)
{
    uint32_t id = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&id), TEGRA_HOST1X_OK);
    value(id) = 0x84000120u;
    tegra_host1x_syncpt_free(id);

    ASSERT_EQ(tegra_host1x_syncpt_alloc(&id), TEGRA_HOST1X_OK);
    value(id) = 0x84000120u;
    const uint32_t job[2] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
                             tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id)};
    ASSERT_EQ(switch_host1x_job_copy(TEGRA_HOST1X_CHANNEL_NVJPG, job, 2, id, 0x84000122u), TEGRA_HOST1X_OK);
    try {
        tegra_host1x_syncpt_free(id);
        ADD_FAILURE() << "no panic";
    } catch (const std::runtime_error &e) {
        EXPECT_EQ(std::string(e.what()), "TegraHost1x: syncpoint " + std::to_string(id) +
                                             " freed at 2214592800 while a job a channel ran still takes it to "
                                             "2214592802");
    }
    value(id) = 0x84000122u;
    tegra_host1x_syncpt_free(id);
}

// host1x's general interrupt failing a stopped channel's jobs (SwitchHost1xErrors.cpp, switch_host1x_channels_failed):
// what its bound would have failed, at once, with the bound's status.
namespace {
uint32_t &syncReg(uint32_t offset)
{
    return reg(offset);
}

IOFilterInterruptEventSource *general()
{
    const auto &made = IOFilterInterruptEventSource::made();
    return made.size() >= 2 ? made[1] : nullptr;
}

// The interrupt as it arrives, only when CPU0's line carries it (TRM 14.9.2-14.9.8: the channel's HINTMASK_EXT bit,
// HINTMASK's EXT enable, INTC0MASK's HOST_INT, INTMASK's master): its filter, then (taken) its action on the work loop.
bool illegalOpcode(uint32_t channel)
{
    syncReg(0x028) |= 1u << channel;
    const bool line = (syncReg(0x028) & syncReg(0x02c)) && (syncReg(0x024) & (1u << 31)) && (syncReg(0x008) & 1u) &&
                      (syncReg(0x004) & 1u);
    IOFilterInterruptEventSource *source = general();
    if (!line || !source || !source->filter(nullptr, source))
        return false;
    source->action(nullptr, source, 1);
    return true;
}
}  // namespace

TEST_F(Host1xFence, TheGeneralInterruptIsOnWithEveryChannelsIllegalOpcodeUnmasked)
{
    ASSERT_NE(general(), nullptr);
    EXPECT_TRUE(general()->enabled);
    EXPECT_EQ(syncReg(0x02c), 0x3fffu);
    EXPECT_EQ(syncReg(0x024) & (1u << 31), 1u << 31);
    EXPECT_EQ(syncReg(0x008), 1u) << "INTC0MASK: host1x's own HOST_INT reaches CPU0";
    EXPECT_EQ(syncReg(0x004) & 1u, 1u);
}

// A CPU wait whose fence a stopped channel fails returns the failure: before, it read as reached.
TEST_F(Host1xFence, AWaitWhoseFenceIsFailedReturnsTheFailure)
{
    value(175) = 0;
    switch_test_sleep = [&](uint64_t) {
        switch_host1x_fences_fail(175, TEGRA_HOST1X_ETIMEDOUT);
        return THREAD_AWAKENED;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait(175, 3, 1000000), TEGRA_HOST1X_ETIMEDOUT);
    EXPECT_FALSE(switch_host1x_fence_names(175));
}

// The stopped channel's bounded fences fail at once (a queue's job fence: VIC's and NVDEC's cancel on it); an unbounded
// consumer's fence on the same syncpoint, a fence already reached and another syncpoint's are left as they were.
TEST_F(Host1xFence, AnIllegalOpcodeFailsTheChannelsBoundedFencesAndLeavesTheRest)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_VIC;
    value(176) = 1;
    value(177) = 0;
    const uint32_t job[2] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
                             tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, 176)};
    ASSERT_EQ(switch_host1x_job_copy(channel, job, 2, 176, 2), TEGRA_HOST1X_OK);
    switch_host1x_fence bounded, consumer, other;
    switch_host1x_fence_init(&bounded, 176, 2, signaled);
    switch_host1x_fence_init(&consumer, 176, 3, signaled);
    switch_host1x_fence_init(&other, 177, 4, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&bounded, 2000000), TEGRA_HOST1X_OK);
    ASSERT_EQ(switch_host1x_fence_add(&consumer, 0), TEGRA_HOST1X_OK);
    ASSERT_EQ(switch_host1x_fence_add(&other, 2000000), TEGRA_HOST1X_OK);
    ASSERT_TRUE(illegalOpcode(channel));
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[2].calls, 1) << "failed at once, not at its bound";
    EXPECT_EQ(gSignals[2].status, TEGRA_HOST1X_ETIMEDOUT);
    EXPECT_EQ(consumer.state, SWITCH_HOST1X_FENCE_QUEUED) << "a consumer ends by the cancellation's release";
    EXPECT_EQ(other.state, SWITCH_HOST1X_FENCE_QUEUED);
    EXPECT_EQ(syncReg(0x02c) & (1u << channel), 0u) << "masked until the channel restarts";
    tegra_host1x_channel_teardown(channel);
    tegra_host1x_channel_restart(channel);
    EXPECT_EQ(syncReg(0x02c) & (1u << channel), 1u << channel) << "the restart unmasks it";
    EXPECT_TRUE(switch_host1x_fence_remove(&consumer));
    EXPECT_TRUE(switch_host1x_fence_remove(&other));
}

// A stopped channel's syncpoints stay stopped until it restarts: a job's bound that starts later (its inputs came in),
// a new bounded fence and a CPU wait that comes after the interrupt fail at once too, not after their bound.
TEST_F(Host1xFence, AWaitOrBoundThatStartsAfterTheStopFailsAtOnceUntilTheRestart)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_NVENC;
    value(178) = 0;
    const uint32_t job[2] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
                             tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, 178)};
    ASSERT_EQ(switch_host1x_job_copy(channel, job, 2, 178, 3), TEGRA_HOST1X_OK);
    switch_host1x_fence later;
    switch_host1x_fence_init(&later, 178, 2, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&later, 0), TEGRA_HOST1X_OK) << "inputs not in yet: unbounded";
    ASSERT_TRUE(illegalOpcode(channel));
    switch_host1x_intr_handle();
    EXPECT_EQ(later.state, SWITCH_HOST1X_FENCE_QUEUED) << "unbounded: nothing a bound would have failed yet";
    switch_host1x_fence_watch(&later, 2000000);
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[2].calls, 1) << "its bound starts on a stopped syncpoint: failed at once";
    EXPECT_EQ(gSignals[2].status, TEGRA_HOST1X_ETIMEDOUT);
    switch_test_sleep = [](uint64_t) -> int {
        ADD_FAILURE() << "a wait on a stopped syncpoint slept";
        return THREAD_TIMED_OUT;
    };
    EXPECT_EQ(tegra_host1x_syncpt_wait(178, 3, 2000000), TEGRA_HOST1X_ETIMEDOUT);
    switch_host1x_fence bounded;
    switch_host1x_fence_init(&bounded, 178, 3, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&bounded, 2000000), TEGRA_HOST1X_OK);
    switch_host1x_intr_handle();
    EXPECT_EQ(gSignals[3].calls, 1);
    EXPECT_EQ(gSignals[3].status, TEGRA_HOST1X_ETIMEDOUT);
    switch_test_sleep = nullptr;
    tegra_host1x_channel_teardown(channel);
    tegra_host1x_channel_restart(channel);
    switch_host1x_fence again;
    switch_host1x_fence_init(&again, 178, 4, signaled);
    ASSERT_EQ(switch_host1x_fence_add(&again, 2000000), TEGRA_HOST1X_OK);
    EXPECT_EQ(again.state, SWITCH_HOST1X_FENCE_QUEUED) << "restarted: bounds run their course again";
    EXPECT_TRUE(switch_host1x_fence_remove(&again));
}
