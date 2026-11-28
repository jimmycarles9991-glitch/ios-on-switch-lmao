// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "../SwitchNVENCFlight.h"

namespace {
struct HostFlightPlatform {
    std::mutex mutex;
    std::condition_variable cv;
    uint32_t baseVal = 100;
    uint32_t issuedVal = 0;
    std::atomic<uint32_t> syncptVal{100};
    std::atomic<bool> lockHeld{false};
    bool suspendedVal = false;
    bool wedgedVal = false;
    std::function<int(uint32_t)> onBoundedWait;
    std::function<int(uint32_t)> onStalled;

    void lock() __attribute__((no_thread_safety_analysis))
    {
        mutex.lock();
        lockHeld.store(true, std::memory_order_release);
    }

    void unlock() __attribute__((no_thread_safety_analysis))
    {
        lockHeld.store(false, std::memory_order_release);
        mutex.unlock();
    }

    void sleep(void *) __attribute__((no_thread_safety_analysis))
    {
        std::unique_lock<std::mutex> lk(mutex, std::adopt_lock);
        lockHeld.store(false, std::memory_order_release);
        cv.wait(lk);
        lockHeld.store(true, std::memory_order_release);
        lk.release();
    }

    void wake(void *)
    {
        cv.notify_all();
    }

    uint32_t base() const
    {
        return baseVal;
    }

    uint32_t issued() const
    {
        return issuedVal;
    }

    bool suspended() const
    {
        return suspendedVal;
    }

    bool wedged() const
    {
        return wedgedVal;
    }

    uint32_t syncptRead() const
    {
        return syncptVal.load(std::memory_order_acquire);
    }

    int boundedWait(uint32_t fence)
    {
        return onBoundedWait ? onBoundedWait(fence) : 0;
    }

    int stalledLocked(uint32_t fence)
    {
        return onStalled ? onStalled(fence) : -5;
    }
};

TEST(SwitchNVENCFlight, BoundedWaitDropsEngineLockAndAllowsConcurrentRetirement)
{
    SwitchNVENCFlight<HostFlightPlatform> flight;
    auto &p = flight.platform();

    SwitchNVENCFlightSlot *slots[kSwitchNVENCInFlight] = {};
    p.lock();
    for (uint32_t i = 0; i < kSwitchNVENCInFlight; ++i) {
        uint32_t job = 0;
        ASSERT_EQ(flight.waitSlotLocked(&job), 0);
        EXPECT_EQ(job, i + 1);
        p.issuedVal = job;
        slots[i] = flight.armLocked(job, p.baseVal + job, {});
    }
    p.unlock();

    std::mutex waitMu;
    std::condition_variable waitCv;
    bool inBoundedWait = false;
    bool allowReturn = false;
    bool lockWasFreeInsideWait = false;

    p.onBoundedWait = [&](uint32_t fence) -> int {
        EXPECT_EQ(fence, 101u);
        p.lock();
        lockWasFreeInsideWait = true;
        p.unlock();
        std::unique_lock<std::mutex> lk(waitMu);
        inBoundedWait = true;
        waitCv.notify_all();
        waitCv.wait(lk, [&] {
            return allowReturn;
        });
        return 0;
    };

    uint32_t nextJob = 0;
    int waitResult = -1;
    std::thread submitter([&] {
        p.lock();
        waitResult = flight.waitSlotLocked(&nextJob);
        if (!waitResult) {
            p.issuedVal = nextJob;
            flight.armLocked(nextJob, p.baseVal + nextJob, {});
        }
        p.unlock();
    });

    {
        std::unique_lock<std::mutex> lk(waitMu);
        waitCv.wait(lk, [&] {
            return inBoundedWait;
        });
    }
    EXPECT_TRUE(lockWasFreeInsideWait);

    p.syncptVal.store(101, std::memory_order_release);
    EXPECT_FALSE(flight.retireSlot(slots[0], 0));

    {
        std::lock_guard<std::mutex> lk(waitMu);
        allowReturn = true;
        waitCv.notify_all();
    }
    submitter.join();

    EXPECT_EQ(waitResult, 0);
    EXPECT_EQ(nextJob, 5u);
    EXPECT_EQ(flight.inFlight(), kSwitchNVENCInFlight);
}

TEST(SwitchNVENCFlight, EpochResetDuringUnlockedWaitRetriesUnderNewEpoch)
{
    SwitchNVENCFlight<HostFlightPlatform> flight;
    auto &p = flight.platform();

    SwitchNVENCFlightSlot *slots[kSwitchNVENCInFlight] = {};
    p.lock();
    for (uint32_t i = 0; i < kSwitchNVENCInFlight; ++i) {
        uint32_t job = 0;
        ASSERT_EQ(flight.waitSlotLocked(&job), 0);
        p.issuedVal = job;
        slots[i] = flight.armLocked(job, p.baseVal + job, {});
    }
    p.unlock();

    bool stalledCalled = false;
    p.onStalled = [&](uint32_t) {
        stalledCalled = true;
        return -5;
    };
    p.onBoundedWait = [&](uint32_t) -> int {
        for (auto *s : slots)
            flight.retireSlot(s, -5);
        p.lock();
        p.baseVal = 200;
        p.issuedVal = 0;
        p.syncptVal.store(200, std::memory_order_release);
        flight.bumpEpochLocked();
        p.unlock();
        return TEGRA_HOST1X_ETIMEDOUT;
    };

    p.lock();
    uint32_t job = 0;
    EXPECT_EQ(flight.waitSlotLocked(&job), 0);
    p.unlock();

    EXPECT_FALSE(stalledCalled);
    EXPECT_EQ(job, 1u);
}

TEST(SwitchNVENCFlight, OverdueDrainAndWatchdogRetireCallbacksOutsideLock)
{
    SwitchNVENCFlight<HostFlightPlatform> flight;
    auto &p = flight.platform();

    struct Seen {
        uint32_t fence;
        int result;
    };

    std::vector<Seen> seen;
    auto cb = [](void *ctx, uint32_t fence, int result) {
        auto *pair = static_cast<std::pair<HostFlightPlatform *, std::vector<Seen> *> *>(ctx);
        EXPECT_FALSE(pair->first->lockHeld.load(std::memory_order_acquire))
            << "completion callback must run outside the engine lock";
        pair->second->push_back({fence, result});
    };
    std::pair<HostFlightPlatform *, std::vector<Seen> *> ctx{&p, &seen};

    p.lock();
    auto *s1 = flight.armLocked(1, 101, {cb, &ctx});
    auto *s2 = flight.armLocked(2, 102, {cb, &ctx});
    flight.markOverdueLocked(s2);
    flight.markOverdueLocked(s1);

    uint32_t threshold = 0, epoch = 0;
    bool wasOverdue = false;
    ASSERT_TRUE(flight.findWatchdogLocked(false, &threshold, &epoch, &wasOverdue));
    EXPECT_EQ(threshold, 101u);
    EXPECT_TRUE(wasOverdue);
    p.unlock();

    EXPECT_FALSE(flight.retireOverdue(101, 0));
    EXPECT_FALSE(flight.retireOverdue(101, -5));  // second attempt is a no-op

    p.lock();
    flight.drainOverdueAndWaitLocked(-5);
    EXPECT_EQ(flight.inFlight(), 0u);
    p.unlock();

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].fence, 101u);
    EXPECT_EQ(seen[0].result, 0);
    EXPECT_EQ(seen[1].fence, 102u);
    EXPECT_EQ(seen[1].result, -5);
}
}  // namespace
