// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include "bazel/testing/require.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"

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

int switch_gpu_surface_prepare(uint32_t, void *, uint32_t, switch_gpu_backing **out,
			       switch_gpu_cpu_lease **owned, void **descriptor)
{
	*out = nullptr;
	*owned = nullptr;
	*descriptor = nullptr;
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

int switch_gpu_backing_prepare_retirement()
{
	return 0;
}

void switch_gpu_backing_schedule_retirement()
{
}

uint64_t switch_gpu_backing_time_ns()
{
	return 100;
}

void switch_gpu_backing_arm_expiry(uint64_t)
{
}

void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t, uint64_t, int)
{
}

void switch_gpu_backing_retire();
}

namespace
{
struct Timeline {
	uint64_t completed = 0;
	int refs = 0, error = 0;
};

std::map<uint32_t, Timeline> timelines;
std::mutex lock;
uint64_t nextPage = 0xc0000000;
constexpr uint32_t kWrite = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE;
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE;

int status(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(lock);
	const auto &state = timelines[token->queue];
	return state.completed >= token->value ? 1 : state.error;
}

int wait(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(lock);
	auto &state = timelines[token->queue];
	state.completed = token->value;
	return state.error ? state.error : 1;
}

int retain(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(lock);
	++timelines[token->queue].refs;
	return 0;
}

void release(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(lock);
	REQUIRE(timelines[token->queue].refs > 0);
	--timelines[token->queue].refs;
}

switch_gpu_backing *make()
{
	const uint64_t page = nextPage += 4096;
	auto *backing = switch_gpu_backing_register(&page, 1, 0, nullptr, nullptr);
	REQUIRE(backing);
	return backing;
}

switch_gpu_cpu_lease *acquire(switch_gpu_backing *backing, uint32_t access = kWrite,
			      uint64_t bytes = 4096)
{
	switch_gpu_cpu_lease *lease = nullptr;
	REQUIRE(switch_gpu_backing_cpu_acquire_owned(backing, 0, bytes, access, &lease) == 0);
	return lease;
}

int submit(switch_gpu_backing *backing, volatile uint64_t *word, uint64_t expected, uint32_t queue)
{
	switch_gpu_backing_use use{backing, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_guard guard{backing, word, expected};
	switch_gpu_backing_token completion{1, 1, SWITCH_GPU_ENGINE_GM20B, queue};
	switch_gpu_backing_transaction job{};
	const int result = switch_gpu_backing_begin_checked(&use, 1, &guard, 1, &completion, &job);
	if (!result)
		switch_gpu_backing_commit(&job);
	return result;
}

class Generation : public testing::Test
{
protected:
	void SetUp() override
	{
		static bool registered;
		if (!registered) {
			const switch_gpu_backing_engine engine{status, wait, retain, release, 64};
			ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_GM20B,
								     &engine),
				  0);
			registered = true;
		}
		timelines.clear();
	}

	void TearDown() override
	{
		for (auto &entry : timelines)
			entry.second.error = -5;
		switch_gpu_backing_retire();
		for (const auto &entry : timelines)
			EXPECT_EQ(entry.second.refs, 0);
	}
};
}

TEST_F(Generation, StaleGuardCannotPublishOrLeakPreparedTokens)
{
	auto *logical = make(), *native = make();
	volatile uint64_t generation = 2;
	switch_gpu_backing_use uses[]{{native, SWITCH_GPU_BACKING_READ, 0},
				      {logical, SWITCH_GPU_BACKING_READ, 0}};
	switch_gpu_backing_guard guard{logical, &generation, 1};
	switch_gpu_backing_token completion{1, 1, SWITCH_GPU_ENGINE_GM20B, 1};
	switch_gpu_backing_transaction job{};
	EXPECT_EQ(switch_gpu_backing_begin_checked(uses, 2, &guard, 1, &completion, &job),
		  SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(job.locked, 0u);
	EXPECT_EQ(timelines[1].refs, 0);
	guard.expected = 2;
	ASSERT_EQ(switch_gpu_backing_begin_checked(uses, 2, &guard, 1, &completion, &job), 0);
	switch_gpu_backing_commit(&job);
	EXPECT_EQ(timelines[1].refs, 4);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(logical, 0, 4096,
						 kWrite | SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	switch_gpu_backing_release(native);
	switch_gpu_backing_release(logical);
}

TEST_F(Generation, FullOwnedWriterAdvancesGenerationBeforeReleasingExclusion)
{
	auto *backing = make();
	volatile uint64_t generation = 1;
	auto *lease = acquire(backing);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(backing, lease, &generation, 1, 2), 0);
	EXPECT_EQ(submit(backing, &generation, 1, 1), SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(submit(backing, &generation, 2, 1), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(submit(backing, &generation, 2, 1), 0);
	EXPECT_EQ(submit(backing, &generation, 1, 2), SWITCH_GPU_BACKING_STALE);
	switch_gpu_backing_release(backing);
}

TEST_F(Generation, PublicationRejectsForeignForgedReadAndPartialLeases)
{
	auto *one = make(), *two = make();
	volatile uint64_t generation = 1;
	auto *writer = acquire(one);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(two, writer, &generation, 1, 2), -22);
	auto *forged = reinterpret_cast<switch_gpu_cpu_lease *>(uintptr_t(1));
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(one, forged, &generation, 1, 2), -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&writer, 0), 0);
	auto *reader = acquire(one, kRead);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(one, reader, &generation, 1, 2), -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&reader, 0), 0);
	auto *partial = acquire(one, kWrite, 64);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(one, partial, &generation, 1, 2), -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&partial, 0), 0);
	EXPECT_EQ(generation, 1u);
	switch_gpu_backing_release(two);
	switch_gpu_backing_release(one);
}

TEST_F(Generation, PublicationRequiresSoleOwnershipAndMonotonicExpectedValue)
{
	auto *backing = make();
	volatile uint64_t generation = 1;
	auto *one = acquire(backing, kWrite | SWITCH_GPU_BACKING_CPU_CONCURRENT);
	auto *two = acquire(backing, kWrite | SWITCH_GPU_BACKING_CPU_CONCURRENT);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(backing, one, &generation, 1, 2), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&two, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(backing, one, &generation, 2, 3),
		  SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(backing, one, &generation, 1, 1), -22);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(backing, one, &generation, 1, 0), -22);
	EXPECT_EQ(switch_gpu_backing_cpu_publish_generation(backing, one, &generation, 1, 2), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&one, 0), 0);
	switch_gpu_backing_release(backing);
}

TEST_F(Generation, DescriptorGenerationSurvivesLogicalBackingRetirement)
{
	const uint64_t page = nextPage += 4096;
	volatile uint64_t generation = 1;
	auto *old = switch_gpu_backing_register(&page, 1, 0, nullptr, nullptr);
	auto *writer = acquire(old);
	ASSERT_EQ(switch_gpu_backing_cpu_publish_generation(old, writer, &generation, 1, 2), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&writer, 0), 0);
	switch_gpu_backing_release(old);
	switch_gpu_backing_retire();
	auto *current = switch_gpu_backing_register(&page, 1, 0, nullptr, nullptr);
	ASSERT_NE(current, nullptr);
	EXPECT_EQ(submit(current, &generation, 1, 1), SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(submit(current, &generation, 2, 1), 0);
	switch_gpu_backing_release(current);
}

TEST_F(Generation, ConversionChecksGenerationAndActualLeaseMembership)
{
	auto *backing = make();
	volatile uint64_t generation = 3;
	auto *lease = acquire(backing);
	switch_gpu_backing_use use{backing, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_guard guard{backing, &generation, 2};
	switch_gpu_backing_token completion{1, 1, SWITCH_GPU_ENGINE_GM20B, 1};
	switch_gpu_backing_transaction job{};
	EXPECT_EQ(switch_gpu_backing_begin_conversion_checked(&use, 1, &lease, 1, &guard, 1,
							      &completion, &job),
		  SWITCH_GPU_BACKING_STALE);
	guard.expected = 3;
	ASSERT_EQ(switch_gpu_backing_begin_conversion_checked(&use, 1, &lease, 1, &guard, 1,
							      &completion, &job),
		  0);
	switch_gpu_backing_commit(&job);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	auto *forged = reinterpret_cast<switch_gpu_cpu_lease *>(uintptr_t(1));
	completion.queue = 2;
	EXPECT_EQ(switch_gpu_backing_begin_conversion_checked(&use, 1, &forged, 1, &guard, 1,
							      &completion, &job),
		  -22);
	EXPECT_EQ(timelines[2].refs, 0);
	switch_gpu_backing_release(backing);
}

TEST_F(Generation, GuardMustNameALockedBackingAndAlignedNonzeroStamp)
{
	auto *one = make(), *two = make();
	volatile uint64_t generation = 1;
	switch_gpu_backing_use use{one, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_guard guard{two, &generation, 1};
	switch_gpu_backing_token completion{1, 1, SWITCH_GPU_ENGINE_GM20B, 1};
	switch_gpu_backing_transaction job{};
	EXPECT_EQ(switch_gpu_backing_begin_checked(&use, 1, &guard, 1, &completion, &job), -22);
	guard.backing = one;
	guard.expected = 0;
	EXPECT_EQ(switch_gpu_backing_begin_checked(&use, 1, &guard, 1, &completion, &job), -22);
	guard.expected = 1;
	guard.generation = reinterpret_cast<const volatile uint64_t *>(uintptr_t(1));
	EXPECT_EQ(switch_gpu_backing_begin_checked(&use, 1, &guard, 1, &completion, &job), -22);
	switch_gpu_backing_release(two);
	switch_gpu_backing_release(one);
}
