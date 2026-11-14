// SPDX-License-Identifier: MIT
// SwitchGpuPathStats.cpp in a debug kernel: a SubmitJob's path counts (runJobUses' switch_gpu_path_job and four
// switch_gpu_path_taken) cost no lock and no atomic read-modify-write once the process has its slot: each CPU stores its
// own cells with preemption off. Each process still reads its own counts, summed over the CPUs it counted on.
#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include "switch_gpu_path_stats_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BSched.h"
}

namespace
{
int pid = 100;
}

extern "C" int proc_selfpid(void)
{
	return pid;
}

extern "C" uint32_t switch_gpu_info_flags(void)
{
	return 0;
}

extern "C" void tgm_hold_samples_take(int, uint32_t *n, uint64_t *max_ns,
				      switch_gpu_preempt_counts *)
{
	*n = 0;
	*max_ns = 0;
}

namespace
{
void submit_job(uint32_t shared)
{
	switch_gpu_path_job(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT, 0, 0, shared);
	switch_gpu_path_taken(SWITCH_GPU_PATH_NATIVE_READ, 0);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SET_JOB, 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SHARED_USE, shared);
	switch_gpu_path_taken(SWITCH_GPU_PATH_QUIET_JOB, 1);
}

std::string line()
{
	char text[1024];
	switch_gpu_path_line(text, sizeof(text));
	return text;
}

uint64_t takes_of(uint32_t jobs)
{
	const uint64_t before = ioLockTakes().load();
	for (uint32_t i = 0; i < jobs; ++i)
		submit_job(60);
	return ioLockTakes().load() - before;
}
}  // namespace

TEST(PathStats, AProcessWithItsSlotCountsAJobWithoutALock)
{
	pid = 4242;
	const uint64_t first = takes_of(1), next = takes_of(100);
	std::printf(
		"path counts: %llu IOLock takes for a process's first job, %llu for its next 100\n",
		static_cast<unsigned long long>(first), static_cast<unsigned long long>(next));
	EXPECT_LE(first, 1u);
	EXPECT_EQ(next, 0u);
	EXPECT_EQ(preemptionOff(), 0);
	const std::string counts = line();
	EXPECT_NE(counts.find("pid=4242 "), std::string::npos) << counts;
	EXPECT_NE(counts.find(" submit_jobs=101 "), std::string::npos) << counts;
	EXPECT_NE(counts.find(" shared_uses=6060 "), std::string::npos) << counts;
	EXPECT_NE(counts.find(" ranged_uses=6060 "), std::string::npos) << counts;
	EXPECT_NE(counts.find(" internal_jobs=101 "), std::string::npos) << counts;
}

TEST(PathStats, AProcessCountsOnEveryCpuItRunsOnAndTheWindowSumsThem)
{
	pid = 777;
	char text[1024];
	switch_gpu_path_window(text, sizeof(text));
	for (int cpu = 0; cpu < 4; ++cpu) {
		hostCpu() = cpu;
		submit_job(10);
	}
	hostCpu() = 0;
	EXPECT_NE(line().find(" submit_jobs=4 "), std::string::npos) << line();
	EXPECT_NE(line().find(" shared_uses=40 "), std::string::npos) << line();
	switch_gpu_path_window(text, sizeof(text));
	EXPECT_NE(std::string(text).find("submit_jobs=4 "), std::string::npos) << text;
	switch_gpu_path_window(text, sizeof(text));
	EXPECT_NE(std::string(text).find("submit_jobs=0 "), std::string::npos) << text;
}

TEST(PathStats, EachProcessReadsItsOwnAndTheLeastRecentGivesUpItsSlot)
{
	for (int p = 1; p <= 17; ++p) {
		pid = p;
		switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, uint32_t(p));
	}
	pid = 17;
	EXPECT_NE(line().find(" submit_jobs=17 "), std::string::npos) << line();
	pid = 2;
	EXPECT_NE(line().find(" submit_jobs=2 "), std::string::npos) << line();
	pid = 4242;
	EXPECT_NE(line().find(" submit_jobs=0 "), std::string::npos) << line();
}
