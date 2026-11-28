// SPDX-License-Identifier: MIT
// GpuMemoryRelease.h: a device's GPU memory goes back on memory pressure and when its app goes to the background, never
// because the GPU was idle. Each test replays one sequence of a client's life: frames, pauses, background and
// foreground transitions, a pressure event (the pressure source's handler is release(); a test cannot raise the
// kernel's memorystatus level).
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "drivers/TegraGM20BMetal/plugin/GpuMemoryRelease.h"

namespace {

using namespace std::chrono_literals;

struct Released {
    std::atomic<int> count{0};

    static void release(void *self)
    {
        static_cast<Released *>(self)->count++;
    }
};

enum class Event {
    Pause,
    Background,
    Foreground,
    Pressure
};

// The process's release and its dispatch sources live for the process, as a device's do
int replay(const std::vector<Event> &events)
{
    auto *released = new Released;
    auto *release = new GpuMemoryRelease;
    release->start(&Released::release, released);
    for (const Event e : events) {
        switch (e) {
        case Event::Pause:
            std::this_thread::sleep_for(1500ms);  // past the second the idle timer used to give back after
            break;
        case Event::Background:
            release->backgrounded();
            break;
        case Event::Foreground:
            release->foregrounded();
            break;
        case Event::Pressure:
            release->release();
            break;
        }
    }
    return released->count.load();
}

// 25 Nov: a client's 1 s idle give-back parked its chunks and dropped its kept imports at every pause, and its next
// frame paid for unparks and imports (gpu.path.parked_bo, ui.scale_pacing's zoom imports)
TEST(GpuMemoryRelease, AForegroundAppThatPausesKeepsItsMemory)
{
    EXPECT_EQ(replay({Event::Pause, Event::Pause}), 0);
}

TEST(GpuMemoryRelease, GoingToTheBackgroundGivesBackBeforeTheCallReturns)
{
    EXPECT_EQ(replay({Event::Background}), 1);
}

TEST(GpuMemoryRelease, EachBackgroundTransitionGivesBackOnce)
{
    EXPECT_EQ(replay({Event::Background, Event::Background, Event::Pause}), 1)
        << "a second post before the app came back";
    EXPECT_EQ(replay({Event::Background, Event::Foreground, Event::Pause, Event::Background}), 2);
}

TEST(GpuMemoryRelease, MemoryPressureGivesBackInTheForeground)
{
    EXPECT_EQ(replay({Event::Pause, Event::Pressure}), 1);
    EXPECT_EQ(replay({Event::Foreground, Event::Pressure, Event::Pressure}), 2);
}

}  // namespace
