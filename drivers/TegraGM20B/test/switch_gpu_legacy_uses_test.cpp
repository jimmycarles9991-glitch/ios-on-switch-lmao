// SPDX-License-Identifier: MIT
// A legacy submission's uses (SwitchGpuLegacyUses.h) over the ownership layer: only its listed imports are claims; its
// connection's other BOs order the CPU after it and nothing else, so its kill fails nobody.
#include <array>
#include <utility>
#include <vector>
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuLegacyUses.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kUnlisted = SWITCH_GPU_BACKING_UNLISTED,
		   kAvoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
constexpr uint32_t kBesideCpu = SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;

struct Bo {
	Bo *next;
	switch_gpu_backing *backing;
	void *coh;
	bool readOnly;
	uint64_t handle;
};

// A connection with `privates` BOs of its own, then one import of a surface (handle 1000) another process also uses.
struct Connection {
	std::vector<Bo> bos;
	std::vector<switch_gpu_backing_use> uses;
	int coh;

	Connection(switch_gpu_backing *surface, uint32_t privates) : bos(privates + 1)
	{
		for (uint32_t i = 0; i < privates; ++i)
			bos[i] = {&bos[i + 1], backing(), nullptr, false, i + 1};
		bos[privates] = {nullptr, surface, &coh, false, 1000};
		switch_gpu_backing_retain(surface);
		uses.resize(bos.size());
	}

	~Connection()
	{
		for (auto &bo : bos)
			switch_gpu_backing_release(bo.backing);
	}

	switch_gpu_backing *surface() const
	{
		return bos.back().backing;
	}

	switch_gpu_backing *own(size_t i) const
	{
		return bos[i].backing;
	}

	// A legacy Exec on `completion`; listed: its list names the import with `access` (TegraGM20BSurface*).
	int exec(switch_gpu_backing_token completion, bool listed, uint32_t access = 0)
	{
		uint32_t count = 0;
		EXPECT_EQ(switch_gpu_legacy_uses(
				  &bos[0], listed,
				  [&](uint64_t handle) {
					  return handle == 1000 ? access : 0u;
				  },
				  [&](const Bo *bo, uint32_t use) {
					  uses[count++] = {bo->backing, use, 0};
					  return 0;
				  }),
			  0);
		uses.resize(count);
		switch_gpu_backing_transaction transaction = {};
		int result =
			switch_gpu_backing_begin(uses.data(), count, &completion, &transaction);
		std::array<switch_gpu_backing_token, 8> dependencies = {};
		uint32_t needed = 0;
		if (!result)
			result = switch_gpu_backing_dependencies(&transaction, dependencies.data(),
								 8, &needed);
		if (!result)
			switch_gpu_backing_commit(&transaction);
		else if (transaction.locked)
			switch_gpu_backing_abort(&transaction);
		uses.resize(bos.size());
		return result;
	}
};

uint32_t dependencies_of(switch_gpu_backing *allocation, uint32_t access,
			 switch_gpu_backing_token completion)
{
	switch_gpu_backing_use use = {allocation, access, 0};
	switch_gpu_backing_transaction transaction = {};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	std::array<switch_gpu_backing_token, 4> dependencies = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 4, &needed),
		  0);
	switch_gpu_backing_abort(&transaction);
	return needed;
}
}  // namespace

TEST(LegacyAccess, OnlyAListedImportIsAClaim)
{
	EXPECT_EQ(switch_gpu_legacy_access(false, false, true, 0), kWrite | kUnlisted);
	EXPECT_EQ(switch_gpu_legacy_access(false, false, false, 0), kWrite | kUnlisted);
	EXPECT_EQ(switch_gpu_legacy_access(true, false, false, 0), kWrite | kUnlisted);
	EXPECT_EQ(switch_gpu_legacy_access(true, true, false, 0), kRead | kUnlisted);
	EXPECT_EQ(switch_gpu_legacy_access(true, false, true, 0), 0u);
	EXPECT_EQ(switch_gpu_legacy_access(true, false, true, TegraGM20BSurfaceRead),
		  kRead | kBesideCpu);
	EXPECT_EQ(switch_gpu_legacy_access(true, false, true, TegraGM20BSurfaceWrite),
		  kWrite | kBesideCpu);
	EXPECT_EQ(switch_gpu_legacy_access(true, true, true, TegraGM20BSurfaceWrite),
		  kRead | kBesideCpu);
	EXPECT_EQ(switch_gpu_legacy_access(true, false, true,
					   TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite),
		  kRead | kWrite | kBesideCpu);
}

TEST_F(Backing, ListedLegacySubmitClaimsOnlyItsListedImport)
{
	auto *surface = backing();
	Connection nvk(surface, 100);
	const auto job = token(1);
	ASSERT_EQ(nvk.exec(job, true, TegraGM20BSurfaceRead), 0);
	// A record per BO, and a host acquire per BO: each is new, so its contents count as CPU-written.
	EXPECT_EQ(refs(job), 2 * 101);
	EXPECT_EQ(dependencies_of(nvk.surface(), kWrite, token(2)), 1u);
	EXPECT_EQ(dependencies_of(nvk.surface(), kRead, token(2)), 0u);
	for (size_t i = 0; i < 100; ++i) {
		EXPECT_EQ(dependencies_of(nvk.own(i), kRead, token(2)), 0u);
		EXPECT_EQ(dependencies_of(nvk.own(i), kWrite, token(2)), 0u);
	}
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(nvk.own(7), 0, 4096, kRead | kAvoid), -16);
	complete(job);
	const size_t before = cache.size();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(nvk.own(7), 0, 4096, kRead | kAvoid), 0);
	ASSERT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	EXPECT_EQ(switch_gpu_backing_cpu_release(nvk.own(7), 0, 4096, kRead | kAvoid), 0);
	switch_gpu_backing_release(surface);
}

TEST_F(Backing, KilledLegacyJobFailsNoneOfItsConnectionsBos)
{
	auto *surface = backing();
	Connection nvk(surface, 100);
	const auto job = token(1);
	ASSERT_EQ(nvk.exec(job, true, TegraGM20BSurfaceRead), 0);
	kill_channel(job);
	for (size_t i = 0; i < 100; ++i) {
		EXPECT_EQ(publish(nvk.own(i), kRead, token(2, i + 1)), 0);
		EXPECT_EQ(switch_gpu_backing_cpu_acquire(nvk.own(i), 0, 4096, kRead), 0);
		EXPECT_EQ(switch_gpu_backing_cpu_release(nvk.own(i), 0, 4096, kRead), 0);
	}
	EXPECT_EQ(publish(surface, kRead, token(3)), 0);
	EXPECT_EQ(nvk.exec(token(4), true, TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite), 0);
	complete(token(2, 100));
	complete(token(4));
	for (size_t i = 0; i < 100; ++i) {
		EXPECT_EQ(switch_gpu_backing_cpu_acquire(nvk.own(i), 0, 4096, kRead), 0);
		EXPECT_EQ(switch_gpu_backing_cpu_release(nvk.own(i), 0, 4096, kRead), 0);
	}
	EXPECT_EQ(refs(job), 0);
	switch_gpu_backing_release(surface);
}

TEST_F(Backing, KilledListedWriterFailsNoReaderOfThatImport)
{
	auto *surface = backing();
	Connection nvk(surface, 4);
	const auto job = token(1);
	ASSERT_EQ(nvk.exec(job, true, TegraGM20BSurfaceWrite), 0);
	kill_channel(job);
	const int logged = ioLogLines();
	EXPECT_EQ(publish(nvk.own(0), kRead, token(2)), 0);
	EXPECT_EQ(ioLogLines(), logged);
	EXPECT_EQ(nvk.exec(token(3), false), 0);
	EXPECT_EQ(ioLogLines(), logged + 1);
	complete(token(2));
	complete(token(3));
	EXPECT_EQ(publish(surface, kRead, token(2, 2)), 0);
	for (size_t i = 0; i < 4; ++i) {
		EXPECT_EQ(switch_gpu_backing_cpu_acquire(nvk.own(i), 0, 4096, kRead), 0);
		EXPECT_EQ(switch_gpu_backing_cpu_release(nvk.own(i), 0, 4096, kRead), 0);
	}
	EXPECT_EQ(refs(job), 0);
	switch_gpu_backing_release(surface);
}

TEST_F(Backing, UnlistedLegacySubmitOrdersTheCpuAndOtherEnginesOnly)
{
	auto *surface = backing();
	Connection nvk(surface, 2);
	const auto job = token(1);
	ASSERT_EQ(nvk.exec(job, false), 0);
	EXPECT_EQ(dependencies_of(surface, kRead, token(2)), 0u);
	EXPECT_EQ(dependencies_of(surface, kRead, token(5, 1, SWITCH_GPU_ENGINE_VIC)), 1u);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096, kRead | kAvoid), -16);
	EXPECT_EQ(switch_gpu_backing_wait(surface, kRead | kAvoid), -16);
	kill_channel(job);
	EXPECT_EQ(publish(surface, kRead, token(2)), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096, kRead), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(surface, 0, 4096, kRead), 0);
	switch_gpu_backing_release(surface);
}

// IOSurfaceLock(kIOSurfaceLockReadOnly) in another process while the legacy job lists the surface as read only.
TEST_F(Backing, IOSurfaceReadLockLetsAListedReaderSubmit)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *surface = switch_gpu_backing_register(&page, 1, 801, nullptr, nullptr);
	Connection nvk(surface, 2);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 801, 1), 0);
	switch_gpu_surface_lock_did(&client, 801, 1);
	EXPECT_EQ(nvk.exec(token(1), true, TegraGM20BSurfaceRead), 0);
	switch_gpu_surface_unlock_will(&client, 801, 1);
	switch_gpu_surface_unlock_did(&client, 801, 1);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(surface);
}

TEST_F(Backing, CpuOwnersNeverRefuseWhatALegacySubmitDidNotList)
{
	auto *surface = backing();
	Connection nvk(surface, 2);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(nvk.own(0), 0, 4096, kRead | kAvoid), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096,
						 kWrite | SWITCH_GPU_BACKING_CPU_CONCURRENT),
		  0);
	EXPECT_EQ(nvk.exec(token(1), false), 0);
	EXPECT_EQ(nvk.exec(token(1, 2), true, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(nvk.own(0), 0, 4096, kRead | kAvoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(surface, 0, 4096,
						 kWrite | SWITCH_GPU_BACKING_CPU_CONCURRENT),
		  0);
	switch_gpu_backing_release(surface);
}

// IOSurfaceLock in another process (SwitchGpuSurfaceAccess.cpp) while the legacy job lists the surface.
TEST_F(Backing, IOSurfaceLocksNeverRefuseAListedLegacySubmit)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *surface = switch_gpu_backing_register(&page, 1, 800, nullptr, nullptr);
	Connection nvk(surface, 2);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 800, 1), 0);
	switch_gpu_surface_lock_did(&client, 800, 1);
	EXPECT_EQ(nvk.exec(token(1), true, TegraGM20BSurfaceRead), 0);
	const auto writer = token(1, 2);
	EXPECT_EQ(nvk.exec(writer, true, TegraGM20BSurfaceWrite), 0);
	switch_gpu_surface_unlock_will(&client, 800, 1);
	switch_gpu_surface_unlock_did(&client, 800, 1);
	EXPECT_EQ(switch_gpu_surface_lock_will(&client, 800, 1 | 2), -16);
	complete(writer);
	const size_t before = cache.size();
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 800, 1 | 2), 0);
	switch_gpu_surface_lock_did(&client, 800, 1 | 2);
	ASSERT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	switch_gpu_surface_unlock_will(&client, 800, 1 | 2);
	switch_gpu_surface_unlock_did(&client, 800, 1 | 2);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 800, 0), 0);
	switch_gpu_surface_lock_did(&client, 800, 0);
	EXPECT_EQ(nvk.exec(token(1, 3), true, TegraGM20BSurfaceRead), 0);
	switch_gpu_surface_unlock_will(&client, 800, 0);
	switch_gpu_surface_unlock_did(&client, 800, 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(surface);
}

// A CPU writer's lines up to the submit are cleaned for the job; what it writes after reaches DRAM at its release.
TEST_F(Backing, ListedSubmitUnderACpuWriterStillCleansEveryCpuWrite)
{
	auto *surface = backing();
	Connection nvk(surface, 0);
	ASSERT_EQ(publish(surface, kRead, token(9)), 0);
	complete(token(9));
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096, kWrite), 0);
	const size_t before = cache.size();
	EXPECT_EQ(nvk.exec(token(1), true, TegraGM20BSurfaceRead), 0);
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

namespace
{
// A native Submit or SubmitAddress on `completion`: every use named, with its TegraGM20BSurface* access.
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

TEST(LegacyAccess, NativeUsesAreClaimsNoCpuOwnerRefusesUnlessAdmitted)
{
	EXPECT_EQ(switch_gpu_claim_flags(false), kBesideCpu);
	EXPECT_EQ(switch_gpu_claim_flags(true), 0u);
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
