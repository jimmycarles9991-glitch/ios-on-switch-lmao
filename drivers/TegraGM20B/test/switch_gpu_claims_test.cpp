// SPDX-License-Identifier: MIT
// A SubmitJob's uses (SwitchGpuClaims.h) over the ownership layer: every use it names is a claim no CPU owner refuses,
// unless an admission ordered the job behind the CPU owners, and then only for its writes.
#include <array>
#include <utility>
#include <vector>
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuClaims.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kAvoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
constexpr uint32_t kBesideCpu = SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;

// A SubmitJob on `completion`: every use named, with its TegraGM20BSurface* access.
int native_submit(switch_gpu_backing_token completion,
		  std::vector<std::pair<switch_gpu_backing *, uint32_t>> named)
{
	std::vector<switch_gpu_backing_use> uses;
	for (const auto &[allocation, access] : named)
		uses.push_back({allocation, access | switch_gpu_claim_flags(false), 0});
	switch_gpu_backing_transaction transaction = {};
	int result = switch_gpu_backing_begin(uses.data(), uint32_t(uses.size()), &completion,
					      &transaction);
	std::array<switch_gpu_backing_token, 8> dependencies = {};
	uint32_t needed = 0;
	if (!result)
		result = switch_gpu_backing_dependencies(&transaction, dependencies.data(), 8,
							 &needed);
	if (!result)
		switch_gpu_backing_commit(&transaction);
	else if (transaction.locked)
		switch_gpu_backing_abort(&transaction);
	return result;
}
}  // namespace

TEST(Claims, NativeUsesAreClaimsNoCpuOwnerRefusesUnlessAdmitted)
{
	EXPECT_EQ(switch_gpu_claim_flags(false), kBesideCpu);
	EXPECT_EQ(switch_gpu_claim_flags(true), 0u);
}

// An admitted job's read goes on beside CPU owners as any job's does; only its write keeps the exclusion
TEST_F(Backing, AnAdmittedReadGoesOnBesideACpuWriter)
{
	auto *vertices = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(vertices, 0, 4096, kWrite), 0);
	const uint32_t admitted = switch_gpu_claim_flags(true);
	EXPECT_EQ(switch_gpu_claim_use(admitted, kWrite), 0u);
	switch_gpu_backing_use use = {vertices, kRead | switch_gpu_claim_use(admitted, kRead), 0};
	switch_gpu_backing_transaction transaction = {};
	const auto job = token(1);
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &job, &transaction), 0);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(switch_gpu_backing_cpu_release(vertices, 0, 4096, kWrite), 0);
	switch_gpu_backing_release(vertices);
}

// A submission that took an admission was ordered behind CPU owners by readiness: begin keeps refusing a holder.
TEST_F(Backing, AdmittedNativeSubmitKeepsCpuExclusion)
{
	auto *vertices = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(vertices, 0, 4096, kWrite), 0);
	switch_gpu_backing_use use = {vertices, kRead | switch_gpu_claim_flags(true), 0};
	switch_gpu_backing_transaction transaction = {};
	const auto job = token(1);
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &job, &transaction), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_release(vertices, 0, 4096, kWrite), 0);
	switch_gpu_backing_release(vertices);
}

// IOSurfaceLock in another process while backboardd's raw submission names the surface.
TEST_F(Backing, IOSurfaceLocksNeverRefuseANativeSubmit)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *surface = switch_gpu_backing_register(&page, 1, 802, nullptr, nullptr);
	auto *push = backing();
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 802, 1), 0);
	switch_gpu_surface_lock_did(&client, 802, 1);
	const auto writer = token(1);
	EXPECT_EQ(native_submit(writer,
				{{surface, TegraGM20BSurfaceWrite}, {push, TegraGM20BSurfaceRead}}),
		  0);
	switch_gpu_surface_unlock_will(&client, 802, 1);
	switch_gpu_surface_unlock_did(&client, 802, 1);
	EXPECT_EQ(switch_gpu_surface_lock_will(&client, 802, 1 | 2), -16);
	complete(writer);
	size_t before = cache.size();
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 802, 1 | 2), 0);
	switch_gpu_surface_lock_did(&client, 802, 1 | 2);
	ASSERT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	switch_gpu_surface_unlock_will(&client, 802, 1 | 2);
	switch_gpu_surface_unlock_did(&client, 802, 1 | 2);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 802, 0), 0);
	switch_gpu_surface_lock_did(&client, 802, 0);
	before = cache.size();
	EXPECT_EQ(native_submit(token(1, 2),
				{{surface, TegraGM20BSurfaceRead}, {push, TegraGM20BSurfaceRead}}),
		  0);
	EXPECT_EQ(native_submit(token(1, 3), {{surface, TegraGM20BSurfaceRead}}), 0);
	switch_gpu_surface_unlock_will(&client, 802, 0);
	switch_gpu_surface_unlock_did(&client, 802, 0);
	ASSERT_EQ(cache.size(), before + 3);
	for (size_t i = before; i < cache.size(); ++i) {
		EXPECT_EQ(cache[i].invalidate, SWITCH_GPU_CACHE_CLEAN);
		EXPECT_EQ(cache[i].offset, 0u);
		EXPECT_EQ(cache[i].bytes, 4096u);
	}
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(push);
	switch_gpu_backing_release(surface);
}

// A CPU writer's lines up to the submit are cleaned for the job; what it writes after reaches DRAM at its release.
TEST_F(Backing, ASubmitUnderACpuWriterStillCleansEveryCpuWrite)
{
	auto *surface = backing();
	ASSERT_EQ(publish(surface, kRead, token(9)), 0);
	complete(token(9));
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096, kWrite), 0);
	const size_t before = cache.size();
	EXPECT_EQ(native_submit(token(1), {{surface, TegraGM20BSurfaceRead}}), 0);
	ASSERT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_CLEAN);
	EXPECT_EQ(cache.back().offset, 0u);
	EXPECT_EQ(cache.back().bytes, 4096u);
	ASSERT_EQ(switch_gpu_backing_cpu_release(surface, 0, 4096, kWrite), 0);
	ASSERT_EQ(cache.size(), before + 2);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_CLEAN);
	EXPECT_EQ(cache.back().bytes, 4096u);
	switch_gpu_backing_release(surface);
}

// raw's geometry read lease: its CPU side still backs off, its submission never fails.
TEST_F(Backing, GeometryLeaseNeverRefusesANativeSubmit)
{
	auto *vertices = backing();
	const uint32_t lease = kRead | kAvoid;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(vertices, 0, 4096, lease), 0);
	const auto job = token(1);
	EXPECT_EQ(native_submit(job, {{vertices, TegraGM20BSurfaceWrite}}), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(vertices, 0, 4096, lease), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_release(vertices, 0, 4096, lease), 0);
	switch_gpu_backing_release(vertices);
}
