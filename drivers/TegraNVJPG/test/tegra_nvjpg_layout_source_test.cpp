// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// TegraNVJPGUserClient.cpp builds in no host test. A GPU sampler whose first ask of a surface nothing has written comes
// while NVJPG decodes into it would tile in place the rows NVJPG is writing (TegraGM20BTiled.cpp's seed-0 rule): the
// decode moves the seed before it reads the layout, so such an ask finds a written surface and keeps it pitch, and a
// surface decided block linear before is held, its texels unmoved, until the decode has ended.
TEST(NVJPGLayoutWiring, TheSeedMovesBeforeTheLayoutIsReadAndTheHoldOutlastsTheDecode)
{
    const std::string source = SourceText("../TegraNVJPGUserClient.cpp");
    const std::string run = Body(source, "IOReturn runJob(Job *job)\n{");
    const auto seed = run.find("tegra_vic_surface_written(&job->dst);");
    const auto layout = run.find("takeLayout(job, gOutList[0], &hold)");
    const auto decode = run.find("tegra_nvjpg_run(&job->engine");
    ASSERT_NE(seed, std::string::npos);
    ASSERT_NE(layout, std::string::npos);
    ASSERT_NE(decode, std::string::npos);
    EXPECT_LT(seed, layout);
    EXPECT_LT(layout, decode);
    EXPECT_NE(run.find("tegra_gm20b_surface_unhold(hold);", decode), std::string::npos);
    const std::string take = Body(source, "const char *takeLayout(Job *job, uint64_t firstPage, void **hold)\n{");
    EXPECT_LT(take.find("tegra_gm20b_surface_hold(firstPage, &tiled)"), take.find("tegra_gm20b_surface_layout("));
    EXPECT_THAT(take, HasSubstr("tegra_nvjpg_plan_block_linear("));
}

// A '422f' or '444f' decode: VIC's re-pack is queued while NVJPG's job is in flight, to wait for its fence on the
// hardware (no CPU between the engines), and the decode ends when VIC's completion arrives.
TEST(NVJPGLayoutWiring, TheRepackWaitsForTheDecodeOnTheHardwareAndTheDecodeEndsOnVIC)
{
    const std::string run =
        Body(SourceText("../TegraNVJPG.cpp"),
             "extern \"C\" int tegra_nvjpg_run(const TegraNVJPGJob *job, nvjpg_dec_status *status, "
             "uint64_t *busyUs,\n                               const TegraNVJPGFollow *follow)\n{");
    const auto submit = run.find("switch_host1x_job_submit(");
    const auto follow = run.find("follow->queue(follow->context, gSyncpt, fence);");
    const auto wait = run.find("tegra_host1x_syncpt_wait(gSyncpt, fence");
    ASSERT_NE(follow, std::string::npos);
    EXPECT_LT(submit, follow);
    EXPECT_LT(follow, wait);
    const std::string repack = SourceText("../TegraNVJPGRepack.cpp");
    EXPECT_THAT(repack, HasSubstr("const TegraVICAcquire after = {syncpt, fence};"));
    EXPECT_THAT(repack, HasSubstr("ret = tegra_nvjpg_run(engine, status, busyUs, &follow);"));
    EXPECT_THAT(repack, HasSubstr("while (!wait.done)\n            IOLockSleep(gWaitLock, &wait, THREAD_UNINT);"));
}
}  // namespace
