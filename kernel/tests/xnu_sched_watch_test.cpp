// kernel/switch/sched/watch.h and commit.h on the host: a watched thread's stretch to each commit or touch delivery,
// one per burst of messages, its cause and lines; a commit's way through the render server to the display thread's
// wake; and where the kernel feeds them (the scheduler hooks under the thread's lock: nothing there locks or logs).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "bazel/testing/source_text.h"
#include "sched/commit.h"
#include "sched/watch.h"

using testing::HasSubstr;
using testing::Not;

namespace {

constexpr uint64_t kBurst = 20;

struct Watched {
    switch_watch_thread t = {};
    switch_watch_event ev = {};
    switch_watch_summary sum = {};

    switch_watch_step message(uint64_t now, uint64_t cpu)
    {
        return switch_watch_message(&t, now, cpu, kBurst, &ev, &sum);
    }

    void leave(uint64_t now, uint64_t cpu, bool waiting, bool idle, uint8_t hint = 0)
    {
        switch_busy_wait w = {};
        w.hint = hint;
        switch_busy_departed(&t.busy, now, cpu, waiting, idle, &w, UINT64_MAX, nullptr);
    }

    void run(uint64_t now, uint64_t made)
    {
        switch_busy_dispatched(&t.busy, now, made);
    }
};

TEST(XnuSchedWatch, TheStretchRunsFromTheIdleWakeToTheCommit)
{
    Watched w;
    EXPECT_EQ(w.message(1000, 50), SWITCH_WATCH_FIRST);
    w.leave(1010, 60, true, true);  // back in its run loop
    w.run(5020, 5000);              // woken at 5000, on a core at 5020
    w.leave(5100, 140, true, false, 0x16);
    w.run(5400, 5390);  // a page wait of 290
    EXPECT_EQ(w.message(5500, 240), SWITCH_WATCH_TIMED);
    EXPECT_EQ(w.ev.e.woke, 5000u);
    EXPECT_EQ(w.ev.e.end, 5500u);
    EXPECT_EQ(switch_watch_length(&w.ev.e), 500u);
    EXPECT_EQ(w.ev.e.cpu, 180u);
    EXPECT_EQ(w.ev.e.blocked, 290u);
    EXPECT_EQ(w.ev.e.runnable, 30u);
    EXPECT_EQ(w.ev.e.longest[0].hint, 0x16);
    EXPECT_EQ(w.ev.gap, 4500u);
    EXPECT_FALSE(w.ev.from_event);
    EXPECT_EQ(switch_watch_cause(&w.ev.e), SWITCH_WATCH_BLOCKED);
}

TEST(XnuSchedWatch, ABurstIsOneCommitAndTheNextStretchStartsAtTheLastCommit)
{
    Watched w;
    w.message(1000, 50);
    EXPECT_EQ(w.message(1005, 52), SWITCH_WATCH_SAME_BURST);
    EXPECT_EQ(w.message(1024, 54), SWITCH_WATCH_SAME_BURST) << "each message within the burst gap of the one before";
    // Never back in its run loop: the next commit's stretch is from the commit before.
    w.leave(1100, 120, true, false, 0x0a);
    w.run(1300, 1290);
    EXPECT_EQ(w.message(1500, 300), SWITCH_WATCH_TIMED);
    EXPECT_TRUE(w.ev.from_event);
    EXPECT_EQ(w.ev.e.woke, 1000u);
    EXPECT_EQ(w.ev.e.cpu, 250u);
    EXPECT_EQ(w.ev.e.blocked, 190u);
    EXPECT_EQ(switch_watch_cause(&w.ev.e), SWITCH_WATCH_ON_CORE);
}

TEST(XnuSchedWatch, AParkedWorkQueueThreadIsIdleLikeARunLoop)
{
    EXPECT_TRUE(switch_watch_idle(true, 0x03));
    EXPECT_TRUE(switch_watch_idle(false, SWITCH_WATCH_HINT_PARKED));
#if defined(XNU_12377_121_6) && XNU_12377_121_6
    EXPECT_TRUE(switch_watch_idle(false, SWITCH_WATCH_HINT_PARKED_BOUND));
#else
    EXPECT_FALSE(switch_watch_idle(false, 0x15));
#endif
    EXPECT_FALSE(switch_watch_idle(false, 0x02)) << "a receive on one port is a wait for a reply";
    EXPECT_FALSE(switch_watch_idle(false, 0x16));
}

TEST(XnuSchedWatch, ANullEventStillTimesAndClassifiesTheStretch)
{
    Watched w;
    switch_watch_summary sum = {};
    EXPECT_EQ(switch_watch_message(&w.t, 1000, 50, kBurst, nullptr, &sum), SWITCH_WATCH_FIRST);
    w.leave(1010, 60, true, true);
    w.run(5020, 5000);
    w.leave(5100, 140, true, false, 0x16);
    w.run(5400, 5390);
    EXPECT_EQ(switch_watch_message(&w.t, 5500, 240, kBurst, nullptr, &sum), SWITCH_WATCH_TIMED);
    EXPECT_EQ(sum.length, 500u);
    EXPECT_EQ(sum.cause, SWITCH_WATCH_BLOCKED);

    // Next stretch starts fresh at 5500: mostly waiting for a core.
    w.leave(5520, 260, false, false);
    w.run(5900, 5520);
    EXPECT_EQ(switch_watch_message(&w.t, 5920, 280, kBurst, nullptr, &sum), SWITCH_WATCH_TIMED);
    EXPECT_EQ(sum.length, 420u);
    EXPECT_EQ(sum.cause, SWITCH_WATCH_FOR_CORE);

    // Next stretch mostly on a core.
    EXPECT_EQ(switch_watch_message(&w.t, 6220, 580, kBurst, nullptr, &sum), SWITCH_WATCH_TIMED);
    EXPECT_EQ(sum.length, 300u);
    EXPECT_EQ(sum.cause, SWITCH_WATCH_ON_CORE);
}

TEST(XnuSchedWatch, LaunchSeqlockRejectsConcurrentWriterAndOddRead)
{
    switch_watch_launch l = {};
    switch_watch_launch_snap snap = {};
    EXPECT_FALSE(switch_watch_launch_take(&l, &snap)) << "nothing written yet";

    uint32_t seq = 0;
    ASSERT_TRUE(switch_watch_launch_begin(&l, &seq));
    EXPECT_EQ(l.seq, 1u);
    EXPECT_FALSE(switch_watch_launch_set(&l, 200, "Other", 999)) << "second writer must back off without changing seq";
    EXPECT_EQ(l.seq, 1u);
    EXPECT_FALSE(switch_watch_launch_take(&l, &snap)) << "reader must skip while seq is odd";
    l.pid = 100;
    std::memcpy(l.name, "Preferences", sizeof("Preferences"));
    l.focal_at = 5000;
    l.first_commit = 0;
    switch_watch_launch_end(&l, seq);
    EXPECT_EQ(l.seq, 2u);

    EXPECT_FALSE(switch_watch_launch_take(&l, &snap)) << "no commit yet";
    switch_watch_launch_commit(&l, 999, 6000);
    EXPECT_FALSE(switch_watch_launch_take(&l, &snap)) << "wrong pid ignored";
    switch_watch_launch_commit(&l, 100, 6100);
    switch_watch_launch_commit(&l, 100, 6200);
    ASSERT_TRUE(switch_watch_launch_take(&l, &snap));
    EXPECT_EQ(snap.pid, 100);
    EXPECT_STREQ(snap.name, "Preferences");
    EXPECT_EQ(snap.focal_at, 5000u);
    EXPECT_EQ(snap.first_commit, 6100u) << "first commit kept";
    EXPECT_FALSE(switch_watch_launch_take(&l, &snap)) << "each launch logged once";
}

TEST(XnuSchedWatch, LaunchSeqlockTwoWritersAndReaderNeverSeeTornRecord)
{
    switch_watch_launch l = {};
    std::atomic<bool> done{false};

    auto writer = [&](int32_t pid, const char *name, uint64_t parity) {
        for (uint64_t i = 1; i <= 5000; i++) {
            const uint64_t t = i * 2 + parity;
            if (switch_watch_launch_set(&l, pid, name, t)) {
                switch_watch_launch_commit(&l, pid, t + 5);
            }
        }
    };
    std::thread w1(writer, 111, "Preferences", 1);
    std::thread w2(writer, 222, "MobileSafari", 0);
    std::thread reader([&] {
        switch_watch_launch_snap snap = {};
        while (!done.load(std::memory_order_relaxed)) {
            if (switch_watch_launch_take(&l, &snap)) {
                if (snap.pid == 111) {
                    EXPECT_STREQ(snap.name, "Preferences");
                    EXPECT_EQ(snap.focal_at & 1u, 1u);
                } else {
                    EXPECT_EQ(snap.pid, 222);
                    EXPECT_STREQ(snap.name, "MobileSafari");
                    EXPECT_EQ(snap.focal_at & 1u, 0u);
                }
                EXPECT_GT(snap.first_commit, 0u);
            }
        }
    });
    w1.join();
    w2.join();
    done.store(true, std::memory_order_relaxed);
    reader.join();
    EXPECT_EQ(l.seq & 1u, 0u) << "seq must end even";
    EXPECT_GT(l.seq, 0u);
}

TEST(XnuSchedWatch, TheWindowCountsEventsAndLateOnesByCause)
{
    switch_watch_counts thread = {}, reported = {}, sum = {};
    switch_watch_count(&thread, SWITCH_WATCH_COMMIT, false, SWITCH_WATCH_ON_CORE);
    switch_watch_count(&thread, SWITCH_WATCH_COMMIT, true, SWITCH_WATCH_BLOCKED);
    switch_watch_count(&thread, SWITCH_WATCH_TOUCH, true, SWITCH_WATCH_FOR_CORE);
    thread.dropped = 1;
    switch_watch_add(&sum, &thread, &reported);
    reported = thread;
    switch_watch_count(&thread, SWITCH_WATCH_COMMIT, true, SWITCH_WATCH_ON_CORE);
    switch_watch_add(&sum, &thread, &reported);
    char line[300];
    switch_watch_counts_format(line, sizeof(line), &sum);
    EXPECT_EQ(std::string(line),
              "commits 3, 2 late (on core 1, waiting for a core 0, blocked 1); touch deliveries 1, 1 "
              "late (on core 0, waiting for a core 1, blocked 0); 1 late not logged");
}

TEST(XnuSchedWatch, ALateEventLineNamesItsShareThenTheSplit)
{
    Watched w;
    w.message(1000, 0);
    w.leave(1010, 10, true, true);
    w.run(2000, 2000);
    w.leave(2100, 110, true, false, 0x16);
    w.run(20000, 20000);
    w.message(25000, 5110);
    char line[900];
    switch_watch_event_format(line, sizeof(line), &w.ev, 1000, 1);  // a tick is a microsecond
    EXPECT_THAT(std::string(line), HasSubstr("23.0 ms since its idle wait ended (the one before 24 ms earlier), "
                                             "mostly blocked: on core 5100 us, blocked 17900 us in 1 waits"));
}

TEST(XnuSchedWatch, ALineCutShortStaysInItsBuffer)
{
    Watched w;
    w.message(1000, 0);
    w.message(9000, 100);
    char line[40];
    EXPECT_LT(switch_watch_event_format(line, sizeof(line), &w.ev, 1, 1), sizeof(line));
    EXPECT_LT(std::string(line).size(), sizeof(line));
}

uint64_t Us(uint64_t ticks)
{
    return ticks;
}

// The display thread's wake reading what the render-server thread published.
void Woke(switch_commit_shown *d, const switch_commit_server &s, uint64_t now)
{
    switch_commit_woke(d, &s.applied[s.published % 2], s.published, s.published, now, Us);
}

bool Departed(switch_commit_server *s, uint64_t sent, bool receiving, uint64_t now, bool port_set = true)
{
    if (!switch_commit_departed(s, sent, receiving, port_set, now))
        return false;
    s->published++;
    return true;
}

TEST(XnuSchedCommit, ACommitIsReceivedAppliedThenShownAtTheDisplaysWake)
{
    switch_commit_server s = {};
    switch_commit_shown d = {};
    switch_commit_dispatched(&s, 1000, 1010, 1040);  // woken by it at 1010, on a core at 1040
    EXPECT_FALSE(Departed(&s, 1000, false, 1100)) << "a wait that is not its receive";
    EXPECT_TRUE(Departed(&s, 1000, true, 1300));
    EXPECT_FALSE(Departed(&s, 1000, true, 1400)) << "applied once";
    Woke(&d, s, 9000);
    EXPECT_EQ(d.receive.runs, 1u);
    EXPECT_EQ(d.receive.max_us, 10u);
    EXPECT_EQ(d.apply.max_us, 290u);
    EXPECT_EQ(d.wait.max_us, 7700u);
    EXPECT_EQ(d.total.max_us, 8000u);
    EXPECT_EQ(d.on_core, 0u);
    Woke(&d, s, 9500);
    EXPECT_EQ(d.total.runs, 1u) << "a commit counts at the first wake after it was applied";
}

TEST(XnuSchedCommit, ACommitTakenWithoutWaitingIsReceivedAtItsSend)
{
    switch_commit_server s = {};
    switch_commit_shown d = {};
    switch_commit_dispatched(&s, 1000, 900, 1040);  // runnable before the send: it was not waiting for it
    EXPECT_TRUE(Departed(&s, 1000, true, 1300));
    // The next commit arrives while it is on a core and it takes it before it waits again.
    EXPECT_TRUE(Departed(&s, 2000, true, 2600));
    Woke(&d, s, 3000);
    EXPECT_EQ(d.on_core, 1u);
    EXPECT_EQ(d.receive.max_us, 0u);
    EXPECT_EQ(d.apply.max_us, 600u);
}

TEST(XnuSchedCommit, AReadThePublicationMovedUnderIsSkipped)
{
    switch_commit_server s = {};
    switch_commit_shown d = {};
    switch_commit_dispatched(&s, 1000, 1010, 1040);
    Departed(&s, 1000, true, 1300);
    switch_commit_woke(&d, &s.applied[s.published % 2], s.published, s.published + 1, 2000, Us);
    EXPECT_EQ(d.skipped, 1u);
    EXPECT_EQ(d.total.runs, 0u);
}

TEST(XnuSchedCommit, TheLineGivesEachStageForTheWindow)
{
    switch_commit_server s = {};
    switch_commit_shown d = {}, was = {};
    switch_commit_dispatched(&s, 1000, 1010, 1040);
    Departed(&s, 1000, true, 1300);
    Woke(&d, s, 9000);
    char line[400];
    switch_commit_format(line, sizeof(line), &d, &was);
    EXPECT_EQ(std::string(line),
              "1 commits to the frame's wake, p50/p99 us: received 10/10, applied 290/290, waited "
              "7700/7700, all 8000/8000 (0 taken on core, 0 skipped; applied at 1 port-set receives, "
              "0 single-port)");
}

// The kernel's hint is the same for any receive: each applied stamp is counted by the receive's kind.
TEST(XnuSchedCommit, AnApplyAtASinglePortReceiveIsCountedApart)
{
    switch_commit_server s = {};
    switch_commit_shown d = {}, was = {};
    switch_commit_dispatched(&s, 1000, 1010, 1040);
    Departed(&s, 1000, true, 1300, false);
    Woke(&d, s, 9000);
    EXPECT_EQ(d.port_set, 0u);
    char line[400];
    switch_commit_format(line, sizeof(line), &d, &was);
    EXPECT_THAT(std::string(line), HasSubstr("applied at 0 port-set receives, 1 single-port)"));
}

TEST(XnuSchedWatchSource, NothingUnderTheThreadLockLocksSleepsAllocatesOrLogs)
{
    const std::string source = switch_ios::test::SourceText("../switch/sched/watch.c");
    const std::string commit = switch_ios::test::SourceText("../switch/sched/commit.c");
    const std::string slots = switch_ios::test::SourceText("../switch/sched/slots.h");
    const std::string hot = switch_ios::test::Body(source, "\nstatic struct watch_slot *slot_of(") +
                            switch_ios::test::Body(source, "\nstatic struct watch_slot *slot_take(") +
                            switch_ios::test::Body(slots, "\nstatic inline int switch_slot_find(") +
                            switch_ios::test::Body(slots, "\nstatic inline int switch_slot_claim(") +
                            switch_ios::test::Body(slots, "\nstatic inline void switch_slot_publish(") +
                            switch_ios::test::Body(source, "\nvoid switch_sched_watch_departed(") +
                            switch_ios::test::Body(source, "\nvoid switch_sched_watch_dispatched(") +
                            switch_ios::test::Body(commit, "\nbool switch_sched_commit_server(") +
                            switch_ios::test::Body(commit, "\nvoid switch_sched_watch_display_woke(");
    ASSERT_FALSE(hot.empty());
    for (const char *call : {"lock(", "lck_", "alloc", "kfree", "os_log", "printf", "thread_call", "IOLock", "IOSleep",
                             "assert_wait", "thread_block", "copyin", "msleep", "zalloc"})
        EXPECT_THAT(hot, Not(HasSubstr(call))) << call;
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/sched.h");
    EXPECT_THAT(
        switch_ios::test::Body(header, "static inline void switch_sched_on_core(thread_t thread, uint64_t now)"),
        HasSubstr("switch_sched_watch_dispatched(thread, now);"));
    EXPECT_THAT(
        switch_ios::test::Body(header, "static inline void switch_sched_off_core(thread_t thread, uint64_t now)"),
        HasSubstr("switch_sched_watch_departed(thread, now);"));
}

}  // namespace
