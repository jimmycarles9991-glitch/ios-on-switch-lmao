// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/SwitchGpuPreemptLanding.h on the host: where a compositor kick's preempt of an app landed, from the
// app's last batch release before the compositor's start stamp, and the latency buckets the paths line gives.
#include <gtest/gtest.h>

#include "../SwitchGpuPreemptLanding.h"

TEST(GM20BPreemptLanding, AReleaseInsideTheJobBetweenTheAskAndTheStartIsABandsRelease)
{
	const switch_gpu_batch_release band = {1000, 0};
	EXPECT_EQ(switch_gpu_preempt_landing(&band, 900, 1100), SWITCH_GPU_LANDED_MID_JOB);
	EXPECT_EQ(switch_gpu_preempt_landing(&band, 1000, 1000), SWITCH_GPU_LANDED_MID_JOB);
}

TEST(GM20BPreemptLanding, TheJobsLastReleaseIsItsEnd)
{
	const switch_gpu_batch_release last = {1000, 1};
	EXPECT_EQ(switch_gpu_preempt_landing(&last, 900, 1100), SWITCH_GPU_LANDED_JOB_END);
}

TEST(GM20BPreemptLanding, NoReleaseBetweenTheAskAndTheStartIsUnknown)
{
	const switch_gpu_batch_release before = {800, 0};
	EXPECT_EQ(switch_gpu_preempt_landing(&before, 900, 1100), SWITCH_GPU_LANDED_UNKNOWN);
	const switch_gpu_batch_release after = {1200, 0};
	EXPECT_EQ(switch_gpu_preempt_landing(&after, 900, 1100), SWITCH_GPU_LANDED_UNKNOWN);
	const switch_gpu_batch_release none = {0, 0};
	EXPECT_EQ(switch_gpu_preempt_landing(&none, 0, 1100), SWITCH_GPU_LANDED_UNKNOWN);
}

TEST(GM20BPreemptLanding, LatenciesBucketUnder250UsThenDoublingTo4Ms)
{
	EXPECT_EQ(switch_gpu_preempt_bucket(0), 0u);
	EXPECT_EQ(switch_gpu_preempt_bucket(249999), 0u);
	EXPECT_EQ(switch_gpu_preempt_bucket(250000), 1u);
	EXPECT_EQ(switch_gpu_preempt_bucket(999999), 2u);
	EXPECT_EQ(switch_gpu_preempt_bucket(1000000), 3u);
	EXPECT_EQ(switch_gpu_preempt_bucket(3999999), 4u);
	EXPECT_EQ(switch_gpu_preempt_bucket(4000000), 5u);
	EXPECT_EQ(switch_gpu_preempt_bucket(40000000), 5u);
}

TEST(GM20BPreemptLanding, CountsAddUp)
{
	switch_gpu_preempt_counts into = {{1, 2, 3}, {1, 0, 0, 0, 0, 1}};
	const switch_gpu_preempt_counts from = {{0, 5, 1}, {0, 2, 0, 0, 0, 0}};
	switch_gpu_preempt_add(&into, &from);
	EXPECT_EQ(into.landed[SWITCH_GPU_LANDED_MID_JOB], 7u);
	EXPECT_EQ(into.landed[SWITCH_GPU_LANDED_JOB_END], 4u);
	EXPECT_EQ(into.bucket[1], 2u);
	EXPECT_EQ(into.bucket[5], 1u);
}
