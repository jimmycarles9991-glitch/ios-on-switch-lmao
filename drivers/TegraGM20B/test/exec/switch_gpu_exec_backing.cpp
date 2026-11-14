// SPDX-License-Identifier: MIT
#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_backing.h"
#include <map>
#include <mutex>
#include <utility>
#include <gtest/gtest.h>

namespace
{
using Key = std::pair<uint32_t, uint64_t>;

// Each GM20B timeline's completed value; while `failed`, all other work failed (a test's end).
std::map<Key, uint32_t> completed;
bool failed;
std::mutex timeline_lock;
uint64_t next_page = 0x80000000;
bool retirement_scheduled;

Key key(const switch_gpu_backing_token *token)
{
	return {token->queue, token->reset_generation};
}

int status(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	if (int32_t(completed[key(token)] - uint32_t(token->value)) >= 0)
		return 1;
	return failed ? -5 : 0;
}

int wait(const switch_gpu_backing_token *token)
{
	ADD_FAILURE() << "a CPU wait on GM20B queue " << token->queue << " value " << token->value;
	return status(token);
}

int retain(const switch_gpu_backing_token *)
{
	return 0;
}

void release(const switch_gpu_backing_token *)
{
}
}  // namespace

extern "C" {
struct tgm_coh;
struct switch_gpu_cpu_lease;

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

void switch_gpu_backing_retire(void);

int switch_gpu_backing_prepare_retirement(void)
{
	return 0;
}

void switch_gpu_backing_schedule_retirement(void)
{
	retirement_scheduled = true;
}

uint64_t switch_gpu_backing_time_ns(void)
{
	return 100;
}

void switch_gpu_backing_arm_expiry(uint64_t)
{
}

void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t, uint64_t, int)
{
}
}

namespace switch_exec_backing
{
void register_engines()
{
	static bool registered;
	if (registered)
		return;
	const switch_gpu_backing_engine operations = {status, wait, retain, release, 32};
	ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_GM20B, &operations), 0);
	registered = true;
}

switch_gpu_backing *make()
{
	const uint64_t page = next_page += 4096;
	return switch_gpu_backing_register(&page, 1, 0, nullptr, nullptr);
}

switch_gpu_backing_token token(uint64_t channel_id, uint32_t value)
{
	return {value, (channel_id >> 32) + 1, SWITCH_GPU_ENGINE_GM20B, uint32_t(channel_id)};
}

void complete(const switch_gpu_backing_token &token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	completed[key(&token)] = uint32_t(token.value);
}

void publish_write(switch_gpu_backing *backing, const switch_gpu_backing_token &token)
{
	switch_gpu_backing_use use = {backing, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &token, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
}

uint32_t pending(switch_gpu_backing *backing)
{
	uint32_t engines = 0, owners = 0;
	EXPECT_EQ(switch_gpu_backing_pending(backing, SWITCH_GPU_BACKING_READ, &engines, &owners),
		  0);
	return engines;
}

void retire_all()
{
	{
		std::lock_guard<std::mutex> held(timeline_lock);
		failed = true;
	}
	do {
		retirement_scheduled = false;
		switch_gpu_backing_retire();
	} while (retirement_scheduled);
	std::lock_guard<std::mutex> held(timeline_lock);
	failed = false;
	completed.clear();
}
}  // namespace switch_exec_backing
