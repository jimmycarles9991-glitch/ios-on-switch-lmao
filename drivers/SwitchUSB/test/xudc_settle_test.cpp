// XudcSettle.h: XUDC endpoint state changes (EP_RELOAD, EP_STCHG, EP_THREAD_ACTIVE, EP_STOPPED) check immediately,
// poll briefly in microseconds for normal hardware completion, and only release the work-loop gate via commandSleep
// when hardware stays busy past the fast window.
#include "drivers/SwitchUSB/XudcSettle.h"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace {

TEST(XudcSettle, AlreadySettledSkipsPollAndSleep)
{
    unsigned reads = 0;
    unsigned clocks = 0;
    unsigned sleeps = 0;
    uint32_t seen = 0xdeadbeefu;
    const bool ok = xudc::settleRegister(
        1u << 4, 0u, xudc::kFastSettleUs, xudc::kStateChangeMs,
        [&] {
            ++reads;
            return 0u;
        },
        [&] {
            ++clocks;
            return uint64_t{0};
        },
        [&](uint64_t) {
            ++sleeps;
        },
        &seen);
    EXPECT_TRUE(ok);
    EXPECT_EQ(reads, 1u);
    EXPECT_EQ(clocks, 0u);
    EXPECT_EQ(sleeps, 0u);
    EXPECT_EQ(seen, 0u);
}

TEST(XudcSettle, FastMicrosecondPollSettlesWithoutSleeping)
{
    uint64_t now = 1000000ULL;
    unsigned reads = 0;
    unsigned sleeps = 0;
    uint32_t seen = 0;
    const bool ok = xudc::settleRegister(
        1u << 2, 1u << 2, xudc::kFastSettleUs, xudc::kStateChangeMs,
        [&] {
            ++reads;
            return now >= 1005000ULL ? (1u << 2) : 0u;
        },
        [&] {
            now += 1000ULL;
            return now;
        },
        [&](uint64_t) {
            ++sleeps;
        },
        &seen);
    EXPECT_TRUE(ok);
    EXPECT_GT(reads, 1u);
    EXPECT_EQ(sleeps, 0u);
    EXPECT_EQ(seen, 1u << 2);
}

TEST(XudcSettle, SlowPathReleasesWorkloopGateForOtherEndpoints)
{
    std::mutex workLoopGate;
    std::condition_variable timerCv;
    bool timerFired = false;
    bool otherEndpointDrained = false;
    std::atomic<bool> gateReleased{false};
    std::atomic<uint32_t> reg{1u << 3};
    std::atomic<uint64_t> now{0};

    // Simulates the XUDC interrupt thread draining another endpoint while the settling caller releases the gate.
    std::thread irqThread([&] {
        while (!gateReleased.load(std::memory_order_acquire))
            std::this_thread::yield();
        {
            const std::lock_guard<std::mutex> lock(workLoopGate);
            otherEndpointDrained = true;
            reg.store(0u, std::memory_order_release);
            now.store(500000ULL, std::memory_order_release);
            timerFired = true;
        }
        timerCv.notify_all();
    });

    unsigned sleeps = 0;
    uint32_t seen = 1u << 3;
    std::unique_lock<std::mutex> held(workLoopGate);
    const bool ok = xudc::settleRegister(
        1u << 3, 0u, xudc::kFastSettleUs, xudc::kStateChangeMs,
        [&] {
            return reg.load(std::memory_order_acquire);
        },
        [&] {
            return now.fetch_add(5000ULL, std::memory_order_acq_rel);
        },
        [&](uint64_t remainingNs) {
            ++sleeps;
            EXPECT_GT(remainingNs, 0ULL);
            gateReleased.store(true, std::memory_order_release);
            timerCv.wait(held, [&] {
                return timerFired;
            });
        },
        &seen);
    held.unlock();
    irqThread.join();

    EXPECT_TRUE(ok);
    EXPECT_EQ(sleeps, 1u);
    EXPECT_TRUE(otherEndpointDrained);
    EXPECT_EQ(seen, 0u);
}

TEST(XudcSettle, EarlyWakeupRechecksAndSleepsRemainingWindow)
{
    uint64_t now = 0;
    uint32_t reg = 1u << 6;
    unsigned sleeps = 0;
    uint64_t firstRemaining = 0;
    uint64_t secondRemaining = 0;
    const bool ok = xudc::settleRegister(
        1u << 6, 0u, xudc::kFastSettleUs, xudc::kStateChangeMs,
        [&] {
            return reg;
        },
        [&] {
            now += 10000ULL;
            return now;
        },
        [&](uint64_t remainingNs) {
            ++sleeps;
            if (sleeps == 1u) {
                firstRemaining = remainingNs;
                now += 200000ULL;
            } else {
                secondRemaining = remainingNs;
                now += remainingNs;
                reg = 0u;
            }
        });
    EXPECT_TRUE(ok);
    EXPECT_EQ(sleeps, 2u);
    EXPECT_GT(firstRemaining, secondRemaining);
}

TEST(XudcSettle, TimeoutReportsLastObservedBits)
{
    uint64_t now = 0;
    unsigned sleeps = 0;
    uint32_t seen = 0;
    const bool ok = xudc::settleRegister(
        0x3u << 4, 0u, xudc::kFastSettleUs, xudc::kStateChangeMs,
        [] {
            return 0x2u << 4;
        },
        [&] {
            now += 10000ULL;
            return now;
        },
        [&](uint64_t remainingNs) {
            ++sleeps;
            now += remainingNs;
        },
        &seen);
    EXPECT_FALSE(ok);
    EXPECT_GE(sleeps, 1u);
    EXPECT_EQ(seen, 0x2u << 4);
}

}  // namespace
