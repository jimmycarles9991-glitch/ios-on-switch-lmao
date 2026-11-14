// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// VideoToolbox fills a process's registry once: AVDRegister decides after waiting for TegraNVDEC, or for the kernel's
// word that it will not come (kits/SwitchServiceWait, whose own test runs the wait against IOKit), never from what
// happens to be published at that moment.
TEST(AVDServiceWait, RegistrationWaitsForTheServiceOrItsAbsence)
{
    EXPECT_THAT(Body(SourceText("../../AVD/AVDRegister.c"), "void AVDRegister(void)"),
                HasSubstr("const bool published = nvdec_service_wait();"));
    EXPECT_THAT(Body(SourceText("../../AVD/NVDECClient.c"), "bool nvdec_service_wait(void)"),
                HasSubstr("switch_service_wait(\"TegraNVDEC\", TEGRA_NVDEC_ABSENT_RESOURCE, kServiceWaitMs)"));
}

// A bring-up that ends without the service (failed, or switch-no-nvdec) says so, so no process waits out the bound.
TEST(AVDServiceWait, TheKernelPublishesTheAbsenceWhenItsBringUpEndsWithoutTheService)
{
    const std::string source = SourceText("../../../TegraNVDEC/TegraNVDEC.cpp");
    EXPECT_THAT(Body(source, "void settle(int how)"),
                HasSubstr("if (how != 1)\n        IOService::publishResource(TEGRA_NVDEC_ABSENT_RESOURCE);"));
    EXPECT_THAT(Body(source, "void bringUpStages()"), HasSubstr("gPublished = !tegra_nvdec_publish();"));
    EXPECT_THAT(Body(source, "void bringUpStages()"), Not(HasSubstr("gPublished = true;")));
}
}  // namespace
