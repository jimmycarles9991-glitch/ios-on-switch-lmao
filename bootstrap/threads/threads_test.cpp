// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bootstrap/threads/threads.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace {

switch_thread_sample MakeSample(int32_t pid, uint64_t tid, uint64_t user_ns, uint64_t sys_ns, int32_t permille,
                                int32_t state, int32_t pri, const char *proc, const char *name)
{
    switch_thread_sample sample = {};
    sample.pid = pid;
    sample.tid = tid;
    sample.user_ns = user_ns;
    sample.sys_ns = sys_ns;
    sample.cpu_permille = permille;
    sample.run_state = state;
    sample.pri = pri;
    snprintf(sample.proc, sizeof(sample.proc), "%s", proc);
    snprintf(sample.name, sizeof(sample.name), "%s", name);
    return sample;
}

TEST(SwitchThreadsTest, ComputesIntervalDeltasAndSortsByCpu)
{
    switch_thread_sample before[] = {
        MakeSample(20, 200, 100'000'000ULL, 50'000'000ULL, 10, 3, 31, "backboardd", "render"),
        MakeSample(10, 100, 500'000'000ULL, 0, 50, 3, 47, "SpringBoard", "main"),
    };
    switch_thread_sample after[] = {
        MakeSample(10, 100, 600'000'000ULL, 0, 100, 1, 47, "SpringBoard", "main"),
        MakeSample(20, 200, 350'000'000ULL, 150'000'000ULL, 200, 1, 31, "backboardd", "render"),
        MakeSample(30, 300, 50'000'000ULL, 0, 50, 4, 20, "newproc", "worker\nline"),
    };
    switch_thread_row rows[3] = {};
    ASSERT_EQ(switch_threads_compute(before, 2, after, 3, 1'000'000'000ULL, rows), 3u);

    EXPECT_EQ(rows[0].pid, 20);
    EXPECT_EQ(rows[0].tid, 200u);
    EXPECT_NEAR(rows[0].cpu_pct, 35.0, 0.01);
    EXPECT_EQ(rows[0].state, 'R');

    EXPECT_EQ(rows[1].pid, 10);
    EXPECT_NEAR(rows[1].cpu_pct, 10.0, 0.01);

    EXPECT_EQ(rows[2].pid, 30);
    EXPECT_NEAR(rows[2].cpu_pct, 5.0, 0.01);
    EXPECT_EQ(rows[2].state, 'U');
    EXPECT_STREQ(rows[2].name, "worker line");
}

TEST(SwitchThreadsTest, ZeroIntervalUsesKernelPermilleAndFormatsTable)
{
    switch_thread_sample after[] = {
        MakeSample(1, 10, 2'000'000ULL, 1'000'000ULL, 125, 3, 31, "launchd", ""),
        MakeSample(2, 20, 5'000'000ULL, 5'000'000ULL, 420, 1, 63, "kernel", "sched"),
    };
    switch_thread_row rows[2] = {};
    ASSERT_EQ(switch_threads_compute(nullptr, 0, after, 2, 0, rows), 2u);
    EXPECT_EQ(rows[0].pid, 2);
    EXPECT_NEAR(rows[0].cpu_pct, 42.0, 0.01);
    EXPECT_EQ(rows[1].pid, 1);
    EXPECT_NEAR(rows[1].cpu_pct, 12.5, 0.01);

    FILE *tmp = tmpfile();
    ASSERT_NE(tmp, nullptr);
    switch_threads_format(tmp, rows, 2, 1);
    rewind(tmp);
    char buffer[512] = {};
    const size_t n = fread(buffer, 1, sizeof(buffer) - 1, tmp);
    fclose(tmp);
    const std::string output(buffer, n);
    EXPECT_NE(output.find("PID"), std::string::npos);
    EXPECT_NE(output.find("kernel"), std::string::npos);
    EXPECT_NE(output.find("42.0"), std::string::npos);
    EXPECT_EQ(output.find("launchd"), std::string::npos);
}

}  // namespace
