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
    e.waits = 1;
    e.hint_ticks[0x1c] = 9;
    e.hint_waits[0x1c] = 1;
    e.longest[0] = {.ticks = 9, .hint = 0x1c};
    const std::string line = Line(e);
    EXPECT_THAT(line, HasSubstr("(wait kind 0x1c 9 us/1)"));
    EXPECT_THAT(line, HasSubstr(" 9 us wait kind 0x1c, user pc"));
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
        switch_ios::test::Body(source, "\nstatic void busy_wait_of(") +
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
    EXPECT_THAT(source, HasSubstr("\n#if DEVELOPMENT || DEBUG\n"));
    EXPECT_THAT(source, HasSubstr("\n#endif /* DEVELOPMENT || DEBUG */\n"));
}

}  // namespace
