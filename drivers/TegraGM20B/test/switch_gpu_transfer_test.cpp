// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <IOKit/IOUserClient.h>
#include <vector>
#include "bazel/testing/require.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kNoCache = SWITCH_GPU_BACKING_CPU_NO_CACHE,
		   kConcurrent = SWITCH_GPU_BACKING_CPU_CONCURRENT;
constexpr uint32_t kAvoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
uint64_t nextPage = 0xe0000000, completed;
unsigned tokenRefs, waits, descriptorUsers;
switch_gpu_backing *surfaceBacking;
bool materialize, importDuringPrepare;
std::vector<int> cacheOps;
const switch_gpu_backing_token completion{1, 1, SWITCH_GPU_ENGINE_VIC, 1};

int status(const switch_gpu_backing_token *token)
{
	return completed >= token->value ? 1 : 0;
}

int wait(const switch_gpu_backing_token *token)
{
	++waits;
	completed = token->value;
	return 1;
}

int retain(const switch_gpu_backing_token *)
{
	++tokenRefs;
	return 0;
}

void release(const switch_gpu_backing_token *)
{
	REQUIRE(tokenRefs);
	--tokenRefs;
}

switch_gpu_backing *make()
{
	const uint64_t page = nextPage += 4096;
	auto *backing = switch_gpu_backing_register(&page, 1, 0, nullptr, nullptr);
	REQUIRE(backing);
	return backing;
}

switch_gpu_cpu_lease *own(switch_gpu_backing *backing, uint32_t access = kWrite | kNoCache,
			  uint64_t bytes = 4096)
{
	switch_gpu_cpu_lease *lease = nullptr;
	REQUIRE(switch_gpu_backing_cpu_acquire_owned(backing, 0, bytes, access, &lease) == 0);
	return lease;
}

void convert(switch_gpu_backing *backing, switch_gpu_cpu_lease *lease)
{
	switch_gpu_backing_use use{backing, kWrite, 0};
	switch_gpu_backing_transaction job{};
	REQUIRE(switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &job) == 0);
	switch_gpu_backing_commit(&job);
}
}

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

int switch_gpu_backing_prepare_retirement()
{
	return 0;
}

void switch_gpu_backing_schedule_retirement()
{
}

void switch_gpu_backing_arm_expiry(uint64_t)
{
}

uint64_t switch_gpu_backing_time_ns()
{
	return 100;
}

void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t, uint64_t, int op)
{
	cacheOps.push_back(op);
}

void switch_gpu_backing_retire();

int switch_gpu_surface_adopt(void *, switch_gpu_backing *);

int switch_gpu_surface_prepare(uint32_t, void *, uint32_t, switch_gpu_backing **backing,
			       switch_gpu_cpu_lease **owned, void **descriptor)
{
	++descriptorUsers;
	*descriptor = &descriptorUsers;
	*backing = surfaceBacking;
	*owned = nullptr;
	if (importDuringPrepare) {
		surfaceBacking = make();
		REQUIRE(switch_gpu_surface_adopt(&descriptorUsers, surfaceBacking) == 0);
	}
	if (*backing) {
		switch_gpu_backing_retain(*backing);
		if (materialize) {
			*owned = own(*backing);
			convert(*backing, *owned);
		}
	}
	return 0;
}

void switch_gpu_surface_idle(void *descriptor);
int switch_gpu_surface_adopt(void *, switch_gpu_backing *);

uint32_t switch_gpu_surface_users(void *)
{
	return descriptorUsers;
}

void switch_gpu_surface_descriptor_hold(void *, int)
{
}

void switch_gpu_surface_finish(void *descriptor)
{
	REQUIRE(descriptor == &descriptorUsers);
	REQUIRE(descriptorUsers);
	--descriptorUsers;
	switch_gpu_surface_idle(descriptor);
}

int switch_gpu_surface_lock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_failed(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_sweep();
}

namespace
{
class Transfer : public testing::Test
{
protected:
	void SetUp() override
	{
		static bool registered;
		if (!registered) {
			const switch_gpu_backing_engine engine{status, wait, retain, release, 64};
			ASSERT_EQ(
				switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_VIC, &engine),
				0);
			registered = true;
		}
		ASSERT_EQ(tokenRefs, 0u);
		ASSERT_EQ(descriptorUsers, 0u);
		completed = waits = 0;
		surfaceBacking = nullptr;
		materialize = importDuringPrepare = false;
		cacheOps.clear();
	}

	void TearDown() override
	{
		completed = UINT64_MAX;
		switch_gpu_backing_retire();
		EXPECT_EQ(tokenRefs, 0u);
		EXPECT_EQ(descriptorUsers, 0u);
	}
};
}

TEST_F(Transfer, ConvertedWriterBecomesReaderWithoutAllowingReplacement)
{
	auto *backing = make();
	auto *lease = own(backing);
	convert(backing, lease);
	cacheOps.clear();
	ASSERT_EQ(switch_gpu_backing_cpu_transfer_owned(backing, &lease, kRead | kConcurrent), 0);
	EXPECT_EQ(lease, nullptr);
	EXPECT_EQ(waits, 1u);
	ASSERT_EQ(cacheOps.size(), 1u);
	EXPECT_EQ(cacheOps[0], SWITCH_GPU_CACHE_INVALIDATE);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kWrite | kAvoid), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(backing, 0, 4096, kRead | kConcurrent), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kWrite | kAvoid), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(backing, 0, 4096, kWrite | kAvoid), 0);
	switch_gpu_backing_release(backing);
}

TEST_F(Transfer, AvoidSyncFailurePreservesOwnedLeaseAndDoesNotTouchCache)
{
	auto *backing = make();
	auto *lease = own(backing);
	convert(backing, lease);
	cacheOps.clear();
	EXPECT_EQ(switch_gpu_backing_cpu_transfer_owned(backing, &lease, kRead | kAvoid), -16);
	EXPECT_NE(lease, nullptr);
	EXPECT_EQ(waits, 0u);
	EXPECT_TRUE(cacheOps.empty());
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kRead | kConcurrent | kAvoid),
		  -16);
	completed = 1;
	ASSERT_EQ(switch_gpu_backing_cpu_transfer_owned(backing, &lease, kRead), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(backing, 0, 4096, kRead), 0);
	switch_gpu_backing_release(backing);
}

TEST_F(Transfer, SharedReadHandoffRetainsEveryReaderAndRejectsWriteUpgrade)
{
	auto *backing = make();
	auto *one = own(backing, kRead | kNoCache), *two = own(backing, kRead | kNoCache);
	EXPECT_EQ(switch_gpu_backing_cpu_transfer_owned(backing, &one, kWrite), -22);
	ASSERT_EQ(switch_gpu_backing_cpu_transfer_owned(backing, &one, kRead | kConcurrent), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_transfer_owned(backing, &two, kRead | kConcurrent), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(backing, 0, 4096, kRead | kConcurrent), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kWrite | kAvoid), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(backing, 0, 4096, kRead | kConcurrent), 0);
	switch_gpu_backing_release(backing);
}

TEST_F(Transfer, PartialOrForeignLeasesCannotTransfer)
{
	auto *one = make(), *two = make();
	auto *lease = own(one, kWrite | kNoCache, 64);
	EXPECT_EQ(switch_gpu_backing_cpu_transfer_owned(one, &lease, kRead), -22);
	EXPECT_EQ(switch_gpu_backing_cpu_transfer_owned(two, &lease, kRead), -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(one);
	switch_gpu_backing_release(two);
}

TEST_F(Transfer, ConcurrentCpuLocksCannotBypassAnExclusiveWriter)
{
	auto *backing = make();
	auto *exclusive = own(backing);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kRead | kConcurrent | kAvoid),
		  -16);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kWrite | kConcurrent | kAvoid),
		  -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&exclusive, 0), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kWrite | kConcurrent), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kRead | kConcurrent), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(backing, 0, 4096, kRead | kConcurrent), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release(backing, 0, 4096, kWrite | kConcurrent), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(backing, 0, 4096, kWrite | kAvoid), 0);
	switch_gpu_backing_cpu_cancel(backing, kWrite | kAvoid);
	switch_gpu_backing_release(backing);
}

TEST_F(Transfer, UnimportedSurfaceKeepsDescriptorGateThroughAppleLockAndFailure)
{
	IOUserClient client;
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 1), 0);
	EXPECT_EQ(descriptorUsers, 1u);
	switch_gpu_surface_lock_did(&client, 1, 1);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 0), 0);
	EXPECT_EQ(descriptorUsers, 2u);
	switch_gpu_surface_lock_failed(&client, 1, 0);
	EXPECT_EQ(descriptorUsers, 1u);
	switch_gpu_surface_unlock_did(&client, 1, 1);
	EXPECT_EQ(descriptorUsers, 0u);
	EXPECT_EQ(client.refs, 1u);
}

TEST_F(Transfer, InactiveClientReleasesUnimportedDescriptorReservations)
{
	IOUserClient client;
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 0), 0);
	switch_gpu_surface_lock_did(&client, 1, 0);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 1), 0);
	client.inactive = true;
	switch_gpu_surface_sweep();
	EXPECT_EQ(descriptorUsers, 0u);
	EXPECT_EQ(client.refs, 1u);
}

TEST_F(Transfer, MaterializationLeaseTransfersThroughTheActualSurfaceLockPath)
{
	IOUserClient client;
	surfaceBacking = make();
	materialize = true;
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 1), 0);
	EXPECT_EQ(waits, 1u);
	EXPECT_EQ(descriptorUsers, 1u);
	switch_gpu_surface_lock_did(&client, 1, 1);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(surfaceBacking, 0, 4096, kWrite | kAvoid), -16);
	switch_gpu_surface_unlock_did(&client, 1, 1);
	EXPECT_EQ(descriptorUsers, 0u);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(surfaceBacking);
}

TEST_F(Transfer, ImportAdoptsOpenCpuLocksAndNestedLocksReleaseOnlyTheirOwnReservation)
{
	IOUserClient client;
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 0), 0);
	switch_gpu_surface_lock_did(&client, 1, 0);
	surfaceBacking = make();
	ASSERT_EQ(switch_gpu_surface_adopt(&descriptorUsers, surfaceBacking), 0);
	switch_gpu_backing_use use{surfaceBacking, kWrite, 0};
	switch_gpu_backing_transaction job{};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), -16);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 1), 0);
	switch_gpu_surface_lock_did(&client, 1, 1);
	switch_gpu_surface_unlock_did(&client, 1, 1);
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), -16);
	switch_gpu_surface_unlock_will(&client, 1, 0);
	switch_gpu_surface_unlock_did(&client, 1, 0);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), 0);
	switch_gpu_backing_commit(&job);
	EXPECT_EQ(descriptorUsers, 0u);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(surfaceBacking);
}

TEST_F(Transfer, ImportBetweenResolutionAndPendingInsertionIsAdoptedBeforeCpuExposure)
{
	IOUserClient client;
	importDuringPrepare = true;
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 0), 0);
	switch_gpu_surface_lock_did(&client, 1, 0);
	switch_gpu_backing_use use{surfaceBacking, kWrite, 0};
	switch_gpu_backing_transaction job{};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), -16);
	switch_gpu_surface_unlock_did(&client, 1, 0);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), 0);
	switch_gpu_backing_commit(&job);
	switch_gpu_backing_release(surfaceBacking);
}

TEST_F(Transfer, ImportAdoptsOtherClientsReadLocksAndPendingFailureBalancesOnlyItsOwn)
{
	IOUserClient first, second;
	ASSERT_EQ(switch_gpu_surface_lock_will(&first, 1, 1), 0);
	switch_gpu_surface_lock_did(&first, 1, 1);
	ASSERT_EQ(switch_gpu_surface_lock_will(&second, 2, 0), 0);
	surfaceBacking = make();
	ASSERT_EQ(switch_gpu_surface_adopt(&descriptorUsers, surfaceBacking), 0);
	switch_gpu_surface_lock_failed(&second, 2, 0);
	switch_gpu_backing_use use{surfaceBacking, kWrite, 0};
	switch_gpu_backing_transaction job{};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), -16);
	switch_gpu_surface_unlock_did(&first, 1, 1);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), 0);
	switch_gpu_backing_commit(&job);
	EXPECT_EQ(first.refs, 1u);
	EXPECT_EQ(second.refs, 1u);
	switch_gpu_backing_release(surfaceBacking);
}

TEST_F(Transfer, ASecondDescriptorExtentIsRefusedWithoutDroppingTheFirstReservation)
{
	IOUserClient client;
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 1, 0), 0);
	switch_gpu_surface_lock_did(&client, 1, 0);
	auto *one = make(), *two = make();
	ASSERT_EQ(switch_gpu_surface_adopt(&descriptorUsers, one), 0);
	EXPECT_EQ(switch_gpu_surface_adopt(&descriptorUsers, two), -95);
	switch_gpu_backing_use use{one, kWrite, 0};
	switch_gpu_backing_transaction job{};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &job), -16);
	switch_gpu_surface_unlock_did(&client, 1, 0);
	EXPECT_EQ(switch_gpu_surface_adopt(&descriptorUsers, two), 0);
	switch_gpu_backing_release(one);
	switch_gpu_backing_release(two);
}
