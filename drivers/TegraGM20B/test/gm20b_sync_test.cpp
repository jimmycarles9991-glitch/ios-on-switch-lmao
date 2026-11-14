// SPDX-License-Identifier: MIT
// tegra_gm20b_sync_for_cpu (gpu_sync.c) on threads: a CPU sync sleeps with no lock held, and one that starts while
// another sleeps waits only for the work submitted before it.
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include "drivers/TegraGM20B/test/gm20b_sync_stubs.h"

struct fences_state tgm_fence_pages;
struct gpu_state tgm_gpu;
struct list_head tgm_live_chans = {&tgm_live_chans, &tgm_live_chans};
struct mutex tgm_live_lock;

namespace
{
using namespace std::chrono_literals;

std::mutex wakeLock;
std::condition_variable woken;
std::atomic<u32> wakeSeq;
thread_local int held;
std::atomic<int> sleptHolding, allocations, failAllocations, logs;

std::mutex &impl(struct mutex *m)
{
	static std::mutex registry;
	std::lock_guard guard(registry);
	if (!m->impl)
		m->impl = new std::mutex;
	return *static_cast<std::mutex *>(m->impl);
}

struct Channel {
	tgm_chan chan{};
	u32 fence = 0;

	explicit Channel(u64 id)
	{
		chan.id = id;
		chan.fence_cpu = &fence;
		mutex_lock(&tgm_live_lock);
		chan.live.prev = tgm_live_chans.prev;
		chan.live.next = &tgm_live_chans;
		tgm_live_chans.prev->next = &chan.live;
		tgm_live_chans.prev = &chan.live;
		mutex_unlock(&tgm_live_lock);
	}

	~Channel()
	{
		mutex_lock(&tgm_live_lock);
		chan.live.prev->next = chan.live.next;
		chan.live.next->prev = chan.live.prev;
		mutex_unlock(&tgm_live_lock);
	}

	void submit()
	{
		mutex_lock(&tgm_live_lock);
		WRITE_ONCE(chan.submitted, chan.submitted + 1);
		mutex_unlock(&tgm_live_lock);
	}

	void complete(u32 seqno)
	{
		__atomic_store_n(&fence, seqno, __ATOMIC_RELEASE);
		{
			std::lock_guard guard(wakeLock);
			++wakeSeq;
		}
		woken.notify_all();
	}
};

std::future<int> syncInBackground()
{
	return std::async(std::launch::async, [] {
		return tegra_gm20b_sync_for_cpu(nullptr, 0, 5000000000ULL);
	});
}

class Sync : public testing::Test
{
protected:
	void SetUp() override
	{
		sleptHolding = 0;
		allocations = 0;
		failAllocations = 0;
		logs = 0;
	}
};
}  // namespace

extern "C" {
// The kernel mutex's stand-in locks and unlocks in separate calls, which thread-safety analysis cannot pair.
__attribute__((no_thread_safety_analysis)) void mutex_lock(struct mutex *m)
{
	impl(m).lock();
	++held;
}

__attribute__((no_thread_safety_analysis)) void mutex_unlock(struct mutex *m)
{
	--held;
	impl(m).unlock();
}

void *kmalloc_array(size_t n, size_t size, gfp_t)
{
	if (failAllocations) {
		--failAllocations;
		return nullptr;
	}
	++allocations;
	return std::malloc(n * size);
}

void kfree(const void *p)
{
	std::free(const_cast<void *>(p));
}

void nvxnu_log(const char *, ...)
{
	++logs;
}

void switch_sync_push(struct nvif_push *)
{
}

u32 nvxnu_wake_seq(void)
{
	return wakeSeq.load(std::memory_order_acquire);
}

u64 nvxnu_nanotime(void)
{
	return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
			   std::chrono::steady_clock::now().time_since_epoch())
			   .count());
}

int nvxnu_idle(u32 seq, u64 deadline_ns)
{
	if (held)
		++sleptHolding;
	const auto deadline =
		std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline_ns));
	std::unique_lock guard(wakeLock);
	return woken.wait_until(guard, deadline,
				[seq] {
					return wakeSeq.load() != seq;
				})
		       ? 1
		       : 0;
}

void nvxnu_wake_due(u32, const char *, u32)
{
	ADD_FAILURE() << "a sync's wake was due";
}

void nvxnu_dcache_clean_inval(const void *, u64)
{
}

void nvxnu_dcache_clean_inval_pages(const u64 *, u32)
{
}

void tgm_chan_need_id(u64, u32)
{
}

void tgm_chan_need_uid(uint32_t, uint32_t)
{
}

int tgm_chan_announced_id(u64, u32)
{
	return 0;
}

int tgm_chan_announced_uid(u32, u32)
{
	return 0;
}

void tgm_chan_wanted(u32, u32)
{
}

void tgm_stats_unlocked(enum tgm_unlocked, uint64_t)
{
}

uint32_t tgm_waiter_seq(uint32_t)
{
	return nvxnu_wake_seq();
}

int tgm_waiter_idle(uint32_t, uint32_t seq, uint64_t deadline_ns)
{
	return nvxnu_idle(seq, deadline_ns);
}

void tgm_waiter_unwoken(uint32_t, uint32_t, uint32_t)
{
	ADD_FAILURE() << "a sync's wake was due";
}

enum tgm_hang tgm_hang_recover(struct tgm_chan *, const char *, u32, u64)
{
	return TGM_HANG_PROGRESSING;
}

void tgm_chan_hung(struct tgm_chan *, const char *, uint32_t, uint64_t)
{
	std::abort();
}

void tgm_hold_need(struct nvkm_device *, unsigned int)
{
}

int vm_imports(struct tgm_vm *, u64)
{
	return 0;
}
}

TEST_F(Sync, ASyncSleepsForTheGpuWithNoLockHeld)
{
	Channel a(1);
	a.submit();
	auto sync = syncInBackground();
	std::this_thread::sleep_for(50ms);
	a.complete(1);
	EXPECT_EQ(sync.get(), 0);
	// base 1: sync_lock across every sleep
	EXPECT_EQ(sleptHolding.load(), 0);
}

TEST_F(Sync, ASyncThatStartsWhileAnotherSleepsWaitsOnlyForWorkSubmittedBeforeIt)
{
	Channel a(2);
	a.submit();
	auto first = syncInBackground();
	std::this_thread::sleep_for(50ms);
	auto second = syncInBackground();
	std::this_thread::sleep_for(100ms);
	a.submit();
	a.complete(1);
	ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
	EXPECT_EQ(first.get(), 0);
	// base 1: the second sync took its snapshot only once the first let sync_lock go, and waited for seqno 2 too
	const int waitedForLaterWork =
		second.wait_for(300ms) == std::future_status::timeout ? 1 : 0;
	EXPECT_EQ(waitedForLaterWork, 0);
	a.complete(2);
	EXPECT_EQ(second.get(), 0);
}

TEST_F(Sync, NoChannelMeansNoWaitAndNoAllocation)
{
	EXPECT_EQ(tegra_gm20b_sync_for_cpu(nullptr, 0, 1000000), 0);
	EXPECT_EQ(allocations.load(), 0);
}

TEST_F(Sync, ASnapshotThatCannotBeAllocatedFailsThatSyncWithOneLine)
{
	Channel a(3);
	a.submit();
	failAllocations = 1;
	EXPECT_EQ(tegra_gm20b_sync_for_cpu(nullptr, 0, 1000000), -ENOMEM);
	EXPECT_EQ(logs.load(), 1);
	a.complete(1);
}
