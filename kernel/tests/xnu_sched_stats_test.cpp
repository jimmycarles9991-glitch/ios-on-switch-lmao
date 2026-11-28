// kernel/switch/sched/stats.h, the scheduling stats line's histogram and text, and where XNU feeds it: the
// incoming thread's dispatch in both of thread_invoke's paths, under that thread's lock, and nothing in a release kernel.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bazel/testing/source_text.h"
#include "xnu_sched_stats_harness.h"

using testing::HasSubstr;
using testing::Not;

namespace {
std::string Format(const char *label, int pid, uint8_t base, uint8_t sched, uint8_t mode, uint8_t qos, uint8_t bucket,
                   size_t size = 256)
{
    std::string out(size, '\0');
    const size_t n = xnu_sched_format(out.data(), out.size(), label, pid, base, sched, mode, qos, bucket);
    EXPECT_LT(n, size);
    out.resize(n);
    return out;
}
}  // namespace

TEST(XnuSchedStats, BucketsAreLog2Microseconds)
{
    EXPECT_EQ(xnu_sched_bucket(0), 0u);
    EXPECT_EQ(xnu_sched_bucket(1), 1u);
    EXPECT_EQ(xnu_sched_bucket(2), 2u);
    EXPECT_EQ(xnu_sched_bucket(3), 2u);
    EXPECT_EQ(xnu_sched_bucket(4), 3u);
    EXPECT_EQ(xnu_sched_bucket(1023), 10u);
    EXPECT_EQ(xnu_sched_bucket(1024), 11u);
    // A frame at 60 Hz is 16667 us: its own bucket, [16384, 32768).
    EXPECT_EQ(xnu_sched_bucket(16667), 15u);
    EXPECT_EQ(xnu_sched_bucket(UINT64_MAX), xnu_sched_buckets() - 1);
    EXPECT_EQ(xnu_sched_bucket(uint64_t{1} << (xnu_sched_buckets() - 1)), xnu_sched_buckets() - 1);
}

TEST(XnuSchedStats, PercentilesAreBucketEdgesNeverAboveTheMax)
{
    xnu_sched_reset();
    EXPECT_EQ(xnu_sched_percentile(50), 0u);
    for (int i = 0; i < 98; i++)
        xnu_sched_note(10);   // [8, 16)
    xnu_sched_note(3000);     // [2048, 4096)
    xnu_sched_note(8700000);  // the stall SwitchStall saw, in the last bucket
    EXPECT_EQ(xnu_sched_runs(), 100u);
    EXPECT_EQ(xnu_sched_percentile(50), 16u);
    EXPECT_EQ(xnu_sched_percentile(98), 16u);
    EXPECT_EQ(xnu_sched_percentile(99), 4096u);
    EXPECT_EQ(xnu_sched_percentile(100), 8700000u);
    EXPECT_EQ(xnu_sched_max(), 8700000u);

    xnu_sched_reset();
    xnu_sched_note(5);  // one run: every percentile is it, not its bucket's edge
    EXPECT_EQ(xnu_sched_percentile(50), 5u);
    EXPECT_EQ(xnu_sched_percentile(99), 5u);
}

TEST(XnuSchedStats, EntryNamesTheClassAndTheWaits)
{
    xnu_sched_reset();
    for (int i = 0; i < 99; i++)
        xnu_sched_note(20);
    xnu_sched_note(900);
    // CA's display thread: fixed 54 (SCHED_FIFO), no QoS, the above-UI bucket.
    EXPECT_EQ(Format("display.primary", 53, 54, 54, 2, 0, 0),
              "display.primary[53] 54/54 FX - FIX: 100 runs 32/32/900 us");
    // SpringBoard's main thread: timeshare UI, decayed to 31, clutch's FG bucket.
    EXPECT_EQ(Format("SpringBoard.main", 237, 47, 31, 3, 6, 1),
              "SpringBoard.main[237] 47/31 TS UI FG: 100 runs 32/32/900 us");
    EXPECT_EQ(Format("x", 1, 97, 97, 1, 0, 0).substr(0, 15), "x[1] 97/97 RT -");

    xnu_sched_set_counts(3, 2, 5400);
    EXPECT_THAT(Format("display.primary", 53, 31, 31, 3, 0, 3), HasSubstr(", 3 fail-safe, CPU limit 2/5400 us"));
}

// 25 Nov: backboardd's main thread had no run in the 19:16:42-52 and 19:16:52-17:02 windows, then waited 32.6 s for its
// first run of 19:17:02-12. The wait was real (SwitchBusy saw 63.96 s of the stretch waiting for a core), but its entry
// read as a wait inside the window.
TEST(XnuSchedStats, ALongestWaitThatBeganBeforeTheWindowSaysSo)
{
    xnu_sched_reset();
    xnu_sched_note(32555950);
    xnu_sched_take_longest(32555950, 1036000, 1058555);
    EXPECT_EQ(xnu_sched_before_ms(), 22555u);
    EXPECT_THAT(Format("backboardd.main", 69, 53, 53, 3, 4, 1),
                HasSubstr("1 runs 32555950/32555950/32555950 us, longest began 22555 ms before the window"));
}

// The 25 Nov sequence through stats.c's own helpers, in uptime: ten runs in the first window, two windows with no run,
// then the dispatch that ends a 32.556 s wait. Its entry names the window start two empty takes moved forward.
TEST(XnuSchedStats, AWaitAcrossTwoEmptyWindowsIsPlacedAgainstTheWindowItEndsIn)
{
    constexpr uint64_t kSecond = 1000000;
    const auto take = [](uint64_t at) {
        std::string out(256, '\0');
        out.resize(xnu_sched_timeline_take(at, out.data(), out.size()));
        return out;
    };
    xnu_sched_timeline_reset();
    (void)take(1000 * kSecond);
    for (uint64_t i = 0; i < 10; i++) {
        xnu_sched_timeline_dispatch((1000 * kSecond) + (100000 * i), (1000 * kSecond) + (100000 * i) + 64);
    }
    EXPECT_EQ(take(1010 * kSecond), "backboardd.main[69] 53/53 TS IN FG: 10 runs 64/64/64 us");
    EXPECT_EQ(take(1020 * kSecond), "backboardd.main[69] 53/53 TS IN FG: 0 runs 0/0/0 us");
    EXPECT_EQ(take(1030 * kSecond), "backboardd.main[69] 53/53 TS IN FG: 0 runs 0/0/0 us");
    xnu_sched_timeline_dispatch(1007 * kSecond, (1007 * kSecond) + 32556000);
    EXPECT_EQ(take(1040 * kSecond), "backboardd.main[69] 53/53 TS IN FG: 1 runs 32556000/32556000/32556000 us, "
                                    "longest began 23000 ms before the window");
}

TEST(XnuSchedStats, ALongestWaitInsideTheWindowSaysNothingMore)
{
    xnu_sched_reset();
    xnu_sched_note(900);
    xnu_sched_take_longest(900, 1060000, 1058555);
    EXPECT_EQ(xnu_sched_before_ms(), 0u);
    xnu_sched_take_longest(0, 0, 1058555);
    EXPECT_EQ(xnu_sched_max(), 0u);
    EXPECT_EQ(xnu_sched_before_ms(), 0u);
    xnu_sched_take_longest(900, 1000, 0);
    EXPECT_EQ(xnu_sched_before_ms(), 0u);
    EXPECT_THAT(Format("display.primary", 53, 54, 54, 2, 0, 0), Not(HasSubstr("began")));
}

// The word's halves saturate rather than wrap, and its order is the wait's: a longer wait wins whatever its start.
TEST(XnuSchedStats, TheLongestWordSaturatesAndOrdersByWait)
{
    EXPECT_GT(xnu_sched_longest(2, 0), xnu_sched_longest(1, UINT32_MAX));
    EXPECT_EQ(xnu_sched_longest(UINT64_MAX, UINT64_MAX), UINT64_MAX);
    xnu_sched_reset();
    xnu_sched_take_longest(1ULL << 40, 5, 10);
    EXPECT_EQ(xnu_sched_max(), UINT32_MAX);
    EXPECT_EQ(xnu_sched_before_ms(), 5u);
    xnu_sched_take_longest(900, UINT64_MAX, 1ULL << 33);
    EXPECT_EQ(xnu_sched_before_ms(), 0u) << "a start saturated after 49.7 days of uptime places nothing";
}

TEST(XnuSchedStats, EntryStaysInsideItsBuffer)
{
    xnu_sched_reset();
    xnu_sched_note(1);
    xnu_sched_set_counts(1, 1, 1);
    for (size_t size : {1, 8, 40, 64}) {
        std::string out(size, 'Z');
        const size_t n = xnu_sched_format(out.data(), size, "a-long-thread-label", 12345, 47, 47, 3, 6, 1);
        EXPECT_LT(n, size);
        EXPECT_EQ(out[n], '\0');
    }
    char none = 'Z';
    EXPECT_EQ(xnu_sched_format(&none, 0, "x", 1, 0, 0, 0, 0, 0), 0u);
    EXPECT_EQ(none, 'Z');
}

TEST(XnuSchedStatsSource, DispatchIsCountedUnderTheIncomingThreadsLock)
{
    const std::string prim = switch_ios::test::SourceText("../xnu-10063.141.1/osfmk/kern/sched_prim.c");
    const std::string invoke = switch_ios::test::Body(prim, "\nthread_invoke(\n");
    const std::string hook =
        "\tast_context(thread);\n\t\t\tswitch_sched_on_core(thread, ctime);\n\n\t\t\tthread_unlock(thread);";
    const std::string full =
        "\tast_context(thread);\n\tswitch_sched_on_core(thread, ctime);\n\n\tthread_unlock(thread);";
    EXPECT_THAT(invoke, HasSubstr(hook));  // stack handoff
    EXPECT_THAT(invoke, HasSubstr(full));  // full context switch
}

TEST(XnuSchedStatsSource, ReleaseKernelsCompileEveryHookToNothing)
{
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/sched.h");
    const size_t release = header.find("#else /* DEVELOPMENT || DEBUG */");
    ASSERT_NE(release, std::string::npos);
    const std::string stubs = header.substr(release);
    for (const char *hook : {"switch_sched_on_core(", "switch_sched_named(", "switch_sched_role(",
                             "switch_sched_limit_block(", "switch_sched_tick(", "switch_sched_boot_complete("})
        EXPECT_THAT(stubs, HasSubstr(std::string("static inline void ") + hook)) << hook;
    const std::string source = switch_ios::test::SourceText("../switch/sched/stats.c");
    EXPECT_THAT(source, HasSubstr("\n#if DEVELOPMENT || DEBUG\n"));
}
