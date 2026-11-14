// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// TegraVIC.cpp builds in no host test: these guard the wiring switch_vic_jobs_test's gate scenarios rely on. An engine
// a cancellation's reset did not bring back goes to the idle gate once its cancelled jobs have ended, and the gate
// powers it on at a later job; nothing keeps refusing work for the boot.
TEST(VICRecoverWiring, ALostEngineIsHandedBackToTheIdleGate)
{
    const std::string source = SourceText("../TegraVIC.cpp");
    EXPECT_THAT(Body(source, "extern \"C\" void switch_vic_power_lost(void)"), HasSubstr("gGate.lost();"));
    EXPECT_THAT(Body(SourceText("../SwitchVICJobs.cpp"), "void renumberLocked()"),
                HasSubstr("if (!resumed)\n        switch_vic_power_lost();"));
    EXPECT_THAT(Body(source, "extern \"C\" bool tegra_vic_ready(void)"), HasSubstr("return gUp;"));
    EXPECT_THAT(Body(source, "const char *switch_vic_engine_wedge()"), Not(HasSubstr("lost")));
}

// A power-on attaches the SMMU domain a resume could not; a resume that could not leaves the next job to try.
TEST(VICRecoverWiring, APowerOnAttachesWhatAResumeCouldNot)
{
    const std::string source = SourceText("../TegraVIC.cpp");
    const std::string gateOn = Body(source, "int gateOn(void *)\n{");
    EXPECT_THAT(gateOn, HasSubstr("if (!gAttached)\n        gAttached = smmuAttach();"));
    EXPECT_THAT(gateOn, HasSubstr("gAttached && powerUp(&wasOn) && switch_vic_engine_boot()"));
    EXPECT_THAT(gateOn, HasSubstr("this job fails, the next one powers VIC on again"));
    const std::string resume = Body(source, "void engineResume()\n{");
    EXPECT_THAT(resume, HasSubstr("gAttached = attached;"));
    EXPECT_THAT(resume, HasSubstr("the next job tries again"));
    EXPECT_THAT(Body(source, "void engineSuspend()\n{"), HasSubstr("gAttached = false;"));
}
}  // namespace
