// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// TegraNVENC.cpp builds in no host test, so these guard the wiring TegraNVENCInputs.h and SwitchNVENCFlight.h depend
// on: a queued job's producers are recorded before its fence is handed out, the client's Wait asks for their verdict
// once NVENC's own fence is reached, and bounded waits drop gEngineLock before sleeping.
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

// NVENC's every bounded wait (a client's, a full queue's, the idle gate's drain) counts its bound from the inputs and
// releases gEngineLock across the sleep.
TEST(NVENCInputsWiring, EveryBoundedWaitCountsFromTheInputsAndDropsEngineLock)
{
    const std::string source = SourceText("../TegraNVENC.cpp");
    const std::string flight = SourceText("../SwitchNVENCFlight.h");
    EXPECT_THAT(Body(source, "int run("), HasSubstr("gFlight.waitSlotLocked(&job)"));
    const std::string waitSlot = Body(flight, "int waitSlotLocked(uint32_t *outJob)");
    const size_t slotUnlock = waitSlot.find("platform_.unlock();");
    const size_t slotWait = waitSlot.find("platform_.boundedWait(waitFence);");
    const size_t slotRelock = waitSlot.find("platform_.lock();");
    ASSERT_NE(slotUnlock, std::string::npos);
    ASSERT_NE(slotWait, std::string::npos);
    ASSERT_NE(slotRelock, std::string::npos);
    EXPECT_LT(slotUnlock, slotWait);
    EXPECT_LT(slotWait, slotRelock);

    EXPECT_THAT(Body(source, "extern \"C\" int tegra_nvenc_wait("), HasSubstr("boundedWait(fence)"));
    const std::string gateOff = Body(source, "int gateOff(void *, bool drain)");
    const size_t gateUnlock = gateOff.find("IOLockUnlock(gEngineLock);");
    const size_t gateWait = gateOff.find("boundedWait(drainFence)");
    ASSERT_NE(gateWait, std::string::npos);
    EXPECT_NE(gateUnlock, std::string::npos);
    EXPECT_LT(gateOff.rfind("IOLockUnlock(gEngineLock);", gateWait), gateWait);
    EXPECT_EQ(switch_ios::test::Count(source, "tegra_host1x_syncpt_wait(gSyncpt"), 0u);
    EXPECT_EQ(switch_ios::test::Count(source, "tegra_host1x_syncpt_wait_unless(gSyncpt"), 1u)
        << "only boundedWait's own";
}

TEST(NVENCInputsWiring, UserClientDropsLockAcrossSubmitAndBufferFreeAndSupportsCompletionPort)
{
    const std::string client = SourceText("../TegraNVENCUserClient.cpp");
    const std::string ext = Body(client, "IOReturn AppleAVE2UserClient::externalMethod(");
    EXPECT_THAT(ext, HasSubstr("if (method == TegraNVENCMethodWait)"));
    EXPECT_THAT(ext, HasSubstr("else if (method == TegraNVENCMethodBufferFree)"));
    EXPECT_THAT(ext, HasSubstr("else if (method == TegraNVENCMethodSubmit)"));
    EXPECT_THAT(Body(client, "IOReturn AppleAVE2UserClient::registerNotificationPort("),
                HasSubstr("SWITCH_NVENC_COMPLETION_PORT"));
    const std::string freeBody = Body(client, "IOReturn AppleAVE2UserClient::bufferFreeMethod(");
    EXPECT_LT(freeBody.find("IOLockUnlock(lock_);"), freeBody.find("quiesceFence("));
    const std::string submitBody = Body(client, "IOReturn AppleAVE2UserClient::submitMethod(");
    EXPECT_LT(submitBody.find("IOLockUnlock(lock_);"), submitBody.find("submitPrepared("));
}
}  // namespace
