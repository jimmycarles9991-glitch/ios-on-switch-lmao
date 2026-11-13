// SPDX-License-Identifier: MIT
// SwitchGpuProfileSequence.h: which pushes of a profiled job stamp (and so wait for idle), and the window's per-owner
// "gpuprof cost" line that tells a pacing change from kernels with and without the stamps apart.
#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include "../SwitchGpuProfileSequence.h"

namespace
{
std::string cost(const switch_gpu_prof_cost &c)
{
	char line[160] = {};
	return switch_gpu_prof_cost_format(line, sizeof line, &c) ? line : "";
}

tgm_push push(bool fence, bool incomplete = false)
{
	tgm_push p = {};
	p.dwords = 4;
	p.fence = fence;
	p.incomplete = incomplete;
	return p;
}
}  // namespace

// An app's profiled job stamps after every readable whole push; the compositor's only where its job already waits for
// idle: the end of a batch (an internal fence) and its last push (the job's fence).
TEST(ProfStamps, TheCompositorStampsOnlyWhereItsJobAlreadyWaitsForIdle)
{
	const tgm_push job[] = {push(false), push(true), push(false), push(false, true),
				push(false)};
	const uint32_t count = 5;
	int app = 0, compositor = 0;
	for (uint32_t i = 0; i < count; ++i) {
		app += switch_gpu_prof_stamped(0, &job[i], i, count, TGM_PC_DRAW);
		compositor += switch_gpu_prof_stamped(1, &job[i], i, count, TGM_PC_DRAW);
	}
	EXPECT_EQ(app, 4) << "every push but the incomplete one";
	EXPECT_EQ(compositor, 2) << "the batch's end and the last push";
	EXPECT_FALSE(switch_gpu_prof_stamped(1, &job[0], 0, count, TGM_PC_DRAW));
	EXPECT_TRUE(switch_gpu_prof_stamped(1, &job[1], 1, count, TGM_PC_DRAW));
	EXPECT_TRUE(switch_gpu_prof_stamped(1, &job[4], 4, count, TGM_PC_DRAW));
	EXPECT_FALSE(switch_gpu_prof_stamped(0, &job[0], 0, count, TGM_PC_SKIP));
	EXPECT_FALSE(switch_gpu_prof_stamped(1, &job[3], 3, count, TGM_PC_DRAW))
		<< "incomplete: no stamp, last or not";
}

TEST(ProfCost, NothingStampedPrintsNothing)
{
	EXPECT_EQ(cost({}), "");
	EXPECT_EQ(cost({0, 0, 0, 12, 0, 12000000}), "");
}

// Two jobs of 30 profiled, each 1300 us; the other 28 average 1000 us: the stamps cost about 300 us a profiled job.
TEST(ProfCost, TheProfiledMeanLessTheOthersIsTheStampsCost)
{
	const switch_gpu_prof_cost c = {2, 48, 2, 30, 2600000, 2600000 + 28000000};
	EXPECT_EQ(cost(c), " 2 stamped (48 stamps): 1300 us each vs 1000 us unstamped (+300 us)");
}

// The compositor's stamps sit where its jobs already drain: its profiled jobs cost what the others do.
TEST(ProfCost, ACompositorThatAddsNoDrainShowsNoCost)
{
	const switch_gpu_prof_cost c = {1, 3, 1, 9, 1000000, 9000000};
	EXPECT_EQ(cost(c), " 1 stamped (3 stamps): 1000 us each vs 1000 us unstamped (+0 us)");
}

// Stamped but nothing to compare it with (not read back yet, or no other job): the counts alone.
TEST(ProfCost, NoSpanToCompareGivesTheCountsAlone)
{
	EXPECT_EQ(cost({1, 7, 0, 5, 0, 5000000}), " 1 stamped (7 stamps), no span to compare");
	EXPECT_EQ(cost({1, 7, 1, 1, 1000000, 1000000}),
		  " 1 stamped (7 stamps), no span to compare");
}
