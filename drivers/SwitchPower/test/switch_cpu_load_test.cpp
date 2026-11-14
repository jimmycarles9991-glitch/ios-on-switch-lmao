// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchCpuLoad.h: a CPU's busy time kept by its context-switch callout with two plain stores, read by another CPU. Read
// alone it is exact; read while the CPU switches, it is off by at most one interval per switch that raced the read (on
// the console a read of two words takes nanoseconds and switches come microseconds apart, so one), never more. Under
// TSAN too (switch_cpu_load_tsan_test), with the reader racing the writer.
#include "drivers/SwitchPower/SwitchCpuLoad.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <thread>

namespace {
constexpr uint64_t kBase = 1'000'000;
constexpr uint64_t kInterval = 1000;

// The CPU switches at kBase + k * kInterval for k = 1, 2, ..., to busy after odd k and to idle after even k: its busy
// time up to `now` once it has made k switches.
uint64_t truth(uint64_t k, uint64_t now)
{
    const uint64_t done = kInterval * (k / 2);
    const uint64_t since = kBase + k * kInterval;
    return done + ((k & 1) && now > since ? now - since : 0);
}
}  // namespace

TEST(SwitchCpuLoad, ReadAloneItIsExact)
{
    switch_cpu_load load = {};
    for (uint64_t k = 1; k <= 1000; ++k) {
        switch_cpu_load_switch(&load, kBase + k * kInterval, static_cast<int>(k & 1));
        const uint64_t now = kBase + k * kInterval + kInterval / 2;
        ASSERT_EQ(switch_cpu_load_busy(&load, now), truth(k, now)) << "after " << k << " switches";
    }
}

TEST(SwitchCpuLoad, AStaleOrEarlyTimestampAddsNothing)
{
    switch_cpu_load load = {};
    switch_cpu_load_switch(&load, kBase, 1);
    switch_cpu_load_switch(&load, kBase - 10, 1);
    EXPECT_EQ(switch_cpu_load_busy(&load, kBase + 100), 100u);
    switch_cpu_load_switch(&load, kBase + 100, 0);
    EXPECT_EQ(switch_cpu_load_busy(&load, kBase + 500), 100u);
}

// The reader on another thread while the writer switches as fast as it can: each read lies within one interval per
// racing switch of what the CPU had accumulated at some point during the read. The writer announces each switch before
// it makes it (`begun`, then a release fence) and after (`made`); the reader's acquire fence after its read makes
// `begun` an upper bound on any switch whose stores it saw.
TEST(SwitchCpuLoad, ARacingReadIsOffByOneIntervalPerRacingSwitch)
{
    constexpr uint64_t kSwitches = 400'000;
    constexpr uint64_t kNow = kBase + (kSwitches + 10) * kInterval;
    switch_cpu_load load = {};
    std::atomic<uint64_t> begun{0};
    std::atomic<uint64_t> made{0};
    std::atomic<bool> done{false};
    uint64_t reads = 0;
    uint64_t raced = 0;
    uint64_t beyond = 0;
    std::thread writer([&] {
        for (uint64_t k = 1; k <= kSwitches; ++k) {
            begun.store(k, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            switch_cpu_load_switch(&load, kBase + k * kInterval, static_cast<int>(k & 1));
            made.store(k, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
        const uint64_t low = made.load(std::memory_order_acquire);
        const uint64_t read = switch_cpu_load_busy(&load, kNow);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t high = begun.load(std::memory_order_relaxed);
        uint64_t least = UINT64_MAX;
        uint64_t most = 0;
        for (const uint64_t k : {low, low + 1, high > 0 ? high - 1 : 0, high}) {
            if (k < low || k > high)
                continue;
            least = std::min(least, truth(k, kNow));
            most = std::max(most, truth(k, kNow));
        }
        const uint64_t off = read < least ? least - read : read > most ? read - most : 0;
        beyond += off > kInterval * std::max<uint64_t>(1, high - low) ? 1 : 0;
        raced += high > low ? 1 : 0;
        ++reads;
    }
    writer.join();
    EXPECT_GT(reads, 0u);
    EXPECT_EQ(beyond, 0u) << "of " << reads << " reads, " << raced << " racing a switch";
    EXPECT_EQ(switch_cpu_load_busy(&load, kNow), truth(kSwitches, kNow));
}
