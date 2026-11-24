// kernel/switch/recorder/recorder.h on the host: which busy stretches of a UI main thread are a hang, which triggers
// take the dump, the gap, the slots dumps rotate through, and the status line HW Test reads.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "recorder/recorder.h"

namespace {

constexpr uint64_t kMs = 1000000;  // ticks are nanoseconds
constexpr uint64_t kBound = SWITCH_RECORDER_HANG_NS;

TEST(XnuRecorder, AStretchOpenForTheBoundIsOneHang)
{
    switch_recorder_watch w{};
    const uint64_t woke = 1000 * kMs;

    EXPECT_FALSE(switch_recorder_watch_due(&w, true, woke, woke + 249 * kMs, kBound)) << "open 249 ms";
    EXPECT_TRUE(switch_recorder_watch_due(&w, true, woke, woke + 250 * kMs, kBound));
    switch_recorder_watch_fired(&w, woke);
    EXPECT_FALSE(switch_recorder_watch_due(&w, true, woke, woke + 900 * kMs, kBound)) << "the same stretch again";
    EXPECT_TRUE(switch_recorder_watch_due(&w, true, woke + 2000 * kMs, woke + 2300 * kMs, kBound)) << "a new stretch";
}

TEST(XnuRecorder, AThreadBackInItsRunLoopIsNoHangHoweverLongAgoItWoke)
{
    switch_recorder_watch w{};

    // The tick's first sample after a long idle system: busy.c saw the thread return to its run loop, so it is closed.
    EXPECT_FALSE(switch_recorder_watch_due(&w, false, 1000 * kMs, 9000 * kMs, kBound));
    // Two samples that find it away, with a run-loop wait between them, are two stretches: neither has the bound.
    EXPECT_FALSE(switch_recorder_watch_due(&w, true, 1000 * kMs, 1125 * kMs, kBound));
    EXPECT_FALSE(switch_recorder_watch_due(&w, true, 1200 * kMs, 1250 * kMs, kBound));
}

TEST(XnuRecorder, AHangATriggerTurnedAwayFiresAgainWhileItLasts)
{
    switch_recorder_watch w{};
    const uint64_t woke = 1000 * kMs;

    EXPECT_TRUE(switch_recorder_watch_due(&w, true, woke, woke + 300 * kMs, kBound));  // turned away: not latched
    EXPECT_TRUE(switch_recorder_watch_due(&w, true, woke, woke + 400 * kMs, kBound));
}

TEST(XnuRecorder, ATriggerTakesAnIdleRecorderAndAHangTakesAQueuedTail)
{
    EXPECT_TRUE(switch_recorder_takes(SWITCH_RECORDER_IDLE, SWITCH_RECORDER_MARK));
    EXPECT_TRUE(switch_recorder_takes(SWITCH_RECORDER_LATE_FRAME, SWITCH_RECORDER_HANG));
    EXPECT_TRUE(switch_recorder_takes(SWITCH_RECORDER_MARK, SWITCH_RECORDER_HANG));
    EXPECT_FALSE(switch_recorder_takes(SWITCH_RECORDER_HANG, SWITCH_RECORDER_HANG));
    EXPECT_FALSE(switch_recorder_takes(SWITCH_RECORDER_LATE_FRAME, SWITCH_RECORDER_MARK));
    EXPECT_FALSE(switch_recorder_takes(SWITCH_RECORDER_CLAIM, SWITCH_RECORDER_HANG)) << "a claim still writing";
    EXPECT_FALSE(switch_recorder_takes(SWITCH_RECORDER_RUNNING | SWITCH_RECORDER_MARK, SWITCH_RECORDER_HANG))
        << "a dump already reading the ring";
}

TEST(XnuRecorder, NoDumpWithinTheGapAndTheSlotsTurnRound)
{
    const uint64_t gap = SWITCH_RECORDER_GAP_NS;
    switch_recorder_gate g{};

    EXPECT_FALSE(switch_recorder_in_gap(0, 5 * kMs, gap)) << "no dump yet";
    EXPECT_TRUE(switch_recorder_in_gap(1000 * kMs, 1000 * kMs + gap - 1, gap));
    EXPECT_FALSE(switch_recorder_in_gap(1000 * kMs, 1000 * kMs + gap, gap));
    for (unsigned i = 0; i < SWITCH_RECORDER_SLOTS; i++) {
        EXPECT_EQ(g.slot, i);
        switch_recorder_gate_done(&g);
    }
    EXPECT_EQ(g.slot, 0u);
    EXPECT_EQ(g.dumps, SWITCH_RECORDER_SLOTS);
}

TEST(XnuRecorder, TheStatusLineCarriesTheCostAndEveryFieldHWTestReads)
{
    char line[640];
    switch_recorder_cost cost{};
    switch_recorder_counts n{};
    switch_recorder_gate g{};
    switch_recorder_last last{};

    cost.ring = 262144;
    cost.span_ms = 2000;
    cost.samples = 12345;
    n.triggers[SWITCH_RECORDER_HANG] = 3;
    n.skipped = 2;
    n.held = 1;
    g.dumps = 4;
    last.reason = SWITCH_RECORDER_HANG;
    last.tid = 77;
    last.value = 412;
    last.trace_bytes = 16 << 20;
    switch_recorder_status(line, sizeof(line), &cost, &n, &g, &last, 1500);
    const std::string s(line);
    for (const char *want : {"ring=262144 ", "span_ms=2000 ", "events_s=131072 ", "samples=12345 ", "dumps=4 ",
                             "skipped=2 ", "held=1 ", "taken=0 ", "hang=3 ", "last_reason=hang ", "last_tid=77 ",
                             "last_value=412 ", "last_trace_bytes=16777216 ", "last_age_ms=1500"}) {
        EXPECT_NE(s.find(want), std::string::npos) << want << " in " << s;
    }
    switch_recorder_cost unwrapped{};
    unwrapped.ring = 262144;
    EXPECT_EQ(switch_recorder_events_s(&unwrapped), 0u) << "no rate before the ring has wrapped";
}

}  // namespace
