// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// A GPU sampler whose first ask of a surface nothing has written comes while NVJPG decodes into it would tile in place
// the rows NVJPG is writing (TegraGM20BTiled.cpp's seed-0 rule): the decode moves the seed before it reads the layout,
// so such an ask finds a written surface and keeps it pitch, and a surface decided block linear before is held, its
// texels unmoved, until the decode and any copy-engine re-pack have ended. Every route but VIC's asks.
TEST(NVJPGLayoutWiring, TheSeedMovesBeforeTheLayoutIsReadAndTheHoldOutlastsTheDecodeAndItsCopy)
{
    const std::string source = SourceText("../TegraNVJPGJobs.cpp");
    const std::string begin = Body(source, "bool beginJob(Job *job)\n{");
    const auto vic = begin.find("if (job->plan.repack == TEGRA_NVJPG_REPACK_VIC)\n        return beginRepack(job);");
    const auto seed = begin.find("tegra_vic_surface_written(&job->dst);");
    const auto layout = begin.find("routeJob(job, target, &job->hold, &route)");
    const auto decode = begin.find("return job->plan.repack ? beginRepack(job) : beginDirect(job, target, extent);");
    ASSERT_NE(vic, std::string::npos);
    ASSERT_NE(seed, std::string::npos);
    ASSERT_NE(layout, std::string::npos);
    ASSERT_NE(decode, std::string::npos);
    EXPECT_LT(vic, seed);
    EXPECT_LT(seed, layout);
    EXPECT_LT(layout, decode);
    EXPECT_GT(begin.find("beginRepack(job)", seed), layout) << "no copy-engine re-pack before the ask";
    const std::string route = Body(
        source, "const char *routeJob(Job *job, const SwitchNVJPGBacking &target, void **hold, uint32_t *route)\n{");
    EXPECT_LT(route.find("tegra_gm20b_surface_hold(target.pages[0], &tiled)"),
              route.find("tegra_gm20b_surface_layout("));
    EXPECT_THAT(route, HasSubstr("tegra_nvjpg_route("));
    EXPECT_THAT(Body(source, "void giveHold(Job *job)\n{"), HasSubstr("tegra_gm20b_surface_unhold(job->hold);"));
    EXPECT_THAT(Body(source, "void conclude(Job *job)\n{"), HasSubstr("giveHold(job);"));
    for (const char *step : {"bool beginDirect(Job *job, const SwitchNVJPGBacking &target, uint32_t extent)\n{",
                             "bool beginRepack(Job *job)\n{", "bool advanceJob(Job *job)\n{",
                             "IOReturn completeDirect(Job *job)\n{", "IOReturn completeRepack(Job *job)\n{"})
        EXPECT_THAT(Body(source, step), Not(HasSubstr("Hold"))) << step;
}

// The destination is mapped for the plan the route leaves, and the job lock is held only while NVJPG has the job: from
// its start to its finish.
TEST(NVJPGLayoutWiring, TheDestinationIsMappedForThePlanTheRouteLeavesAndOnlyTheEngineJobHoldsTheJobLock)
{
    const std::string source = SourceText("../TegraNVJPGJobs.cpp");
    const std::string direct =
        Body(source, "bool beginDirect(Job *job, const SwitchNVJPGBacking &target, uint32_t extent)\n{");
    const auto map = direct.find("switch_nvjpg_mapping(target, extent, p.writeEnd, &why, job->pid)");
    const auto lock = direct.find("tegra_nvjpg_lock();");
    const auto unlock = direct.find("tegra_nvjpg_unlock();");
    ASSERT_NE(map, std::string::npos);
    ASSERT_NE(lock, std::string::npos);
    ASSERT_NE(unlock, std::string::npos);
    EXPECT_LT(map, lock);
    EXPECT_EQ(direct.substr(lock, unlock - lock),
              "tegra_nvjpg_lock();\n    const int error = tegra_nvjpg_start(&job->engine, "
              "&ended);\n    if (!error) {\n        job->phase = "
              "Job::Phase::OnEngine;\n        return true;\n    }\n    ");
    EXPECT_THAT(Body(source, "bool advanceJob(Job *job)\n{"),
                HasSubstr("job->engineResult = tegra_nvjpg_finish(&job->engine, &job->engineStatus, "
                          "&job->engineBusy);\n        tegra_nvjpg_unlock();"));
}

// A '422f' or '444f' decode, or one the copy engine re-packs: the re-pack is queued while NVJPG's job is in flight, to
// wait for its fence on the hardware (no CPU between the engines). No CPU thread waits on either engine: NVJPG's fence
// and the re-pack's completion end the decode by callback, and the engine thread sleeps only on its own queue.
TEST(NVJPGLayoutWiring, TheRepackWaitsOnTheHardwareAndNoThreadWaitsOnAnEngine)
{
    const std::string engine = SourceText("../TegraNVJPGFlight.cpp");
    const std::string start = Body(engine, "extern \"C\" int tegra_nvjpg_start(const TegraNVJPGJob *job, const "
                                           "TegraNVJPGSignal *signal,\n                                 const "
                                           "TegraNVJPGFollow *follow)\n{");
    const auto submit = start.find("switch_host1x_job_submit(");
    const auto follow = start.find("follow->queue(follow->context, gSyncpt, fence);");
    const auto armed = start.find("switch_host1x_fence_add(&gFlight.fence");
    ASSERT_NE(follow, std::string::npos);
    ASSERT_NE(armed, std::string::npos);
    EXPECT_LT(submit, follow);
    EXPECT_LT(follow, armed);
    const std::string finish = Body(engine, "extern \"C\" int tegra_nvjpg_finish(const TegraNVJPGJob *job, "
                                            "nvjpg_dec_status *status, uint64_t *busyUs)\n{");
    EXPECT_THAT(finish, Not(HasSubstr("syncpt_wait")));
    const std::string repack = SourceText("../TegraNVJPGRepack.cpp");
    EXPECT_THAT(repack, HasSubstr("const TegraVICAcquire after = {syncpt, fence};"));
    EXPECT_THAT(repack, HasSubstr("const int ret = tegra_nvjpg_start(engine, ended, &follow);"));
    EXPECT_THAT(repack, Not(HasSubstr("IOLockSleep")));
    const std::string thread = Body(SourceText("../TegraNVJPGJobs.cpp"), "void engineThread(void *, wait_result_t)\n{");
    EXPECT_THAT(thread, HasSubstr("IOLockSleep(gQueueLock, &gHead, THREAD_UNINT);"));
}
}  // namespace
