// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "switch_gpu_allocated_memory_peer.h"
#include "drivers/TegraGM20B/SwitchGpuAllocatedMemory.h"

struct tgm_mem {
	bool smmu;
	bool compressible;
	tgm_mem *later = nullptr;
};

namespace
{
std::atomic<unsigned> directNewCount, exclusiveNewCount, smmuNewCount, bigSmmuNewCount, delCount;
std::atomic<unsigned> laterCount, finishCount, ownerReleases, ownerReleasesAtFinish;
std::atomic<unsigned> releaseCount, ownerReleasesAtRelease, flushAsks;
// The kernel copy queue's ordered flush (switch_gpu_copy_l2_flush): taken at once and completed before it returns, as
// though the GPU had run it, unless a test captures it (its completion then runs when the test says) or refuses it
std::atomic<bool> captureFlushes{false};
std::atomic<int> refuseFlushes{0};

struct Flush {
	void (*fn)(void *, int);
	void *arg;
};

std::vector<Flush> flushes;
std::atomic<bool> bigSmmuFallbackToDirect{false};
std::mutex hookMutex;
std::function<void()> smmuHook;
}  // namespace

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++directNewCount;
	*pmem = new tgm_mem{false, false};
	return 0;
}

extern "C" int tgm_mem_new_exclusive(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++exclusiveNewCount;
	*pmem = new tgm_mem{false, false};
	return 0;
}

extern "C" int tgm_mem_new_exclusive_smmu(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++smmuNewCount;
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> guard(hookMutex);
		hook = smmuHook;
	}
	if (hook)
		hook();
	*pmem = new tgm_mem{true, true};
	return 0;
}

extern "C" int tgm_mem_new_smmu_big(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++bigSmmuNewCount;
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> guard(hookMutex);
		hook = smmuHook;
	}
	if (hook)
		hook();
	*pmem = new tgm_mem{!bigSmmuFallbackToDirect.load(std::memory_order_acquire), false};
	return 0;
}

extern "C" int tgm_mem_can_compress(const tgm_mem *mem)
{
	return mem && mem->compressible ? 1 : 0;
}

extern "C" int tgm_mem_is_smmu(const tgm_mem *mem)
{
	return mem && mem->smmu ? 1 : 0;
}

extern "C" void tgm_mem_del(tgm_mem **pmem)
{
	if (!pmem || !*pmem)
		return;
	++delCount;
	delete *pmem;
	*pmem = nullptr;
}

extern "C" void tgm_mem_del_later(tgm_mem **pmem, tgm_mem **later)
{
	if (!*pmem || !(*pmem)->smmu) {
		tgm_mem_del(pmem);
		return;
	}
	++laterCount;
	(*pmem)->later = *later;
	*later = *pmem;
	*pmem = nullptr;
}

extern "C" uint64_t tgm_retire_gpu_flushes = 0;

extern "C" void tgm_mem_del_release(tgm_mem *later)
{
	if (!later)
		return;
	++releaseCount;
	ownerReleasesAtRelease = ownerReleases.load();
	while (later) {
		tgm_mem *next = later->later;
		++delCount;
		delete later;
		later = next;
	}
}

extern "C" int switch_gpu_copy_l2_flush(void (*fn)(void *, int), void *arg)
{
	++flushAsks;
	if (refuseFlushes.load()) {
		--refuseFlushes;
		return -16;
	}
	if (captureFlushes.load()) {
		std::lock_guard<std::mutex> guard(hookMutex);
		flushes.push_back({fn, arg});
		return 0;
	}
	fn(arg, 0);
	return 0;
}

extern "C" void tgm_mem_del_finish(tgm_mem *later)
{
	if (!later)
		return;
	++finishCount;
	ownerReleasesAtFinish = ownerReleases.load();
	while (later) {
		tgm_mem *next = later->later;
		++delCount;
		delete later;
		later = next;
	}
}

namespace
{
class AllocatedMemoryCrossTu : public Backing
{
protected:
	void SetUp() override
	{
		Backing::SetUp();
		directNewCount = exclusiveNewCount = smmuNewCount = bigSmmuNewCount = delCount = 0;
		laterCount = finishCount = ownerReleases = ownerReleasesAtFinish = 0;
		releaseCount = ownerReleasesAtRelease = flushAsks = 0;
		captureFlushes = false;
		refuseFlushes = 0;
		flushes.clear();
		bigSmmuFallbackToDirect.store(false, std::memory_order_release);
		std::lock_guard<std::mutex> guard(hookMutex);
		smmuHook = {};
	}

	void TearDown() override
	{
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = {};
		}
		Backing::TearDown();
		EXPECT_EQ(directNewCount.load() + exclusiveNewCount.load() + smmuNewCount.load() +
				  bigSmmuNewCount.load(),
			  delCount.load());
	}
};

// Backings retired together release their SMMU memories after one L2 write-back and invalidate, the GPU's on the kernel
// copy queue (switch_gpu_copy_l2_flush), not one per backing, and their owners' pinned pages only after it: its flush
// and the SMMU unmaps must not reach a page back with the VM. A canonical memory released outside retirement goes at
// once.
TEST_F(AllocatedMemoryCrossTu, OneRetirementBatchSharesOneL2Invalidate)
{
	captureFlushes = true;
	int owners[3] = {};
	switch_gpu_backing *made[3] = {};
	for (uint32_t i = 0; i < 3; ++i) {
		std::vector<uint64_t> pages;
		for (uint32_t p = 0; p < 16; ++p)
			pages.push_back(next_page += 4096);
		made[i] = switch_gpu_backing_register(pages.data(), 16, 0, &owners[i], [](void *) {
			++ownerReleases;
		});
		switch_gpu_backing *b = made[i];
		ASSERT_NE(b, nullptr);
		tgm_mem *mem = nullptr;
		ASSERT_EQ(allocatedMemorySmmu(b, 16, &mem), 0);
		ASSERT_TRUE(tgm_mem_is_smmu(mem));
	}
	for (switch_gpu_backing *b : made)
		switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(laterCount.load(), 3u);
	ASSERT_EQ(flushes.size(), 1u);
	EXPECT_EQ(delCount.load(), 0u);
	EXPECT_EQ(ownerReleases.load(), 0u)
		<< "a pinned page went back before the batch's flush completed";
	flushes[0].fn(flushes[0].arg, 0);
	EXPECT_EQ(finishCount.load(), 0u) << "the CPU waited for the L2";
	EXPECT_EQ(releaseCount.load(), 1u);
	EXPECT_EQ(delCount.load(), 3u);
	EXPECT_EQ(ownerReleasesAtRelease.load(), 0u)
		<< "a pinned page went back before the batch's SMMU unmaps";
	EXPECT_EQ(ownerReleases.load(), 3u);

	tgm_mem *alone = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(nullptr, nullptr, 16, &alone), 0);
	switch_gpu_allocated_memory_destroy(alone);
	EXPECT_EQ(laterCount.load(), 3u);
	EXPECT_EQ(delCount.load(), 4u);
}

TEST_F(AllocatedMemoryCrossTu, CrossTuCanonicalLookupReturnsSameObjectNeverEopnotsupp)
{
	EXPECT_EQ(peer_destroy_address(), &switch_gpu_allocated_memory_destroy);
	auto *b = backing(16);
	tgm_mem *fromPeer = nullptr;
	ASSERT_EQ(peer_allocated_memory_smmu(b, 16, &fromPeer), 0);
	ASSERT_NE(fromPeer, nullptr);
	EXPECT_TRUE(tgm_mem_is_smmu(fromPeer));
	EXPECT_TRUE(tgm_mem_can_compress(fromPeer));

	tgm_mem *fromMain = nullptr, *fromHeader = nullptr, *fromDirectCheck = nullptr;
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing(b, 16, &fromMain), 0);
	ASSERT_EQ(allocatedMemoryExisting(b, 16, &fromHeader), 0);
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing_or_direct(b, 16, &fromDirectCheck), 0);
	EXPECT_EQ(fromMain, fromPeer);
	EXPECT_EQ(fromHeader, fromPeer);
	EXPECT_EQ(fromDirectCheck, fromPeer);
	EXPECT_EQ(smmuNewCount.load(), 1u);
	EXPECT_EQ(directNewCount.load(), 0u);
	const auto write = token(701, 1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, write), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 0u);
	timelines[key(&write)].completed = 1;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 1u);
}

TEST_F(AllocatedMemoryCrossTu, DirectPaTouchRefusesSubsequentSmmuUpgradeAcrossTus)
{
	auto *b = backing(16);
	tgm_mem *existing = nullptr;
	EXPECT_EQ(peer_allocated_memory_existing_or_direct(b, 16, &existing), -2);
	EXPECT_EQ(existing, nullptr);

	tgm_mem *mem = nullptr;
	ASSERT_EQ(allocatedMemorySmmu(b, 16, &mem), 0);
	ASSERT_NE(mem, nullptr);
	EXPECT_FALSE(tgm_mem_is_smmu(mem));
	EXPECT_FALSE(tgm_mem_can_compress(mem));
	EXPECT_EQ(smmuNewCount.load(), 0u);
	EXPECT_EQ(directNewCount.load(), 1u);

	tgm_mem *fromPeer = nullptr;
	ASSERT_EQ(peer_allocated_memory_existing(b, 16, &fromPeer), 0);
	EXPECT_EQ(fromPeer, mem);
	switch_gpu_backing_release(b);
}

TEST_F(AllocatedMemoryCrossTu, ConcurrentDirectPaAndSmmuRaceNeverExposesBothAddressSpaces)
{
	// Deterministic race: peer latches direct PA while SMMU creation is paused mid-flight.
	{
		auto *b = backing(16);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = [&] {
				tgm_mem *existing = nullptr;
				const int err =
					peer_allocated_memory_existing_or_direct(b, 16, &existing);
				EXPECT_EQ(err, -2);
				EXPECT_EQ(existing, nullptr);
			};
		}
		tgm_mem *mem = nullptr;
		ASSERT_EQ(allocatedMemorySmmu(b, 16, &mem), 0);
		ASSERT_NE(mem, nullptr);
		EXPECT_FALSE(tgm_mem_is_smmu(mem));
		EXPECT_FALSE(tgm_mem_can_compress(mem));
		EXPECT_EQ(smmuNewCount.load(), 1u);
		EXPECT_EQ(directNewCount.load(), 1u);
		EXPECT_EQ(delCount.load(), 1u);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = {};
		}
		switch_gpu_backing_release(b);
	}

	// Parallel stress: two threads racing SMMU acquisition against direct-PA fallback.
	for (unsigned round = 0; round < 64; ++round) {
		auto *b = backing(16);
		std::atomic<bool> go{false};
		tgm_mem *smmuOut = nullptr, *directOut = nullptr;
		int smmuRet = -1, directRet = -1;
		std::thread t1([&] {
			while (!go.load(std::memory_order_acquire))
				;
			smmuRet = allocatedMemorySmmu(b, 16, &smmuOut);
		});
		std::thread t2([&] {
			while (!go.load(std::memory_order_acquire))
				;
			directRet = peer_allocated_memory_existing_or_direct(b, 16, &directOut);
		});
		go.store(true, std::memory_order_release);
		t1.join();
		t2.join();
		ASSERT_EQ(smmuRet, 0);
		ASSERT_NE(smmuOut, nullptr);
		if (directRet == 0) {
			EXPECT_EQ(directOut, smmuOut);
		} else {
			EXPECT_EQ(directRet, -2);
			EXPECT_FALSE(tgm_mem_is_smmu(smmuOut));
		}
		switch_gpu_backing_release(b);
	}
}

TEST_F(AllocatedMemoryCrossTu, CrossTuBigSmmuCanonicalLookupReturnsSameUncompressedObject)
{
	auto *b = backing(900);
	tgm_mem *fromPeer = nullptr;
	ASSERT_EQ(peer_allocated_memory_big(b, 900, &fromPeer), 0);
	ASSERT_NE(fromPeer, nullptr);
	EXPECT_TRUE(tgm_mem_is_smmu(fromPeer));
	EXPECT_FALSE(tgm_mem_can_compress(fromPeer));

	tgm_mem *fromMain = nullptr, *fromSubrange = nullptr, *fromDirectCheck = nullptr;
	ASSERT_EQ(allocatedMemoryBig(b, 900, &fromMain), 0);
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing(b, 16, &fromSubrange), 0);
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing_or_direct(b, 900, &fromDirectCheck), 0);
	EXPECT_EQ(fromMain, fromPeer);
	EXPECT_EQ(fromSubrange, fromPeer);
	EXPECT_EQ(fromDirectCheck, fromPeer);
	EXPECT_EQ(bigSmmuNewCount.load(), 1u);

	const auto write = token(702, 1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, write), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 0u);
	timelines[key(&write)].completed = 1;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 1u);
}

TEST_F(AllocatedMemoryCrossTu, BigSmmuReserveFallbackLatchesDirectPaAndNeverUpgrades)
{
	auto *b = backing(900);
	bigSmmuFallbackToDirect.store(true, std::memory_order_release);
	tgm_mem *first = nullptr;
	ASSERT_EQ(allocatedMemoryBig(b, 900, &first), 0);
	ASSERT_NE(first, nullptr);
	EXPECT_FALSE(tgm_mem_is_smmu(first));
	EXPECT_FALSE(tgm_mem_can_compress(first));

	bigSmmuFallbackToDirect.store(false, std::memory_order_release);
	tgm_mem *second = nullptr;
	ASSERT_EQ(peer_allocated_memory_big(b, 900, &second), 0);
	EXPECT_EQ(second, first);
	EXPECT_EQ(bigSmmuNewCount.load(), 1u);
	switch_gpu_backing_release(b);
}

TEST_F(AllocatedMemoryCrossTu, ConcurrentDirectPaAndBigSmmuRaceNeverExposesBothAddressSpaces)
{
	{
		auto *b = backing(900);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = [&] {
				tgm_mem *existing = nullptr;
				EXPECT_EQ(
					peer_allocated_memory_existing_or_direct(b, 900, &existing),
					-2);
				EXPECT_EQ(existing, nullptr);
			};
		}
		tgm_mem *mem = nullptr;
		ASSERT_EQ(allocatedMemoryBig(b, 900, &mem), 0);
		ASSERT_NE(mem, nullptr);
		EXPECT_FALSE(tgm_mem_is_smmu(mem));
		EXPECT_EQ(bigSmmuNewCount.load(), 1u);
		EXPECT_EQ(directNewCount.load(), 1u);
		EXPECT_EQ(delCount.load(), 1u);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = {};
		}
		switch_gpu_backing_release(b);
	}

	for (unsigned round = 0; round < 64; ++round) {
		auto *b = backing(900);
		std::atomic<bool> go{false};
		tgm_mem *bigOut = nullptr, *directOut = nullptr;
		int bigRet = -1, directRet = -1;
		std::thread t1([&] {
			while (!go.load(std::memory_order_acquire))
				;
			bigRet = allocatedMemoryBig(b, 900, &bigOut);
		});
		std::thread t2([&] {
			while (!go.load(std::memory_order_acquire))
				;
			directRet = peer_allocated_memory_existing_or_direct(b, 900, &directOut);
		});
		go.store(true, std::memory_order_release);
		t1.join();
		t2.join();
		ASSERT_EQ(bigRet, 0);
		ASSERT_NE(bigOut, nullptr);
		if (directRet == 0) {
			EXPECT_EQ(directOut, bigOut);
		} else {
			EXPECT_EQ(directRet, -2);
			EXPECT_FALSE(tgm_mem_is_smmu(bigOut));
		}
		switch_gpu_backing_release(b);
	}
}

int batchOwner;

// `count` backings of SMMU memory released and retired in one batch, their owners counted by ownerReleases
void retireSmmuBatch(uint64_t &next, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i) {
		std::vector<uint64_t> pages;
		for (uint32_t p = 0; p < 16; ++p)
			pages.push_back(next += 4096);
		switch_gpu_backing *b =
			switch_gpu_backing_register(pages.data(), 16, 0, &batchOwner, [](void *) {
				++ownerReleases;
			});
		ASSERT_NE(b, nullptr);
		tgm_mem *mem = nullptr;
		ASSERT_EQ(allocatedMemorySmmu(b, 16, &mem), 0);
		switch_gpu_backing_release(b);
	}
	switch_gpu_backing_retire();
}

// The two orders of a batch's flush against the next batch's retirement: each batch's pages go back at its own flush's
// completion, never at an earlier batch's, which the GPU queued before that batch's unmaps
TEST_F(AllocatedMemoryCrossTu, ABatchsPagesGoBackAtItsOwnFlushWhicheverCompletesFirst)
{
	captureFlushes = true;
	retireSmmuBatch(next_page, 2);
	ASSERT_EQ(flushes.size(), 1u);
	flushes[0].fn(flushes[0].arg, 0);
	EXPECT_EQ(ownerReleases.load(), 2u)
		<< "the first batch's flush completed before the second retired";
	retireSmmuBatch(next_page, 3);
	ASSERT_EQ(flushes.size(), 2u);
	EXPECT_EQ(ownerReleases.load(), 2u);
	flushes[1].fn(flushes[1].arg, 0);
	EXPECT_EQ(ownerReleases.load(), 5u);

	retireSmmuBatch(next_page, 2);
	retireSmmuBatch(next_page, 1);
	ASSERT_EQ(flushes.size(), 4u) << "a batch rode a flush queued before its own unmaps";
	flushes[2].fn(flushes[2].arg, 0);
	EXPECT_EQ(ownerReleases.load(), 7u)
		<< "the earlier flush released the later batch, or not its own";
	flushes[3].fn(flushes[3].arg, 0);
	EXPECT_EQ(ownerReleases.load(), 8u);
	EXPECT_EQ(finishCount.load(), 0u);
}

// A copy queue the retirement work cannot take now (its lock, a submitter's turn, its slots or its ring) parks the
// batch with its pages held; a later pass submits every parked batch in one flush, queued after all their unmaps
TEST_F(AllocatedMemoryCrossTu, ABusyCopyQueueParksTheBatchUntilALaterPass)
{
	captureFlushes = true;
	// each pass asks for the parked batches first, then for its own: the first batch's ask, then both of the second's
	refuseFlushes = 3;
	retireSmmuBatch(next_page, 2);
	retireSmmuBatch(next_page, 1);
	EXPECT_TRUE(flushes.empty());
	EXPECT_EQ(ownerReleases.load(), 0u) << "a parked batch's pages went back with no flush";
	switch_gpu_backing_retire();
	ASSERT_EQ(flushes.size(), 1u) << "the parked batches were not submitted together";
	EXPECT_EQ(ownerReleases.load(), 0u);
	flushes[0].fn(flushes[0].arg, 0);
	EXPECT_EQ(ownerReleases.load(), 3u);
	EXPECT_EQ(releaseCount.load(), 2u);
}

// A failed copy channel's job may not have flushed: the CPU writes back and invalidates (tgm_mem_del_finish) before any
// of the batch's pages goes back
TEST_F(AllocatedMemoryCrossTu, AFailedFlushWritesBackOnTheCpuBeforeThePagesGoBack)
{
	captureFlushes = true;
	retireSmmuBatch(next_page, 2);
	ASSERT_EQ(flushes.size(), 1u);
	flushes[0].fn(flushes[0].arg, -5);
	EXPECT_EQ(finishCount.load(), 1u);
	EXPECT_EQ(ownerReleasesAtFinish.load(), 0u);
	EXPECT_EQ(releaseCount.load(), 0u);
	EXPECT_EQ(ownerReleases.load(), 2u);
}

// A batch with no SMMU memory waits for no flush: its pages go back in the same pass
TEST_F(AllocatedMemoryCrossTu, ABatchWithNothingDeferredReleasesAtOnce)
{
	captureFlushes = true;
	std::vector<uint64_t> pages;
	for (uint32_t p = 0; p < 16; ++p)
		pages.push_back(next_page += 4096);
	switch_gpu_backing *b =
		switch_gpu_backing_register(pages.data(), 16, 0, &batchOwner, [](void *) {
			++ownerReleases;
		});
	ASSERT_NE(b, nullptr);
	tgm_mem *mem = nullptr;
	ASSERT_EQ(switch_gpu_backing_tgm_mem_direct(b, 16, &mem), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(flushAsks.load(), 0u);
	EXPECT_EQ(ownerReleases.load(), 1u);
}
}  // namespace
