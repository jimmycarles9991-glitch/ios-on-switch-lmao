// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// TegraNVENC.cpp builds in no host test, so these guard the wiring TegraNVENCInputs.h depends on: a queued job's
// producers are recorded before its fence is handed out, and the client's Wait asks for their verdict once NVENC's
// own fence is reached.
TEST(NVENCInputsWiring, AQueuedJobsProducersAreRecordedBeforeItsFenceIsHandedOut)
{
    const std::string run = Body(SourceText("../TegraNVENC.cpp"), "int run(");
    const size_t submitted = run.find("switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_NVENC");
    const size_t recorded = run.find("gInputs.record(gBase + job, producers, count);");
    const size_t handed = run.find("*fence = gBase + job;");
    ASSERT_NE(recorded, std::string::npos);
    EXPECT_LT(submitted, recorded);
    EXPECT_LT(recorded, handed);
    EXPECT_THAT(run, HasSubstr("producers[count++] = after->producer;"));
    EXPECT_THAT(run, HasSubstr("producers[count++] = ordered[k].producer;"));
}

TEST(NVENCInputsWiring, BothRoutesNameTheJobThatWroteTheInput)
{
    EXPECT_THAT(Body(SourceText("../TegraNVENCSource.cpp"), "int tegra_nvenc_source("),
                HasSubstr("*after = {tegra_vic_syncpt(), fence, written};"));
    EXPECT_THAT(Body(SourceText("../TegraNVENCDirect.cpp"), "int begin("), HasSubstr("producer};"));
}

TEST(NVENCInputsWiring, TheClientsWaitAnswersTheInputsFailure)
{
    const std::string client = SourceText("../TegraNVENCUserClient.cpp");
    const size_t waited = client.find("int ret = tegra_nvenc_wait(w->fence);");
    const size_t asked = client.find("ret = tegra_nvenc_input_failure(w->fence);");
    ASSERT_NE(waited, std::string::npos);
    ASSERT_NE(asked, std::string::npos);
    EXPECT_LT(waited, asked);
}

// NVENC's every bounded wait (a client's, a full queue's, the idle gate's drain) counts its bound from the inputs.
TEST(NVENCInputsWiring, EveryBoundedWaitCountsFromTheInputs)
{
    const std::string source = SourceText("../TegraNVENC.cpp");
    EXPECT_THAT(Body(source, "int run("), HasSubstr("boundedWait(gBase + job - kInFlight)"));
    EXPECT_THAT(Body(source, "extern \"C\" int tegra_nvenc_wait("), HasSubstr("boundedWait(fence)"));
    EXPECT_THAT(Body(source, "int gateOff(void *, bool drain)"), HasSubstr("boundedWait(gBase + gIssued)"));
    EXPECT_EQ(switch_ios::test::Count(source, "tegra_host1x_syncpt_wait(gSyncpt"), 1u) << "only boundedWait's own";
}
}  // namespace
