// SPDX-License-Identifier: BSD-2-Clause-Patent
// What VIC hands another engine, and what it writes, goes through the GPU's ownership layer.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(VicOwnershipSource, CopyEngineCopiesAreCheckedAndReportTheirFailure)
{
    EXPECT_THAT(SourceText("../../TegraGM20B/TegraGM20BCopy.h"), Not(HasSubstr("int tegra_gm20b_ce_copy_plane(")));
    const std::string bounce = SourceText("../SwitchVICBounceSubmit.cpp");
    EXPECT_THAT(bounce, HasSubstr("const tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, completed, work};"));
    EXPECT_THAT(bounce, HasSubstr("nullptr, &done,"));
}

// Every VIC submission names what it reads and writes (switch_vic_submit_checked's uses), so a write VIC makes moves
// its allocation's host epoch and the next GPU reader invalidates the GPU's L2 before reading it. The entry points that
// submitted with no uses are gone.
TEST(VicOwnershipSource, EveryVicSubmissionRecordsItsUses)
{
    const std::string engine = SourceText("../TegraVIC.h");
    EXPECT_THAT(engine, Not(HasSubstr("int tegra_vic_run(")));
    EXPECT_THAT(engine, Not(HasSubstr("int tegra_vic_run_after(")));
    EXPECT_THAT(SourceText("../SwitchVICJobs.h"), Not(HasSubstr("switch_vic_submit_after(")));
    const std::string bounce = SourceText("../SwitchVICBounceSubmit.cpp");
    EXPECT_THAT(bounce, HasSubstr("switch_gpu_backing_use uses[4] = {{output, SWITCH_GPU_BACKING_WRITE, 0}};"));
    EXPECT_THAT(bounce, Not(HasSubstr("if (bounceSrc && !alias)")));
}

// A job's surfaces map through VIC's mapping space (switch_vic_mapping_create) and jobs queue in SwitchVICJobs: the
// per-job windows, the lock one job held across them, and the copy engine's descriptor registration have no callers.
TEST(VicOwnershipSource, NoJobMapsAWindowOrHoldsTheEngine)
{
    const std::string engine = SourceText("../TegraVIC.h");
    EXPECT_THAT(engine, Not(HasSubstr("tegra_vic_window_map(")));
    EXPECT_THAT(engine, Not(HasSubstr("tegra_vic_window_clear(")));
    EXPECT_THAT(engine, Not(HasSubstr("tegra_vic_lock(")));
    EXPECT_THAT(engine, Not(HasSubstr("tegra_vic_unlock(")));
    EXPECT_THAT(SourceText("../TegraVIC.cpp"), Not(HasSubstr("gJobLock")));
    EXPECT_THAT(SourceText("../../TegraGM20B/SwitchGpuCopyMappings.h"), Not(HasSubstr("switch_gpu_copy_register(")));
}

TEST(VicOwnershipSource, EventBearingJobsReserveDestinationWhenReadyAndFailUncommittedSignals)
{
    const std::string client = SourceText("../TegraVICUserClient.cpp");
    const auto transform = switch_ios::test::Block(client, "IOReturn transform(");
    EXPECT_TRUE(transform.before("if (!job->events && !job->mayDeferTurn)", "switch_vic_events_arm(job->events);"));
    const auto ready = switch_ios::test::Block(client, "void eventsReady(");
    EXPECT_TRUE(ready.before("switch_vic_surface_reserve_write(&job->dst, &job->dstReserve);",
                             "gRequests.ready(job, status);"));
    const auto run = switch_ios::test::Block(client, "bool runJob(");
    EXPECT_TRUE(run.before("const int prepared = job->eventStatus ? job->eventStatus",
                           "WriteReservationGuard dstReserve{job->dstReserve};"));
    const auto finish = switch_ios::test::Block(client, "void finishJob(");
    EXPECT_TRUE(finish.has("switch_vic_events_fail(&job->events,"));
    const auto abandon = switch_ios::test::Block(client, "void abandonUnqueued(");
    EXPECT_FALSE(abandon.has("switch_vic_events_fail"));
}
