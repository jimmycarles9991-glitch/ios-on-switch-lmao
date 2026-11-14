// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// NVENC reads a client surface two ways only: a decoder's native picture in place, planned from the layout that picture
// records (switch_native_surface_acquire, tegra_nvenc_direct_plan), or a picture VIC wrote into NVENC's own buffer
// (switch_vic_encode_input, which bounces a surface GM20B keeps block linear through the copy engine). No client
// IOSurface's pages are programmed into NVENC as rows.
TEST(NVENCLayoutWiring, NoClientSurfacePagesAreProgrammedIntoNVENC)
{
    for (const char *file :
         {"../TegraNVENCDirect.cpp", "../TegraNVENCSource.cpp", "../TegraNVENC.cpp", "../TegraNVENCUserClient.cpp"}) {
        const std::string source = SourceText(file);
        EXPECT_THAT(source, Not(HasSubstr("tegra_vic_surface_prepare("))) << file;
        EXPECT_THAT(source, Not(HasSubstr("getPhysicalSegment("))) << file;
    }
    const std::string direct = SourceText("../TegraNVENCDirect.cpp");
    EXPECT_THAT(direct, HasSubstr("switch_native_surface_acquire(&d->surface, &d->view, true)"));
    EXPECT_THAT(direct, HasSubstr("tegra_nvenc_direct_plan(&cfg, &s->source, &d->view.layout"));
    EXPECT_THAT(SourceText("../TegraNVENCSource.cpp"), HasSubstr("switch_vic_encode_input("));
}
}  // namespace
