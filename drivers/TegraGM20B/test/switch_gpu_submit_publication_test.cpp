// SPDX-License-Identifier: MIT
// SubmitJob's reads of its connection's BOs and bindings with the submit gate open, under ThreadSanitizer: a job on each
// channel finds a BO by handle (the index) and by GPU address (the published bindings) and reads it, while the
// connection imports and binds BOs, maps over and unmaps live ranges, frees BOs, and retirement gives back what left
// (retireBatch when every submit lock is free at once, else releaseLeft after waiting for each).
#include "switch_gpu_client_fixture.h"

#include <random>

namespace
{
constexpr uint32_t kReaders = 4, kSteps = 6000;
constexpr uint64_t kVa = 0x100000000ULL, kPages = 256, kPage = 4096;

struct Connection {
	AGXDeviceUserClient client;
	std::atomic<bool> stop{false};
	std::atomic<uint64_t> issued{0};
	std::atomic<unsigned long> byHandle{0}, byAddress{0};
	uint64_t nextHandle = 1;
	std::vector<uint64_t> live;

	// runJobUses' resolve (TegraGM20BUserClientJobSubmit.cpp), inside its channel's submit lock
	void job(uint32_t slot, std::mt19937 &random)
	{
		IOLockLock(client.submitLocks_[slot]);
		const uint64_t top = issued.load(std::memory_order_relaxed);
		if (top) {
			const uint64_t handle = 1 + random() % top;
			if (const auto *bo = client.boIndex_.find(handle)) {
				EXPECT_EQ(bo->canary, kAlive);
				EXPECT_EQ(bo->handle, handle);
				++byHandle;
			}
		}
		const uint64_t va = kVa + (random() % kPages) * kPage;
		if (const auto *binding = client.bindTable_.find(va, 1)) {
			uint64_t offset = 0;
			EXPECT_TRUE(switch_gpu_binding_offset(*binding, va, 1, offset));
			EXPECT_EQ(binding->bo->canary, kAlive);
			++byAddress;
		}
		IOLockUnlock(client.submitLocks_[slot]);
	}

	// BoImport's publication (publishBo), then its VmBind: over whatever the range held
	void import(std::mt19937 &random)
	{
		const uint64_t pages = 1 + random() % 8, at = random() % (kPages - pages);
		auto *bo = new AGXDeviceUserClient::Bo;
		bo->bytes = pages * kPage;
		bo->mem = new tgm_mem{nullptr};
		IOLockLock(client.lock_);
		ASSERT_TRUE(client.boIndex_.reserve());
		bo->handle = nextHandle++;
		bo->next = client.bos_;
		client.bos_ = bo;
		client.boIndex_.insert(bo->handle, bo);
		IOLockUnlock(client.lock_);
		issued.store(bo->handle, std::memory_order_relaxed);
		live.push_back(bo->handle);
		EXPECT_EQ(client.bind(kVa + at * kPage, bo->bytes, bo), 0);
	}

	void free(std::mt19937 &random)
	{
		const size_t which = random() % live.size();
		const TegraGM20BAbiBoFree request{live[which]};
		live.erase(live.begin() + long(which));
		IOLockLock(client.lock_);
		EXPECT_EQ(client.boFree(&request), 0);
		IOLockUnlock(client.lock_);
	}

	void unmap(std::mt19937 &random)
	{
		const uint64_t pages = 1 + random() % 16, at = random() % (kPages - pages);
		EXPECT_EQ(
			client.bind(kVa + at * kPage, pages * kPage, nullptr, TegraGM20BBindUnmap),
			0);
	}
};

TEST(SubmitPublication, JobsReadBosAndBindingsWhileTheConnectionChangesAndRetiresThem)
{
	Connection c;
	std::vector<std::thread> readers;
	for (uint32_t slot = 0; slot < kReaders; ++slot)
		readers.emplace_back([&c, slot] {
			std::mt19937 random(slot + 1);
			while (!c.stop.load())
				c.job(slot, random);
		});
	std::thread retirement([&c] {
		while (!c.stop.load()) {
			c.client.retireBatch();
			std::this_thread::yield();
		}
	});
	std::thread release([&c] {
		while (!c.stop.load()) {
			c.client.releaseLeft();
			std::this_thread::yield();
		}
	});
	std::mt19937 random(99);
	for (uint32_t step = 0; step < kSteps; ++step) {
		const uint32_t pick = random() % 8;
		if (c.live.size() < 8 || pick < 4)
			c.import(random);
		else if (pick < 7)
			c.free(random);
		else
			c.unmap(random);
	}
	c.stop = true;
	for (auto &reader : readers)
		reader.join();
	retirement.join();
	release.join();
	c.client.retireBatch();
	c.client.releaseLeft();
	EXPECT_GT(c.byHandle.load(), 0u);
	EXPECT_GT(c.byAddress.load(), 0u);
	EXPECT_GT(c.client.releases.load(), 0u) << "retirement released freed BOs while jobs ran";
	c.client.drainResources();
}
}  // namespace
