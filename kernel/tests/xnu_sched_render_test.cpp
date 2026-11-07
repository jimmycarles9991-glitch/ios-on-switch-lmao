// kernel/switch/sched/render.h on the host: how the display thread's dispatches split its time from a vsync to its swap.
#include <gtest/gtest.h>

#include "sched/render.h"

namespace {

constexpr uint64_t kMinBlock = 10;

struct Window {
    switch_render_window w = {};

    bool dispatch(uint64_t vsyncs, uint64_t now, uint64_t cpu, uint64_t made)
    {
        return switch_render_dispatched(&w, vsyncs, 900, now, cpu, made, kMinBlock);
    }
};

TEST(XnuSchedRender, AVsyncStartsTheWindowAtTheNextDispatch)
{
    Window t;
    t.dispatch(1, 1000, 500, 990);
    EXPECT_EQ(t.w.vsync, 900u);
    EXPECT_EQ(t.w.woke, 1000u);
    EXPECT_EQ(t.w.cpu0, 500u);
    EXPECT_EQ(t.w.blocked, 0u);
    EXPECT_EQ(t.w.runnable, 0u);

    t.w.blocked = 7;
    t.dispatch(2, 2000, 900, 1990);
    EXPECT_EQ(t.w.woke, 2000u);
    EXPECT_EQ(t.w.blocked, 0u);
}

TEST(XnuSchedRender, TheGapBetweenDispatchesIsRunBlockedAndWaiting)
{
    Window t;
    t.dispatch(1, 1000, 500, 990);
    // Ran 200 (off the core at 1200), made runnable at 1500, on a core at 1520.
    EXPECT_TRUE(t.dispatch(1, 1520, 700, 1500));
    EXPECT_EQ(t.w.blocked, 300u);
    EXPECT_EQ(t.w.blocks, 1u);
    EXPECT_EQ(t.w.longest, 300u);
    EXPECT_EQ(t.w.runnable, 20u);
    // A shorter block is counted but is not the longest.
    EXPECT_FALSE(t.dispatch(1, 1800, 800, 1700));
    EXPECT_EQ(t.w.blocked, 380u);
    EXPECT_EQ(t.w.blocks, 2u);
    EXPECT_EQ(t.w.runnable, 120u);
    EXPECT_TRUE(t.dispatch(1, 3000, 900, 2900));
    EXPECT_EQ(t.w.longest, 1000u);
}

TEST(XnuSchedRender, APreemptionIsWaitingNotABlock)
{
    Window t;
    t.dispatch(1, 1000, 500, 990);
    // Preempted at 1100 (made runnable then), back on a core at 1400.
    t.dispatch(1, 1400, 600, 1100);
    EXPECT_EQ(t.w.blocks, 0u);
    EXPECT_EQ(t.w.blocked, 0u);
    EXPECT_EQ(t.w.runnable, 300u);
    // Requeued a few ticks after it left: under the minimum, no block counted.
    t.dispatch(1, 1600, 700, 1505);
    EXPECT_EQ(t.w.blocks, 0u);
    EXPECT_EQ(t.w.blocked, 5u);
}

TEST(XnuSchedRender, AnUnknownRunnableTimeOnlyMovesTheDispatch)
{
    Window t;
    t.dispatch(1, 1000, 500, 990);
    EXPECT_FALSE(t.dispatch(1, 2000, 600, 0));
    EXPECT_EQ(t.w.blocked, 0u);
    EXPECT_EQ(t.w.runnable, 0u);
    EXPECT_EQ(t.w.on, 2000u);
    EXPECT_EQ(t.w.cpu, 600u);
}

TEST(XnuSchedRender, TheWindowKeepsItsFirstSubmission)
{
    Window t;
    switch_render_submitted(&t.w, 50, 10);
    EXPECT_EQ(t.w.first, 0u);  // no window yet

    t.dispatch(1, 1000, 500, 990);
    t.dispatch(1, 1520, 700, 1500);
    switch_render_submitted(&t.w, 1600, 780);
    EXPECT_EQ(t.w.first, 1600u);
    EXPECT_EQ(t.w.first_cpu, 280u);
    EXPECT_EQ(t.w.first_blocked, 300u);
    EXPECT_EQ(t.w.first_blocks, 1u);
    switch_render_submitted(&t.w, 1700, 880);
    EXPECT_EQ(t.w.first, 1600u);

    t.dispatch(2, 3000, 900, 2990);
    t.w.swapped = true;
    switch_render_submitted(&t.w, 3100, 950);
    EXPECT_EQ(t.w.first, 0u);
}

}  // namespace
