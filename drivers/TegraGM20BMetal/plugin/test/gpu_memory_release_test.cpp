// SPDX-License-Identifier: MIT
// GpuMemoryRelease.h: the idle release waits for the caller's command buffers in flight and comes at the deadline its drain
// sets, a second after it (after the last of a stream of drains), in every process; without a count (m2v's) it comes at
// the deadline; a release asked for, memory pressure's, runs with work in flight.
#include <gtest/gtest.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "drivers/TegraGM20BMetal/plugin/GpuMemoryRelease.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Released {
    std::atomic<int> count{0};
    std::atomic<Clock::rep> at{0};

    static void release(void *self)
    {
        auto *released = static_cast<Released *>(self);
        released->at = Clock::now().time_since_epoch().count();
        released->count++;
    }
};

// GpuMemoryRelease's idle time, the timer's 100 ms leeway and a margin
constexpr auto kPastDeadline = 1000ms + 100ms + 250ms;

bool releasedWithin(const Released &released, std::chrono::milliseconds most)
{
    const auto until = std::chrono::steady_clock::now() + most;
    while (!released.count && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(5ms);
    return released.count != 0;
}

class GpuMemoryReleaseTest : public testing::Test {};

// Its dispatch sources keep firing into `released`: each test's release and count live for the process
TEST_F(GpuMemoryReleaseTest, ACommandBufferInFlightHoldsTheIdleReleaseUntilItsDrain)
{
    auto *released = new Released;
    auto *inFlight = new std::atomic<uint32_t>{1};
    auto *release = new GpuMemoryRelease;
    release->start(&Released::release, released, inFlight);
    std::this_thread::sleep_for(kPastDeadline);
    EXPECT_EQ(released->count, 0) << "the deadline passed with a command buffer in flight";
    inFlight->store(0);
    std::this_thread::sleep_for(kPastDeadline);
    EXPECT_EQ(released->count, 0) << "the deadline is the drain's to set again";
    release->drained();
    EXPECT_TRUE(releasedWithin(*released, 3000ms));
    EXPECT_EQ(released->count, 1);
}

// Drains every 50 ms, past the first one's deadline: the deadline that releases is the last drain's before it
TEST_F(GpuMemoryReleaseTest, AStreamOfDrainsReleasesASecondAfterItsLast)
{
    struct Drain {
        Clock::time_point before, after;
    };

    auto *released = new Released;
    auto *inFlight = new std::atomic<uint32_t>{0};
    auto *release = new GpuMemoryRelease;
    std::vector<Drain> drains;
    drains.reserve(64);
    drains.push_back({Clock::now(), {}});
    release->start(&Released::release, released, inFlight);
    drains.back().after = Clock::now();
    const Clock::time_point until = drains.front().before + 1600ms;
    while (Clock::now() < until && !released->count) {
        std::this_thread::sleep_for(50ms);
        drains.push_back({Clock::now(), {}});
        release->drained();
        drains.back().after = Clock::now();
    }
    ASSERT_TRUE(releasedWithin(*released, 3000ms));
    const Clock::time_point at{Clock::duration(released->at.load())};
    Clock::time_point last = drains.front().before;
    for (const Drain &d : drains)
        if (d.after <= at)
            last = d.before;
    EXPECT_GE(at - last, 990ms) << "released "
                                << std::chrono::duration_cast<std::chrono::milliseconds>(at - last).count()
                                << " ms after the last drain before it";
}

constexpr uint64_t kIdle = uint64_t(GpuMemoryRelease::kIdleNs);

// The deadline a thread's own wait carries: a second after the last drain, given back once
TEST_F(GpuMemoryReleaseTest, AWaitsIdleDeadlineIsTheLastDrainsAndClaimedOnce)
{
    IdleDeadline idle;
    EXPECT_EQ(idle.due(0), 0u) << "a deadline with no drain";
    idle.drained(100);
    const uint64_t first = idle.due(0);
    EXPECT_EQ(first, 100 + kIdle);
    idle.drained(700);
    EXPECT_EQ(idle.due(0), 700 + kIdle);
    EXPECT_FALSE(idle.claim(first)) << "claimed a deadline a later drain moved";
    EXPECT_TRUE(idle.claim(700 + kIdle));
    EXPECT_EQ(idle.due(0), 0u) << "due again with no drain since its release";
    EXPECT_FALSE(idle.claim(700 + kIdle));
}

TEST_F(GpuMemoryReleaseTest, WorkInFlightHoldsAWaitsIdleDeadlineUntilTheNextDrain)
{
    IdleDeadline idle;
    idle.drained(100);
    EXPECT_EQ(idle.due(1), 0u);
    idle.drained(5000);
    EXPECT_EQ(idle.due(0), 5000 + kIdle);
}

TEST_F(GpuMemoryReleaseTest, WithoutAnInFlightCountTheDeadlineReleases)
{
    auto *released = new Released;
    auto *release = new GpuMemoryRelease;
    release->start(&Released::release, released);
    EXPECT_TRUE(releasedWithin(*released, 3000ms));
}

// The render server's old setting: the environment no longer turns the idle release off
TEST_F(GpuMemoryReleaseTest, NoEnvironmentTurnsTheIdleReleaseOff)
{
    setenv("NMB_IDLE_RELEASE_MS", "0", 1);
    auto *released = new Released;
    auto *release = new GpuMemoryRelease;
    release->start(&Released::release, released);
    unsetenv("NMB_IDLE_RELEASE_MS");
    EXPECT_TRUE(releasedWithin(*released, 3000ms));
}

// The memory pressure source's handler is release(): a pressure event (the kernel's memorystatus, which a test cannot
// raise) gives back at once with a command buffer in flight
TEST_F(GpuMemoryReleaseTest, AReleaseAskedForRunsWithACommandBufferInFlight)
{
    auto *released = new Released;
    auto *inFlight = new std::atomic<uint32_t>{1};
    auto *release = new GpuMemoryRelease;
    release->start(&Released::release, released, inFlight);
    release->release();
    EXPECT_EQ(released->count, 1);
}

}  // namespace
