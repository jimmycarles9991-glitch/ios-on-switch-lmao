// SPDX-License-Identifier: MIT
// SubmitJob's lock-free reads under ThreadSanitizer: a compositor's jobs over shared textures read their producers'
// timelines through record handles while the apps draw into them, fail semantic waits and close their channels, and
// every thread counts its paths and jobs into its CPU's cells while a reader sums them.
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "switch_gpu_submit_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuJobStats.h"
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BSched.h"
}

extern "C" int proc_selfpid(void)
{
	return 50;
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
constexpr uint32_t kTextures = 16, kFrames = 400, kDraws = 200;

struct Channel {
	volatile uint32_t fence = 0;
	switch_gpu_timeline *timeline = nullptr;

	switch_gpu_backing_token token(uint32_t value) const
	{
		switch_gpu_backing_token made = {};
		switch_gpu_timeline_token(timeline, value, &made);
		return made;
	}

	void complete(uint32_t value)
	{
		__atomic_store_n(&fence, value, __ATOMIC_RELEASE);
	}
};

switch_gpu_backing *surface(uint64_t first)
{
	uint64_t pages[4];
	for (uint64_t i = 0; i < 4; ++i)
		pages[i] = first + i * 4096;
	return switch_gpu_backing_register(pages, 4, 0, nullptr, nullptr);
}

void count_job(uint32_t shared)
{
	const uint64_t ns[TGM_SUBMIT_PHASES] = {1000, 1000, 1000, 1000};
	switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, 1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_SHARED_USE, shared);
	switch_gpu_job_submitted(ns, 0, shared, shared, 0);
}
}  // namespace

TEST(SubmitRace, CompositorJobsReadProducersWithoutLocksWhileAppsDrawFailAndClose)
{
	ASSERT_EQ(switch_gpu_timeline_init(), 0);
	Channel compositor, apps[2];
	compositor.timeline = switch_gpu_timeline_create(1, &compositor.fence);
	for (uint32_t a = 0; a < 2; ++a)
		apps[a].timeline = switch_gpu_timeline_create(2 + a, &apps[a].fence);
	std::vector<switch_gpu_backing *> textures;
	for (uint32_t i = 0; i < kTextures; ++i)
		textures.push_back(surface(0xa00000000ull + uint64_t(i) * 0x100000));
	switch_gpu_backing *target = surface(0xb00000000ull);

	std::thread composite([&] {
		hostCpu() = 0;
		std::vector<switch_gpu_backing_use> uses;
		std::vector<switch_gpu_backing_token> dependencies(TegraGM20BMaxJobDependencies);
		for (uint32_t frame = 1; frame <= kFrames; ++frame) {
			uses.clear();
			for (auto *texture : textures)
				uses.push_back({texture, SWITCH_GPU_BACKING_READ, 0});
			uses.push_back({target, SWITCH_GPU_BACKING_WRITE, 0});
			const switch_gpu_backing_token completion = compositor.token(frame);
			switch_gpu_backing_transaction transaction = {};
			uint32_t needed = 0;
			ASSERT_EQ(switch_gpu_backing_begin_held(uses.data(), uint32_t(uses.size()),
								nullptr, 0, nullptr, 0, &completion,
								&transaction),
				  0);
			EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(),
								  TegraGM20BMaxJobUses, &needed),
				  0);
			switch_gpu_backing_commit(&transaction);
			compositor.complete(frame - 1);
			count_job(kTextures);
		}
	});
	std::vector<std::thread> drawing;
	for (uint32_t a = 0; a < 2; ++a)
		drawing.emplace_back([&, a] {
			hostCpu() = 1 + int(a);
			for (uint32_t value = 1; value <= kDraws; ++value) {
				const switch_gpu_backing_token drawn = apps[a].token(value);
				switch_gpu_event_waits authority;
				switch_gpu_timeline_semantic *wait = nullptr;
				ASSERT_EQ(switch_gpu_timeline_semantic_reserve(&drawn, &authority,
									       &wait),
					  0);
				for (uint32_t i = a; i < kTextures; i += 2) {
					switch_gpu_backing_use use = {textures[i],
								      SWITCH_GPU_BACKING_WRITE, 0};
					switch_gpu_backing_transaction transaction = {};
					ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &drawn,
									   &transaction),
						  0);
					switch_gpu_backing_commit(&transaction);
				}
				apps[a].complete(value);
				const int result = value % 7 ? 1 : -5;
				authority.result.store(result);
				switch_gpu_timeline_semantic_finish(&wait, result);
				count_job(kTextures / 2);
			}
		});
	std::thread reader([&] {
		hostCpu() = 3;
		char line[1024];
		for (uint32_t i = 0; i < 200; ++i) {
			switch_gpu_path_line(line, sizeof(line));
			switch_gpu_path_window(line, sizeof(line));
			switch_gpu_job_window(line, sizeof(line));
		}
	});
	for (auto &thread : drawing)
		thread.join();
	switch_gpu_timeline_close(apps[0].timeline);
	composite.join();
	reader.join();
	switch_gpu_timeline_close(apps[1].timeline);
	switch_gpu_timeline_close(compositor.timeline);
	for (auto *texture : textures)
		switch_gpu_backing_release(texture);
	switch_gpu_backing_release(target);
	for (int pass = 0; pass < 4; ++pass)
		switch_gpu_backing_retire();
	EXPECT_EQ(fenceHolds.load(), 0) << "a timeline reference was lost or dropped twice";
}
