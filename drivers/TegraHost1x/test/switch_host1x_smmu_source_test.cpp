// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's confinement (SwitchHost1xSmmu.cpp): both swgroups host1xdmar may use, hc and its in-band hc1 (TRM 18.5.3),
// go to the one view, and a confinement that fails part way is undone.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(Host1xSmmuSource, BothSwgroupsOfTheChannelFetchGoToTheViewOrNeither)
{
    const std::string source = SourceText("../SwitchHost1xSmmu.cpp");
    EXPECT_THAT(source, HasSubstr("{TEGRA_SMMU_HC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, kHostReads}"));
    EXPECT_THAT(source, HasSubstr("{TEGRA_SMMU_HC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_1, kHostWrites}"));
    EXPECT_THAT(source, HasSubstr("{TEGRA_SMMU_HC1_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, kChannelFetch}"));
    const std::string confine = Body(source, "bool switch_host1x_channels_confine(char *line, size_t size)\n");
    EXPECT_THAT(confine, HasSubstr("tegra_smmu_detach(TEGRA_SMMU_ASID_HOST1X, kSwgroups[j].asidReg);"));
}
