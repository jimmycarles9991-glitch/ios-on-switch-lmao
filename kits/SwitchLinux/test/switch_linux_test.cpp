// kits/SwitchLinux/SwitchLinux.cpp against recording stand-ins: which XNU call backs each Linux primitive, with what
// arguments, and the heap's size header.
#include "kits/SwitchLinux/SwitchLinux.h"
#include "switch_linux_record.h"
#include "switch_linux_stubs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using ::testing::ElementsAre;
using ::testing::IsEmpty;

namespace {
class SwitchLinux : public ::testing::Test {
protected:
    void SetUp() override
    {
        gSwitchLinuxCalls.clear();
        switch_linux_stub_interrupts = TRUE;
        switch_linux_stub_preemption = 0;
        switch_linux_stub_now = 5000;
        switch_linux_stub_wait_result = THREAD_AWAKENED;
    }
};

int gFired;

void fire(void *arg)
{
    gFired += *static_cast<int *>(arg);
}
}  // namespace

TEST_F(SwitchLinux, LogsThroughIOLogOnlyWhereItMayBlock)
{
    switch_linux_log("bpmp: ch %d busy\n", 3);
    switch_linux_stub_preemption = 1;
    switch_linux_log("held %s\n", "spinlock");
    switch_linux_stub_preemption = 0;
    switch_linux_stub_interrupts = FALSE;
    switch_linux_log_line("masked\n");
    EXPECT_THAT(gSwitchLinuxCalls,
                ElementsAre("IOLog bpmp: ch 3 busy\n", "os_log_error held spinlock\n", "os_log_error masked\n"));
}

TEST_F(SwitchLinux, HeapBlocksCarryTheirSize)
{
    auto *zeroed = static_cast<uint8_t *>(switch_linux_malloc(100, 1));
    auto *dirty = static_cast<uint8_t *>(switch_linux_malloc(24, 0));
    ASSERT_NE(zeroed, nullptr);
    ASSERT_NE(dirty, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(zeroed) % 16, 0u);
    EXPECT_EQ(switch_linux_malloc_size(zeroed), 100u);
    EXPECT_EQ(switch_linux_malloc_size(dirty), 24u);
    EXPECT_EQ(switch_linux_malloc_size(nullptr), 0u);
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(zeroed[i], 0);
    std::memset(dirty, 1, 24);
    switch_linux_free(zeroed);
    switch_linux_free(dirty);
    switch_linux_free(nullptr);
    EXPECT_THAT(gSwitchLinuxCalls, ElementsAre("IOMallocZero 116", "IOMalloc 40", "IOFree 116", "IOFree 40"));
}

TEST_F(SwitchLinux, HeapRefusesMoreThanAGibibyte)
{
    EXPECT_EQ(switch_linux_malloc((1ULL << 30) + 1, 1), nullptr);
    EXPECT_THAT(gSwitchLinuxCalls, IsEmpty());
}

TEST_F(SwitchLinux, IrqSaveReturnsWhatItFound)
{
    const unsigned long on = switch_linux_irq_save();
    EXPECT_EQ(on, 1UL);
    EXPECT_TRUE(switch_linux_irqs_disabled());
    const unsigned long off = switch_linux_irq_save();
    EXPECT_EQ(off, 0UL);
    switch_linux_irq_restore(off);
    EXPECT_TRUE(switch_linux_irqs_disabled());
    switch_linux_irq_restore(on);
    EXPECT_FALSE(switch_linux_irqs_disabled());
}

TEST_F(SwitchLinux, TimeIsMachAbsoluteTimeInNanoseconds)
{
    switch_linux_stub_now = 9000;
    EXPECT_EQ(switch_linux_nanotime(), 4500u);
    EXPECT_EQ(switch_linux_abstime_to_ns(30), 15u);
}

TEST_F(SwitchLinux, DelaysAreXnusDelayWithTheRangeAsLeeway)
{
    switch_linux_udelay(40);
    switch_linux_usleep_range(1000, 2000);
    switch_linux_usleep_range(50, 50);
    switch_linux_cond_resched();
    EXPECT_THAT(gSwitchLinuxCalls, ElementsAre("delay 40 1000", "delay 1000 leeway 1000 1000", "delay 50 leeway 0 1000",
                                               "thread_yield_to_preemption"));
}

TEST_F(SwitchLinux, EventSleepsAssertThenBlock)
{
    int event = 0;
    char expect[96];
    switch_linux_wait_prepare(&event, ~0ULL);
    EXPECT_TRUE(switch_linux_wait_block());
    switch_linux_wait_prepare(&event, 700);
    switch_linux_stub_wait_result = THREAD_TIMED_OUT;
    EXPECT_FALSE(switch_linux_wait_block());
    switch_linux_wakeup(&event);
    switch_linux_wakeup_one(&event);
    ASSERT_EQ(gSwitchLinuxCalls.size(), 6u);
    std::snprintf(expect, sizeof(expect), "assert_wait %p", static_cast<void *>(&event));
    EXPECT_EQ(gSwitchLinuxCalls[0], expect);
    std::snprintf(expect, sizeof(expect), "assert_wait_deadline %p 1400", static_cast<void *>(&event));
    EXPECT_EQ(gSwitchLinuxCalls[2], expect);
    std::snprintf(expect, sizeof(expect), "wakeup_one %p", static_cast<void *>(&event));
    EXPECT_EQ(gSwitchLinuxCalls[5], expect);
}

TEST_F(SwitchLinux, ASlotGetsItsSleepingLockOnce)
{
    void *mutexSlot = nullptr;
    void *rwSlot = nullptr;
    void *mutex = switch_linux_mutex_of(&mutexSlot);
    EXPECT_EQ(switch_linux_mutex_of(&mutexSlot), mutex);
    switch_linux_mutex_lock(mutex);
    switch_linux_mutex_unlock(mutex);
    void *rw = switch_linux_rwsem_of(&rwSlot);
    EXPECT_EQ(switch_linux_rwsem_of(&rwSlot), rw);
    switch_linux_down_read(rw);
    switch_linux_up_read(rw);
    switch_linux_down_write(rw);
    switch_linux_up_write(rw);
    switch_linux_mutex_free(mutex);
    switch_linux_rwsem_free(rw);
    ASSERT_GE(gSwitchLinuxCalls.size(), 10u);
    EXPECT_EQ(gSwitchLinuxCalls.front(), "lck_grp SwitchLinux");
    EXPECT_EQ(std::count_if(gSwitchLinuxCalls.begin(), gSwitchLinuxCalls.end(),
                            [](const std::string &call) {
                                return call.rfind("lck_mtx ", 0) == 0;
                            }),
              1);
    EXPECT_EQ(std::count_if(gSwitchLinuxCalls.begin(), gSwitchLinuxCalls.end(),
                            [](const std::string &call) {
                                return call.rfind("lck_rw ", 0) == 0;
                            }),
              1);
}

TEST_F(SwitchLinux, MutexSleepTakesANanotimeDeadline)
{
    void *mutex = switch_linux_mutex_alloc();
    int event = 0;
    gSwitchLinuxCalls.clear();
    EXPECT_TRUE(switch_linux_mutex_sleep(mutex, &event, ~0ULL));
    switch_linux_stub_wait_result = THREAD_TIMED_OUT;
    EXPECT_FALSE(switch_linux_mutex_sleep(mutex, &event, 3000));
    ASSERT_EQ(gSwitchLinuxCalls.size(), 2u);
    EXPECT_NE(gSwitchLinuxCalls[0].find("lck_mtx_sleep "), std::string::npos);
    EXPECT_NE(gSwitchLinuxCalls[1].find(" 6000"), std::string::npos);
    switch_linux_mutex_free(mutex);
}

TEST_F(SwitchLinux, CallsMoveWhenEnteredAgainAndCancel)
{
    int weight = 3;
    gFired = 0;
    void *call = switch_linux_call_alloc(fire, &weight);
    EXPECT_FALSE(switch_linux_call_enter_after(call, 100));
    EXPECT_TRUE(switch_linux_call_enter_after(call, 0));
    switch_linux_stub_fire(call);
    EXPECT_EQ(gFired, 3);
    EXPECT_FALSE(switch_linux_call_enter(call));
    EXPECT_TRUE(switch_linux_call_cancel(call));
    EXPECT_FALSE(switch_linux_call_cancel_wait(call));
    switch_linux_call_free(call);
    EXPECT_THAT(gSwitchLinuxCalls,
                ElementsAre("thread_call_allocate priority 0 options 1", "thread_call_enter_delayed 5200",
                            "thread_call_enter", "thread_call_enter", "thread_call_cancel", "thread_call_cancel_wait",
                            "thread_call_free"));
}

// flush_work: a run still queued is made by the flusher; with none queued nothing runs.
TEST_F(SwitchLinux, FlushMakesAPendingRun)
{
    int weight = 5;
    gFired = 0;
    void *call = switch_linux_call_alloc(fire, &weight);
    EXPECT_FALSE(switch_linux_call_flush(call, fire, &weight));
    EXPECT_EQ(gFired, 0);
    EXPECT_FALSE(switch_linux_call_enter(call));
    EXPECT_TRUE(switch_linux_call_flush(call, fire, &weight));
    EXPECT_EQ(gFired, 5);
    EXPECT_FALSE(switch_linux_call_cancel(call));
    switch_linux_call_free(call);
}

TEST_F(SwitchLinux, IoremapPicksTheMapping)
{
    EXPECT_EQ(switch_linux_ioremap(0x60004000, 0x1000, 0), reinterpret_cast<volatile void *>(0x61004000));
    EXPECT_EQ(switch_linux_ioremap(0x57000000, 0x1000000, 1), reinterpret_cast<volatile void *>(0x59000000));
    EXPECT_THAT(gSwitchLinuxCalls, ElementsAre("ml_io_map 0x60004000 0x1000", "ml_io_map_wcomb 0x57000000 0x1000000"));
}
