// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "drivers/TegraGM20B/SwitchGpuParked.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"

namespace
{
SwitchGpuParkEligibility validEligibility()
{
	return {true, false, false, false, false, false, false, false,
		true, false, false, 1u,    true,  false, false, false};
}
}  // namespace

TEST(SwitchGpuParkedAbi, SelectorsFlagsAndRecordLayouts)
{
	EXPECT_EQ(uint32_t(TegraGM20BMethodBoPark), 35u);
	EXPECT_EQ(uint32_t(TegraGM20BMethodBoUnpark), 36u);
	EXPECT_NE(uint32_t(TegraGM20BMethodBoPark), uint32_t(TegraGM20BMethodPackImageMap));
	EXPECT_NE(uint32_t(TegraGM20BMethodBoUnpark), uint32_t(TegraGM20BMethodPackImageMap));
	EXPECT_EQ(uint32_t(TegraGM20BInfoBoPark), 1u << 28);
	EXPECT_NE(uint32_t(TegraGM20BInfoBoPark), uint32_t(TegraGM20BInfoPackImage));
	EXPECT_NE(uint32_t(TegraGM20BInfoBoPark), uint32_t(TegraGM20BInfoPushFences));
	EXPECT_EQ(uint32_t(TegraGM20BBoSystem), 1u << 4);
	EXPECT_EQ(uint32_t(TegraGM20BParkGuard), 1u << 0);
	EXPECT_EQ(uint32_t(TegraGM20BUnparkResident), 0u);
	EXPECT_EQ(uint32_t(TegraGM20BUnparkReclaimed), 1u);

	EXPECT_EQ(sizeof(TegraGM20BAbiBoPark), 16u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoPark, handle), 0u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoPark, flags), 8u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoPark, reserved), 12u);

	EXPECT_EQ(sizeof(TegraGM20BAbiBoUnpark), 16u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoUnpark, handle), 0u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoUnpark, flags), 8u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoUnpark, reserved), 12u);

	EXPECT_EQ(sizeof(TegraGM20BAbiBoUnparkReply), 24u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoUnparkReply, address), 0u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoUnparkReply, bytes), 8u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoUnparkReply, status), 16u);
	EXPECT_EQ(offsetof(TegraGM20BAbiBoUnparkReply, reserved), 20u);
}

TEST(SwitchGpuParkedState, TransitionsRejectDoubleParkUnparkAndRaces)
{
	EXPECT_TRUE(switch_gpu_park_is_live(SwitchGpuParkState::Live));
	EXPECT_FALSE(switch_gpu_park_blocks_use(SwitchGpuParkState::Live));
	EXPECT_FALSE(switch_gpu_park_is_transient(SwitchGpuParkState::Live));

	for (SwitchGpuParkState s : {SwitchGpuParkState::Parking, SwitchGpuParkState::Parked,
				     SwitchGpuParkState::Unparking, SwitchGpuParkState::Reclaiming,
				     SwitchGpuParkState::Reclaimed}) {
		EXPECT_FALSE(switch_gpu_park_is_live(s));
		EXPECT_TRUE(switch_gpu_park_blocks_use(s));
	}
	for (SwitchGpuParkState s : {SwitchGpuParkState::Parking, SwitchGpuParkState::Unparking,
				     SwitchGpuParkState::Reclaiming})
		EXPECT_TRUE(switch_gpu_park_is_transient(s));
	for (SwitchGpuParkState s :
	     {SwitchGpuParkState::Live, SwitchGpuParkState::Parked, SwitchGpuParkState::Reclaimed})
		EXPECT_FALSE(switch_gpu_park_is_transient(s));

	EXPECT_EQ(switch_gpu_park_begin_park(SwitchGpuParkState::Live), SwitchGpuParkAction::Ok);
	EXPECT_EQ(switch_gpu_park_begin_park(SwitchGpuParkState::Parking),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_park(SwitchGpuParkState::Unparking),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_park(SwitchGpuParkState::Reclaiming),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_park(SwitchGpuParkState::Parked),
		  SwitchGpuParkAction::InvalidState);
	EXPECT_EQ(switch_gpu_park_begin_park(SwitchGpuParkState::Reclaimed),
		  SwitchGpuParkAction::InvalidState);

	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Parked),
		  SwitchGpuParkAction::Ok);
	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Reclaimed),
		  SwitchGpuParkAction::Reclaimed);
	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Parking),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Unparking),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Reclaiming),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Live),
		  SwitchGpuParkAction::InvalidState);

	EXPECT_EQ(switch_gpu_park_begin_reclaim(SwitchGpuParkState::Parked),
		  SwitchGpuParkAction::Ok);
	EXPECT_EQ(switch_gpu_park_begin_reclaim(SwitchGpuParkState::Parking),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_reclaim(SwitchGpuParkState::Unparking),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_reclaim(SwitchGpuParkState::Reclaiming),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_reclaim(SwitchGpuParkState::Live),
		  SwitchGpuParkAction::InvalidState);
	EXPECT_EQ(switch_gpu_park_begin_reclaim(SwitchGpuParkState::Reclaimed),
		  SwitchGpuParkAction::InvalidState);

	EXPECT_EQ(switch_gpu_park_finish_reclaim(SwitchGpuParkState::Reclaiming),
		  SwitchGpuParkAction::Ok);
	EXPECT_EQ(switch_gpu_park_finish_reclaim(SwitchGpuParkState::Parked),
		  SwitchGpuParkAction::InvalidState);
	EXPECT_EQ(switch_gpu_park_finish_reclaim(SwitchGpuParkState::Reclaimed),
		  SwitchGpuParkAction::InvalidState);
}

TEST(SwitchGpuParkedEligibility, RejectsIneligibleOrBusyBos)
{
	EXPECT_EQ(switch_gpu_park_check_eligible(validEligibility()), SwitchGpuParkReject::None);
	EXPECT_EQ(switch_gpu_park_expected_live_retains(false), 4u);
	EXPECT_EQ(switch_gpu_park_expected_live_retains(true), 5u);

	const auto rejectOf = [](auto mutate) {
		SwitchGpuParkEligibility e = validEligibility();
		mutate(e);
		return switch_gpu_park_check_eligible(e);
	};
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.bufferMemory = false;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.wired = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.poolVa = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.chunked = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.contiguous = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.control = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.nativeView = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.surface = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.hasBacking = false;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.sharedBacking = true;
		  }),
		  SwitchGpuParkReject::Unsupported);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.freeing = true;
		  }),
		  SwitchGpuParkReject::Freeing);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.mappingCount = 0;
		  }),
		  SwitchGpuParkReject::NotMapped);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.mappingCount = 2;
		  }),
		  SwitchGpuParkReject::MultipleMappings);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.mappingSizeMatches = false;
		  }),
		  SwitchGpuParkReject::SizeMismatch);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.mappingInFlight = true;
		  }),
		  SwitchGpuParkReject::MappingInFlight);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.cpuLeaseActive = true;
		  }),
		  SwitchGpuParkReject::CpuLeaseActive);
	EXPECT_EQ(rejectOf([](auto &e) {
			  e.gpuBusy = true;
		  }),
		  SwitchGpuParkReject::GpuBusy);
}

TEST(SwitchGpuParkedLru, FifoOrderAndCachedLookup)
{
	SwitchGpuParkedLru lru;
	EXPECT_TRUE(lru.empty());
	EXPECT_EQ(lru.peekHead(), nullptr);
	EXPECT_EQ(lru.peekOldestCached(), nullptr);

	SwitchGpuParkedNode s0 = {nullptr, nullptr, nullptr, 1, 4ULL << 20, true, false};
	SwitchGpuParkedNode c1 = {nullptr, nullptr, nullptr, 2, 2ULL << 20, false, false};
	SwitchGpuParkedNode s2 = {nullptr, nullptr, nullptr, 3, 8ULL << 20, true, false};
	SwitchGpuParkedNode c3 = {nullptr, nullptr, nullptr, 4, 1ULL << 20, false, false};

	lru.pushBack(&s0);
	lru.pushBack(&s0);  // idempotent
	lru.pushBack(&c1);
	lru.pushBack(&s2);
	lru.pushBack(&c3);

	EXPECT_EQ(lru.count(), 4u);
	EXPECT_EQ(lru.cachedCount(), 2u);
	EXPECT_EQ(lru.totalBytes(), 15ULL << 20);
	EXPECT_EQ(lru.systemBytes(), 12ULL << 20);
	EXPECT_EQ(lru.cachedBytes(), 3ULL << 20);
	EXPECT_EQ(lru.peekHead(), &s0);
	EXPECT_EQ(lru.peekOldestCached(), &c1);

	lru.remove(&c1);
	lru.remove(&c1);  // idempotent
	EXPECT_EQ(lru.count(), 3u);
	EXPECT_EQ(lru.cachedCount(), 1u);
	EXPECT_EQ(lru.cachedBytes(), 1ULL << 20);
	EXPECT_EQ(lru.peekOldestCached(), &c3);

	lru.remove(&c3);
	EXPECT_EQ(lru.cachedCount(), 0u);
	EXPECT_EQ(lru.peekOldestCached(), nullptr);
	EXPECT_EQ(lru.peekHead(), &s0);

	lru.remove(&s0);
	EXPECT_EQ(lru.peekHead(), &s2);
	lru.remove(&s2);
	EXPECT_TRUE(lru.empty());
	EXPECT_EQ(lru.totalBytes(), 0u);
	EXPECT_EQ(lru.systemBytes(), 0u);
	EXPECT_EQ(lru.cachedBytes(), 0u);
}

TEST(SwitchGpuParkedPolicy, CapPressureDemandAndDestinations)
{
	EXPECT_EQ(switch_gpu_park_reclaim_pick(0, 0, true, 4096, true),
		  SwitchGpuParkReclaimPick::None);
	EXPECT_EQ(switch_gpu_park_reclaim_pick(SWITCH_GPU_PARKED_CAP_BYTES,
					       SWITCH_GPU_PARKED_CAP_BYTES, false, 0, true),
		  SwitchGpuParkReclaimPick::None);
	EXPECT_EQ(switch_gpu_park_reclaim_pick(SWITCH_GPU_PARKED_CAP_BYTES + 4096,
					       SWITCH_GPU_PARKED_CAP_BYTES + 4096, false, 0, true),
		  SwitchGpuParkReclaimPick::Head);
	EXPECT_EQ(switch_gpu_park_reclaim_pick(4096, 0, true, 0, false),
		  SwitchGpuParkReclaimPick::Head);
	EXPECT_EQ(switch_gpu_park_reclaim_pick(8ULL << 20, 4ULL << 20, false, 4096, true),
		  SwitchGpuParkReclaimPick::OldestCached);
	EXPECT_EQ(switch_gpu_park_reclaim_pick(8ULL << 20, 4ULL << 20, false, 4096, false),
		  SwitchGpuParkReclaimPick::None);
	EXPECT_EQ(switch_gpu_park_reclaim_pick(8ULL << 20, 0, false, 4096, true),
		  SwitchGpuParkReclaimPick::None);

	EXPECT_EQ(switch_gpu_park_clamp_demand(0, 4ULL << 20), 4ULL << 20);
	EXPECT_EQ(switch_gpu_park_clamp_demand(SWITCH_GPU_PARKED_DEMAND_MAX_BYTES - 4096, 8192),
		  SWITCH_GPU_PARKED_DEMAND_MAX_BYTES);
	EXPECT_EQ(switch_gpu_park_clamp_demand(1024, UINT64_MAX),
		  SWITCH_GPU_PARKED_DEMAND_MAX_BYTES);
	EXPECT_EQ(switch_gpu_park_consume_demand(8192, 4096), 4096u);
	EXPECT_EQ(switch_gpu_park_consume_demand(4096, 8192), 0u);
}

TEST(SwitchGpuParkedConcurrency, ParkUnparkReclaimAndFreeRespectLockOrder)
{
	struct SimBo {
		uint64_t handle = 0;
		uint64_t bytes = 0;
		bool system = false;
		bool mapped = true;
		bool pagesHeld = true;
		bool freeing = false;
		SwitchGpuParkState state = SwitchGpuParkState::Live;
		SwitchGpuParkedNode node = {};
	};

	struct SimClient {
		std::mutex lock;
		std::vector<SimBo> bos;
		std::atomic<uint32_t> refs{1};
	};

	struct SimGlobal {
		std::mutex lock;
		SwitchGpuParkedLru lru;
		uint64_t demand = 0;
		std::atomic<bool> stop{false};
		std::atomic<uint64_t> parkedCount{0};
		std::atomic<uint64_t> unparkResidentCount{0};
		std::atomic<uint64_t> unparkReclaimedCount{0};
		std::atomic<uint64_t> reclaimedCount{0};
	} global;

	constexpr uint32_t kClients = 4;
	constexpr uint32_t kBosPerClient = 12;
	constexpr uint64_t kCap = 4ULL << 20;
	std::vector<SimClient> clients(kClients);
	for (uint32_t c = 0; c < kClients; ++c) {
		clients[c].bos.resize(kBosPerClient);
		for (uint32_t i = 0; i < kBosPerClient; ++i) {
			clients[c].bos[i].handle = i + 1;
			clients[c].bos[i].bytes = (1ULL << 20);
			clients[c].bos[i].system = (i % 3u) == 0u;
		}
	}

	auto runClient = [&](uint32_t cid) {
		SimClient &cl = clients[cid];
		for (uint32_t iter = 0; iter < 150; ++iter) {
			const uint32_t idx = (iter * 7u + cid) % kBosPerClient;
			// Try BoPark (L2 -> unlock VM -> L2 -> L4)
			bool enteredPark = false;
			{
				std::lock_guard<std::mutex> lg(cl.lock);
				SimBo &bo = cl.bos[idx];
				if (!bo.freeing &&
				    switch_gpu_park_begin_park(bo.state) ==
					    SwitchGpuParkAction::Ok &&
				    bo.mapped && bo.pagesHeld) {
					bo.state = SwitchGpuParkState::Parking;
					bo.mapped = false;
					enteredPark = true;
				}
			}
			if (enteredPark) {
				std::this_thread::yield();
				std::lock_guard<std::mutex> lg(cl.lock);
				SimBo &bo = cl.bos[idx];
				EXPECT_EQ(bo.state, SwitchGpuParkState::Parking);
				bo.state = SwitchGpuParkState::Parked;
				bo.node = {nullptr,  nullptr,   &cl,  bo.handle,
					   bo.bytes, bo.system, false};
				{
					std::lock_guard<std::mutex> pg(global.lock);
					global.lru.pushBack(&bo.node);
				}
				global.parkedCount.fetch_add(1, std::memory_order_relaxed);
			}

			// Try BoUnpark (L2 -> L4 -> unlock VM -> L2)
			const uint32_t uidx = (iter * 5u + cid) % kBosPerClient;
			bool enteredUnpark = false;
			{
				std::lock_guard<std::mutex> lg(cl.lock);
				SimBo &bo = cl.bos[uidx];
				const SwitchGpuParkAction act =
					switch_gpu_park_begin_unpark(bo.state);
				if (act == SwitchGpuParkAction::Reclaimed) {
					EXPECT_FALSE(bo.pagesHeld);
					EXPECT_FALSE(bo.mapped);
					// Re-allocate fresh pages as Metal does after Reclaimed
					bo.pagesHeld = true;
					bo.mapped = true;
					bo.state = SwitchGpuParkState::Live;
					global.unparkReclaimedCount.fetch_add(
						1, std::memory_order_relaxed);
				} else if (act == SwitchGpuParkAction::Ok) {
					EXPECT_TRUE(bo.pagesHeld);
					bo.state = SwitchGpuParkState::Unparking;
					{
						std::lock_guard<std::mutex> pg(global.lock);
						global.lru.remove(&bo.node);
					}
					enteredUnpark = true;
				}
			}
			if (enteredUnpark) {
				std::this_thread::yield();
				std::lock_guard<std::mutex> lg(cl.lock);
				SimBo &bo = cl.bos[uidx];
				EXPECT_EQ(bo.state, SwitchGpuParkState::Unparking);
				EXPECT_TRUE(bo.pagesHeld);
				bo.mapped = true;
				bo.state = SwitchGpuParkState::Live;
				global.unparkResidentCount.fetch_add(1, std::memory_order_relaxed);
			}
		}
	};

	auto runReaper = [&](uint32_t rid) {
		uint32_t step = 0;
		while (!global.stop.load(std::memory_order_acquire)) {
			const bool pressure = ((step + rid) % 11u) == 0u;
			const bool room = !pressure;
			SimClient *pickedClient = nullptr;
			uint64_t pickedHandle = 0;
			{
				std::lock_guard<std::mutex> pg(global.lock);
				if ((step % 5u) == 0u)
					global.demand = switch_gpu_park_clamp_demand(global.demand,
										     1ULL << 20);
				const SwitchGpuParkReclaimPick pick = switch_gpu_park_reclaim_pick(
					global.lru.totalBytes(), global.lru.cachedBytes(), pressure,
					global.demand, room, kCap);
				SwitchGpuParkedNode *node =
					pick == SwitchGpuParkReclaimPick::Head
						? global.lru.peekHead()
					: pick == SwitchGpuParkReclaimPick::OldestCached
						? global.lru.peekOldestCached()
						: nullptr;
				if (node) {
					pickedClient = static_cast<SimClient *>(node->client);
					pickedHandle = node->handle;
					pickedClient->refs.fetch_add(1, std::memory_order_acq_rel);
				}
			}
			if (pickedClient) {
				{
					std::lock_guard<std::mutex> lg(pickedClient->lock);
					SimBo &bo = pickedClient->bos[pickedHandle - 1];
					if (!bo.freeing &&
					    switch_gpu_park_begin_reclaim(bo.state) ==
						    SwitchGpuParkAction::Ok) {
						EXPECT_FALSE(bo.mapped);
						EXPECT_TRUE(bo.pagesHeld);
						bo.state = SwitchGpuParkState::Reclaiming;
						{
							std::lock_guard<std::mutex> pg(global.lock);
							global.lru.remove(&bo.node);
							if (!bo.system)
								global.demand =
									switch_gpu_park_consume_demand(
										global.demand,
										bo.bytes);
						}
						bo.pagesHeld = false;
						EXPECT_EQ(switch_gpu_park_finish_reclaim(bo.state),
							  SwitchGpuParkAction::Ok);
						bo.state = SwitchGpuParkState::Reclaimed;
						global.reclaimedCount.fetch_add(
							1, std::memory_order_relaxed);
					}
				}
				pickedClient->refs.fetch_sub(1, std::memory_order_acq_rel);
			}
			++step;
			std::this_thread::yield();
		}
	};

	std::vector<std::thread> threads;
	for (uint32_t c = 0; c < kClients; ++c)
		threads.emplace_back(runClient, c);
	std::thread reaper0(runReaper, 0);
	std::thread reaper1(runReaper, 1);
	for (auto &t : threads)
		t.join();
	global.stop.store(true, std::memory_order_release);
	reaper0.join();
	reaper1.join();

	// Drain all remaining parked BOs just like clientClose/drainResources
	for (auto &cl : clients) {
		std::lock_guard<std::mutex> lg(cl.lock);
		for (auto &bo : cl.bos) {
			EXPECT_FALSE(switch_gpu_park_is_transient(bo.state));
			std::lock_guard<std::mutex> pg(global.lock);
			global.lru.remove(&bo.node);
		}
		EXPECT_EQ(cl.refs.load(std::memory_order_acquire), 1u);
	}
	EXPECT_TRUE(global.lru.empty());
	EXPECT_EQ(global.lru.totalBytes(), 0u);
	EXPECT_GT(global.parkedCount.load(), 0u);
	EXPECT_GT(global.reclaimedCount.load(), 0u);
}

TEST(SwitchGpuParkedConcurrency, BoUnparkWaitsForReclaimingAndReturnsReclaimed)
{
	std::mutex lock;
	std::condition_variable parkWait;
	SwitchGpuParkState state = SwitchGpuParkState::Reclaiming;
	bool closed = false;
	bool freeing = false;
	bool waiterEntered = false;

	SwitchGpuParkAction result = SwitchGpuParkAction::InvalidState;
	std::thread unparker([&] {
		std::unique_lock<std::mutex> lg(lock);
		if (state == SwitchGpuParkState::Reclaiming) {
			waiterEntered = true;
			parkWait.notify_all();
			parkWait.wait(lg, [&] {
				return closed || !switch_gpu_park_is_transient(state);
			});
		}
		if (!closed && !freeing)
			result = switch_gpu_park_begin_unpark(state);
	});

	{
		std::unique_lock<std::mutex> lg(lock);
		parkWait.wait(lg, [&] {
			return waiterEntered;
		});
		EXPECT_EQ(switch_gpu_park_finish_reclaim(state), SwitchGpuParkAction::Ok);
		state = SwitchGpuParkState::Reclaimed;
		parkWait.notify_all();
	}
	unparker.join();
	EXPECT_EQ(result, SwitchGpuParkAction::Reclaimed);

	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Parking),
		  SwitchGpuParkAction::Busy);
	EXPECT_EQ(switch_gpu_park_begin_unpark(SwitchGpuParkState::Unparking),
		  SwitchGpuParkAction::Busy);
}
