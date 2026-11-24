// kernel/switch/sched/cpu.h on the host: each process's CPU in a window, across exits, execs and full tables, and the
// two lines that report it.
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "sched/cpu.h"

namespace {

constexpr uint32_t kNumer = 1, kDenom = 1;  // ticks are nanoseconds
constexpr uint64_t kMs = 1000000;

struct Tasks {
    std::unique_ptr<switch_cpu_tasks> t = std::make_unique<switch_cpu_tasks>();

    std::pair<uint64_t, uint64_t> take(uint64_t uid, uint64_t user, uint64_t sys)
    {
        uint64_t du = 0, ds = 0;
        switch_cpu_take(t.get(), uid, user, sys, &du, &ds);
        return {du, ds};
    }
};

TEST(XnuSchedCpu, AProcessGivesWhatItUsedSinceTheLastWindow)
{
    Tasks t;
    EXPECT_EQ(t.take(7, 100, 40), (std::pair<uint64_t, uint64_t>{100, 40})) << "first seen: its whole life";
    switch_cpu_end_window(t.t.get());
    EXPECT_EQ(t.take(7, 130, 50), (std::pair<uint64_t, uint64_t>{30, 10}));
    switch_cpu_end_window(t.t.get());
    EXPECT_EQ(t.take(7, 130, 50), (std::pair<uint64_t, uint64_t>{0, 0}));
}

TEST(XnuSchedCpu, AnExitIsCountedOnceWithThisWindowsShareOnly)
{
    Tasks t;
    t.take(7, 100, 40);
    switch_cpu_end_window(t.t.get());
    switch_cpu_exited(t.t.get(), 7, 70, "prngseedd", 180, 45);
    // The report still finds the exiting task in the list: nothing left to give.
    EXPECT_EQ(t.take(7, 180, 45), (std::pair<uint64_t, uint64_t>{0, 0}));
    const switch_cpu_exits e = switch_cpu_end_window(t.t.get());
    EXPECT_EQ(e.exits, 1u);
    EXPECT_EQ(e.user, 80u);
    EXPECT_EQ(e.sys, 5u);
    EXPECT_STREQ(e.most.name, "prngseedd");
    EXPECT_EQ(e.most.pid, 70);
    const switch_cpu_exits next = switch_cpu_end_window(t.t.get());
    EXPECT_EQ(next.exits, 0u) << "a window's exits start over";
}

TEST(XnuSchedCpu, AProcessBornAndExitedInAWindowIsCountedOnce)
{
    Tasks t;
    switch_cpu_exited(t.t.get(), 9, 90, "seputil", 80, 20);
    EXPECT_EQ(t.take(9, 80, 20), (std::pair<uint64_t, uint64_t>{0, 0}));
    const switch_cpu_exits e = switch_cpu_end_window(t.t.get());
    EXPECT_EQ(e.user + e.sys, 100u);
}

TEST(XnuSchedCpu, AnExecKeepsTheRowAndItsCounts)
{
    Tasks t;
    t.take(11, 500, 100);
    switch_cpu_end_window(t.t.get());
    // The new task carries the old one's counts (task_rollup_accounting_info) under the same unique id, and both
    // may be in the list at the report.
    EXPECT_EQ(t.take(11, 520, 110), (std::pair<uint64_t, uint64_t>{20, 10}));
    EXPECT_EQ(t.take(11, 525, 112), (std::pair<uint64_t, uint64_t>{5, 2}));
}

TEST(XnuSchedCpu, AFullTableCountsWhatItCouldNotTake)
{
    Tasks t;
    uint32_t untracked = 0;
    for (uint64_t uid = 1; uid <= SWITCH_CPU_ROWS + 64; uid++) {
        uint64_t du = 0, ds = 0;
        untracked += switch_cpu_take(t.t.get(), uid, 1, 1, &du, &ds) ? 0 : 1;
    }
    EXPECT_GE(untracked, 64u);
    EXPECT_EQ(switch_cpu_end_window(t.t.get()).untracked, untracked);
}

TEST(XnuSchedCpu, TheTopIsTheLargestByUserPlusSystem)
{
    switch_cpu_task top[SWITCH_CPU_TOP] = {};
    const uint64_t used[] = {5, 50, 0, 20, 70, 1, 2, 3, 4, 6, 7};
    for (uint32_t i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
        const int at = switch_cpu_rank(top, used[i]);
        if (at >= 0) {
            switch_cpu_task task = {static_cast<int32_t>(i), "p", used[i] / 2, used[i] - used[i] / 2};
            switch_cpu_place(top, at, &task);
        }
    }
    EXPECT_EQ(top[0].pid, 4);
    EXPECT_EQ(top[1].pid, 1);
    EXPECT_EQ(top[2].pid, 3);
    EXPECT_EQ(top[7].user + top[7].sys, 3u) << "1 and 2 fell off, 0 never entered";
}

TEST(XnuSchedCpu, TheCoresLineClosesEachCoresBooks)
{
    const switch_cpu_core cores[2] = {
        {5600 * kMs, 300 * kMs, 150 * kMs, 4100 * kMs, 12000},
        {11000 * kMs, 0, 0, 0, 0},
    };
    char line[400];
    switch_cpu_cores_format(line, sizeof(line), cores, 2, 10000 * kMs, kNumer, kDenom);
    EXPECT_EQ(std::string(line),
              "cpu0 threads 5600 (irq 150), idle 4400 = asleep 4100 + irq 150 + rest 150, 12000 sleeps; "
              "cpu1 threads 11000 (irq 0), idle 0 = asleep 0 + irq 0 + rest 0, 0 sleeps");
}

TEST(XnuSchedCpu, TheTasksLineNamesTheTopTheSumAndTheExits)
{
    switch_cpu_task top[SWITCH_CPU_TOP] = {};
    top[0] = {393, "SpringBoard", 1200 * kMs, 300 * kMs};
    top[1] = {69, "backboardd", 900 * kMs, 200 * kMs};
    const switch_cpu_exits e = {2, 1, 410 * kMs, 30 * kMs, {1234, "prngseedd", 80 * kMs, 2 * kMs}};
    char line[400];
    switch_cpu_tasks_format(line, sizeof(line), top, 312, 3900 * kMs, 1800 * kMs, &e, kNumer, kDenom);
    EXPECT_EQ(std::string(line),
              "SpringBoard[393] 1200+300, backboardd[69] 900+200; all 312 processes 3900+1800, exited 2 (410+30, "
              "most prngseedd[1234] 80+2), untracked 1");
}

TEST(XnuSchedCpu, ALineCutShortStaysInItsBuffer)
{
    switch_cpu_core cores[SWITCH_CPU_CORES] = {};
    char line[64];
    const size_t n = switch_cpu_cores_format(line, sizeof(line), cores, SWITCH_CPU_CORES, 1, kNumer, kDenom);
    EXPECT_EQ(n, sizeof(line) - 1);
    EXPECT_EQ(std::string(line).size(), sizeof(line) - 1);
}

}  // namespace
