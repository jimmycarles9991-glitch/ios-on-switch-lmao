// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// NVDEC writes only buffers of its own, in the layout it chooses (Tegra block linear, 2 GOBs a block), and a surface it
// outputs to is published that native picture with its layout (switch_native_surface_publish); VIC and NVENC read it
// from there. No client IOSurface's pages are programmed into NVDEC as rows, so GM20B's decided layout of a client
// surface (tegra_gm20b_surface_layout) has nothing in NVDEC to reach.
TEST(NVDECLayoutWiring, NoClientSurfacePagesAreProgrammedIntoNVDEC)
{
    for (const char *file :
         {"../SwitchNVDECOutput.cpp", "../SwitchNVDECJobs.cpp", "../TegraNVDEC.cpp", "../TegraNVDECUserClient.cpp"}) {
        const std::string source = SourceText(file);
        EXPECT_THAT(source, Not(HasSubstr("tegra_vic_surface_prepare("))) << file;
        EXPECT_THAT(source, Not(HasSubstr("getPhysicalSegment("))) << file;
    }
    EXPECT_THAT(SourceText("../SwitchNVDECOutput.cpp"), HasSubstr("switch_native_surface_publish("));
}
}  // namespace
