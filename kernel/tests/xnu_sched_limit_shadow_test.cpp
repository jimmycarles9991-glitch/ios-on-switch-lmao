// kernel/switch/sched/limit_shadow.h on the host: Apple's blocking CPU limit replayed beside a lifted one, the blocks
// it would have made and the time run past each budget; the table of thread slots and process rows, and both trees
// ending a slot where XNU ends the limit.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "bazel/testing/source_text.h"
#include "sched/limit_shadow.h"

namespace {

constexpr uint64_t kMs = 1000000;

switch_limit_shadow Shadow(uint64_t interval_ms, uint8_t percentage)
{
    switch_limit_shadow s;
    switch_limit_shadow_init(&s, interval_ms * kMs, percentage);
    return s;
}

TEST(XnuSchedLimitShadow, RunningUnderTheBudgetNeverBlocks)
{
    switch_limit_shadow s = Shadow(30, 90);
    for (uint64_t t = 1; t < 30 * 100; t += 30)
        switch_limit_shadow_ran(&s, t * kMs, (t + 20) * kMs);
    EXPECT_EQ(s.blocks, 0u);
    EXPECT_EQ(s.over, 0u);
}

TEST(XnuSchedLimitShadow, AnIntervalPastItsBudgetIsOneBlockAndItsOverrun)
{
    switch_limit_shadow s = Shadow(30, 90);
    switch_limit_shadow_ran(&s, 1 * kMs, 30 * kMs);  // 29 ms of a 30 ms interval: 2 ms past 27
    EXPECT_EQ(s.blocks, 1u);
    EXPECT_EQ(s.over, 2 * kMs);
    switch_limit_shadow_ran(&s, 30 * kMs, 31 * kMs);  // still the first interval (1..31): one block, 1 ms more
    EXPECT_EQ(s.blocks, 1u);
    EXPECT_EQ(s.over, 3 * kMs);
}

TEST(XnuSchedLimitShadow, ARunAcrossIntervalsIsSplitAtEachRefill)
{
    switch_limit_shadow s = Shadow(30, 90);
    switch_limit_shadow_ran(&s, 10 * kMs, 100 * kMs);  // intervals 10-40, 40-70, 70-100, all spinning
    EXPECT_EQ(s.blocks, 3u);
    EXPECT_EQ(s.over, 9 * kMs);
    switch_limit_shadow_ran(&s, 200 * kMs, 205 * kMs);  // after idle intervals: a fresh one, under budget
    EXPECT_EQ(s.blocks, 3u);
    EXPECT_EQ(s.start, 190 * kMs) << "intervals follow on whole from the first run";
}

switch_limit_key Key(const std::string &name, uint8_t percentage, uint64_t interval_ms)
{
    switch_limit_key key{};
    name.copy(key.name, sizeof(key.name) - 1);
    key.percentage = percentage;
    key.interval = interval_ms * kMs;
    key.interval_ms = static_cast<uint32_t>(interval_ms);
    return key;
}

struct Table {
    std::unique_ptr<switch_limit_table> t = std::make_unique<switch_limit_table>();

    switch_limit_counts Window(uint32_t row, unsigned *threads = nullptr)
    {
        switch_limit_counts window{};
        unsigned held = 0;
        EXPECT_TRUE(switch_limit_window(t.get(), row, &window, &held));
        if (threads)
            *threads = held;
        return window;
    }
};

// A 90 %/30 ms limit asked at 1 ms while running, the thread leaving at `until`.
void RunOnce(Table &table, uint64_t tid, uint64_t until_ms, const std::string &name = "SpringBoard")
{
    const switch_limit_key key = Key(name, 90, 30);
    switch_limit_requested(table.t.get(), 0, tid, &key, 1 * kMs);
    switch_limit_departed(table.t.get(), 0, tid, until_ms * kMs);
}

TEST(XnuSchedLimitShadowTable, ALiftedLimitCountsItsBlocksOnItsProcessRow)
{
    Table table;
    RunOnce(table, 7, 30);  // 29 ms of a 30 ms interval: one block, 2 ms over
    unsigned threads = 0;
    const switch_limit_counts window = table.Window(0, &threads);
    EXPECT_EQ(window.blocks, 1u);
    EXPECT_EQ(window.over, 2 * kMs);
    EXPECT_EQ(threads, 1u);
    EXPECT_EQ(table.Window(0).blocks, 0u) << "a window counts once";
}

TEST(XnuSchedLimitShadowTable, AskingTheSameLimitAgainKeepsTheTimeRunSinceDispatch)
{
    Table table;
    const switch_limit_key key = Key("SpringBoard", 90, 30);
    switch_limit_requested(table.t.get(), 0, 7, &key, 1 * kMs);
    switch_limit_requested(table.t.get(), 0, 7, &key, 20 * kMs);  // the workqueue asks at each request change
    switch_limit_departed(table.t.get(), 0, 7, 30 * kMs);
    const switch_limit_counts window = table.Window(0);
    EXPECT_EQ(window.blocks, 1u) << "1 to 30 ms is one run, not 20 to 30";
    EXPECT_EQ(window.over, 2 * kMs);
}

TEST(XnuSchedLimitShadowTable, ANewLimitReplaysTheRunSoFarUnderTheOldOne)
{
    Table table;
    const switch_limit_key old_key = Key("SpringBoard", 90, 30);
    const switch_limit_key new_key = Key("SpringBoard", 90, 50);
    switch_limit_requested(table.t.get(), 0, 7, &old_key, 1 * kMs);
    switch_limit_requested(table.t.get(), 0, 7, &new_key, 30 * kMs);
    switch_limit_departed(table.t.get(), 0, 7, 35 * kMs);
    EXPECT_EQ(table.Window(0).blocks, 1u) << "1-30 ms under 90 %/30";
    EXPECT_EQ(table.Window(1).blocks, 0u) << "30-35 ms under 90 %/50: 5 of 45 ms";
}

TEST(XnuSchedLimitShadowTable, EndingTheLimitCountsTheRunAndStopsCounting)
{
    Table table;
    const switch_limit_key key = Key("SpringBoard", 90, 30);
    switch_limit_requested(table.t.get(), 0, 7, &key, 1 * kMs);
    switch_limit_ended(table.t.get(), 0, 7, 30 * kMs);  // DISABLE, a limit Apple's ledger applies, or exit
    switch_limit_dispatched(table.t.get(), 7, 40 * kMs);
    switch_limit_departed(table.t.get(), 0, 7, 100 * kMs);
    unsigned threads = 9;
    const switch_limit_counts window = table.Window(0, &threads);
    EXPECT_EQ(window.blocks, 1u) << "the run up to the end is counted, 40-100 ms is not";
    EXPECT_EQ(window.over, 2 * kMs);
    EXPECT_EQ(threads, 0u);
    EXPECT_EQ(table.t->tid[0], 0u) << "the slot is free";
}

TEST(XnuSchedLimitShadowTable, EndedSlotsAreReusedSoLaterThreadsAreWatched)
{
    Table table;
    for (uint64_t tid = 1; tid <= 10 * SWITCH_LIMIT_SLOTS; tid++) {
        RunOnce(table, tid, 30);
        switch_limit_ended(table.t.get(), 0, tid, 31 * kMs);
    }
    EXPECT_EQ(table.t->slots_dropped, 0u);
    EXPECT_EQ(table.Window(0).blocks, 10 * SWITCH_LIMIT_SLOTS) << "one row for the process and limit";
}

TEST(XnuSchedLimitShadowTable, FullTablesAreCountedNotOverwritten)
{
    Table table;
    for (uint64_t tid = 1; tid <= SWITCH_LIMIT_SLOTS + 2; tid++)
        RunOnce(table, tid, 2);
    EXPECT_EQ(table.t->slots_dropped, 2u);
    Table rows;
    for (unsigned name = 1; name <= SWITCH_LIMIT_ROWS + 3; name++)
        RunOnce(rows, name, 2, "daemon" + std::to_string(name));
    EXPECT_EQ(rows.t->rows_dropped, 3u);
}

TEST(XnuSchedLimitShadowTable, ARelaunchedProcessCountsOnItsRow)
{
    Table table;
    for (uint64_t launch = 1; launch <= 5 * SWITCH_LIMIT_ROWS; launch++) {
        RunOnce(table, launch, 30);  // a new pid and thread each launch, the same name and limit
        switch_limit_ended(table.t.get(), 0, launch, 31 * kMs);
    }
    EXPECT_EQ(table.t->rows_dropped, 0u);
    EXPECT_EQ(table.t->slots_dropped, 0u);
    EXPECT_EQ(table.Window(0).blocks, 5 * SWITCH_LIMIT_ROWS);
    const switch_limit_key other = Key("SpringBoard", 90, 50);
    switch_limit_requested(table.t.get(), 0, 1000, &other, 1 * kMs);
    unsigned threads = 0;
    table.Window(1, &threads);
    EXPECT_EQ(threads, 1u) << "the same name with another limit is its own row";
}

TEST(XnuSchedLimitShadowTable, EachCpuWritesItsOwnCountsAndTheWindowSumsThem)
{
    Table table;
    const switch_limit_key key = Key("SpringBoard", 90, 30);
    switch_limit_requested(table.t.get(), 0, 7, &key, 1 * kMs);
    switch_limit_requested(table.t.get(), 3, 8, &key, 1 * kMs);
    switch_limit_departed(table.t.get(), 0, 7, 30 * kMs);
    switch_limit_departed(table.t.get(), 3, 8, 30 * kMs);
    EXPECT_EQ(table.t->rows[0].cpu[0].blocks, 1u);
    EXPECT_EQ(table.t->rows[0].cpu[3].blocks, 1u);
    unsigned threads = 0;
    EXPECT_EQ(table.Window(0, &threads).blocks, 2u);
    EXPECT_EQ(threads, 2u);
}

class XnuSchedLimitShadowSource : public testing::TestWithParam<const char *> {};

TEST_P(XnuSchedLimitShadowSource, AnyLimitButALiftedOneEndsTheSlot)
{
    const std::string thread = switch_ios::test::SourceText(std::string("../") + GetParam() + "/osfmk/kern/thread.c");
    const std::string cpulimit = switch_ios::test::Body(thread, "\nthread_set_cpulimit(int action");
    EXPECT_THAT(cpulimit, testing::HasSubstr("switch_sched_cpulimit_lift(&percentage, interval_ns)) {\n\t\taction = "
                                             "THREAD_CPULIMIT_DISABLE;\n\t} else {\n\t\tswitch_sched_limit_ended();\n"))
        << "DISABLE (workqueue request changes, ledger.c, the sysctl), a limit Apple applies, EXCEPTION";
    const size_t forced = cpulimit.find("TH_OPT_FORCED_LEDGER");
    const size_t ended = cpulimit.find("switch_sched_limit_ended()");
    ASSERT_NE(forced, std::string::npos);
    EXPECT_LT(forced, ended) << "a refused change leaves the limit, and its slot, as they were";
    const std::string terminate = switch_ios::test::Body(thread, "\nthread_terminate_self(void)");
    EXPECT_THAT(terminate, testing::HasSubstr("pal_thread_terminate_self(thread);\n\tswitch_sched_limit_ended();\n"))
        << "at exit, while the thread still runs";
}

INSTANTIATE_TEST_SUITE_P(BothTrees, XnuSchedLimitShadowSource, testing::Values("xnu-10063.141.1", "xnu-12377.121.6"));

TEST(XnuSchedLimitShadowSource, ReleaseKernelsCompileTheEndToNothing)
{
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/sched.h");
    EXPECT_THAT(header.substr(header.find("#else /* DEVELOPMENT || DEBUG */")),
                testing::HasSubstr("static inline void switch_sched_limit_ended(void)"));
}

}  // namespace
