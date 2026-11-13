// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include "drivers/TegraGM20B/SwitchGpuSurfaceRefresh.h"

struct switch_gpu_backing {
	unsigned refs = 0;
};

struct switch_gpu_cpu_lease {
	switch_gpu_backing *backing;
};

namespace
{
struct Surface {
	switch_gpu_backing *backing;
	volatile uint64_t *generation;
	int error = 0;
	unsigned tickets = 0;
	bool detach = false;
};

unsigned prepared, released, lastAccess;
switch_gpu_backing *redirect;

class SurfaceRefresh : public testing::Test
{
protected:
	switch_gpu_backing backing;
	volatile uint64_t generation = 2;
	uint64_t cache[2] = {1, 1};
	Surface source{&backing, &generation};
	std::array<SwitchGpuSurfaceRefresh, 2> uses;
	switch_gpu_backing_guard guards[2];
	switch_gpu_cpu_lease *leases[2];
	uint32_t leaseCount;

	void SetUp() override
	{
		prepared = released = lastAccess = 0;
		redirect = nullptr;
		for (uint32_t i = 0; i < uses.size(); ++i)
			uses[i] = {&backing,  &source, &generation,
				   &cache[i], 8,       SWITCH_GPU_BACKING_READ,
				   0,         nullptr, nullptr,
				   nullptr};
	}

	void TearDown() override
	{
		SwitchGpuSurfaceRefresh::release(uses.data(), uint32_t(uses.size()));
		EXPECT_EQ(backing.refs, 0u);
		EXPECT_EQ(source.tickets, 0u);
	}

	int prepare()
	{
		return SwitchGpuSurfaceRefresh::prepare(uses.data(), uint32_t(uses.size()), nullptr,
							guards, leases, &leaseCount);
	}
};
}

extern "C" int switch_gpu_surface_prepare_gpu(void *object, uint32_t, void *, uint32_t access,
					      switch_gpu_backing **backing,
					      switch_gpu_cpu_lease **owned, void **descriptor)
{
	auto *source = static_cast<Surface *>(object);
	++prepared;
	lastAccess = access;
	if (source->error)
		return source->error;
	*backing = redirect ? redirect : source->backing;
	++(*backing)->refs;
	*owned = new switch_gpu_cpu_lease{*backing};
	++(*backing)->refs;
	++source->tickets;
	*descriptor = source;
	if (source->detach)
		__atomic_add_fetch(source->generation, uint64_t(1), __ATOMIC_RELEASE);
	return 0;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
	--backing->refs;
}

extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **lease, int wrote)
{
	EXPECT_EQ(wrote, 0);
	--(*lease)->backing->refs;
	delete *lease;
	*lease = nullptr;
	++released;
	return 0;
}

extern "C" void switch_gpu_surface_finish(void *descriptor)
{
	--static_cast<Surface *>(descriptor)->tickets;
}

TEST_F(SurfaceRefresh, WarmOriginalPitchUsesOnlyGenerationChecks)
{
	generation = 1;
	ASSERT_EQ(prepare(), 0);
	EXPECT_EQ(prepared, 0u);
	EXPECT_EQ(leaseCount, 0u);
	EXPECT_EQ(guards[0].expected, 1u);
	EXPECT_EQ(guards[1].expected, 1u);
}

TEST_F(SurfaceRefresh, AliasesShareOneOwnedReservationAndRetainItThroughPublication)
{
	ASSERT_EQ(prepare(), 0);
	EXPECT_EQ(prepared, 1u);
	EXPECT_EQ(leaseCount, 1u);
	EXPECT_EQ(cache[0], 2u);
	EXPECT_EQ(cache[1], 2u);
	EXPECT_EQ(source.tickets, 1u);
	EXPECT_EQ(backing.refs, 2u);
	EXPECT_EQ(released, 0u);
}

TEST_F(SurfaceRefresh, SameBackingWriteIsKnownBeforeFirstMaterialization)
{
	uses[1].access = SWITCH_GPU_BACKING_WRITE;
	ASSERT_EQ(prepare(), 0);
	EXPECT_EQ(prepared, 1u);
	EXPECT_NE(lastAccess & SWITCH_GPU_BACKING_WRITE, 0u);
}

TEST_F(SurfaceRefresh, MutableUseCapturesDetachedGeneration)
{
	cache[0] = cache[1] = 2;
	uses[0].access = uses[1].access = SWITCH_GPU_BACKING_WRITE;
	source.detach = true;
	ASSERT_EQ(prepare(), 0);
	EXPECT_EQ(prepared, 1u);
	EXPECT_EQ(generation, 3u);
	EXPECT_EQ(guards[0].expected, 3u);
	EXPECT_EQ(guards[1].expected, 3u);
}

TEST_F(SurfaceRefresh, FailureKeepsEarlierReservationUntilCallerUnwinds)
{
	switch_gpu_backing allocations[2];
	volatile uint64_t otherGeneration = 4;
	Surface first{&allocations[0], &generation}, second{&allocations[1], &otherGeneration, -5};
	uses[0].backing = &allocations[1];
	uses[0].surface = &second;
	uses[0].generation = &otherGeneration;
	uses[1].backing = &allocations[0];
	uses[1].surface = &first;
	EXPECT_EQ(prepare(), -5);
	EXPECT_EQ(first.tickets, 1u);
	SwitchGpuSurfaceRefresh::release(uses.data(), uint32_t(uses.size()));
	EXPECT_EQ(released, 1u);
	EXPECT_EQ(allocations[0].refs, 0u);
	EXPECT_EQ(allocations[1].refs, 0u);
}

TEST_F(SurfaceRefresh, DifferentCanonicalBackingFailsWithoutLeakingReturnedLease)
{
	switch_gpu_backing other;
	redirect = &other;
	EXPECT_EQ(prepare(), -95);
	SwitchGpuSurfaceRefresh::release(uses.data(), uint32_t(uses.size()));
	EXPECT_EQ(other.refs, 0u);
}

TEST_F(SurfaceRefresh, DistinctRepresentationWordsCannotShareAnExclusiveDiscount)
{
	volatile uint64_t otherGeneration = 5;
	uses[1].generation = &otherGeneration;
	EXPECT_EQ(prepare(), -95);
	EXPECT_EQ(prepared, 1u);
}

TEST_F(SurfaceRefresh, WarmNativePitchReadDoesNotPrepareAgain)
{
	cache[0] = cache[1] = 2;
	ASSERT_EQ(prepare(), 0);
	EXPECT_EQ(prepared, 0u);
	EXPECT_EQ(leaseCount, 0u);
	generation = 3;
	EXPECT_EQ(guards[0].expected, 2u);
	ASSERT_EQ(prepare(), 0);
	EXPECT_EQ(prepared, 1u);
	EXPECT_EQ(guards[0].expected, 3u);
}
