// kernel/switch/include/switch/focal.h on the host: the places the debug watchers keep for focal apps.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "switch/focal.h"

namespace {

constexpr uint64_t kHWTest = 0x1000, kScreenshots = 0x2000, kPreferences = 0x3000, kMaps = 0x4000;

// 25 Nov: a Capture press made ScreenshotServicesService focal over HW Test; when it left, HW Test lost its watch for
// 15 minutes, although it was focal all along.
TEST(XnuFocal, AnOverlayLeavingKeepsTheAppUnderItWatched)
{
    switch_focal f{};
    uint64_t evicted = 1;

    const unsigned hwtest = switch_focal_enter(&f, kHWTest, 10, &evicted);
    EXPECT_EQ(evicted, 0u);
    const unsigned overlay = switch_focal_enter(&f, kScreenshots, 20, &evicted);
    EXPECT_NE(overlay, hwtest);
    EXPECT_EQ(switch_focal_leave(&f, kScreenshots), overlay);
    EXPECT_EQ(f.owner[hwtest], kHWTest) << "HW Test keeps its place";
    EXPECT_EQ(switch_focal_leave(&f, kScreenshots), SWITCH_FOCAL_PLACES) << "a second leave frees nothing";
    EXPECT_EQ(switch_focal_leave(&f, kHWTest), hwtest);
    for (const auto owner : f.owner) {
        EXPECT_EQ(owner, 0u);
    }
}

TEST(XnuFocal, ATaskFocalAgainKeepsItsPlace)
{
    switch_focal f{};
    uint64_t evicted = 0;

    const unsigned first = switch_focal_enter(&f, kHWTest, 10, &evicted);
    EXPECT_EQ(switch_focal_enter(&f, kHWTest, 50, &evicted), first);
    EXPECT_EQ(evicted, 0u);
}

TEST(XnuFocal, AFourthFocalAppTakesThePlaceFocalTheLongest)
{
    switch_focal f{};
    uint64_t evicted = 0;

    switch_focal_enter(&f, kHWTest, 30, &evicted);
    const unsigned oldest = switch_focal_enter(&f, kScreenshots, 10, &evicted);
    switch_focal_enter(&f, kPreferences, 20, &evicted);
    EXPECT_EQ(switch_focal_enter(&f, kMaps, 40, &evicted), oldest);
    EXPECT_EQ(evicted, kScreenshots);
    EXPECT_EQ(switch_focal_leave(&f, kScreenshots), SWITCH_FOCAL_PLACES);
}

// A focal app that exits with no role change frees its place in every watcher: the exit hook ends its role, and a
// role other than focal leaves each watcher's place (stats.c's switch_sched_role, the one way into all three).
TEST(XnuFocal, AnAppThatExitsWhileFocalLeavesEveryWatcher)
{
    const std::string exit = switch_ios::test::SourceText("../switch/diag/exit.c");
    EXPECT_NE(exit.find("switch_sched_role(proc_task(p), TASK_UNSPECIFIED);"), std::string::npos);
    const std::string stats = switch_ios::test::SourceText("../switch/sched/stats.c");
    const size_t role = stats.find("void switch_sched_role(task_t task, int role)");
    ASSERT_NE(role, std::string::npos);
    const std::string body = stats.substr(role, stats.find("\n}\n", role) - role);
    for (const char *watcher : {"switch_stall_frame_role(task, role);", "switch_sched_busy_focal(task, role, main);",
                                "switch_recorder_role(task, role, main);"}) {
        EXPECT_NE(body.find(watcher), std::string::npos) << watcher;
    }
}

}  // namespace
