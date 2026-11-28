// kernel/switch/recorder/recorder.h on the host: which busy stretches of a UI main thread are a hang, which triggers
// are taken and in which order they are captured, which slot a capture replaces, the dump header, and the status line
// HW Test reads.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
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

const switch_recorder_ask *ptr(const switch_recorder_ask &a)
{
    static switch_recorder_ask held;
    held = a;
    return &held;
}

switch_recorder_ask Ask(uint32_t reason, uint64_t at_ms, uint64_t tid = 0, uint32_t seq = 0)
{
    return {.at = at_ms * kMs, .tid = tid, .value = 0, .reason = reason, .seq = seq};
}

constexpr uint64_t kTail = SWITCH_RECORDER_TAIL_NS;

switch_recorder_recent Spacing()
{
    switch_recorder_recent r{};
    r.gap = SWITCH_RECORDER_GAP_NS;
    r.repeat = SWITCH_RECORDER_REPEAT_NS;
    return r;
}

// 25 Nov: the owner pressed Capture eight times in the first 3.5 minutes after boot, each while a dump was being
// written or inside the gap after one; none was dumped. A mark is taken whatever else waits.
TEST(XnuRecorder, EveryMarkIsTakenWhileAnAutomaticCaptureWaits)
{
    switch_recorder_asks q{};
    switch_recorder_recent r = Spacing();

    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 1000, 606)), &r, 0, 0), SWITCH_RECORDER_ADMITTED);
    for (uint32_t i = 0; i < 8; i++) {
        EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_MARK, 1100 + i * 100)), &r, 0, 0),
                  SWITCH_RECORDER_ADMITTED)
            << "mark " << i;
    }
    EXPECT_EQ(q.n, 9u);
}

// With the real 2 s gap: a late frame waits out its 300 ms tail, and a hang inside that tail takes its place (it was never
// captured, so its gap does not count). A hang 1 s after a capture that ran is still turned away.
TEST(XnuRecorder, OneAutomaticAskWaitsAtATimeAndAHangReplacesALowerOne)
{
    switch_recorder_asks q{};
    switch_recorder_recent r = Spacing();

    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_LATE_FRAME, 100)), &r, 0, 0), SWITCH_RECORDER_ADMITTED);
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_LATE_FRAME, 120)), &r, 0, 0),
              SWITCH_RECORDER_REFUSED_QUEUED);
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 250, 514)), &r, 0, 0), SWITCH_RECORDER_ADMITTED);
    ASSERT_EQ(q.n, 1u);
    EXPECT_EQ(q.a[0].reason, SWITCH_RECORDER_HANG) << "the hang took the late frame's place";
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 260, 606)), &r, 0, 0),
              SWITCH_RECORDER_REFUSED_QUEUED);
    q = {};
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_LATE_FRAME, 1250)), &r, 0, 0),
              SWITCH_RECORDER_REFUSED_GAP)
        << "the hang was captured: its gap counts, the replaced late frame's does not";
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_LATE_FRAME, 2250)), &r, 0, 0),
              SWITCH_RECORDER_ADMITTED);
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 2300, 707)), &r, 0, 0), SWITCH_RECORDER_ADMITTED)
        << "the hang replaces the waiting late frame before that late frame's gap";
    q = {};
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 3300, 606)), &r, 0, 0),
              SWITCH_RECORDER_REFUSED_GAP)
        << "1 s after the captured hang";
}

// A capture stops every CPU for its stackshot and takes 16 MiB: another thread's waits 2 s after any automatic one,
// the same thread's same reason 10 s.
TEST(XnuRecorder, AutomaticCapturesAreSpacedAndHWTestsHeldThreadGoesThrough)
{
    switch_recorder_asks q{};
    switch_recorder_recent r = Spacing();
    const uint64_t until = (1000 + 5000) * kMs;

    ASSERT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 1000, 514)), &r, 0, 0), SWITCH_RECORDER_ADMITTED);
    q = {};
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 2999, 606)), &r, 0, 0),
              SWITCH_RECORDER_REFUSED_GAP);
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 3000, 606)), &r, 0, 0), SWITCH_RECORDER_ADMITTED)
        << "another thread after the gap";
    q = {};
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 6000, 514)), &r, 0, 0),
              SWITCH_RECORDER_REFUSED_REPEAT);
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_LATE_FRAME, 6000, 514)), &r, 0, 0),
              SWITCH_RECORDER_ADMITTED)
        << "the same thread for another reason";
    q = {};
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 11000, 514)), &r, 0, 0), SWITCH_RECORDER_ADMITTED)
        << "the same thread and reason after the repeat";
    q = {};
    switch_recorder_recent held = Spacing();
    ASSERT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 1000, 514)), &held, 0, 0),
              SWITCH_RECORDER_ADMITTED);
    q = {};
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 1500, 514)), &held, 77, until),
              SWITCH_RECORDER_REFUSED_HELD)
        << "another thread while HW Test holds the recorder";
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 1500, 77)), &held, 77, until),
              SWITCH_RECORDER_ADMITTED)
        << "the held thread goes through the spacing";
}

TEST(XnuRecorder, AFullQueueOfMarksRefusesTheNextAndAMarkTakesTheAutomaticPlace)
{
    switch_recorder_asks q{};
    switch_recorder_recent r = Spacing();

    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 1, 9)), &r, 0, 0), SWITCH_RECORDER_ADMITTED);
    for (uint32_t i = 1; i < SWITCH_RECORDER_ASKS; i++) {
        ASSERT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_MARK, 10 + i)), &r, 0, 0),
                  SWITCH_RECORDER_ADMITTED);
    }
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_MARK, 500)), &r, 0, 0), SWITCH_RECORDER_ADMITTED);
    EXPECT_EQ(switch_recorder_queued_auto(&q), q.n) << "the mark took the hang's place";
    EXPECT_EQ(switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_MARK, 600)), &r, 0, 0), SWITCH_RECORDER_REFUSED_FULL);
}

TEST(XnuRecorder, MarksAreCapturedFirstAndATailWaitsItsTime)
{
    switch_recorder_asks q{};
    switch_recorder_recent r = Spacing();
    switch_recorder_ask got{};
    uint64_t next = 0;

    switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_LATE_FRAME, 100, 0, 1)), &r, 0, 0);
    switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_MARK, 150, 0, 2)), &r, 0, 0);
    switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_MARK, 120, 0, 3)), &r, 0, 0);
    EXPECT_FALSE(switch_recorder_take(&q, 200 * kMs, kTail, &got, &next)) << "no tail has passed";
    EXPECT_EQ(next, 100 * kMs + kTail);
    ASSERT_TRUE(switch_recorder_take(&q, 500 * kMs, kTail, &got, &next));
    EXPECT_EQ(got.seq, 3u) << "the earlier mark first";
    ASSERT_TRUE(switch_recorder_take(&q, 500 * kMs, kTail, &got, &next));
    EXPECT_EQ(got.seq, 2u);
    ASSERT_TRUE(switch_recorder_take(&q, 500 * kMs, kTail, &got, &next));
    EXPECT_EQ(got.seq, 1u);
    EXPECT_FALSE(switch_recorder_take(&q, 500 * kMs, kTail, &got, &next));
    EXPECT_EQ(next, UINT64_MAX);
    switch_recorder_admit(&q, ptr(Ask(SWITCH_RECORDER_HANG, 9000, 514, 4)), &r, 0, 0);
    EXPECT_TRUE(switch_recorder_take(&q, 9000 * kMs, kTail, &got, &next)) << "a hang is due at once";
}

// dumpcost.txt, 25 Nov: one refused hang fired ten times in 1.25 s, once per scheduler tick.
TEST(XnuRecorder, ARefusalIsLoggedOnceASecondPerReasonAndThread)
{
    switch_recorder_refusal_seen seen{};

    EXPECT_TRUE(switch_recorder_refusal_logged(&seen, ptr(Ask(SWITCH_RECORDER_HANG, 1000, 606)), 1000 * kMs));
    for (uint64_t ms = 1125; ms < 2000; ms += 125) {
        EXPECT_FALSE(switch_recorder_refusal_logged(&seen, ptr(Ask(SWITCH_RECORDER_HANG, ms, 606)), 1000 * kMs));
    }
    EXPECT_TRUE(switch_recorder_refusal_logged(&seen, ptr(Ask(SWITCH_RECORDER_HANG, 2100, 606)), 1000 * kMs));
    EXPECT_TRUE(switch_recorder_refusal_logged(&seen, ptr(Ask(SWITCH_RECORDER_HANG, 2150, 514)), 1000 * kMs));
    EXPECT_FALSE(switch_recorder_refusal_logged(&seen, ptr(Ask(SWITCH_RECORDER_HANG, 2275, 606)), 1000 * kMs))
        << "two threads alternating are each once a second";
    EXPECT_FALSE(switch_recorder_refusal_logged(&seen, ptr(Ask(SWITCH_RECORDER_HANG, 2400, 514)), 1000 * kMs));
}

switch_recorder_slot Slot(bool this_boot, bool mark, uint32_t seq)
{
    return {.used = true, .this_boot = this_boot, .mark = mark, .seq = seq};
}

// 25 Nov: two of the eight slots held 24 Nov dumps, and this boot's first dumps were overwritten by later ones.
TEST(XnuRecorder, ACaptureReplacesOtherBootsThenLaterAutomaticCapturesAndNeverAMarkForAnAutomaticOne)
{
    switch_recorder_slot s[4] = {Slot(true, false, 9), Slot(false, true, 1), Slot(false, false, 5),
                                 Slot(true, true, 3)};

    EXPECT_EQ(switch_recorder_pick_slot(s, 4, false), 2) << "another boot's automatic capture first";
    s[2] = Slot(true, false, 10);
    EXPECT_EQ(switch_recorder_pick_slot(s, 4, false), 1) << "then another boot's mark";
    s[1] = Slot(true, false, 11);
    EXPECT_EQ(switch_recorder_pick_slot(s, 4, false), 0) << "then this boot's oldest past its first ones";

    switch_recorder_slot kept[3] = {Slot(true, false, 2), Slot(true, true, 4), Slot(true, true, 5)};
    EXPECT_EQ(switch_recorder_pick_slot(kept, 3, false), -1) << "boot captures and marks stay";
    EXPECT_EQ(switch_recorder_pick_slot(kept, 3, true), 0) << "a mark takes a boot capture before a mark";
    kept[0] = Slot(true, true, 6);
    EXPECT_EQ(switch_recorder_pick_slot(kept, 3, true), 1) << "then the oldest mark";
    switch_recorder_slot one_free[2] = {Slot(false, false, 1), {}};
    EXPECT_EQ(switch_recorder_pick_slot(one_free, 2, false), 1) << "a free slot first";
}

TEST(XnuRecorder, TheStoreTakesATenthOfTheFreeSpaceWithinItsBounds)
{
    EXPECT_EQ(switch_recorder_slots_for(0), 0u) << "a nearly full volume gets no slot";
    EXPECT_EQ(switch_recorder_slots_for((60ULL << 20) - 1), 0u);
    EXPECT_EQ(switch_recorder_slots_for(60ULL << 20), 1u) << "a tenth of 60 MiB holds one 6 MiB slot";
    EXPECT_EQ(switch_recorder_slots_for(2ULL << 30), 2048u / 10 / 6);
    EXPECT_EQ(switch_recorder_slots_for(1ULL << 40), SWITCH_RECORDER_SLOTS_MAX);
}

TEST(XnuRecorder, TheHeaderFitsItsPlaceAndReadsBackAsItsSlot)
{
    switch_recorder_head h{};
    char text[SWITCH_RECORDER_HEADER];
    const char *boot = "2693043C-01C2-4E03-9DA5-FDBCF1DD9490";

    std::snprintf(h.boot, sizeof(h.boot), "%s", boot);
    std::snprintf(h.kernel, sizeof(h.kernel), "724459E3-00EE-E993-E2A1-D165FC92893A");
    h.seq = 7;
    h.reason = SWITCH_RECORDER_MARK;
    h.at = UINT64_MAX;
    h.shot_at = h.frozen_at = h.done_at = UINT64_MAX;
    h.shot_us = h.ring_us = h.tod_s = h.shot_bytes = h.ring_bytes = h.deflate_bytes = h.tid = h.value = UINT64_MAX;
    h.tod_us = 999999;
    h.ask = h.on_ms = h.blocked_ms = h.runnable_ms = h.ring_lost = UINT32_MAX;
    ASSERT_TRUE(switch_recorder_header(text, &h)) << "the widest header fits";
    EXPECT_EQ(text[SWITCH_RECORDER_HEADER - 1], '\n');
    EXPECT_EQ(std::strncmp(text, "SwitchRecorder 2 boot=2693043C", 30), 0);
    const switch_recorder_slot mine = switch_recorder_slot_of(text, sizeof(text), boot);
    EXPECT_TRUE(mine.used && mine.this_boot && mine.mark);
    EXPECT_EQ(mine.seq, 7u);
    EXPECT_FALSE(switch_recorder_slot_of(text, sizeof(text), "11111111-2222-3333-4444-555555555555").this_boot);
    EXPECT_EQ(switch_recorder_header_field(text, sizeof(text), "deflate"), UINT64_MAX);
    char empty[SWITCH_RECORDER_HEADER] = {};
    EXPECT_FALSE(switch_recorder_slot_of(empty, sizeof(empty), boot).used) << "a file pulled before its header";
}

// tools/perf/perf_test.py's RecFileTest reads this same line: a change to the header changes both.
TEST(XnuRecorder, TheHeaderIsTheLineToolsPerfReads)
{
    const char *gold =
        "SwitchRecorder 2 boot=2693043C-01C2-4E03-9DA5-FDBCF1DD9490 kernel=724459E3-00EE-E993-E2A1-D165FC9289"
        "3A seq=3 ask=41 reason=hang tid=514 value=320 at=4340000000 shot_at=4340057600 frozen_at=4340096000 "
        "done_at=4340672000 shot_us=3000 ring_us=30000 tod=1795548000.250000 shot=19536 ring=23552 deflate=90"
        "00 ring_lost=0"
        " on_ms=20 blocked_ms=145 runnable_ms=98";
    switch_recorder_head h{};
    char text[SWITCH_RECORDER_HEADER];

    std::snprintf(h.boot, sizeof(h.boot), "2693043C-01C2-4E03-9DA5-FDBCF1DD9490");
    std::snprintf(h.kernel, sizeof(h.kernel), "724459E3-00EE-E993-E2A1-D165FC92893A");
    h.seq = 3;
    h.ask = 41;
    h.reason = SWITCH_RECORDER_HANG;
    h.tid = 514;
    h.value = 320;
    h.at = 4340000000;
    h.shot_at = 4340057600;
    h.frozen_at = 4340096000;
    h.done_at = 4340672000;
    h.shot_us = 3000;
    h.ring_us = 30000;
    h.tod_s = 1795548000;
    h.tod_us = 250000;
    h.shot_bytes = 19536;
    h.ring_bytes = 23552;
    h.deflate_bytes = 9000;
    h.on_ms = 20;
    h.blocked_ms = 145;
    h.runnable_ms = 98;
    ASSERT_TRUE(switch_recorder_header(text, &h));
    std::string line(text, sizeof(text));
    line.erase(line.find_last_not_of(" \n") + 1);
    EXPECT_EQ(line, gold);
}

TEST(XnuRecorder, TheStatusLineCarriesTheCostAndEveryFieldHWTestReads)
{
    char line[900];
    switch_recorder_cost cost{};
    switch_recorder_counts n{};
    switch_recorder_last last{};

    cost.ring = 262144;
    cost.span_ms = 2000;
    cost.samples = 12345;
    n.triggers[SWITCH_RECORDER_HANG] = 3;
    n.refused[SWITCH_RECORDER_REFUSED_GAP] = 2;
    n.refused[SWITCH_RECORDER_REFUSED_HELD] = 1;
    n.dumps = 4;
    n.captures = 5;
    n.pending = 1;
    n.pending_bytes = 19 << 20;
    last.head.reason = SWITCH_RECORDER_HANG;
    last.head.tid = 77;
    last.head.value = 412;
    last.head.ring_bytes = 16 << 20;
    last.head.shot_bytes = 1 << 20;
    last.head.at = 100;
    last.head.done_at = 100 + 50000;
    last.slot = 3;
    switch_recorder_status(line, sizeof(line), &cost, &n, &last, 1500);
    const std::string s(line);
    for (const char *want : {"ring=262144 ",
                             "span_ms=2000 ",
                             "events_s=131072 ",
                             "samples=12345 ",
                             "captures=5 ",
                             "dumps=4 ",
                             "pending=1 ",
                             "pending_kib=19456 ",
                             "skipped=2 ",
                             "repeat=0 ",
                             "held=1 ",
                             "no_room=0 ",
                             "taken=0 ",
                             "hang=3 ",
                             "late_frame=0 ",
                             "mark=0 ",
                             "last_reason=hang ",
                             "last_tid=77 ",
                             "last_value=412 ",
                             "last_slot=3 ",
                             "last_trace_bytes=16777216 ",
                             "last_shot_bytes=1048576 ",
                             "last_latency_us=50000 ",
                             "last_trace_us=0 ",
                             "last_shot_us=0 ",
                             "last_age_ms=1500"}) {
        EXPECT_NE(s.find(want), std::string::npos) << want << " in " << s;
    }
    EXPECT_LT(s.size(), sizeof(line) - 1);
    switch_recorder_cost unwrapped{};
    unwrapped.ring = 262144;
    EXPECT_EQ(switch_recorder_events_s(&unwrapped), 0u) << "no rate before the ring has wrapped";
}

}  // namespace
