// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <future>
#include <map>
#include <stdexcept>
#include <utility>
#include "switch_gpu_cached_zero_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuCachedZero.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.h"
#include "gm20b_ce_model.h"

struct tgm_vm {};

struct tgm_mem {
	const uint64_t *pages;
	uint32_t count;
};

struct switch_gpu_mapping {
	uint64_t address, bytes;
};

struct tgm_chan {
	uint32_t submitted = 0, completed = 0;
	bool failed = false;
};

std::vector<thread_call_t> switch_zero_calls;
std::atomic<unsigned> switch_zero_sleepers{0};
uint64_t switch_zero_now = 1000000000;

namespace
{
std::array<uint32_t, 1024> words;
volatile uint32_t fence;
tgm_vm space;
tgm_chan gpu;
std::map<uint64_t, std::vector<uint8_t> *> buffers;
Memory memory;
std::map<uint32_t, std::pair<uint64_t, uint32_t>> pending;  // seqno -> push GPU address, dwords
unsigned maps, unmaps, cacheOps, execs, nextError, progressChecks;
int mapError;
bool early;
// tgm_chan_killed landing at the fence read: the error first, then the fence over the abandoned scrub.
bool killAtFenceRead;
bool pushReadOnly, pushVolatile, destinationReadOnly, destinationVolatile;
switch_gpu_mapping pushWindow;
void (*fenceCallback)(void *);
void *fenceContext;

void pump()
{
	for (auto call : switch_zero_calls)
		if (call->pending.exchange(false))
			call->fn(call->arg, nullptr);
}

void finish(bool failed = false, bool emulate = true)
{
	if (!failed && emulate)
		for (const auto &[sequence, push] : pending) {
			CopyEngine ce(memory);
			ce.runAt(push.first, push.second);
			EXPECT_FALSE(ce.fault);
		}
	pending.clear();
	gpu.completed = gpu.submitted;
	gpu.failed = failed;
	if (fenceCallback)
		fenceCallback(fenceContext);
	pump();
}

struct Allocation {
	std::vector<uint64_t> pages;
	std::vector<uint8_t> bytes;
	switch_gpu_cached_mapping *mapping = nullptr;
	unsigned callbacks = 0;
	int result = 123;

	explicit Allocation(uint32_t count = 4) : bytes(size_t(count) * 4096, 0xa7)
	{
		static uint64_t base = 0x10000000;
		base += 1ULL << 28;
		for (uint32_t i = 0; i < count; ++i)
			pages.push_back(base + uint64_t(i) * 8192);
		buffers[base] = &bytes;
	}

	~Allocation()
	{
		switch_gpu_cached_mapping_drop(&mapping);
		buffers.erase(pages.front());
	}

	static void complete(void *ctx, int error)
	{
		auto &a = *static_cast<Allocation *>(ctx);
		++a.callbacks;
		a.result = error;
	}

	int submit()
	{
		return switch_gpu_cached_zero(pages.data(), uint32_t(pages.size()), bytes.data(),
					      &mapping, complete, this);
	}
};

class CachedZero : public testing::Test
{
	void SetUp() override
	{
		nextError = 0;
		mapError = 0;
		early = false;
		killAtFenceRead = false;
	}

	void TearDown() override
	{
		finish();
		EXPECT_TRUE(pending.empty());
	}
};
}

void switch_zero_cache(const char *, uintptr_t)
{
	++cacheOps;
}

[[noreturn]] void panic(const char *, ...)
{
	throw std::runtime_error("panic");
}

extern "C" void *tgm_pool_alloc(uint64_t bytes, uint64_t, uint64_t *pa)
{
	EXPECT_EQ(bytes, 4096u);
	*pa = 0x100000;
	return words.data();
}

extern "C" int tgm_fence_kernel(unsigned kind, volatile uint32_t **cpu, uint64_t *page,
				uint32_t *offset)
{
	EXPECT_EQ(kind, unsigned(TGM_FENCE_KERNEL_CACHED_ZERO));
	*cpu = &fence;
	*page = 0x200000;
	*offset = 128;
	return 0;
}

extern "C" int tgm_vm_new(tgm_vm **out)
{
	*out = &space;
	return 0;
}

extern "C" void tgm_vm_del(tgm_vm **out)
{
	*out = nullptr;
}

extern "C" void tgm_pool_free(void *, uint64_t, uint64_t)
{
}

extern "C" void tgm_vm_claim(tgm_vm *, const void *)
{
}

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *pages, uint32_t count, tgm_mem **out)
{
	*out = new tgm_mem{pages, count};
	return 0;
}

extern "C" void tgm_mem_del(tgm_mem **value)
{
	delete *value;
	*value = nullptr;
}

extern "C" int tgm_vm_map_ordered(tgm_vm *, uint64_t va, uint64_t bytes, tgm_mem *mem,
				  uint64_t offset, uint8_t, int readOnly, uint8_t *)
{
	EXPECT_EQ(mem->pages[0], 0x100000u);
	EXPECT_EQ(offset, 0u);
	EXPECT_EQ(bytes, sizeof(words));
	pushReadOnly = readOnly;
	memory.map(va, reinterpret_cast<uint8_t *>(words.data()), bytes, readOnly != 0);
	return 0;
}

extern "C" int tgm_vm_kmap(tgm_vm *, tgm_mem *, uint64_t, uint64_t *va)
{
	*va = 0x800000;
	return 0;
}

extern "C" int switch_gpu_vm_map_volatile(tgm_vm *, uint64_t va, uint64_t bytes, tgm_mem *mem,
					  uint64_t offset, uint8_t kind, int readOnly, int vol,
					  switch_gpu_mapping **out)
{
	if (mem->pages[0] == 0x100000u) {
		EXPECT_EQ(offset, 0u);
		EXPECT_EQ(bytes, sizeof(words));
		pushReadOnly = readOnly;
		pushVolatile = vol;
		memory.map(va, reinterpret_cast<uint8_t *>(words.data()), bytes, readOnly != 0,
			   !vol);
		pushWindow = {va, bytes};
		*out = &pushWindow;
		return 0;
	}
	if (mapError)
		return mapError;
	EXPECT_EQ(kind, 0);
	destinationReadOnly = readOnly;
	destinationVolatile = vol;
	++maps;
	memory.map(va, *buffers.at(mem->pages[0]), readOnly != 0);
	*out = new switch_gpu_mapping{va, mem->count * 4096ull};
	return 0;
}

extern "C" int switch_gpu_vm_map(tgm_vm *vm, uint64_t va, uint64_t bytes, tgm_mem *mem,
				 uint64_t offset, uint8_t kind, int readOnly,
				 switch_gpu_mapping **out)
{
	return switch_gpu_vm_map_volatile(vm, va, bytes, mem, offset, kind, readOnly, 0, out);
}

extern "C" void switch_gpu_vm_unmap(tgm_vm *, switch_gpu_mapping **mapping)
{
	if (*mapping == &pushWindow) {
		memory.unmap(pushWindow.address, pushWindow.bytes);
		*mapping = nullptr;
	}
	if (*mapping) {
		++unmaps;
		memory.unmap((*mapping)->address, (*mapping)->bytes);
		delete *mapping;
		*mapping = nullptr;
	}
}

extern "C" int tgm_chan_new_copy(tgm_vm *, const void *, volatile uint32_t *, uint64_t,
				 const char *, int, tgm_chan **out)
{
	gpu = {};
	*out = &gpu;
	return 0;
}

extern "C" void tgm_chan_del(tgm_chan **chan)
{
	*chan = nullptr;
}

extern "C" void tgm_chan_on_fence(tgm_chan *, void (*fn)(void *), void *arg)
{
	fenceCallback = fn;
	fenceContext = arg;
}

extern "C" uint32_t tgm_chan_error(tgm_chan *chan)
{
	return chan->failed ? 1 : 0;
}

extern "C" uint32_t tgm_chan_completed(tgm_chan *chan)
{
	if (killAtFenceRead) {
		killAtFenceRead = false;
		chan->failed = true;
		pending.clear();
		chan->completed = chan->submitted;
	}
	return chan->completed;
}

extern "C" uint32_t tgm_chan_settled(tgm_chan *chan, int *failed)
{
	const uint32_t done = tgm_chan_completed(chan);
	*failed = chan->failed;
	return done;
}

uint64_t nextCheckNs = 2000000000;

extern "C" uint64_t tgm_chan_check_progress(tgm_chan *, const char *, uint32_t)
{
	++progressChecks;
	return nextCheckNs;
}

extern "C" int tgm_chan_exec(tgm_chan *, const tgm_push *push, uint32_t count, uint32_t *seq,
			     const tgm_prof *)
{
	EXPECT_EQ(count, 1u);
	EXPECT_LT(push->dwords, 32u);
	++execs;
	if (nextError) {
		const int result = -int(nextError);
		nextError = 0;
		return result;
	}
	pending[++gpu.submitted] = {push->va, push->dwords};
	*seq = gpu.submitted;
	if (early) {
		gpu.completed = gpu.submitted;
		fenceCallback(fenceContext);
	}
	return 0;
}

namespace
{
TEST_F(CachedZero, ExactClearBecomesReadyOnlyAfterExternalFenceAndDeferredCallback)
{
	Allocation a;
	const auto before = a.bytes;
	const unsigned clean = cacheOps;
	ASSERT_EQ(a.submit(), 0);
	EXPECT_TRUE(pushReadOnly);
	EXPECT_TRUE(pushVolatile);
	EXPECT_FALSE(destinationReadOnly);
	EXPECT_TRUE(destinationVolatile);
	EXPECT_EQ(a.callbacks, 0u);
	EXPECT_EQ(a.bytes, before);
	EXPECT_GT(cacheOps, clean);
	finish();
	EXPECT_EQ(a.callbacks, 1u);
	EXPECT_EQ(a.result, 0);
	EXPECT_TRUE(std::all_of(a.bytes.begin(), a.bytes.end(), [](auto b) {
		return b == 0;
	}));
}

TEST_F(CachedZero, WarmReuseKeepsPhysicalMappingAndIndependentPushSlot)
{
	Allocation a, b;
	ASSERT_EQ(a.submit(), 0);
	ASSERT_EQ(b.submit(), 0);
	finish();
	const unsigned mapped = maps;
	std::fill(a.bytes.begin(), a.bytes.end(), 9);
	ASSERT_EQ(a.submit(), 0);
	EXPECT_EQ(maps, mapped);
	finish();
	EXPECT_EQ(a.callbacks, 2u);
	EXPECT_TRUE(std::all_of(a.bytes.begin(), a.bytes.end(), [](auto b) {
		return b == 0;
	}));
}

TEST_F(CachedZero, EarlyInterruptCannotLosePublishedJobOrInvokeCallbackInline)
{
	Allocation a;
	early = true;
	ASSERT_EQ(a.submit(), 0);
	EXPECT_EQ(a.callbacks, 0u);
	finish();
	EXPECT_EQ(a.callbacks, 1u);
}

TEST_F(CachedZero, FailedAdmissionNeverCallsCompletionAndMappingCanBeRetried)
{
	Allocation a;
	nextError = 5;
	EXPECT_EQ(a.submit(), -5);
	EXPECT_EQ(a.callbacks, 0u);
	const unsigned mapped = maps;
	ASSERT_EQ(a.submit(), 0);
	EXPECT_EQ(maps, mapped);
	finish();
	EXPECT_EQ(a.result, 0);
}

TEST_F(CachedZero, FailedQuiescedChannelRefusesZeroSuccessAndLaterReopens)
{
	Allocation a;
	ASSERT_EQ(a.submit(), 0);
	finish(true);
	EXPECT_EQ(a.result, -5);
	ASSERT_EQ(a.submit(), 0);
	finish();
	EXPECT_EQ(a.result, 0);
	EXPECT_EQ(a.callbacks, 2u);
}

TEST_F(CachedZero, ScrubKilledBetweenRetirementsReadsIsNeverReportedClean)
{
	Allocation a;
	ASSERT_EQ(a.submit(), 0);
	killAtFenceRead = true;
	fenceCallback(fenceContext);
	pump();
	EXPECT_EQ(a.callbacks, 1u);
	EXPECT_EQ(a.result, -5);
	EXPECT_TRUE(std::all_of(a.bytes.begin(), a.bytes.end(), [](auto b) {
		return b == 0xa7;
	}));
	gpu.failed = false;
}

TEST_F(CachedZero, MappingFailureLeavesNoPublishedAllocation)
{
	Allocation a;
	mapError = -12;
	EXPECT_EQ(a.submit(), -12);
	EXPECT_EQ(a.mapping, nullptr);
	EXPECT_EQ(a.callbacks, 0u);
}

TEST_F(CachedZero, DuplicateInFlightClearIsRefusedWithoutReusingItsPush)
{
	Allocation a;
	ASSERT_EQ(a.submit(), 0);
	EXPECT_EQ(a.submit(), -16);
	finish();
	EXPECT_EQ(a.callbacks, 1u);
}

// The spare worker is one thread call: a full queue answers Busy at once, and a retiring scrub's completion runs it again.
TEST_F(CachedZero, FullQueueAnswersBusyWithoutSleepingAndRetirementLetsTheNextIn)
{
	std::vector<std::unique_ptr<Allocation>> a;
	for (unsigned i = 0; i < 9; ++i)
		a.push_back(std::make_unique<Allocation>());
	for (unsigned i = 0; i < 8; ++i)
		ASSERT_EQ(a[i]->submit(), 0);
	EXPECT_EQ(a[8]->submit(), -16);
	EXPECT_EQ(switch_zero_sleepers.load(), 0u);
	finish();
	EXPECT_EQ(a[8]->submit(), 0);
	finish();
	for (const auto &item : a)
		EXPECT_EQ(item->callbacks, 1u);
}

// Spares keep their window between scrubs; when every window is taken, an idle one goes, least recently used first.
TEST_F(CachedZero, IdleWindowsGoLeastRecentlyUsedFirstWhenEveryWindowIsTaken)
{
	std::vector<std::unique_ptr<Allocation>> a;
	for (unsigned i = 0; i < 65; ++i)
		a.push_back(std::make_unique<Allocation>(1));
	for (unsigned i = 0; i < 64; ++i) {
		ASSERT_EQ(a[i]->submit(), 0);
		finish();
	}
	ASSERT_EQ(a[0]->submit(), 0);
	finish();
	const unsigned unmapped = unmaps, mapped = maps;
	ASSERT_EQ(a[64]->submit(), 0);
	finish();
	EXPECT_EQ(unmaps, unmapped + 1);
	EXPECT_EQ(maps, mapped + 1);
	ASSERT_EQ(a[0]->submit(), 0);
	finish();
	EXPECT_EQ(maps, mapped + 1);
	ASSERT_EQ(a[1]->submit(), 0);
	finish();
	EXPECT_EQ(maps, mapped + 2);
	EXPECT_EQ(a[1]->result, 0);
}

// The first check comes a second after the scrub is queued; the next one only when tgm_chan_check_progress says its
// rule can take the next step, not a second later.
TEST_F(CachedZero, DeadlineChecksProgressWithoutPublishingAnUnfinishedScrub)
{
	Allocation a;
	const uint64_t queued = switch_zero_now;
	ASSERT_EQ(a.submit(), 0);
	const unsigned before = progressChecks;
	auto watchdog = switch_zero_calls[1];
	EXPECT_EQ(watchdog->deadline, queued + 1000000000u);
	switch_zero_now = watchdog->deadline;
	watchdog->fn(watchdog->arg, nullptr);
	pump();
	EXPECT_EQ(progressChecks, before + 1);
	EXPECT_EQ(a.callbacks, 0u);
	EXPECT_EQ(watchdog->deadline, switch_zero_now + nextCheckNs);
	nextCheckNs = 700000000;
	switch_zero_now = watchdog->deadline;
	watchdog->fn(watchdog->arg, nullptr);
	EXPECT_EQ(progressChecks, before + 2);
	EXPECT_EQ(watchdog->deadline, switch_zero_now + nextCheckNs);
	nextCheckNs = 2000000000;
	finish();
	EXPECT_EQ(a.callbacks, 1u);
	EXPECT_EQ(watchdog->deadline, 0u);
}

// A check that finds the channel with nothing pending leaves the timer down: the scrub's retirement is already queued.
TEST_F(CachedZero, ACheckWithNothingPendingArmsNothingAgain)
{
	Allocation a;
	ASSERT_EQ(a.submit(), 0);
	auto watchdog = switch_zero_calls[1];
	nextCheckNs = 0;
	switch_zero_now = std::exchange(watchdog->deadline, 0);
	watchdog->fn(watchdog->arg, nullptr);
	nextCheckNs = 2000000000;
	EXPECT_EQ(watchdog->deadline, 0u);
	finish();
	EXPECT_EQ(a.callbacks, 1u);
}

TEST_F(CachedZero, ChangedAllocationCannotReuseAnOldPageMapping)
{
	Allocation a;
	ASSERT_EQ(a.submit(), 0);
	finish();
	++a.pages[1];
	EXPECT_EQ(a.submit(), -22);
}

TEST_F(CachedZero, MappedByteBudgetIsBoundedAndReleasedMappingsRestoreCapacity)
{
	Allocation a(16384), b(16384), c(1);
	ASSERT_EQ(a.submit(), 0);
	ASSERT_EQ(b.submit(), 0);
	EXPECT_EQ(c.submit(), -28);
	finish(false, false);
	ASSERT_EQ(c.submit(), 0);
	finish();
	switch_gpu_cached_mapping_drop(&a.mapping);
}
}  // namespace
