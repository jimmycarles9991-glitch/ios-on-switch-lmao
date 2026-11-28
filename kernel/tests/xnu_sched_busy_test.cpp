// kernel/switch/sched/busy.h on the host: how a UI main thread's departures and dispatches split a stretch between two
// run-loop waits, and the line; and where both kernels feed it (under the thread's lock, nothing that locks, sleeps,
// allocates or logs there, nothing in a release kernel).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "bazel/testing/source_text.h"
#include "sched/busy.h"

using testing::HasSubstr;
using testing::Not;

namespace {

constexpr uint64_t kMin = 100;

struct Thread {
    switch_busy b = {};
    switch_busy_episode done = {};
    int ended = 0;

    void leave(uint64_t now, uint64_t cpu, bool waiting, bool idle, uint8_t hint = 0, uint64_t pc = 0)
    {
        switch_busy_wait w = {};
        w.hint = hint;
        w.pc = pc;
        w.pc_where = SWITCH_STALL_DSC;
        if (switch_busy_departed(&b, now, cpu, waiting, idle, &w, kMin, &done))
            ended++;
    }

    void run(uint64_t now, uint64_t made)
    {
        switch_busy_dispatched(&b, now, made);
    }
};

std::string Line(const switch_busy_episode &e, size_t size = 900)
{
    std::string out(size, '\0');
    const size_t n = switch_busy_format(out.data(), out.size(), &e, 1000, 1, 4);
    EXPECT_LT(n, size);
    out.resize(n);
    return out;
}

TEST(XnuSchedBusy, AStretchSplitsIntoRunningBlockedAndWaitingForACore)
{
    Thread t;
    t.leave(100, 50, true, true);
    t.run(210, 200);
    t.leave(400, 240, true, false, 2, 0x1234);
    t.run(520, 500);
    t.leave(700, 420, false, false);
    t.run(750, 700);
    EXPECT_EQ(t.ended, 0);
    t.leave(900, 570, true, true);
    ASSERT_EQ(t.ended, 1);
    EXPECT_EQ(t.done.woke, 200u);
    EXPECT_EQ(t.done.end, 900u);
    EXPECT_EQ(t.done.cpu, 520u);
    EXPECT_EQ(t.done.blocked, 100u);
    EXPECT_EQ(t.done.runnable, 10u + 20u + 50u);
    EXPECT_EQ(t.done.waits, 1u);
    EXPECT_EQ(t.done.preemptions, 1u);
    EXPECT_EQ(t.done.hint_ticks[2], 100u);
    EXPECT_EQ(t.done.hint_waits[2], 1u);
    EXPECT_EQ(t.done.longest[0].ticks, 100u);
    EXPECT_EQ(t.done.longest[0].pc, 0x1234u);
}

struct Split {
    uint64_t on, blocked, runnable;
};

Split SplitAt(const Thread &t, uint64_t now, uint64_t cpu, uint64_t made)
{
    Split s{};
    switch_busy_split_now(&t.b, now, cpu, made, &s.on, &s.blocked, &s.runnable);
    return s;
}

// A capture reads the stretch while it lasts. backboardd's 64 s boot stretch was spent on a run queue: the segment the
// thread is in counts, whichever of the three it is.
TEST(XnuSchedBusy, AStretchStillOpenCountsTheSegmentItIsIn)
{
    Thread t;
    t.leave(100, 50, true, true);
    t.run(210, 200);
    const Split on = SplitAt(t, 300, 140, 0);
    EXPECT_EQ(on.on, 90u) << "on a core: its CPU time since the stretch began";
    EXPECT_EQ(on.blocked, 0u);
    EXPECT_EQ(on.runnable, 10u);

    t.leave(400, 240, true, false, 2);
    const Split blocked = SplitAt(t, 450, 240, 0);
    EXPECT_EQ(blocked.on, 190u);
    EXPECT_EQ(blocked.blocked, 50u) << "in a wait: since it left the core";
    EXPECT_EQ(blocked.runnable, 10u);
    const Split woken = SplitAt(t, 600, 240, 500);
    EXPECT_EQ(woken.blocked, 100u) << "woken at 500 and not yet run: blocked until then";
    EXPECT_EQ(woken.runnable, 10u + 100u);

    t.run(620, 500);
    t.leave(700, 320, false, false);
    const Split queued = SplitAt(t, 64700, 320, 700);
    EXPECT_EQ(queued.on, 270u);
    EXPECT_EQ(queued.blocked, 100u);
    EXPECT_EQ(queued.runnable, 10u + 120u + 64000u) << "preempted: on a run queue since it left the core";

    Thread idle;
    idle.leave(100, 50, true, true);
    const Split none = SplitAt(idle, 900, 50, 0);
    EXPECT_EQ(none.on + none.blocked + none.runnable, 0u) << "no stretch open";
}

TEST(XnuSchedBusy, ShortStretchesAreNotReportedAndTheNextStartsFresh)
{
    Thread t;
    t.leave(100, 0, true, true);
    t.run(110, 105);
    t.leave(150, 40, true, false, 6);
    t.run(160, 155);
    t.leave(180, 60, true, true);
    EXPECT_EQ(t.ended, 0);
    t.run(300, 290);
    t.leave(500, 260, true, true);
    ASSERT_EQ(t.ended, 1);
    EXPECT_EQ(t.done.woke, 290u);
    EXPECT_EQ(t.done.waits, 0u);
    EXPECT_EQ(t.done.blocked, 0u);
    EXPECT_EQ(t.done.cpu, 200u);
}

TEST(XnuSchedBusy, NothingIsCountedBeforeTheFirstRunLoopWait)
{
    Thread t;
    t.run(10, 5);
    t.leave(50, 40, true, false, 2);
    t.run(80, 70);
    t.leave(400, 300, true, true);
    EXPECT_EQ(t.ended, 0);
    EXPECT_FALSE(t.b.open);
}

TEST(XnuSchedBusy, ANullEpisodeStillReportsEndedAndClosesTheStretch)
{
    switch_busy b = {};
    switch_busy_wait w = {};
    EXPECT_FALSE(switch_busy_departed(&b, 100, 0, true, true, &w, kMin, nullptr));
    switch_busy_dispatched(&b, 200, 200);
    EXPECT_TRUE(switch_busy_departed(&b, 350, 150, true, true, &w, kMin, nullptr));
    EXPECT_FALSE(b.open);
    switch_busy_dispatched(&b, 400, 400);
    EXPECT_FALSE(switch_busy_departed(&b, 450, 200, true, true, &w, kMin, nullptr)) << "under kMin";
    EXPECT_FALSE(b.open);
}

TEST(XnuSchedBusy, PreemptionsSampleWhereTheStretchRan)
{
    Thread t;
    t.leave(1000, 0, true, true);
    t.run(2000, 2000);
    for (uint64_t i = 0; i < 5; i++)
        switch_busy_sampled(&t.b, SWITCH_STALL_DSC, 0x1a2b00 + i);  // one 64-byte bucket
    switch_busy_sampled(&t.b, SWITCH_STALL_MAIN, 0x400);
    switch_busy_sampled(&t.b, SWITCH_STALL_DSC, 0x9000);
    switch_busy_sampled(&t.b, SWITCH_STALL_DSC, 0x9000);
    t.leave(3000, 900, true, true);
    ASSERT_EQ(t.ended, 1);
    EXPECT_EQ(t.done.samples, 8u);
    EXPECT_THAT(Line(t.done),
                HasSubstr(" user pc at its 8 preemptions (the trap site when in the kernel): dsc 0x1a2b00 x5; dsc "
                          "0x9000 x2; main+0x400 x1;"));
}

TEST(XnuSchedBusy, AFullSampleTableGivesItsLeastCountedBucketToANewOne)
{
    switch_busy b = {};
    b.open = true;
    for (uint64_t i = 0; i < SWITCH_BUSY_PCS; i++)
        for (uint64_t k = 0; k <= i; k++)
            switch_busy_sampled(&b, SWITCH_STALL_DSC, 0x1000 * (i + 1));
    switch_busy_sampled(&b, SWITCH_STALL_DSC, 0x9000);
    bool newest = false, least = false;
    for (const switch_busy_pc &p : b.e.pcs) {
        newest = newest || (p.pc == 0x9000 && p.n == 1);
        least = least || p.pc == 0x1000;
    }
    EXPECT_TRUE(newest);
    EXPECT_FALSE(least) << "the bucket counted once went";
    switch_busy closed = {};
    switch_busy_sampled(&closed, SWITCH_STALL_DSC, 0x1000);
    EXPECT_EQ(closed.e.samples, 0u) << "no stretch open: nothing to count";
}

TEST(XnuSchedBusy, TheThreeLongestWaitsAreKeptInOrder)
{
    Thread t;
    t.leave(0, 0, true, true);
    t.run(10, 10);
    uint64_t now = 10;
    for (uint64_t len : {5u, 40u, 7u, 30u, 50u, 1u}) {
        t.leave(now + 1, 0, true, false, static_cast<uint8_t>(len % 3), len);
        now += 1 + len;
        t.run(now, now);
    }
    t.leave(now + 200, 0, true, true);
    ASSERT_EQ(t.ended, 1);
    EXPECT_EQ(t.done.longest[0].ticks, 50u);
    EXPECT_EQ(t.done.longest[1].ticks, 40u);
    EXPECT_EQ(t.done.longest[2].ticks, 30u);
    EXPECT_EQ(t.done.waits, 6u);
}

TEST(XnuSchedBusy, TheLineNamesKindsLargestFirstAndTheLongestWaits)
{
    switch_busy_episode e = {};
    e.cpu = 540;
    e.blocked = 52;
    e.waits = 31;
    e.runnable = 20;
    e.preemptions = 12;
    e.hint_ticks[2] = 40;
    e.hint_waits[2] = 12;
    e.hint_ticks[10] = 8;
    e.hint_waits[10] = 15;
    e.longest[0] = {.ticks = 25,
                    .pc = 0x1a2b,
                    .lr = 0x40,
                    .kernel = 0xfff,
                    .event = 0x77,
                    .pc_where = SWITCH_STALL_DSC,
                    .lr_where = SWITCH_STALL_MAIN,
                    .hint = 2};
    const std::string line = Line(e);
    EXPECT_THAT(line,
                HasSubstr("on core 540 us, blocked 52 us in 31 waits (mach_msg receive 40 us/12, ulock 8 us/15)"));
    EXPECT_THAT(line, HasSubstr("waiting for a core 20 us, 12 preemptions"));
    EXPECT_THAT(line, HasSubstr("25 us mach_msg receive, user pc dsc 0x1a2b lr main+0x40, kernel 0xfff, event 0x77;"));
}

TEST(XnuSchedBusy, AHintStallHasNoNameForGivesItsNumber)
{
    switch_busy_episode e = {};
    e.waits = 2;
    e.hint_ticks[0x1c] = 9;
    e.hint_waits[0x1c] = 1;
    e.longest[0] = {.ticks = 9, .hint = 0x1c};
    e.longest[1] = {.ticks = 4, .hint = 0x23};
    const std::string line = Line(e);
    EXPECT_THAT(line, HasSubstr("(paging in progress 9 us/1)"));
    EXPECT_THAT(line, HasSubstr(" 9 us paging in progress, user pc"));
    EXPECT_THAT(line, HasSubstr(" 4 us wait kind 0x23, user pc"));
}

TEST(XnuSchedBusy, TheLineStaysInsideItsBuffer)
{
    switch_busy_episode e = {};
    for (unsigned h = 0; h < SWITCH_BUSY_HINTS; h++) {
        e.hint_ticks[h] = 1000u + h;
        e.hint_waits[h] = 1;
    }
    for (auto &w : e.longest)
        w.ticks = 99999999;
    for (size_t size : {1u, 7u, 40u, 120u, 300u}) {
        std::string out(size + 8, 'Z');
        const size_t n = switch_busy_format(out.data(), size, &e, 125, 3, 4);
        EXPECT_LT(n, size);
        EXPECT_EQ(out[size], 'Z') << size;
        EXPECT_EQ(out[n], '\0') << size;
    }
    char none = 'Z';
    EXPECT_EQ(switch_busy_format(&none, 0, &e, 125, 3, 4), 0u);
    EXPECT_EQ(none, 'Z');
}

TEST(XnuSchedBusySource, BothKernelsFeedItUnderTheThreadsLock)
{
    for (const char *tree : {"../xnu-10063.141.1", "../xnu-12377.121.6"}) {
        const std::string prim = switch_ios::test::SourceText(std::string(tree) + "/osfmk/kern/sched_prim.c");
        const std::string dispatch = switch_ios::test::Body(prim, "\nthread_dispatch(\n");
        const size_t lock = dispatch.find("\t\t\tthread_lock(thread);\n");
        const size_t hook =
            dispatch.find("thread->computation_metered += (processor->last_dispatch - thread->computation_epoch);\n"
                          "\t\t\tswitch_sched_off_core(thread, processor->last_dispatch);\n");
        const size_t wait = dispatch.find("if (!(thread->state & TH_WAIT)) {");
        ASSERT_NE(lock, std::string::npos) << tree;
        ASSERT_NE(hook, std::string::npos) << tree;
        EXPECT_LT(lock, hook) << tree;
        EXPECT_LT(hook, wait) << tree;
    }
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/sched.h");
    EXPECT_THAT(
        switch_ios::test::Body(header, "static inline void switch_sched_on_core(thread_t thread, uint64_t now)"),
        HasSubstr("switch_sched_busy_dispatched(thread, now);"));
}

TEST(XnuSchedBusySource, NothingUnderTheThreadLockLocksSleepsAllocatesOrLogs)
{
    const std::string source = switch_ios::test::SourceText("../switch/sched/busy.c");
    const std::string header = switch_ios::test::SourceText("../switch/sched/busy.h");
    const std::string hot =
        switch_ios::test::Body(source, "\nstatic struct busy_slot *busy_slot_of(") +
        switch_ios::test::Body(source, "\nuint8_t switch_sched_user_site(") +
        switch_ios::test::Body(source, "\nvoid switch_sched_wait_of(") +
        switch_ios::test::Body(source, "\nvoid switch_sched_busy_departed(") +
        switch_ios::test::Body(source, "\nvoid switch_sched_busy_dispatched(") +
        switch_ios::test::Between(header, "struct switch_busy {", "static inline uint64_t switch_busy_us(");
    ASSERT_FALSE(hot.empty());
    for (const char *call : {"lock(", "lck_", "alloc", "kfree", "os_log", "printf", "thread_call", "IOLock", "IOSleep",
                             "assert_wait", "thread_block", "copyin", "msleep", "zalloc"})
        EXPECT_THAT(hot, Not(HasSubstr(call))) << call;
}

TEST(XnuSchedBusySource, ReleaseKernelsCompileItToNothing)
{
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/sched.h");
    const size_t release = header.find("#else /* DEVELOPMENT || DEBUG */");
    ASSERT_NE(release, std::string::npos);
    const std::string stubs = header.substr(release);
    for (const char *hook : {"switch_sched_off_core(", "switch_sched_busy_exec(", "switch_sched_busy_tick("})
        EXPECT_THAT(stubs, HasSubstr(std::string("static inline void ") + hook)) << hook;
    const std::string source = switch_ios::test::SourceText("../switch/sched/busy.c");
    EXPECT_THAT(source, HasSubstr("\n#if (DEVELOPMENT || DEBUG) && \\\n"
                                  "    ((defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED) || "
                                  "(defined(SWITCH_RECORDER) && SWITCH_RECORDER))\n"));
    EXPECT_THAT(source, HasSubstr("\n#endif /* DEVELOPMENT || DEBUG */\n"));
}

}  // namespace
