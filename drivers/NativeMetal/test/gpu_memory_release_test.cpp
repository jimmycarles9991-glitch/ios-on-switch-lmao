// SPDX-License-Identifier: MIT
// GpuMemoryRelease.h: the idle release waits for the caller's command buffers in flight and comes at the deadline its drain
// sets, a second after it, in every process; without a count (m2v's) it comes at the deadline; a release asked for, memory
// pressure's, runs with work in flight.
#include <gtest/gtest.h>
#include <stdlib.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "drivers/NativeMetal/GpuMemoryRelease.h"

namespace {

using namespace std::chrono_literals;

struct Released {
    std::atomic<int> count{0};

    static void release(void *self)
    {
        static_cast<Released *>(self)->count++;
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
