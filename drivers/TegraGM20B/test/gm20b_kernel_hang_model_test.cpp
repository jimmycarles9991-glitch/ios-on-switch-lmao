// SPDX-License-Identifier: MIT
// T-v5a's pool-scrub panic over gm20b_kernel_hang_model.h: 6079249be's rule against the kernel channel's steps
// (TegraGM20BKernelHang.h) under hundreds of client kills.
#include <algorithm>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/test/gm20b_kernel_hang_model.h"

namespace
{
constexpr int kKills = 600;

// Every run with 6079249be's rule panics at its first stall; seed 38's is boot 20's: PBDMA0 on the scrubber's copy
// method, both engines idle, the last resort resetting nothing and the next check panicking.
TEST(KernelHangModel, Tv5asRuleBeforeEndsInThePoolScrubPanicWithEveryEngineIdle)
{
	for (unsigned seed = 1; seed <= 60; seed++) {
		Gpu gpu(Rule::Before, seed);
		EXPECT_FALSE(gpu.play(kKills)) << seed;
		EXPECT_EQ(gpu.panics, 1) << seed;
		EXPECT_LT(gpu.kills, kKills) << "the panic came first: " << seed;
		EXPECT_TRUE(gpu.panicState)
			<< "PBDMA0 on a method no engine takes, GR and the copy engine idle: "
			<< seed;
	}
	Gpu boot20(Rule::Before, 38);
	EXPECT_FALSE(boot20.play(kKills));
	EXPECT_EQ(boot20.pbdma, kScrub);
	EXPECT_TRUE(boot20.panicState);
}

TEST(KernelHangModel, HundredsOfKillsWithEngineResetsUnderTheScrubberNeverPanicAndTheScrubDrains)
{
	int wedges = 0;
	for (unsigned seed = 1; seed <= 20; seed++) {
		Gpu gpu(Rule::Shipped, seed);
		EXPECT_TRUE(gpu.play(kKills)) << seed;
		EXPECT_EQ(gpu.panics, 0) << seed;
		EXPECT_GE(gpu.kills, kKills) << seed;
		EXPECT_TRUE(gpu.chans[kScrub].jobs.empty()) << seed;
		EXPECT_FALSE(gpu.wedged) << seed;
		for (const int step : gpu.steps)
			EXPECT_NE(step, TGM_KERNEL_HANG_FATAL) << seed;
		if (gpu.wedges)
			EXPECT_GT(gpu.dirty, 0)
				<< "the ranges queued at a reset go back dirty: " << seed;
		wedges += gpu.wedges;
	}
	EXPECT_GT(wedges, 20) << "the model reaches boot 20's stall";
}

// Four waiters find each stall at the same moment (the scrubber's and the present's deadlines, a CPU wait, a copy
// queue): one step per stall, never a race to the fatal step while the first reset frees the GPU.
TEST(KernelHangModel, WaitersFindingTheStallAtOnceTakeOneStepAndNeverPanic)
{
	for (unsigned seed = 1; seed <= 10; seed++) {
		Gpu gpu(Rule::Shipped, seed);
		gpu.waiters = 4;
		EXPECT_TRUE(gpu.play(kKills)) << seed;
		EXPECT_EQ(gpu.panics, 0) << seed;
		for (size_t i = 1; i < gpu.steps.size(); i++) {
			if (gpu.steps[i] != TGM_KERNEL_HANG_OWN)
				EXPECT_GE(gpu.stepNs[i] - gpu.stepNs[i - 1], TGM_KERNEL_HANG_GAP_NS)
					<< seed;
		}
	}
}

// The scrubber's method never taken again once it is reloaded (NVIDIA's rule: the channel an engine reset left on the
// PBDMA is torn down): it stalls again after every reset while the rest of the GPU runs between. Every few steps reset
// every engine, nothing panics, and the clients keep running and being killed; the scrubber never drains.
TEST(KernelHangModel, AChannelThatNeverTakesItsMethodAgainIsResetAgainAndNeverPanics)
{
	Gpu gpu(Rule::Shipped, 38);
	gpu.stickyWedge = true;
	EXPECT_FALSE(gpu.play(kKills));
	EXPECT_EQ(gpu.panics, 0);
	EXPECT_GE(gpu.kills, kKills) << "the GPU ran between the stalls";
	EXPECT_NE(std::find(gpu.steps.begin(), gpu.steps.end(), TGM_KERNEL_HANG_ALL),
		  gpu.steps.end());
	EXPECT_EQ(std::find(gpu.steps.begin(), gpu.steps.end(), TGM_KERNEL_HANG_FATAL),
		  gpu.steps.end());
}

TEST(KernelHangModel, AStallNoResetFreesIsAHardwareFailureOnlyAfterACleanResetOfEveryEngine)
{
	Gpu gpu(Rule::Shipped, 20);
	gpu.brokenEngine = true;
	EXPECT_FALSE(gpu.play(kKills));
	ASSERT_EQ(gpu.panics, 1);
	ASSERT_GE(gpu.steps.size(), 3u);
	EXPECT_EQ(gpu.steps.front(), TGM_KERNEL_HANG_OWN);
	EXPECT_EQ(gpu.steps[1], TGM_KERNEL_HANG_ALL);
	EXPECT_EQ(gpu.steps.back(), TGM_KERNEL_HANG_FATAL);
	EXPECT_EQ(gpu.chans[kScrub].hang.did, TGM_KERNEL_HANG_DID_ALL_CLEAN)
		<< "the last reset had no client context resident";
}
}  // namespace
