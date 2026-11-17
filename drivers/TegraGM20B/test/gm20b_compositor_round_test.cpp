// SwitchGpuCompositorRound.h: what the compositor did between two asks of the spans op, for ui.scale_pacing.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "drivers/TegraGM20B/SwitchGpuCompositorRound.h"

namespace
{

constexpr uint64_t kPeriodNs = 16666667;

TEST(CompositorRound, ARoundCountsJobsOverAPeriodImportsAndScannedPasses)
{
	switch_gpu_comp_round r = {};
	switch_gpu_comp_round_job(&r, 8000000, kPeriodNs);
	switch_gpu_comp_round_job(&r, 17000000, kPeriodNs);
	switch_gpu_comp_round_job(&r, kPeriodNs, kPeriodNs);
	switch_gpu_comp_round_import(&r, 3500000);
	switch_gpu_comp_round_alloc(&r, 100000, 0);
	switch_gpu_comp_round_alloc(&r, 9000000, 1);
	switch_gpu_comp_round_scanned(&r, 15, 90, 28, 0);
	switch_gpu_comp_round_scanned(&r, 7, 17, 0, 12);
	switch_gpu_comp_round taken = {};
	switch_gpu_comp_round_take(&r, &taken);
	char line[256];
	switch_gpu_comp_round_format(line, sizeof(line), &taken);
	EXPECT_STREQ(line, "jobs=3 gpu_us=41666 over_period=1 imports=1 import_us=3500 allocs=2 "
			   "alloc_us=9100 cold_allocs=1 scanned=2 passes=22 draws=107 ilk=28 "
			   "twin=12 windows=0 holds=0 ");
}

// The deadline's counts only grow and wrap at 2^32: each round gives what they gained since the round before
TEST(CompositorRound, ARoundGivesTheWindowsAndHoldsSinceTheRoundBefore)
{
	switch_gpu_comp_deadline seen = {};
	switch_gpu_comp_round first = {};
	switch_gpu_comp_round_deadline(&first, &seen, 162, 586);
	EXPECT_EQ(first.windows, 162u);
	EXPECT_EQ(first.holds, 586u);
	switch_gpu_comp_round second = {};
	switch_gpu_comp_round_deadline(&second, &seen, 162, 586);
	EXPECT_EQ(second.windows, 0u);
	EXPECT_EQ(second.holds, 0u);
	seen = {UINT32_MAX - 1, UINT32_MAX};
	switch_gpu_comp_round wrapped = {};
	switch_gpu_comp_round_deadline(&wrapped, &seen, 3, 5);
	EXPECT_EQ(wrapped.windows, 5u);
	EXPECT_EQ(wrapped.holds, 6u);
	char line[256];
	switch_gpu_comp_round_format(line, sizeof(line), &wrapped);
	EXPECT_NE(std::string(line).find(" windows=5 holds=6 "), std::string::npos) << line;
}

TEST(CompositorRound, TakingStartsTheNextRoundEmpty)
{
	switch_gpu_comp_round r = {};
	switch_gpu_comp_round_import(&r, 1000);
	switch_gpu_comp_round taken = {};
	switch_gpu_comp_round_take(&r, &taken);
	switch_gpu_comp_round_take(&r, &taken);
	EXPECT_EQ(taken.imports, 0u);
	EXPECT_EQ(taken.jobs, 0u);
}

}  // namespace
