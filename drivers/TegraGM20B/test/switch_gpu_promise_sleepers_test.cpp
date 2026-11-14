// SPDX-License-Identifier: MIT
// The writer promises' sleepers list (SwitchGpuPromiseSleepers.cpp). From every IOLock the host model sees taken while
// others are held: a CPU reader asleep behind a promise, the give-up's wake and the registry finding the backing again
// by its pages take no locks in a cycle (each edge alone deadlocked nothing, so no run of them hung). While the list is
// busy, a reader that does not wait for the promise misses no wake, and one that does parks on the promises pending
// when it goes to sleep.
#include <set>
#include <string>

#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuBackingInternal.h"
#include "drivers/TegraGM20B/SwitchGpuWriterPromise.h"

namespace
{
constexpr uint32_t kOffset = 2048;
uint32_t fence[1024];
}  // namespace

extern "C" volatile uint32_t *tgm_fence_word(uint32_t offset)
{
	return offset < sizeof(fence) && !(offset & 3) ? &fence[offset / 4] : nullptr;
}

extern "C" void switch_gpu_fence_hold(const volatile uint32_t *)
{
}

extern "C" void switch_gpu_fence_put(const volatile uint32_t *)
{
}

extern "C" int tgm_promise_open(void)
{
	return 0;
}

namespace
{
std::mutex parkLock;
std::set<uint64_t> parked;  // offset << 32 | value of each promise a CPU reader parked on
}  // namespace

extern "C" int tgm_promise_park(uint32_t offset, uint32_t value, const char *, const char *,
				const void *, int)
{
	std::lock_guard<std::mutex> hold(parkLock);
	parked.insert(uint64_t(offset) << 32 | value);
	return 1;
}

extern "C" void tgm_promise_leave(uint32_t, uint32_t, int)
{
}

namespace
{
class Recording
{
public:
	Recording()
	{
		std::lock_guard<std::mutex> hold(ioLockOrder().m);
		ioLockOrder().edges.clear();
		ioLockOrder().recording = true;
	}

	~Recording()
	{
		ioLockOrder().recording = false;
	}
};

std::string describe(const std::vector<const IOLock *> &cycle, const switch_gpu_backing *b)
{
	std::string out;
	for (const IOLock *lock : cycle)
		out += lock == b->lock ? "the backing's lock -> " : "another lock -> ";
	return out + "the first again";
}

bool sleeping(uint32_t count)
{
	for (int i = 0; i < 2000; ++i) {
		if (__atomic_load_n(&switch_gpu_cpu_sleepers, __ATOMIC_ACQUIRE) == count)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

TEST_F(Backing, APromiseSleeperItsWakeAndTheRegistryTakeNoLocksInACycle)
{
	Recording recording;
	auto *b = backing();
	const auto drawn = token(3, 1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, drawn), 0);
	complete(drawn);
	fence[kOffset / 4] = 0;
	const switch_gpu_promise_ref ref = {tgm_fence_word(kOffset), kOffset, 1};
	ASSERT_EQ(switch_gpu_backing_promise_add(b, &ref), 0);
	std::atomic<int> locked{1};
	std::thread reader([&] {
		locked = switch_gpu_backing_cpu_acquire(b, 0, 4096, SWITCH_GPU_BACKING_READ);
	});
	ASSERT_TRUE(sleeping(1)) << "the CPU lock never went to sleep behind the promise";
	switch_gpu_backing_promise_sleepers_wake();
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(b, &info);
	ASSERT_EQ(switch_gpu_backing_find_pa(info.pages[0]), b);
	switch_gpu_backing_promise_drop(b, kOffset);
	reader.join();
	EXPECT_EQ(locked.load(), 0);
	const auto cycle = ioLockOrderCycle();
	EXPECT_TRUE(cycle.empty()) << describe(cycle, b);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_release(b);
}

bool settled(const std::atomic<int> &result)
{
	for (int i = 0; i < 500 && result.load() == 1; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	return result.load() != 1;
}

// A display source's waiting snapshot orders behind a pending promise on the GPU and sleeps for a CPU writer only, so
// the writer leaving wakes it. Here the sleepers list is held by a give-up wake stuck on another listed backing's lock:
// a snapshot that went to list itself would leave its own lock while the writer leaves, miss the wake, and sleep on
// until the promise ends.
TEST_F(Backing, AWaitingSnapshotBehindAPromiseWakesWhenTheCpuWriterLeaves)
{
	constexpr uint32_t kOther = kOffset + 4;
	auto *listed = backing(), *b = backing();
	fence[kOther / 4] = 0;
	const switch_gpu_promise_ref other = {tgm_fence_word(kOther), kOther, 1};
	ASSERT_EQ(switch_gpu_backing_promise_add(listed, &other), 0);
	std::atomic<int> locked{1};
	std::thread reader([&] {
		locked = switch_gpu_backing_cpu_acquire(listed, 0, 4096, SWITCH_GPU_BACKING_READ);
	});
	ASSERT_TRUE(sleeping(1));
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	fence[kOffset / 4] = 0;
	const switch_gpu_promise_ref ref = {tgm_fence_word(kOffset), kOffset, 1};
	ASSERT_EQ(switch_gpu_backing_promise_add(b, &ref), 0);
	IOLockLock(listed->lock);
	std::thread wake([] {
		switch_gpu_backing_promise_sleepers_wake();
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	std::atomic<int> result{1};
	switch_gpu_backing_token producer = {};
	switch_gpu_cpu_lease *lease = nullptr;
	std::thread source([&] {
		result = switch_gpu_backing_snapshot_read_wait(b, &producer, &lease);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, SWITCH_GPU_BACKING_WRITE), 0);
	IOLockUnlock(listed->lock);
	wake.join();
	EXPECT_TRUE(settled(result)) << "the snapshot slept through the CPU writer's leaving";
	switch_gpu_backing_promise_drop(b, kOffset);
	source.join();
	EXPECT_EQ(result.load(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_promise_drop(listed, kOther);
	reader.join();
	EXPECT_EQ(locked.load(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(listed, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(listed);
	switch_gpu_backing_release(b);
}

// A writer that keeps promising makes its next promise and keeps the last one while a CPU lock waits for the list held
// by a give-up wake stuck on another listed backing: the lock sleeps behind the next promise, and parks on it, since
// the give-up knows a promise only through a park.
TEST_F(Backing, ACpuLockParksOnThePromiseMadeWhileItWaitedForTheList)
{
	constexpr uint32_t kOther = kOffset + 8, kNext = kOffset + 4;
	{
		std::lock_guard<std::mutex> hold(parkLock);
		parked.clear();
	}
	auto *listed = backing(), *b = backing();
	fence[kOther / 4] = fence[kOffset / 4] = fence[kNext / 4] = 0;
	const switch_gpu_promise_ref other = {tgm_fence_word(kOther), kOther, 1};
	ASSERT_EQ(switch_gpu_backing_promise_add(listed, &other), 0);
	std::atomic<int> locked{1}, waited{1};
	std::thread reader([&] {
		locked = switch_gpu_backing_cpu_acquire(listed, 0, 4096, SWITCH_GPU_BACKING_READ);
	});
	ASSERT_TRUE(sleeping(1));
	const switch_gpu_promise_ref kept = {tgm_fence_word(kOffset), kOffset, 1};
	ASSERT_EQ(switch_gpu_backing_promise_add(b, &kept), 0);
	IOLockLock(listed->lock);
	std::thread wake([] {
		switch_gpu_backing_promise_sleepers_wake();
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	std::thread lock([&] {
		waited = switch_gpu_backing_cpu_acquire(b, 0, 4096, SWITCH_GPU_BACKING_READ);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const switch_gpu_promise_ref next = {tgm_fence_word(kNext), kNext, 1};
	ASSERT_EQ(switch_gpu_backing_promise_add(b, &next), 0);
	fence[kOffset / 4] = 1;
	switch_gpu_backing_promise_drop(b, kOffset);
	IOLockUnlock(listed->lock);
	wake.join();
	ASSERT_TRUE(sleeping(2)) << "the CPU lock did not go to sleep behind the next promise";
	{
		std::lock_guard<std::mutex> hold(parkLock);
		EXPECT_TRUE(parked.count(uint64_t(kNext) << 32 | 1))
			<< "asleep behind a promise it never parked on";
	}
	EXPECT_EQ(waited.load(), 1);
	fence[kNext / 4] = 1;
	switch_gpu_backing_promise_drop(b, kNext);
	lock.join();
	EXPECT_EQ(waited.load(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_promise_drop(listed, kOther);
	reader.join();
	EXPECT_EQ(locked.load(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(listed, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(listed);
	switch_gpu_backing_release(b);
}
}  // namespace
