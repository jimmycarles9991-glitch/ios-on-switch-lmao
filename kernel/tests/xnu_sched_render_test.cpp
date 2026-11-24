// kernel/switch/sched/render.h on the host: how the display thread's dispatches split its time from a vsync to its swap.
#include <gtest/gtest.h>

#include "sched/render.h"

namespace {

constexpr uint64_t kMinBlock = 10;

struct Window {
    switch_render_window w = {};

    bool dispatch(uint64_t vsyncs, uint64_t now, uint64_t cpu, uint64_t made)
    {
        return switch_render_dispatched(&w, vsyncs, 900, now, cpu, made, kMinBlock, 0);
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

// Frame ends every 1650 ticks: 1000, 2650, 4300, 5950. The display passes its count, its latest stamp and half its
// period; the thread runs `run` ticks from each wake to its swap.
constexpr uint64_t kPeriod = 1650, kHalf = kPeriod / 2;

uint64_t Vsync(uint64_t k)
{
    return 1000 + (k - 1) * kPeriod;
}

uint64_t CountAt(uint64_t now)
{
    return now < 1000 ? 0 : (now - 1000) / kPeriod + 1;
}

struct Frames {
    switch_render_window w = {};
    uint64_t cpu = 100;
    int lines = 0, dropped = 0;

    void Dispatch(uint64_t now, uint64_t made)
    {
        const uint64_t k = CountAt(now);
        switch_render_dispatched(&w, k, k ? Vsync(k) : 0, now, cpu, made, kMinBlock, kHalf);
    }

    // A wake at `wake`, a run of `run` ticks with no dispatch in it, then the swap.
    void Frame(uint64_t wake, uint64_t run)
    {
        Dispatch(wake, wake);
        cpu += run;
        Swap(wake + run);
    }

    void Swap(uint64_t now)
    {
        const uint64_t k = CountAt(now);
        bool late = false;
        if (!switch_render_swap(&w, k, Vsync(k), now, kHalf, &late)) {
            dropped++;
        } else if (late) {
            lines++;
        }
    }
};

// The thread swaps frame 1, parks at 1200 (its CPU time 290), and its timer wakes it at 2600, before frame 2's end; it
// runs, blocks briefly, and is back after the frame end at 2800.
TEST(XnuSchedRender, AWakeJustBeforeTheFrameEndOpensThatFrame)
{
    switch_render_window w = {};
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, kHalf);
    w.swapped = true;
    switch_render_dispatched(&w, 1, 1000, 2600, 290, 2600, kMinBlock, kHalf);
    EXPECT_TRUE(w.pending);
    EXPECT_EQ(w.vsync, 2650u) << "predicted from the latest stamp";
    switch_render_dispatched(&w, 2, 2650, 2800, 390, 2790, kMinBlock, kHalf);
    EXPECT_FALSE(w.pending);
    EXPECT_EQ(w.vsync, 2650u) << "the stamp the display published";
    EXPECT_EQ(w.woke, 2600u) << "woke 50 ticks before the frame end it rendered for";
    EXPECT_EQ(w.cpu0, 290u);
    EXPECT_EQ(w.blocked, 90u);
    EXPECT_EQ(w.blocks, 1u);
    EXPECT_FALSE(w.swapped);
}

TEST(XnuSchedRender, TheFrameWaitNotABlockAfterTheSwapOpensTheFrame)
{
    switch_render_window w = {};
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, kHalf);
    w.swapped = true;
    // After the swap: a 40-tick lock wait ends at 1240, then the frame wait ends at 2600.
    switch_render_dispatched(&w, 1, 1000, 1240, 290, 1240, kMinBlock, kHalf);
    switch_render_dispatched(&w, 1, 1000, 2600, 300, 2600, kMinBlock, kHalf);
    switch_render_dispatched(&w, 2, 2650, 2800, 400, 2790, kMinBlock, kHalf);
    EXPECT_EQ(w.woke, 2600u);
    EXPECT_EQ(w.cpu0, 300u);
}

TEST(XnuSchedRender, AWakeEarlierThanHalfAPeriodBeforeIsNotThisFrames)
{
    switch_render_window w = {};
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, kHalf);
    w.swapped = true;
    switch_render_dispatched(&w, 1, 1000, 1700, 290, 1700, kMinBlock, kHalf);
    EXPECT_FALSE(w.pending);
    switch_render_dispatched(&w, 2, 2650, 2800, 390, 2790, kMinBlock, kHalf);
    EXPECT_EQ(w.woke, 2800u);
    EXPECT_EQ(w.cpu0, 390u);
    EXPECT_EQ(w.blocked, 0u);
}

TEST(XnuSchedRender, AFrameThatDidNotSwapKeepsItsLateFirstDispatch)
{
    switch_render_window w = {};
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, kHalf);
    switch_render_dispatched(&w, 1, 1000, 2600, 290, 2600, kMinBlock, kHalf);
    switch_render_dispatched(&w, 2, 2650, 2800, 390, 2790, kMinBlock, kHalf);
    EXPECT_EQ(w.woke, 2800u);
}

// The review's case A: every wake 50 ticks before the frame end, and no dispatch between the wake and the swap.
TEST(XnuSchedRender, AStreakOfEarlyWakesWithNoDispatchAfterTheVsyncIsOnTime)
{
    Frames f;
    f.Frame(Vsync(1) + 10, 400);
    for (uint64_t k = 2; k <= 5; k++)
        f.Frame(Vsync(k) - 50, 400);
    EXPECT_EQ(f.lines, 0) << "every frame swapped 350 ticks after its frame end";
    EXPECT_EQ(f.dropped, 0);
    EXPECT_EQ(f.w.vsync, Vsync(5)) << "the swap took the display's stamp";
    EXPECT_EQ(f.w.woke, Vsync(5) - 50);
    EXPECT_FALSE(f.w.pending);
}

// The review's case B: frames woken after their frame end, then early wakes with no dispatch after the vsync. The
// first early frame's swap must not be lost, nor the next frame logged late with the park as a block.
TEST(XnuSchedRender, AnEarlyWakeAfterOnTimeFramesLosesNoSwapAndInventsNoLateFrame)
{
    Frames f;
    f.Frame(Vsync(1) + 300, 400);
    f.Frame(Vsync(2) + 300, 400);
    for (uint64_t k = 3; k <= 6; k++)
        f.Frame(Vsync(k) - 50, 400);
    EXPECT_EQ(f.lines, 0);
    EXPECT_EQ(f.dropped, 0);
    EXPECT_EQ(f.w.blocked, 0u) << "the park before the wake is not this frame's";
}

TEST(XnuSchedRender, ALateFrameIsStillLate)
{
    Frames f;
    f.Frame(Vsync(1) + 300, 400);
    f.Frame(Vsync(2) + 300, 1000);
    EXPECT_EQ(f.lines, 1) << "swapped 1300 ticks after its frame end";
    EXPECT_EQ(f.dropped, 0);
}

TEST(XnuSchedRender, ASwapBeforeItsPredictedFrameEndIsOnTime)
{
    switch_render_window w = {};
    bool late = true;
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, kHalf);
    ASSERT_TRUE(switch_render_swap(&w, 1, 1000, 1400, kHalf, &late));
    switch_render_dispatched(&w, 1, 1000, 2600, 290, 2600, kMinBlock, kHalf);
    ASSERT_TRUE(switch_render_swap(&w, 1, 1000, 2640, kHalf, &late));
    EXPECT_FALSE(late);
    EXPECT_TRUE(w.pending) << "the count had not reached it";
}

// The display's count and stamp stopped (frame ends masked, or the display idle): a dispatch long after the latest
// stamp is not an early wake, and its swap invents no late frame.
TEST(XnuSchedRender, AStaleStampPredictsNothing)
{
    switch_render_window w = {};
    bool late = false;
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, kHalf);
    ASSERT_TRUE(switch_render_swap(&w, 1, 1000, 1400, kHalf, &late));
    switch_render_dispatched(&w, 1, 1000, 1000 + 10 * kPeriod, 290, 1000 + 10 * kPeriod, kMinBlock, kHalf);
    EXPECT_FALSE(w.pending);
    EXPECT_FALSE(switch_render_swap(&w, 1, 1000, 1000 + 10 * kPeriod + 400, kHalf, &late)) << "no window to swap";
}

TEST(XnuSchedRender, NoPredictionBeforeTheDisplayReportsItsPeriod)
{
    switch_render_window w = {};
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, 0);
    w.swapped = true;
    switch_render_dispatched(&w, 1, 1000, 2600, 290, 2600, kMinBlock, 0);
    EXPECT_FALSE(w.pending);
    EXPECT_EQ(w.woke, 1010u);
}

TEST(XnuSchedRender, AWakeStarvedLateFrameIsLoggedAndDoesNotDropTheNextSwap)
{
    Frames f;
    f.Frame(Vsync(1) + 300, 400);
    // Frame 2 is delayed past half a period (900 >= 825) and swaps late at Vsync(2) + 1500.
    f.Frame(Vsync(2) + 900, 600);
    EXPECT_EQ(f.lines, 1) << "wake-starved frame 2 must be reported late, not absorbed as early for frame 3";
    EXPECT_EQ(f.dropped, 0);
    EXPECT_EQ(f.w.vsync, Vsync(2));
    EXPECT_EQ(f.w.woke, Vsync(2) + 900);
    EXPECT_FALSE(f.w.pending);
    f.Frame(Vsync(3) + 300, 400);
    EXPECT_EQ(f.lines, 1);
    EXPECT_EQ(f.dropped, 0) << "frame 3's swap must not be dropped";
    EXPECT_EQ(f.w.vsync, Vsync(3));
}

TEST(XnuSchedRender, APostSwapDispatchAfterALateFrameDoesNotChargeTheParkToTheNextFrame)
{
    Frames f;
    f.Frame(Vsync(1) + 300, 400);
    f.Frame(Vsync(2) + 300, 700);  // late swap at Vsync(2) + 1000
    f.Dispatch(Vsync(2) + 1040, Vsync(2) + 1040);
    f.cpu += 20;
    f.Frame(Vsync(3) + 300, 900);
    EXPECT_EQ(f.lines, 2);
    EXPECT_EQ(f.dropped, 0);
    EXPECT_EQ(f.w.woke, Vsync(3) + 300);
    EXPECT_EQ(f.w.blocked, 0u) << "inter-frame park must not be charged as a block on frame 3";
    EXPECT_EQ(f.w.longest, 0u);
}

TEST(XnuSchedRender, PostSwapWorkEndingPastHalfAPeriodDoesNotOpenTheNextFrameEarly)
{
    Frames f;
    f.Frame(Vsync(1) + 300, 400);  // on-time swap at Vsync(1) + 700
    f.Dispatch(Vsync(1) + 950, Vsync(1) + 950);
    f.cpu += 30;
    f.Frame(Vsync(2) + 300, 900);
    EXPECT_EQ(f.lines, 1);
    EXPECT_EQ(f.dropped, 0);
    EXPECT_EQ(f.w.woke, Vsync(2) + 300);
    EXPECT_EQ(f.w.blocked, 0u);
}

TEST(XnuSchedRender, VsyncJitterPastThePredictedFrameEndDoesNotDropTheSwap)
{
    switch_render_window w = {};
    bool late = false;
    switch_render_dispatched(&w, 1, 1000, 1010, 100, 1005, kMinBlock, kHalf);
    ASSERT_TRUE(switch_render_swap(&w, 1, 1000, 1400, kHalf, &late));
    EXPECT_FALSE(late);
    // Predicted frame 2 is 2650, actual V2 arrives 20 ticks late at 2670; thread wakes at 2660 before V2.
    switch_render_dispatched(&w, 1, 1000, 2660, 290, 2660, kMinBlock, kHalf);
    EXPECT_TRUE(w.pending);
    EXPECT_EQ(w.vsyncs, 2u);
    ASSERT_TRUE(switch_render_swap(&w, 2, 2670, 3660, kHalf, &late));
    EXPECT_TRUE(late);
    EXPECT_FALSE(w.pending);
    EXPECT_EQ(w.vsync, 2670u);
    EXPECT_EQ(w.woke, 2660u);
}

}  // namespace
