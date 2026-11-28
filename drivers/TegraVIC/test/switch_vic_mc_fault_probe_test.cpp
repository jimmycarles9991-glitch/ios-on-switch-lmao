// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICMcFaultProbe.h"
#include "vic_model.h"

namespace {
using namespace switch_vic_mc_fault_probe;

constexpr uint64_t kDstIova = 0x40000000, kConfig = 0x80001000, kFilter = 0x80002000;

// Every read of the copy faults (nothing is mapped at its source) and stays within what the MC filter expects.
TEST(VicMcFaultProbe, TheCopyBuildsAndReadsOnlyTheUnmappedSource)
{
    TegraVICJob job;
    ASSERT_EQ(build(kDstIova, kConfig, kFilter, &job), nullptr);
    std::vector<uint8_t> destination(kBufferBytes, 0xab);
    VicModel model = {};
    model.region[0] = {kDstIova, destination.data(), destination.size()};
    model.regions = 1;
    vic_model_run(&model, &job);
    EXPECT_EQ(model.faults, uint64_t(kPixels) * kRows);
    int found = 0;
    EXPECT_EQ(uint64_t(vic_model_method(&job, TEGRA_VIC_M_SURFACE0_SLOT0_LUMA, &found)) << 8, kFaultIova);
    EXPECT_GE(kFaultBytes, 4 * kRows * kPitch) << "the rows past the last a cache fetch may read too";
}
}  // namespace
