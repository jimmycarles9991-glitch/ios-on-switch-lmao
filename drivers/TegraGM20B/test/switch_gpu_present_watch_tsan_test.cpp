// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuPresentWatch.h"

namespace
{
constexpr uint32_t kProducers = SWITCH_GPU_PRESENT_WATCH_MAX;
std::atomic<uint32_t> gCompleted[kProducers];
std::atomic<int> gExternal[kProducers];

int completedStatus(const switch_gpu_backing_token *token)
{
	const uint32_t slot = token->queue - 1u;
	if (slot >= kProducers)
		return -19;
	return int32_t(gCompleted[slot].load(std::memory_order_acquire) - uint32_t(token->value)) >=
	       0;
}

int externalStatus(const switch_gpu_backing_token *token)
{
	const uint32_t slot = token->queue - 1u;
	if (slot >= kProducers)
		return -19;
	return gExternal[slot].load(std::memory_order_acquire);
}

TEST(PresentWatchTsan, MultiProducerConcurrentPassesClaimsAndReuseDeliverExactlyOnce)
{
	switch_gpu_present_watch watch = {};
	constexpr uint32_t kFrames = 1200;
	std::atomic<bool> stop{false};
	std::atomic<uint32_t> frame{0};
	std::atomic<uint32_t> delivered{0};
	std::atomic<uint32_t> frameDelivered{0};

	for (uint32_t i = 0; i < kProducers; ++i) {
		gCompleted[i].store(0, std::memory_order_relaxed);
		gExternal[i].store(1, std::memory_order_relaxed);
	}

	auto finishClaim = [&] {
		const uint32_t count = frameDelivered.fetch_add(1, std::memory_order_acq_rel) + 1;
		EXPECT_EQ(count, 1u);
		switch_gpu_present_watch_reset(&watch);
		delivered.fetch_add(1, std::memory_order_release);
	};

	std::vector<std::thread> workers;
	for (uint32_t p = 0; p < kProducers; ++p) {
		workers.emplace_back([&, p] {
			uint32_t seen = 0;
			while (!stop.load(std::memory_order_acquire)) {
				const uint32_t target = frame.load(std::memory_order_acquire);
				if (target == seen) {
					std::this_thread::yield();
					continue;
				}
				seen = target;
				gCompleted[p].store(target, std::memory_order_release);
				switch_gpu_present_watch_fenced(&watch, p + 1u, target);
				if (switch_gpu_present_watch_state(&watch) ==
					    SWITCH_GPU_PRESENT_WATCH_DUE &&
				    switch_gpu_present_watch_take_visible(&watch, completedStatus,
									  externalStatus)) {
					finishClaim();
				}
			}
		});
	}

	std::thread workItem([&] {
		while (!stop.load(std::memory_order_acquire)) {
			const int found = switch_gpu_present_watch_claim(&watch, completedStatus,
									 externalStatus);
			if (found != SWITCH_GPU_PRESENT_WATCH_PENDING)
				finishClaim();
			else
				std::this_thread::yield();
		}
	});

	for (uint32_t f = 1; f <= kFrames; ++f) {
		const uint32_t count = 1u + (f % kProducers);
		switch_gpu_backing_token tokens[kProducers];
		for (uint32_t i = 0; i < count; ++i)
			tokens[i] = {f, 1, SWITCH_GPU_ENGINE_GM20B, i + 1u};
		frameDelivered.store(0, std::memory_order_relaxed);
		const uint64_t gen = switch_gpu_present_watch_arm_many(&watch, tokens, count);
		frame.store(f, std::memory_order_release);
		if (switch_gpu_present_watch_check(&watch, gen, tokens, count, completedStatus)) {
			const int found = switch_gpu_present_watch_claim(&watch, completedStatus,
									 externalStatus);
			if (found != SWITCH_GPU_PRESENT_WATCH_PENDING)
				finishClaim();
		}
		while (delivered.load(std::memory_order_acquire) < f)
			std::this_thread::yield();
		EXPECT_EQ(frameDelivered.load(std::memory_order_acquire), 1u);
	}

	stop.store(true, std::memory_order_release);
	for (auto &t : workers)
		t.join();
	workItem.join();
	EXPECT_EQ(delivered.load(std::memory_order_acquire), kFrames);
	EXPECT_FALSE(switch_gpu_present_watch_armed(&watch));
}

TEST(PresentWatchTsan, PromiseKeptRearmRacingClaimNeverReturnsVisibleForHiddenJob)
{
	switch_gpu_present_watch watch = {};
	constexpr uint32_t kRounds = 600;
	std::atomic<bool> stop{false};
	std::atomic<uint32_t> done{0};
	std::atomic<uint32_t> visibleClaims{0};

	static std::atomic<uint32_t> gPromiseWord{0};
	static std::atomic<uint32_t> gJobFence{0};
	gPromiseWord.store(0, std::memory_order_relaxed);
	gJobFence.store(0, std::memory_order_relaxed);

	auto completedFn = [](const switch_gpu_backing_token *token) -> int {
		if (token->engine == SWITCH_GPU_ENGINE_PROMISE)
			return int32_t(gPromiseWord.load(std::memory_order_acquire) -
				       uint32_t(token->value)) >= 0;
		return int32_t(gJobFence.load(std::memory_order_acquire) -
			       uint32_t(token->value)) >= 0;
	};
	auto externalFn = [](const switch_gpu_backing_token *token) -> int {
		if (token->engine == SWITCH_GPU_ENGINE_PROMISE)
			return int32_t(gPromiseWord.load(std::memory_order_acquire) -
				       uint32_t(token->value)) >= 0;
		return 0;
	};

	std::thread claimer([&] {
		while (!stop.load(std::memory_order_acquire)) {
			const int found =
				switch_gpu_present_watch_claim(&watch, completedFn, externalFn);
			if (found == SWITCH_GPU_PRESENT_WATCH_PENDING) {
				std::this_thread::yield();
				continue;
			}
			if (found == SWITCH_GPU_PRESENT_WATCH_VISIBLE)
				visibleClaims.fetch_add(1, std::memory_order_relaxed);
			switch_gpu_present_watch_reset(&watch);
			done.fetch_add(1, std::memory_order_release);
		}
	});

	for (uint32_t r = 1; r <= kRounds; ++r) {
		const switch_gpu_backing_token promise = {r, 0, SWITCH_GPU_ENGINE_PROMISE, 2052};
		const switch_gpu_backing_token job = {r, 1, SWITCH_GPU_ENGINE_GM20B, 1};
		const uint64_t pgen = switch_gpu_present_watch_promise(&watch, &promise);
		if (switch_gpu_present_watch_kept(&watch, 2052, r, &job)) {
			switch_gpu_present_watch_rearm(&watch);
			gJobFence.store(r, std::memory_order_release);
			gPromiseWord.store(r, std::memory_order_release);
			switch_gpu_present_watch_fenced(&watch, 1u, r);
			EXPECT_FALSE(switch_gpu_present_watch_check(&watch, pgen, &promise, 1,
								    completedFn));
		} else {
			gPromiseWord.store(r, std::memory_order_release);
		}
		while (done.load(std::memory_order_acquire) < r)
			std::this_thread::yield();
	}

	stop.store(true, std::memory_order_release);
	claimer.join();
	EXPECT_EQ(visibleClaims.load(std::memory_order_relaxed), 0u);
	EXPECT_FALSE(switch_gpu_present_watch_armed(&watch));
}
}  // namespace
