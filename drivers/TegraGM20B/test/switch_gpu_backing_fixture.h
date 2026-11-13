#pragma once
// SPDX-License-Identifier: MIT
#include <array>
#include <atomic>
#include <map>
#include <future>
#include <functional>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>
#include <gtest/gtest.h>
#include <IOKit/IOUserClient.h>
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/SwitchGpuNativeUse.h"

extern "C" {
struct tgm_coh;

tgm_coh *tgm_coh_find_pa(uint64_t)
{
	return nullptr;
}

void tgm_views_sweep(tgm_coh *)
{
}

void tgm_views_deferred_lock(uint32_t, uint32_t)
{
}

void tgm_coherency_put(tgm_coh *)
{
}

int tegra_gm20b_surface_tiled(uint64_t)
{
	return 0;
}

int switch_gpu_surface_prepare(uint32_t id, void *, uint32_t, switch_gpu_backing **backing,
			       switch_gpu_cpu_lease **owned, void **descriptor)
{
	*owned = nullptr;
	*descriptor = nullptr;
	*backing = switch_gpu_backing_find_surface(id);
	return 0;
}

void switch_gpu_surface_finish(void *)
{
}

uint32_t switch_gpu_surface_users(void *)
{
	return 0;
}

void switch_gpu_surface_descriptor_hold(void *, int)
{
}

int switch_gpu_surface_lock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_failed(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_sweep(void);
}

namespace
{
using Key = std::tuple<uint32_t, uint32_t, uint64_t>;

struct Timeline {
	uint64_t completed = 0;
	int error = 0, refs = 0;
};

std::map<Key, Timeline> timelines;
std::map<Key, uint64_t> external_completed;
unsigned external_waits;
std::mutex timeline_lock;

struct Cache {
	uint64_t offset, bytes;
	int invalidate;
};

std::vector<Cache> cache;
std::function<void(uint64_t, uint64_t, int)> cache_callback;
uint64_t next_page = 0x80000000;
bool retirement_scheduled;

Key key(const switch_gpu_backing_token *token)
{
	return {token->engine, token->queue, token->reset_generation};
}

int status(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	const auto &timeline = timelines[key(token)];
	const bool reached = token->engine == SWITCH_GPU_ENGINE_VIC
				     ? int32_t(uint32_t(timeline.completed - token->value)) >= 0
				     : timeline.completed >= token->value;
	return reached ? 1 : timeline.error;
}

int wait(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	auto &timeline = timelines[key(token)];
	if (timeline.error)
		return timeline.error;
	timeline.completed = token->value;
	return 1;
}

[[maybe_unused]] int external_status(const switch_gpu_backing_token *token)
{
	const int executed = status(token);
	if (executed != 1)
		return executed;
	const auto at = external_completed.find(key(token));
	return at == external_completed.end() || at->second >= token->value ? 1 : 0;
}

[[maybe_unused]] int external_wait(const switch_gpu_backing_token *token)
{
	++external_waits;
	const int executed = wait(token);
	if (executed == 1)
		external_completed[key(token)] = token->value;
	return executed;
}

int retain(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	++timelines[key(token)].refs;
	return 0;
}

void release(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	EXPECT_GT(timelines[key(token)].refs, 0);
	--timelines[key(token)].refs;
}

switch_gpu_backing_token token(uint32_t queue, uint64_t value = 1,
			       uint32_t engine = SWITCH_GPU_ENGINE_GM20B, uint64_t generation = 1)
{
	return {value, generation, engine, queue};
}

// The channel of `of` killed: the work it had not finished reports failure.
[[maybe_unused]] void kill_channel(const switch_gpu_backing_token &of)
{
	timelines[key(&of)].error = -5;
}

[[maybe_unused]] void complete(const switch_gpu_backing_token &of)
{
	timelines[key(&of)].completed = of.value;
}

[[maybe_unused]] int refs(const switch_gpu_backing_token &of)
{
	return timelines[key(&of)].refs;
}

switch_gpu_backing *backing(uint32_t count = 1)
{
	std::vector<uint64_t> pages;
	for (uint32_t i = 0; i < count; ++i)
		pages.push_back(next_page += 4096);
	return switch_gpu_backing_register(pages.data(), count, 0, nullptr, nullptr);
}

int publish(switch_gpu_backing *allocation, uint32_t access, switch_gpu_backing_token completion)
{
	switch_gpu_backing_use use = {allocation, access, 0};
	switch_gpu_backing_transaction transaction = {};
	const int result = switch_gpu_backing_begin(&use, 1, &completion, &transaction);
	if (!result)
		switch_gpu_backing_commit(&transaction);
	return result;
}

class Backing : public testing::Test
{
protected:
	void SetUp() override
	{
		static bool registered;
		if (!registered) {
			const switch_gpu_backing_engine gm = {status, wait, retain, release, 64};
			const switch_gpu_backing_engine vic = {status, wait, retain, release, 32};
			ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_GM20B, &gm),
				  0);
			ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_NVDEC, &gm),
				  0);
			ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_VIC, &vic),
				  0);
			registered = true;
		}
		timelines.clear();
		cache.clear();
		cache_callback = {};
		retirement_scheduled = false;
	}

	void TearDown() override;
};
}  // namespace

extern "C" void switch_gpu_backing_retire(void);

extern "C" int switch_gpu_backing_prepare_retirement(void)
{
	return 0;
}

extern "C" void switch_gpu_backing_schedule_retirement(void)
{
	retirement_scheduled = true;
}

extern "C" uint64_t switch_gpu_backing_time_ns(void)
{
	return 100;
}

extern "C" void switch_gpu_backing_arm_expiry(uint64_t)
{
}

extern "C" void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t offset,
					 uint64_t bytes, int invalidate)
{
	cache.push_back({offset, bytes, invalidate});
	if (cache_callback)
		cache_callback(offset, bytes, invalidate);
}

void Backing::TearDown()
{
	for (auto &entry : timelines)
		entry.second.error = -5;
	do {
		retirement_scheduled = false;
		switch_gpu_backing_retire();
	} while (retirement_scheduled);
	for (const auto &entry : timelines)
		EXPECT_EQ(entry.second.refs, 0);
}
