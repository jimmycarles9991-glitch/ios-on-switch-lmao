// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPerfGovernor.cpp itself on the host, over stand-ins for XNU: what the scheduler's context-switch callout, its
// deadline tick and the touch filter's hint cost (locks, read-modify-writes, fences, counted per call), that none of
// them may lock, and that a touch, a thermal limit and thermalmonitord's ceiling still reach the clock the owner applies
// at once, without waiting for a tick. Under TSAN (switch_perf_governor_callouts_tsan_test) every context runs on its
// own thread at once.
#include "drivers/SwitchPower/SwitchPerfGovernor.h"
#include "drivers/SwitchPower/SwitchPower.h"
#include "drivers/SwitchPower/test/switch_perf_governor_kernel_stubs.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Counts {
    unsigned locks, rmws, fences;
};

thread_local Counts tCounts;
thread_local bool tCallout;
thread_local int tCpu;
std::atomic<uint64_t> gNow{1'000'000'000};
std::atomic<sched_perfcontrol_callbacks *> gCallbacks;
std::atomic<uint64_t> gDeadline;
std::atomic<unsigned> gSignals;
unsigned gTouchSent;

constexpr uint64_t kMs = 1'000'000;  // the stand-in timebase counts nanoseconds
constexpr uint32_t kKhz[] = {918000, 1020000, 1122000, 1224000, 1326000, 1428000, 1581000, 1683000, 1785000};
constexpr uint32_t kMv[] = {620, 640, 660, 690, 720, 760, 820, 880, 950};
constexpr unsigned kCount = sizeof(kKhz) / sizeof(kKhz[0]);
constexpr unsigned kTop = kCount - 1;

void mayNotBlock(const char *what)
{
    if (tCallout)
        throw std::runtime_error(std::string(what) + " in a callout or the touch filter");
}

template <class... Cpu> void signalOwner(void *, Cpu...)
{
    gSignals.fetch_add(1, std::memory_order_relaxed);
}

struct Callout {
    Callout()
    {
        tCallout = true;
    }

    ~Callout()
    {
        tCallout = false;
    }

    Callout(const Callout &) = delete;
    Callout &operator=(const Callout &) = delete;
};

// Runs `fn` as a callout; the lock, sleep or allocation it threw on, or "".
template <class Fn> std::string asCallout(Fn fn)
{
    const Callout callout;
    try {
        fn();
    } catch (const std::runtime_error &error) {
        return error.what();
    }
    return "";
}

void contextSwitch(uint32_t cpu, bool idle, uint64_t at)
{
    gCallbacks.load()->csw(idle ? IDLE : CONTEXT_SWITCH, cpu, at, 0, nullptr, nullptr, nullptr, nullptr);
}

void tick(uint64_t at)
{
    gCallbacks.load()->deadline_passed(at);
}

// The owner's work loop after a signal: the index switchperf decided, applied.
unsigned own()
{
    char line[760];
    const unsigned index = switchperf::take(line, sizeof(line));
    switchperf::applied(index);
    return index;
}

class SwitchPerfCallouts : public ::testing::Test {
protected:
    void SetUp() override
    {
        char why[256] = "";
        gNow += 1000 * kMs;
        ASSERT_TRUE(switchperf::start(kKhz, kMv, kCount, kTop, &signalOwner, nullptr, why, sizeof(why))) << why;
        ASSERT_NE(gCallbacks.load(), nullptr);
        switch_perf_hint(SWITCH_PERF_HINT_BOOT_DONE);
        switch_perf_cltm_ceiling(0x10000);
        switchperf::limits(0, 0);
        own();
    }

    void TearDown() override
    {
        switchperf::stop();
    }

    // `windows` windows of 8 ms on every CPU, busy `percent` of each, each ended by a tick; then the owner applies.
    static void load(unsigned percent, unsigned windows)
    {
        for (unsigned w = 0; w < windows; ++w) {
            const uint64_t start = gNow.load();
            for (uint32_t cpu = 0; cpu < 4; ++cpu) {
                contextSwitch(cpu, false, start);
                contextSwitch(cpu, true, start + 8 * kMs * percent / 100);
            }
            gNow = start + 8 * kMs;
            tick(gNow);
        }
        own();
    }
};
}  // namespace

extern "C" {
void switch_gov_test_rmw(void)
{
    ++tCounts.rmws;
}

void switch_gov_test_fence(void)
{
    ++tCounts.fences;
}

IOSimpleLock *IOSimpleLockAlloc(void)
{
    return reinterpret_cast<IOSimpleLock *>(new std::mutex);
}

// The kernel locks' stand-ins lock and unlock in separate calls, which thread-safety analysis cannot pair.
__attribute__((no_thread_safety_analysis)) IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    ++tCounts.locks;
    mayNotBlock("IOSimpleLockLockDisableInterrupt");
    reinterpret_cast<std::mutex *>(lock)->lock();
    return 0;
}

__attribute__((no_thread_safety_analysis)) void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState)
{
    reinterpret_cast<std::mutex *>(lock)->unlock();
}

IOLock *IOLockAlloc(void)
{
    return reinterpret_cast<IOLock *>(new std::mutex);
}

__attribute__((no_thread_safety_analysis)) void IOLockLock(IOLock *lock)
{
    ++tCounts.locks;
    mayNotBlock("IOLockLock");
    reinterpret_cast<std::mutex *>(lock)->lock();
}

__attribute__((no_thread_safety_analysis)) void IOLockUnlock(IOLock *lock)
{
    reinterpret_cast<std::mutex *>(lock)->unlock();
}

void IOSleep(unsigned)
{
    mayNotBlock("IOSleep");
}

void *IOMalloc(size_t size)
{
    mayNotBlock("IOMalloc");
    return ::operator new(size);
}

void IOLog(const char *, ...)
{}

void os_log(void *, const char *, ...)
{}

uint64_t mach_absolute_time(void)
{
    return gNow.load(std::memory_order_relaxed);
}

void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result)
{
    *result = nanoseconds;
}

void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result)
{
    *result = abstime;
}

int cpu_number(void)
{
    return tCpu;
}

boolean_t ml_set_interrupts_enabled(boolean_t)
{
    return FALSE;
}

void sched_perfcontrol_register_callbacks(struct sched_perfcontrol_callbacks *callbacks, unsigned long)
{
    gCallbacks.store(callbacks);
}

boolean_t sched_perfcontrol_update_callback_deadline(uint64_t deadline)
{
    return gDeadline.exchange(deadline) != 0;
}

unsigned int ml_get_cpu_count(void)
{
    return 4;
}

void switch_boot_io_complete(void)
{}

void tegra_dfll_cpu_status(unsigned long *target_hz, unsigned long *measured_hz, unsigned int *vdd_mv)
{
    *target_hz = *measured_hz = 0;
    *vdd_mv = 0;
}

void tegra_gm20b_perf_hint(unsigned int)
{}

void tegra_gm20b_idle_tick(void)
{}

void switch_emc_hint(unsigned int)
{}

void switch_emc_tick(uint64_t)
{}

void switch_emc_retick(void)
{}

int switch_battery_avg_ma(bool *)
{
    return INT32_MIN;
}
}

TEST_F(SwitchPerfCallouts, AContextSwitchIsPlainStores)
{
    tCounts = {};
    const std::string blocked = asCallout([] {
        for (unsigned i = 0; i < 100; ++i)
            contextSwitch(1, i & 1, gNow + i * kMs);
    });
    EXPECT_EQ(blocked, "");
    EXPECT_EQ(tCounts.fences, 0u) << "fences over 100 context switches";
    EXPECT_EQ(tCounts.rmws, 0u) << "read-modify-writes over 100 context switches";
    EXPECT_EQ(tCounts.locks, 0u);
}

TEST_F(SwitchPerfCallouts, ATickTakesNoLockAndNoReadModifyWrite)
{
    load(50, 2);
    tCounts = {};
    std::string blocked;
    for (unsigned i = 0; i < 10 && blocked.empty(); ++i) {
        gNow += 8 * kMs;
        blocked = asCallout([] {
            tick(gNow);
        });
    }
    EXPECT_EQ(blocked, "");
    EXPECT_EQ(tCounts.locks, 0u) << "locks over 10 ticks";
    EXPECT_EQ(tCounts.rmws, 0u) << "read-modify-writes over 10 ticks";
    EXPECT_EQ(tCounts.fences, 20u) << "fences over 10 ticks: an acquire as each starts, a release before it re-arms";
}

TEST_F(SwitchPerfCallouts, TheTouchFilterTakesNoLockAndNoReadModifyWrite)
{
    tCounts = {};
    std::string blocked;
    for (unsigned i = 0; i < 60 && blocked.empty(); ++i) {
        gNow += 8 * kMs;
        blocked = asCallout([] {
            switch_perf_hint(SWITCH_PERF_HINT_TOUCH);
        });
    }
    EXPECT_EQ(blocked, "");
    EXPECT_EQ(tCounts.locks, 0u) << "locks over 60 touch interrupts";
    EXPECT_EQ(tCounts.rmws, 0u) << "read-modify-writes over 60 touch interrupts";
    EXPECT_EQ(tCounts.fences, 0u) << "fences over 60 touch interrupts";
}

// A touch while the clock idles low: the owner, woken by the filter, applies the top at once, with no tick between.
TEST_F(SwitchPerfCallouts, ATouchRaisesTheClockAtOnce)
{
    load(5, 30);
    ASSERT_LT(switchperf::take(nullptr, 0), kTop) << "an idle load lowered the clock";
    own();
    const unsigned signals = gSignals.load();
    gNow += kMs;
    switch_perf_hint(SWITCH_PERF_HINT_TOUCH);
    EXPECT_GT(gSignals.load(), signals) << "the filter woke the owner";
    EXPECT_EQ(own(), kTop);
    EXPECT_EQ(switch_perf_cpu_khz(), kKhz[kTop]);
    load(5, 10);
    EXPECT_EQ(switchperf::take(nullptr, 0), kTop) << "the touch's hold outlasts 80 ms of idle windows";
}

// A finger moving: one wake for its first interrupt, none for the rest while the clock is at the top.
TEST_F(SwitchPerfCallouts, AMovingFingerWakesTheOwnerOnce)
{
    load(5, 30);
    own();
    const unsigned signals = gSignals.load();
    for (unsigned i = 0; i < 60; ++i) {
        gNow += 8 * kMs;
        switch_perf_hint(SWITCH_PERF_HINT_TOUCH);
        own();
    }
    EXPECT_EQ(gSignals.load() - signals, 1u);
}

// The DFLL's thermal cap and thermalmonitord's ceiling apply at once, from their own threads, both ways.
TEST_F(SwitchPerfCallouts, ThermalLimitsApplyAtOnce)
{
    load(100, 2);
    ASSERT_EQ(own(), kTop);
    switchperf::limits(0, kMv[4]);
    EXPECT_EQ(own(), 4u);
    switchperf::limits(0, 0);
    switch_perf_cltm_ceiling(0x8000);  // half the top clock: 892 MHz, under the lowest point
    EXPECT_EQ(own(), 0u);
    switch_perf_cltm_ceiling(0x10000);
    load(100, 1);
    EXPECT_EQ(own(), kTop);
}

// Every context at once on its own thread: four CPUs' context switches, the tick (one CPU at a time, as XNU's deadline
// cmpxchg runs it), the touch filter, the owner's work loop and thermalmonitord.
TEST_F(SwitchPerfCallouts, EveryContextAtOnce)
{
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (uint32_t cpu = 0; cpu < 4; ++cpu) {
        threads.emplace_back([&stop, cpu] {
            tCpu = static_cast<int>(cpu);
            uint64_t at = gNow.load();
            for (unsigned i = 0; !stop.load(std::memory_order_relaxed); ++i) {
                at += 100'000 + cpu * 1000;
                contextSwitch(cpu, i & 1, at);
            }
        });
    }
    threads.emplace_back([&stop] {
        tCpu = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            const uint64_t at = gNow.fetch_add(kMs) + kMs;
            tick(at);
        }
    });
    threads.emplace_back([&stop] {
        tCpu = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            switch_perf_hint(SWITCH_PERF_HINT_TOUCH);
            std::this_thread::yield();
        }
    });
    threads.emplace_back([&stop] {
        tCpu = 2;
        for (unsigned i = 0; !stop.load(std::memory_order_relaxed); ++i)
            switch_perf_cltm_ceiling(i & 1 ? 0x10000 : 0xc000);
    });
    std::atomic<unsigned> outOfRange{0};
    for (unsigned i = 0; i < 20000; ++i) {
        if (own() >= kCount)
            outOfRange.fetch_add(1);
    }
    stop.store(true);
    for (std::thread &thread : threads)
        thread.join();
    EXPECT_EQ(outOfRange.load(), 0u);
}

// The periodic line: the tick publishes it once per 10 s, the owner's take() makes it, with who woke the owner.
TEST_F(SwitchPerfCallouts, TheTenSecondLineCountsTheOwnersWakes)
{
    char line[760] = "";
    for (unsigned lines = 0; lines < 2;) {
        gNow += 8 * kMs;
        if (lines == 1 && line[0] == 0 && gTouchSent == 0) {
            switch_perf_hint(SWITCH_PERF_HINT_TOUCH);
            gTouchSent = 1;
        }
        line[0] = 0;
        tick(gNow);
        switchperf::applied(switchperf::take(line, sizeof(line)));
        if (line[0])
            ++lines;
    }
    const std::string text = line;
    EXPECT_EQ(text.rfind("SwitchPerf governor: 10", 0), 0u) << text;
    EXPECT_NE(text.find("boosts touch 1 frame 0 launch 0"), std::string::npos) << text;
    EXPECT_NE(text.find("; owner wakes: tick "), std::string::npos) << text;
    EXPECT_NE(text.find(", touch 1, frame 0, launch 0"), std::string::npos) << text;
}
