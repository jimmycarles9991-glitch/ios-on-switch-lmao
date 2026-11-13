// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <regex>
#include <string>
#include "drivers/TegraVIC/SwitchVICStats.h"

namespace {
std::string window(SwitchVICStats &stats, uint64_t start, uint64_t end)
{
    char line[220] = {};
    EXPECT_FALSE(stats.line(start, line, sizeof(line)));
    EXPECT_TRUE(stats.line(end, line, sizeof(line)));
    return line;
}
}  // namespace

TEST(SwitchVICStats, QueuedSubmissionsReportTheirOperationsTimesAndBytes)
{
    SwitchVICStats stats = {};
    char line[220] = {};
    EXPECT_FALSE(stats.line(1000, line, sizeof(line)));
    stats.submission(1, false, 40, 8 << 20);
    stats.submission(2, true, 80, 4 << 20);
    stats.finished(0, 2000);
    stats.finished(-5, 4000);
    EXPECT_FALSE(stats.line(1000 + SwitchVICStats::kWindowUs - 1, line, sizeof(line)));
    ASSERT_TRUE(stats.line(1000 + SwitchVICStats::kWindowUs, line, sizeof(line)));
    EXPECT_STREQ(line, "TegraVIC: 5s jobs=2 ops=3 refused=0 pitch=0 failed=1 engine=3000us/job cpu=60us/job "
                       "mapped=6144KB/job bounced=1 presents=0 present_waits=0 maps=0");
}

TEST(SwitchVICStats, EachWindowStartsFromZeroAndKeepsTheDeviceCheckFormat)
{
    SwitchVICStats stats = {};
    stats.submission(1, false, 10, 4096);
    stats.finished(0, 100);
    window(stats, 1, 1 + SwitchVICStats::kWindowUs);
    stats.add(stats.refused, 3);
    char line[220] = {};
    ASSERT_TRUE(stats.line(1 + 2 * SwitchVICStats::kWindowUs, line, sizeof(line)));
    EXPECT_STREQ(line, "TegraVIC: 5s jobs=0 ops=0 refused=3 pitch=0 failed=0 engine=0us/job cpu=0us/job "
                       "mapped=0KB/job bounced=0 presents=0 present_waits=0 maps=0");
    const std::regex check(R"(TegraVIC: \d+s jobs=(\d+) ops=\d+ refused=(\d+))");
    EXPECT_TRUE(std::regex_search(std::string(line), check));
}

TEST(SwitchVICStats, OnlyTheFirstFailedJobABootIsNamed)
{
    SwitchVICStats stats = {};
    char line[160] = {};
    EXPECT_FALSE(stats.firstFailure("completion", 0, 0x34323076, 0x42475241, 77, line, sizeof(line)));
    EXPECT_FALSE(stats.firstFailure("events", -125, 0x34323076, 0x42475241, 77, line, sizeof(line)))
        << "a job cancelled with its caller";
    ASSERT_TRUE(stats.firstFailure("direct submission", -12, 0x34323076, 0x42475241, 77, line, sizeof(line)));
    EXPECT_STREQ(line, "TegraVIC: first failed job this boot: direct submission, status -12, '420v' to 'BGRA', pid 77");
    EXPECT_FALSE(stats.firstFailure("completion", -5, 0, 0x42475241, 78, line, sizeof(line)));

    SwitchVICStats fill = {};
    ASSERT_TRUE(fill.firstFailure("bounce submission", -22, 0, 0x34323276, 9, line, sizeof(line)));
    EXPECT_STREQ(line, "TegraVIC: first failed job this boot: bounce submission, status -22, 'none' to '422v', pid 9");
}
