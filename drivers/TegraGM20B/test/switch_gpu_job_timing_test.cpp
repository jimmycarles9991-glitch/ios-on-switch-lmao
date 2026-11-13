// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../SwitchGpuJobTiming.h"
#include "../SwitchGpuProfileSequence.h"

namespace
{
// One job of `us` batches, the stamps in microseconds (as PTIMER units read as nanoseconds times 1000)
void job(switch_gpu_job_window &w, uint32_t first, const std::vector<uint64_t> &us)
{
	switch_gpu_job_run run{};
	uint64_t t = 1000000;
	switch_gpu_job_begin(&run, first, first + uint32_t(us.size()) - 1, t);
	for (uint64_t d : us) {
		t += d * 1000;
		switch_gpu_job_batch(&w, &run, t, d * 1000);
	}
	switch_gpu_job_end(&w, &run);
}

std::string line(const switch_gpu_job_window &w)
{
	char buf[1024];
	return switch_gpu_job_format(buf, sizeof(buf), "backboardd", &w) ? buf : "";
}
}  // namespace

TEST(SwitchGpuJobTiming, BucketsDoubleFrom25Microseconds)
{
	EXPECT_EQ(switch_gpu_batch_bucket(0), 0u);
	EXPECT_EQ(switch_gpu_batch_bucket(24999), 0u);
	EXPECT_EQ(switch_gpu_batch_bucket(25000), 1u);
	EXPECT_EQ(switch_gpu_batch_bucket(99999), 2u);
	EXPECT_EQ(switch_gpu_batch_bucket(1599999), 6u);
	EXPECT_EQ(switch_gpu_batch_bucket(1600000), 7u);
	EXPECT_EQ(switch_gpu_batch_bucket(100000000), 7u);
}

TEST(SwitchGpuJobTiming, AJobIsItsBatchesAndTheSlowestIsListed)
{
	switch_gpu_job_window w{};
	job(w, 100, {10, 300, 2000});
	job(w, 103, {40, 40});
	switch_gpu_job_window taken{};
	switch_gpu_job_take(&w, &taken);
	EXPECT_EQ(taken.jobs, 2u);
	EXPECT_EQ(taken.batches, 5u);
	EXPECT_EQ(taken.max_ns, 2310000u);
	EXPECT_EQ(line(taken), "backboardd: 2 jobs, 5 batches (2.5 a job), GPU mean 1195 max 2310 "
			       "us a job; batches by "
			       "GPU time: <25us 1 (10 us) <50us 2 (80 us) <400us 1 (300 us) "
			       ">=1600us 1 (2000 us); slowest "
			       "seqno 100-102, 2310 us: 10 300 2000");
	switch_gpu_job_window next{};
	switch_gpu_job_take(&w, &next);
	EXPECT_EQ(next.jobs, 0u);
	EXPECT_EQ(line(next), "");
}

TEST(SwitchGpuJobTiming, ALongJobListsItsFirstBatchesAndCountsTheRest)
{
	switch_gpu_job_window w{};
	job(w, 1, std::vector<uint64_t>(SWITCH_GPU_JOB_BATCHES + 3, 5));
	switch_gpu_job_window taken{};
	switch_gpu_job_take(&w, &taken);
	EXPECT_EQ(taken.batches, uint32_t(SWITCH_GPU_JOB_BATCHES + 3));
	EXPECT_NE(line(taken).find(" +3"), std::string::npos);
}

TEST(SwitchGpuJobTiming, ProcessesMergeKeepingTheSlowestJob)
{
	switch_gpu_job_window a{}, b{}, ta{}, tb{}, all{};
	job(a, 10, {100});
	job(b, 20, {50, 400});
	switch_gpu_job_take(&a, &ta);
	switch_gpu_job_take(&b, &tb);
	switch_gpu_job_merge(&all, &ta);
	switch_gpu_job_merge(&all, &tb);
	EXPECT_EQ(all.jobs, 2u);
	EXPECT_EQ(all.batches, 3u);
	EXPECT_EQ(all.slow_first, 20u);
	EXPECT_EQ(all.slow_batches, 2u);
}

TEST(SwitchGpuJobTiming, AListBeingWrittenIsLeftOut)
{
	switch_gpu_job_window w{};
	job(w, 5, {100, 100});
	w.slow_gen++;
	switch_gpu_job_window taken{};
	switch_gpu_job_take(&w, &taken);
	EXPECT_EQ(taken.jobs, 1u);
	EXPECT_EQ(taken.slow_batches, 0u);
	EXPECT_EQ(line(taken).find("slowest"), std::string::npos);
}

TEST(SwitchGpuProfileSequence, TheSlowestProfileIsKeptPushByPush)
{
	switch_gpu_prof_seq slow{}, run{}, taken{};
	run.first = 40;
	run.last = 52;
	run.n = run.pushes = 3;
	run.ns = 9000000;
	run.push[0] = {TGM_PC_DRAW + 2, 0, 21, 921, 450};
	run.push[1] = {TGM_PC_SYNC, 0, 0, 0, 12};
	run.push[2] = {TGM_PC_DRAW + 8, 2, 3, 40, 1300};
	switch_gpu_prof_seq_offer(&slow, &run);
	run.ns = 1000;
	switch_gpu_prof_seq_offer(&slow, &run);
	switch_gpu_prof_seq_take(&slow, &taken);
	char buf[512];
	ASSERT_TRUE(switch_gpu_prof_seq_format(buf, sizeof(buf), "backboardd", &taken));
	EXPECT_STREQ(buf, "backboardd seqno 40-52, 9000 us: D921kp/21 450 S 12 D40k/3i2 1300");
	switch_gpu_prof_seq_take(&slow, &taken);
	EXPECT_EQ(taken.n, 0u);
}
