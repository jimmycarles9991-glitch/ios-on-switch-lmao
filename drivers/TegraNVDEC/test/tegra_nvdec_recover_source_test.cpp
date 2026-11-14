// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// TegraNVDEC.cpp builds in no host test: these guard the wiring the queue and gate scenarios of
// switch_nvdec_submit_test rely on. A wedged engine goes back to the idle gate once its cancelled jobs have ended, and
// the gate powers it on at a later decode; nothing keeps refusing work for the boot.
TEST(NVDECRecoverWiring, AWedgedEngineIsHandedBackToTheIdleGate)
{
    const std::string source = SourceText("../TegraNVDEC.cpp");
    EXPECT_THAT(Body(source, "void switch_nvdec_power_lost()"), HasSubstr("gGate.lost();"));
    EXPECT_THAT(Body(SourceText("../SwitchNVDECJobs.cpp"), "void renumberLocked()"),
                HasSubstr("if (!resumed)\n        switch_nvdec_power_lost();"));
    EXPECT_THAT(Body(source, "extern \"C\" bool tegra_nvdec_ready(void)"), Not(HasSubstr("gWedged")));
}

TEST(NVDECRecoverWiring, APowerOnThatWorksClearsTheWedgeAndAttachesWhatAResumeCouldNot)
{
    const std::string gateOn = Body(SourceText("../TegraNVDEC.cpp"), "int gateOn(void *)\n{");
    EXPECT_THAT(gateOn, HasSubstr("__atomic_store_n(&gWedged, !answered, __ATOMIC_RELEASE);"));
    EXPECT_THAT(gateOn, HasSubstr("gAttached = !tegra_nvdec_memory_attach(gLine, sizeof(gLine));"));
}

// A connection closing while NVDEC is wedged waits for nothing: the parked engine touches no memory.
TEST(NVDECRecoverWiring, QuiesceAsksWhetherTheEngineIsWedged)
{
    EXPECT_THAT(Body(SourceText("../TegraNVDECUserClient.cpp"), "bool AppleD5500UserClient::quiesce() const"),
                HasSubstr("tegra_nvdec_wedged()"));
}
}  // namespace
