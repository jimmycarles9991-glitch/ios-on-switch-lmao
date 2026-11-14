// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// VideoToolbox fills a process's encoder registry once: H264H7EncoderRegister decides after waiting for TegraNVENC, or
// for the kernel's word that it will not come (kits/SwitchServiceWait, whose own test runs the wait against IOKit).
TEST(AVEServiceWait, RegistrationWaitsForTheServiceOrItsAbsence)
{
    EXPECT_THAT(Body(SourceText("../AVE/AVEEncoder.c"), "void H264H7EncoderRegister(void)"),
                HasSubstr("const bool published = nvenc_service_wait();"));
    EXPECT_THAT(Body(SourceText("../AVE/NVENCClient.c"), "bool nvenc_service_wait(void)"),
                HasSubstr("switch_service_wait(\"TegraNVENC\", TEGRA_NVENC_ABSENT_RESOURCE, 20000)"));
}

// A bring-up that ends without the service (failed, or switch-no-nvdec) says so, so no process waits out the bound.
TEST(AVEServiceWait, TheKernelPublishesTheAbsenceWhenItsBringUpEndsWithoutTheService)
{
    const std::string source = SourceText("../../TegraNVENC/TegraNVENC.cpp");
    EXPECT_THAT(Body(source, "void bringUp(void *, wait_result_t)"),
                HasSubstr("if (!gPublished)\n        IOService::publishResource(TEGRA_NVENC_ABSENT_RESOURCE);"));
    EXPECT_THAT(Body(source, "extern \"C\" int tegra_nvenc_probe(void)"),
                HasSubstr("IOService::publishResource(TEGRA_NVENC_ABSENT_RESOURCE);"));
}
}  // namespace
