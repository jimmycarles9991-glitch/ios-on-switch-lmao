// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// TegraNVENC.cpp builds in no host test: these guard the wiring by which an engine a stall's reset, a power-on or a
// resume lost is handed to the idle gate (kits/SwitchEngineGate, whose retry the gate's own test drives) and powered on
// by a later encode, instead of refusing work for the boot.
TEST(NVENCRecoverWiring, AStallTheResetDoesNotEndParksTheEngineAndHandsItToTheGate)
{
    const std::string stalled = Body(SourceText("../TegraNVENC.cpp"), "int stalled(uint32_t fence)");
    EXPECT_THAT(stalled, HasSubstr("if (!back) {\n        park();\n        gGate.lost();"));
    EXPECT_THAT(stalled, Not(HasSubstr("refuses work")));
}

TEST(NVENCRecoverWiring, APowerOnNoLongerSkipsAWedgedEngineAndClearsTheWedgeWhenItWorks)
{
    const std::string gateOn = Body(SourceText("../TegraNVENC.cpp"), "int gateOn(void *)\n{");
    EXPECT_THAT(gateOn, Not(HasSubstr("!gWedged &&")));
    EXPECT_THAT(gateOn, HasSubstr("gWedged = !answered;"));
    EXPECT_THAT(gateOn, HasSubstr("gAttached = !tegra_nvenc_memory_attach(gLine, sizeof(gLine));"));
}

// The service keeps taking clients and jobs while the engine is wedged, so a job reaches the gate's retry.
TEST(NVENCRecoverWiring, ReadyIsPublishedAndQuiesceAsksWhetherTheEngineIsWedged)
{
    EXPECT_THAT(Body(SourceText("../TegraNVENC.cpp"), "extern \"C\" bool tegra_nvenc_ready(void)"),
                Not(HasSubstr("gWedged")));
    EXPECT_THAT(Body(SourceText("../TegraNVENCUserClient.cpp"), "bool AppleAVE2UserClient::quiesce() const"),
                HasSubstr("tegra_nvenc_wedged()"));
}
}  // namespace
