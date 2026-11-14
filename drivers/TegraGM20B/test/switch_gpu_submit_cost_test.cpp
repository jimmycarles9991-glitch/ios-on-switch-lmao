// SPDX-License-Identifier: MIT
// A compositor's SubmitJob over N shared uses as the ownership layer runs it (begin, write ranges, dependencies, promise
// dependencies, commit), on the real backing registry and GM20B timeline engine among a device's worth of channels: the
// IOLocks it takes besides its uses' own locks (the ownership order among them), and its time, per job.
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>
#include "switch_gpu_submit_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuScanoutSnapshot.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"

namespace
{
// A device's channels: the compositor's, an app's for each of the textures' producers, and the rest of the boot's
// (keyboards, widgets, daemons), each a timeline in the engine's list.
constexpr uint32_t kApps = 12, kOtherChannels = 48;

struct Channel {
	volatile uint32_t fence = 0;
	switch_gpu_timeline *timeline = nullptr;

	switch_gpu_backing_token token(uint32_t value)
	{
		switch_gpu_backing_token made = {};
		switch_gpu_timeline_token(timeline, value, &made);
		return made;
	}
};

struct Cost {
	uint64_t global, own;
	uint32_t dependencies;
};

uint64_t next_page = 0x900000000ull;

switch_gpu_backing *surface(uint32_t pages)
{
	std::vector<uint64_t> list;
	for (uint32_t i = 0; i < pages; ++i)
		list.push_back(next_page += 4096);
	next_page += 1u << 20;
	return switch_gpu_backing_register(list.data(), pages, 0, nullptr, nullptr);
}

class SubmitCost : public testing::Test
{
protected:
	std::vector<Channel> channels = std::vector<Channel>(1 + kApps + kOtherChannels);
	Channel &compositor = channels[0];
	std::vector<switch_gpu_backing *> textures;
	switch_gpu_backing *target = nullptr;
	std::vector<switch_gpu_backing_use> uses;
	std::vector<switch_gpu_backing_token> dependencies =
		std::vector<switch_gpu_backing_token>(TegraGM20BMaxJobDependencies);
	uint32_t frame = 0;

	void SetUp() override
	{
		ASSERT_EQ(switch_gpu_timeline_init(), 0);
		static uint64_t uid;
		for (auto &channel : channels)
			channel.timeline = switch_gpu_timeline_create(++uid, &channel.fence);
	}

	void TearDown() override
	{
		for (auto &channel : channels)
			switch_gpu_timeline_close(channel.timeline);
		for (auto *texture : textures)
			switch_gpu_backing_release(texture);
		switch_gpu_backing_release(target);
		for (int pass = 0; pass < 4; ++pass)
			switch_gpu_backing_retire();
		EXPECT_EQ(fenceHolds.load(), 0) << "a timeline reference was lost or dropped twice";
	}

	Channel &app(uint32_t i)
	{
		return channels[1 + i % kApps];
	}

	// An app draws a texture, and the GPU finishes it.
	void draw()
	{
		if (!target)
			target = surface(4);
		const uint32_t i = uint32_t(textures.size());
		textures.push_back(surface(16));
		switch_gpu_backing_use use = {textures.back(), SWITCH_GPU_BACKING_WRITE, 0};
		const switch_gpu_backing_token drawn = app(i).token(1);
		switch_gpu_backing_transaction transaction = {};
		ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &drawn, &transaction), 0);
		switch_gpu_backing_commit(&transaction);
		app(i).fence = 1;
	}

	// The compositor composites the apps' textures a few frames behind the GPU.
	void scene(uint32_t count)
	{
		while (textures.size() < count)
			draw();
		for (int warm = 0; warm < 4; ++warm)
			composite();
	}

	// One compositor job as SubmitJob runs it once its uses are resolved; the previous frame is still on the GPU.
	Cost composite()
	{
		uses.clear();
		for (auto *texture : textures)
			uses.push_back(
				{texture,
				 SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0});
		uses.push_back({target,
				SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0});
		switch_gpu_backing_write_range write = {target, 0, 0};
		const switch_gpu_backing_token completion = compositor.token(++frame);
		const uint64_t own = own_takes(), all = ioLockTakes().load();
		switch_gpu_backing_transaction transaction = {};
		uint32_t needed = 0;
		EXPECT_EQ(switch_gpu_backing_begin_held(uses.data(), uint32_t(uses.size()), nullptr,
							0, nullptr, 0, &completion, &transaction),
			  0);
		EXPECT_EQ(switch_gpu_backing_write_ranges(&transaction, &write, 1), 0);
		EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(),
							  TegraGM20BMaxJobUses, &needed),
			  0);
		EXPECT_EQ(switch_gpu_backing_promise_dependencies(&transaction, dependencies.data(),
								  TegraGM20BMaxJobDependencies,
								  &needed, 0, 0),
			  0);
		switch_gpu_backing_commit(&transaction);
		const uint64_t taken = ioLockTakes().load() - all, ownTaken = own_takes() - own;
		compositor.fence = frame - 1;
		return {taken - ownTaken, ownTaken, needed};
	}

	struct Frame {
		uint64_t job, jobOther, display, displayOther;
	};

	uint64_t ownership_takes() const
	{
		return own_takes() + switch_gpu_backing_order()->takes;
	}

	// One compositor job, then one display period on its target: the swap's snapshot, and the interrupt pass's release
	// of its lease at the frame's end.
	Frame frame_takes()
	{
		Frame frame = {};
		uint64_t all = ioLockTakes().load(), owned = ownership_takes();
		composite();
		frame.job = ownership_takes() - owned;
		frame.jobOther = ioLockTakes().load() - all - frame.job;
		SwitchGpuScanoutSnapshot shown = {};
		shown.done = [](void *, int) {};
		all = ioLockTakes().load();
		owned = ownership_takes();
		EXPECT_EQ(shown.begin(nullptr, 0), 0);
		EXPECT_EQ(shown.add(target), 0);
		SwitchGpuScanoutSnapshot::complete(&shown, 0);
		frame.display = ownership_takes() - owned;
		frame.displayOther = ioLockTakes().load() - all - frame.display;
		return frame;
	}

	uint64_t own_takes() const
	{
		uint64_t takes = target->lock->takes;
		for (const auto *texture : textures)
			takes += texture->lock->takes;
		return takes;
	}

	// What its uses' own locks cost a job alone: each taken and let go once
	double own_locks_us(uint32_t jobs)
	{
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t i = 0; i < jobs; ++i)
			for (auto *backing : textures) {
				IOLockLock(backing->lock);
				IOLockUnlock(backing->lock);
			}
		const std::chrono::duration<double, std::micro> spent =
			std::chrono::steady_clock::now() - start;
		return spent.count() / jobs;
	}

	double mean_us(uint32_t jobs)
	{
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t i = 0; i < jobs; ++i)
			composite();
		const std::chrono::duration<double, std::micro> spent =
			std::chrono::steady_clock::now() - start;
		return spent.count() / jobs;
	}
};

// A scroll's 6 shared uses and a scale animation's 60 (boot 21: 55-61) cost the same locks: the ownership order once,
// none of the uses' own while nothing else claims them, and no timeline's once the compositor has recorded on each.
TEST_F(SubmitCost, GlobalLocksAJobDoNotGrowWithItsSharedUses)
{
	scene(6);
	const Cost scroll = composite();
	std::printf(
		"6 shared uses: %llu global IOLock takes, %llu of the uses' own, %.2f us a job\n",
		static_cast<unsigned long long>(scroll.global),
		static_cast<unsigned long long>(scroll.own), mean_us(200));
	scene(60);
	const Cost scale = composite();
	std::printf(
		"60 shared uses: %llu global IOLock takes, %llu of the uses' own, %.2f us a job, "
		"%.2f us of it the uses' own locks\n",
		static_cast<unsigned long long>(scale.global),
		static_cast<unsigned long long>(scale.own), mean_us(200), own_locks_us(200));
	EXPECT_EQ(scale.global, scroll.global);
	EXPECT_EQ(scale.global, 1u) << "the ownership order, once";
	EXPECT_EQ(scroll.own, 0u);
	EXPECT_EQ(scale.own, 0u);
	EXPECT_EQ(scale.dependencies, 0u);
}

// A record a job replaces on its own timeline passes its reference on: the compositor's timeline holds one a record
// (60 reads, the target's write) and its own, frame after frame.
TEST_F(SubmitCost, ReplacedRecordsKeepTheTimelineReferencesLevel)
{
	scene(60);
	const uint32_t held = compositor.timeline->refs;
	for (int i = 0; i < 10; ++i)
		composite();
	EXPECT_EQ(compositor.timeline->refs, held);
	EXPECT_EQ(held, 1u + 61u);
}

// A texture newly in the scene costs the first job that reads it the timelines' locks twice: the reference its new record
// holds, and the app's finished first write dropped as the texture's host acquire. No job after takes either.
TEST_F(SubmitCost, ANewTextureCostsOnlyItsFirstJobTheTimelineLocks)
{
	scene(60);
	draw();
	EXPECT_EQ(composite().global, 3u);
	EXPECT_EQ(composite().global, 1u);
}

// An app still drawing one texture: the job waits for that app's channel, found in the ownership order.
TEST_F(SubmitCost, PendingProducerIsADependencyWithoutAGlobalLockPerUse)
{
	scene(60);
	switch_gpu_backing_use use = {textures[7], SWITCH_GPU_BACKING_WRITE, 0};
	const switch_gpu_backing_token drawing = app(7).token(2);
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &drawing, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
	Cost cost = composite();
	EXPECT_EQ(cost.dependencies, 1u);
	EXPECT_EQ(cost.global, 1u);
	app(7).fence = 2;
	cost = composite();
	EXPECT_EQ(cost.dependencies, 0u);
}

// Every IOLock a compositor frame takes for its ownership, the order and the backings' own: its job, and a display
// period on its target (the swap's snapshot, and the interrupt pass's release of its lease). A scroll and a scale
// animation cost the same. The registry's and the timelines' locks (the snapshot's reference and its producer's) are
// counted apart.
TEST_F(SubmitCost, AFrameTakesTheSameOwnershipLocksForAnyNumberOfSharedUses)
{
	scene(6);
	const Frame scroll = frame_takes();
	scene(60);
	const Frame scale = frame_takes();
	std::printf(
		"IOLock takes a frame (ownership + other): scroll job %llu + %llu, display %llu + "
		"%llu; scale job %llu + %llu, display %llu + %llu\n",
		static_cast<unsigned long long>(scroll.job),
		static_cast<unsigned long long>(scroll.jobOther),
		static_cast<unsigned long long>(scroll.display),
		static_cast<unsigned long long>(scroll.displayOther),
		static_cast<unsigned long long>(scale.job),
		static_cast<unsigned long long>(scale.jobOther),
		static_cast<unsigned long long>(scale.display),
		static_cast<unsigned long long>(scale.displayOther));
	EXPECT_EQ(scroll.job, 1u);
	EXPECT_EQ(scale.job, 1u);
	EXPECT_EQ(scroll.display, 2u);
	EXPECT_EQ(scale.display, 2u);
}

// An app holding a CPU write lock on one texture leaves it CPU-dirty: the job takes that texture's lock alone besides the
// order, which it lets go while it cleans the texture (taken three times), and none of the other 59.
TEST_F(SubmitCost, ACpuWriteLockedTextureCostsTheJobItsLockAlone)
{
	scene(60);
	constexpr uint64_t kBytes = 16 * 4096;
	compositor.fence = frame;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(textures[3], 0, kBytes, SWITCH_GPU_BACKING_WRITE),
		  0);
	const uint64_t owned = ownership_takes(), own = own_takes();
	composite();
	EXPECT_EQ(own_takes() - own, 1u);
	EXPECT_EQ(ownership_takes() - owned, 4u);
	compositor.fence = frame;
	EXPECT_EQ(switch_gpu_backing_cpu_release(textures[3], 0, kBytes, SWITCH_GPU_BACKING_WRITE),
		  0);
	const uint64_t after = ownership_takes();
	composite();
	EXPECT_EQ(ownership_takes() - after, 1u);
}

// A job over its resource set alone, no shared use: no ownership lock.
TEST_F(SubmitCost, AJobWithNoSharedUseTakesNoOwnershipLock)
{
	scene(6);
	const switch_gpu_backing_token completion = compositor.token(++frame);
	const uint64_t owned = ownership_takes();
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin_held(nullptr, 0, nullptr, 0, nullptr, 0, &completion,
						&transaction),
		  0);
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(),
						  TegraGM20BMaxJobUses, &needed),
		  0);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(ownership_takes() - owned, 0u);
	compositor.fence = frame;
}
}  // namespace
