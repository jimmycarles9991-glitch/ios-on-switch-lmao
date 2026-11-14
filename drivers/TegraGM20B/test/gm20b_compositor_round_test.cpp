// SwitchGpuCompositorRound.h: what the compositor did between two asks of the spans op, for ui.scale_pacing.
#include <gtest/gtest.h>

#include <cstdio>

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
	switch_gpu_comp_round_scanned(&r, 15, 90);
	switch_gpu_comp_round_scanned(&r, 7, 17);
	switch_gpu_comp_round taken = {};
	switch_gpu_comp_round_take(&r, &taken);
	char line[256];
	switch_gpu_comp_round_format(line, sizeof(line), &taken);
	EXPECT_STREQ(line, "jobs=3 gpu_us=41666 over_period=1 imports=1 import_us=3500 allocs=2 "
			   "alloc_us=9100 cold_allocs=1 scanned=2 passes=22 draws=107 ");
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
