// kernel/switch/idle/idle.c's frame stamp on the host. Every frame line is steered to one core (ArmGIC frameTarget), so
// that core alone writes the stamp: a load and a store, no read-modify-write. Readers on other cores (the EMC loop,
// every core's idle choice) see whole stamps that never go backwards, and a burst after a quiet spell reports its start
// once.
#include "kits/SwitchIdle/switch_idle_governor.h"
#include "kits/SwitchIdle/test/switch_idle_kernel_stubs.h"

#include <switch/idle.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
constexpr uint64_t kFrame = SWITCH_IDLE_TICKS_US(16667);
constexpr uint64_t kQuiet = SWITCH_IDLE_FRAME_WINDOW + SWITCH_IDLE_TICKS_US(1000);

std::atomic<uint64_t> gClock{SWITCH_IDLE_TICKS_US(1000000)};
std::atomic<unsigned> gStarts;
thread_local unsigned tRmws;
cpu_data gFrameCore = {3, 0};

void started()
{
    gStarts.fetch_add(1, std::memory_order_relaxed);
}

// One frame line on the frame core, `gap` ticks after the clock now.
void frameLine(uint64_t gap)
{
    gClock.fetch_add(gap, std::memory_order_relaxed);
    switch_idle_note_frame();
}

class SwitchIdleFrameStamp : public ::testing::Test {
protected:
    void SetUp() override
    {
        switch_idle_set_frames_started(started);
        frameLine(kQuiet);  // the stamp from earlier tests is a quiet spell ago
        gStarts = 0;
        tRmws = 0;
    }
};
}  // namespace

extern "C" {
void idle_test_count_rmw(void)
{
    ++tRmws;
}

uint64_t mach_absolute_time(void)
{
    return gClock.load(std::memory_order_relaxed);
}

cpu_data_t *getCpuDatap(void)
{
    return &gFrameCore;
}

void ClearIdlePop(int)
{
    std::abort();
}

void switch_secondary_vectors_leave(void)
{
    std::abort();
}

void idle_test_wfi(void)
{
    std::abort();
}

thread_call_t thread_call_allocate_with_options(thread_call_func_t, thread_call_param_t, int, unsigned)
{
    std::abort();
}

bool thread_call_enter(thread_call_t)
{
    std::abort();
}

void os_log_error(void *, const char *, ...)
{
    std::abort();
}
}

TEST_F(SwitchIdleFrameStamp, OneLoadAndOneStorePerFrameLine)
{
    for (int i = 0; i < 60; ++i)
        frameLine(kFrame);
    EXPECT_EQ(tRmws, 0u) << "read-modify-writes over 60 frame lines";
    EXPECT_EQ(switch_idle_last_frame(), gClock.load());
    EXPECT_EQ(gStarts.load(), 0u) << "the frames continued the burst SetUp started";
}

TEST_F(SwitchIdleFrameStamp, EachBurstAfterAQuietSpellStartsOnce)
{
    for (int burst = 0; burst < 5; ++burst) {
        frameLine(kQuiet);
        for (int i = 0; i < 30; ++i)
            frameLine(kFrame);
    }
    EXPECT_EQ(gStarts.load(), 5u);
}

// The frame core writes while three other cores read: no reader sees a stamp older than one it saw before, or newer
// than the clock.
TEST_F(SwitchIdleFrameStamp, ReadersSeeStampsThatNeverGoBack)
{
    constexpr int kBursts = 200, kFrames = 500;
    std::atomic<bool> done{false};
    std::atomic<unsigned> bad{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            uint64_t seen = switch_idle_last_frame();
            while (!done.load(std::memory_order_acquire)) {
                const uint64_t stamp = switch_idle_last_frame();
                if (stamp < seen || stamp > gClock.load(std::memory_order_relaxed))
                    bad.fetch_add(1, std::memory_order_relaxed);
                seen = stamp;
            }
        });
    }
    std::thread writer([&] {
        for (int burst = 0; burst < kBursts; ++burst) {
            frameLine(kQuiet);
            for (int i = 0; i < kFrames; ++i)
                frameLine(kFrame);
        }
        done.store(true, std::memory_order_release);
    });
    writer.join();
    for (std::thread &reader : readers)
        reader.join();
    EXPECT_EQ(bad.load(), 0u) << "stamps read torn or going back";
    EXPECT_EQ(gStarts.load(), static_cast<unsigned>(kBursts));
}
