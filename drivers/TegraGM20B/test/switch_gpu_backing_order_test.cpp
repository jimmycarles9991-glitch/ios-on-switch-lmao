// SPDX-License-Identifier: MIT
// The ownership order (SwitchGpuBackingOrder.cpp). A GM20B job holds the order alone, so its uses take no lock of their
// own, and two jobs on one backing still go one after the other. Each party that keeps a backing's lock claims the
// backing, and these tests stop that party mid-claim:
// - a CPU lock's cache pass, which a job on the same backing waits for, or it would publish a write the pass then
//   forgets;
// - another engine's job, kicked inside its transaction before its records go up;
// - the first clean of a CPU-dirty backing.
// None of these holds the order, so a job on another backing goes on meanwhile.
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuAdmission.h"
#include "drivers/TegraGM20B/SwitchGpuBackingInternal.h"
#include "drivers/TegraGM20B/SwitchGpuScanoutSnapshot.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kBeside = SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
constexpr uint64_t kBytes = 4 * 4096;

// The next cache pass of `operation` (SWITCH_GPU_CACHE_*) stops until let go.
class Stall
{
public:
	explicit Stall(int operation)
	{
		cache_callback = [this, operation](uint64_t, uint64_t, int done) {
			if (done != operation)
				return;
			std::unique_lock<std::mutex> hold(lock_);
			if (used_)
				return;
			used_ = entered_ = true;
			changed_.notify_all();
			changed_.wait(hold, [this] {
				return going_;
			});
		};
	}

	~Stall()
	{
		go();
		cache_callback = {};
	}

	bool entered()
	{
		std::unique_lock<std::mutex> hold(lock_);
		return changed_.wait_for(hold, std::chrono::seconds(2), [this] {
			return entered_;
		});
	}

	void go()
	{
		std::lock_guard<std::mutex> hold(lock_);
		going_ = true;
		changed_.notify_all();
	}

private:
	std::mutex lock_;
	std::condition_variable changed_;
	bool used_ = false, entered_ = false, going_ = false;
};

// A GM20B job as SubmitJob runs it: ordered, the connection's references holding its uses.
std::vector<switch_gpu_backing_token> job(switch_gpu_backing *b, uint32_t access,
					  const switch_gpu_backing_token &completion)
{
	switch_gpu_backing_use use = {b, access | kBeside, 0};
	switch_gpu_backing_transaction transaction = {};
	std::vector<switch_gpu_backing_token> dependencies(8);
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_begin_held(&use, 1, nullptr, 0, nullptr, 0, &completion,
						&transaction),
		  0);
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 8, &needed),
		  0);
	switch_gpu_backing_commit(&transaction);
	dependencies.resize(needed);
	return dependencies;
}

bool finishes(std::future<void> &done, std::chrono::milliseconds within)
{
	return done.wait_for(within) == std::future_status::ready;
}

uint32_t dirty(switch_gpu_backing *b)
{
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(b, &info);
	return info.dirty;
}

// A backing a finished GPU write left dirty in the GPU's caches, as a texture an app drew.
switch_gpu_backing *drawn(uint32_t queue)
{
	auto *b = backing(4);
	const auto write = token(queue, 1);
	EXPECT_EQ(publish(b, kWrite, write), 0);
	complete(write);
	return b;
}

TEST_F(Backing, TwoChannelsJobsOnOneBackingGoOneAfterTheOther)
{
	auto *b = drawn(3);
	std::promise<void> begun, written;
	std::thread writer([&] {
		switch_gpu_backing_use use = {b, kWrite | kBeside, 0};
		switch_gpu_backing_transaction transaction = {};
		const auto completion = token(5, 1);
		ASSERT_EQ(switch_gpu_backing_begin_held(&use, 1, nullptr, 0, nullptr, 0,
							&completion, &transaction),
			  0);
		begun.set_value();
		written.get_future().wait();
		switch_gpu_backing_commit(&transaction);
	});
	begun.get_future().wait();
	auto reading = std::async(std::launch::async, [&] {
		return job(b, kRead, token(6, 1));
	});
	EXPECT_EQ(reading.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout)
		<< "the reader went into the backing while the writer was publishing";
	written.set_value();
	writer.join();
	const auto dependencies = reading.get();
	ASSERT_EQ(dependencies.size(), 1u);
	EXPECT_EQ(dependencies[0].queue, 5u);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, ACpuLocksCachePassHoldsAJobOnItsBackingButNoOther)
{
	auto *a = drawn(3), *b = drawn(4);
	ASSERT_TRUE(dirty(a) & SWITCH_GPU_BACKING_GPU_DIRTY);
	Stall stall(SWITCH_GPU_CACHE_INVALIDATE);
	auto locking = std::async(std::launch::async, [&] {
		return switch_gpu_backing_cpu_acquire(a, 0, kBytes, kRead);
	});
	ASSERT_TRUE(stall.entered()) << "the CPU lock made no invalidate pass";
	auto other = std::async(std::launch::async, [&] {
		job(b, kRead, token(7, 1));
	});
	EXPECT_TRUE(finishes(other, std::chrono::seconds(1)))
		<< "a job on another backing waited for the CPU lock's cache pass";
	auto same = std::async(std::launch::async, [&] {
		job(a, kWrite, token(8, 1));
	});
	EXPECT_FALSE(finishes(same, std::chrono::milliseconds(100)))
		<< "a job wrote the backing while the CPU lock invalidated it";
	stall.go();
	EXPECT_EQ(locking.get(), 0);
	same.get();
	EXPECT_TRUE(dirty(a) & SWITCH_GPU_BACKING_GPU_DIRTY)
		<< "the job's write after the CPU lock's pass was forgotten: the next lock reads "
		   "stale lines";
	EXPECT_EQ(switch_gpu_backing_cpu_release(a, 0, kBytes, kRead), 0);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AnotherEnginesJobKickingInItsTransactionHoldsOnlyItsBackings)
{
	auto *a = drawn(3), *b = drawn(4);
	switch_gpu_backing_use use = {a, kWrite, 0};
	switch_gpu_backing_transaction kicking = {};
	const auto vic = token(160, 3, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &vic, &kicking), 0);
	auto other = std::async(std::launch::async, [&] {
		job(b, kRead, token(7, 1));
	});
	EXPECT_TRUE(finishes(other, std::chrono::seconds(1)))
		<< "a job on another backing waited for the engine's kick";
	auto same = std::async(std::launch::async, [&] {
		return job(a, kRead, token(8, 1));
	});
	EXPECT_EQ(same.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout)
		<< "a job read the backing before the engine's write was recorded";
	switch_gpu_backing_commit(&kicking);
	const auto dependencies = same.get();
	ASSERT_EQ(dependencies.size(), 1u) << "the reader does not wait for the engine's write";
	EXPECT_EQ(dependencies[0].engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, ACpuDirtyBackingsFirstCleanHoldsNoJobOnAnotherBacking)
{
	auto *fresh = backing(4), *b = drawn(4);
	ASSERT_TRUE(dirty(fresh) & SWITCH_GPU_BACKING_CPU_DIRTY);
	Stall stall(SWITCH_GPU_CACHE_CLEAN);
	auto cleaning = std::async(std::launch::async, [&] {
		job(fresh, kRead, token(7, 1));
	});
	ASSERT_TRUE(stall.entered()) << "the job made no clean pass";
	auto other = std::async(std::launch::async, [&] {
		job(b, kRead, token(8, 1));
	});
	EXPECT_TRUE(finishes(other, std::chrono::seconds(1)))
		<< "a job on another backing waited for the clean of a CPU-dirty one";
	stall.go();
	cleaning.get();
	EXPECT_FALSE(dirty(fresh) & SWITCH_GPU_BACKING_CPU_DIRTY);
	switch_gpu_backing_release(fresh);
	switch_gpu_backing_release(b);
}

// A job cleans a CPU writer's bytes with the order let go, then its guard refuses it: the next job cleans again what the
// writer wrote since.
TEST_F(Backing, AJobRefusedAfterItsCleanLeavesTheNextOneToCleanAgain)
{
	auto *b = drawn(3);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, kBytes, kWrite), 0);
	ASSERT_TRUE(dirty(b) & SWITCH_GPU_BACKING_CPU_DIRTY);
	alignas(8) static volatile uint64_t generation = 2;
	const switch_gpu_backing_guard guard = {b, &generation, 1};
	switch_gpu_backing_use use = {b, kRead | kBeside, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto refused = token(7, 1);
	cache.clear();
	EXPECT_EQ(switch_gpu_backing_begin_held(&use, 1, nullptr, 0, &guard, 1, &refused,
						&transaction),
		  SWITCH_GPU_BACKING_STALE);
	ASSERT_EQ(cache.size(), 1u) << "the refused job made no clean pass";
	cache.clear();
	job(b, kRead, token(7, 2));
	ASSERT_EQ(cache.size(), 1u)
		<< "the writer's bytes since the refused job's clean stay dirty";
	EXPECT_EQ(cache[0].offset, 0u);
	EXPECT_EQ(cache[0].bytes, kBytes);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, kBytes, kWrite), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, ACpuLockAsleepBehindACpuWriterHoldsNoJob)
{
	auto *a = drawn(3), *b = drawn(4);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(a, 0, kBytes, kWrite), 0);
	auto waiting = std::async(std::launch::async, [&] {
		return switch_gpu_backing_cpu_acquire(a, 0, kBytes, kRead);
	});
	EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
	auto other = std::async(std::launch::async, [&] {
		job(b, kRead, token(7, 1));
	});
	EXPECT_TRUE(finishes(other, std::chrono::seconds(1)));
	auto same = std::async(std::launch::async, [&] {
		job(a, kRead, token(8, 1));
	});
	EXPECT_TRUE(finishes(same, std::chrono::seconds(1)))
		<< "a job beside the CPU writer waited for the sleeping lock";
	EXPECT_EQ(switch_gpu_backing_cpu_release(a, 0, kBytes, kWrite), 0);
	EXPECT_EQ(waiting.get(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(a, 0, kBytes, kRead), 0);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

// Every party once, with the host IOLock recording which lock each takes while it holds others: no cycle.
TEST_F(Backing, EveryPartyTakesTheLocksInOneOrder)
{
	{
		std::lock_guard<std::mutex> hold(ioLockOrder().m);
		ioLockOrder().edges.clear();
	}
	ioLockOrder().recording = true;
	auto *a = drawn(3), *b = drawn(4), *fresh = backing(4);
	job(a, kRead, token(7, 1));
	job(fresh, kWrite, token(7, 2));
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, kBytes, kWrite), 0);
	auto waiting = std::async(std::launch::async, [&] {
		return switch_gpu_backing_cpu_acquire(b, 0, kBytes, kRead);
	});
	EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
	job(b, kRead, token(7, 3));
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, kBytes, kWrite), 0);
	EXPECT_EQ(waiting.get(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, kBytes, kRead), 0);
	SwitchGpuScanoutSnapshot shown = {};
	shown.done = [](void *, int) {};
	ASSERT_EQ(shown.begin(nullptr, 0), 0);
	ASSERT_EQ(shown.add(a), 0);
	SwitchGpuScanoutSnapshot::complete(&shown, 0);
	SwitchGpuAdmission admission;
	const switch_gpu_backing_use uses[2] = {{a, kRead, 0}, {b, kWrite, 0}};
	ASSERT_EQ(admission.begin(uses, 2, nullptr, 0, nullptr, 0), 0);
	admission.end();
	switch_gpu_backing_use engine = {a, kWrite, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto vic = token(160, 4, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(switch_gpu_backing_begin(&engine, 1, &vic, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(a, &info);
	EXPECT_EQ(switch_gpu_backing_find_pa(info.pages[0]), a);
	switch_gpu_backing_release(a);
	uint32_t engines = 0, owners = 0;
	EXPECT_EQ(switch_gpu_backing_pending(a, kWrite, &engines, &owners), 0);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
	switch_gpu_backing_release(fresh);
	for (auto &entry : timelines)
		entry.second.completed = UINT64_MAX >> 1;
	switch_gpu_backing_retire();
	ioLockOrder().recording = false;
	EXPECT_TRUE(ioLockOrderCycle().empty());
}
}  // namespace
