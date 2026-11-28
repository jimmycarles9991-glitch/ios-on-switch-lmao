// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/SwitchGpuBindingTable.h"
#include "drivers/TegraGM20B/SwitchGpuBoIndex.h"
#include "drivers/TegraGM20B/SwitchGpuUseCache.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/TegraGM20BSchedABI.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace
{
struct HostMemory {
	template <typename T> static T *allocate(uint32_t count)
	{
		return static_cast<T *>(std::malloc(sizeof(T) * count));
	}

	template <typename T> static void release(T *items, uint32_t)
	{
		std::free(items);
	}
};

struct FakeBo {
	uint64_t handle = 0;
	uint64_t bytes = 0x10000ULL;
	switch_gpu_backing *backing = nullptr;
	tgm_coh *coh = nullptr;
	void *surface = nullptr;
	const volatile uint64_t *representation = nullptr;
	uint64_t representationValue = 1;
	bool unlockedReads = false;
};

struct FakeBinding {
	uint64_t va = 0, range = 0;
	FakeBo *bo = nullptr;
	uint64_t bo_offset = 0;
};

struct CountingTables {
	SwitchGpuBoIndex<FakeBo, HostMemory> boIndex;
	SwitchGpuBindingTable<FakeBo, HostMemory> bindTable;
	uint64_t bindGen = 1, boGen = 1;
	uint32_t boLookups = 0, bindLookups = 0;

	~CountingTables()
	{
		boIndex.destroy();
		bindTable.destroy();
	}

	const FakeBo *findBo(uint64_t handle)
	{
		++boLookups;
		return boIndex.find(handle);
	}

	const SwitchGpuBindingTable<FakeBo, HostMemory>::Entry *findBind(uint64_t va,
									 uint64_t bytes)
	{
		++bindLookups;
		return bindTable.find(va, bytes);
	}
};

struct JobRun {
	std::vector<switch_gpu_backing_use> uses;
	std::vector<switch_gpu_backing_write_range> writes;
	std::vector<SwitchGpuSurfaceRefresh> surfaces;
	std::vector<tgm_coh *> written;
	uint32_t useTotal = 0, writeCount = 0, surfaceCount = 0, writtenCount = 0, rangedUses = 0;
	bool unlockedWrites = false, hostWritten = false;

	explicit JobRun(uint32_t cap) : uses(cap), writes(cap), surfaces(cap), written(cap)
	{
	}
};

bool resolveJob(CountingTables &tables, SwitchGpuUseCache<HostMemory> &cache, uint64_t chanUid,
		uint32_t claim, uint32_t setId, bool sameUses,
		const std::vector<TegraGM20BAbiSurfaceUse> &input, JobRun &out)
{
	const uint64_t snapBind = tables.bindGen, snapBo = tables.boGen;
	out.useTotal = out.writeCount = out.surfaceCount = out.writtenCount = out.rangedUses = 0;
	out.unlockedWrites = out.hostWritten = false;
	if (sameUses) {
		if (!cache.matches(chanUid, snapBind, snapBo, claim, setId)) {
			cache.invalidate();
			return false;
		}
		cache.restore(out.uses.data(), out.useTotal, out.writes.data(), out.writeCount,
			      out.surfaces.data(), out.surfaceCount, out.written.data(),
			      out.writtenCount, out.rangedUses, out.unlockedWrites,
			      out.hostWritten);
		return true;
	}
	cache.invalidate();
	bool usesHostWritten = false;
	for (const auto &use : input) {
		const bool addressed = (use.access & TegraGM20BSurfaceGpuAddress) != 0;
		const uint64_t bytes = (use.access & TegraGM20BSurfaceRange) ? use.reserved : 0;
		out.rangedUses += bytes != 0;
		const auto *binding =
			addressed ? tables.findBind(use.handle, bytes ? bytes : 1) : nullptr;
		const FakeBo *bo =
			addressed ? (binding ? binding->bo : nullptr) : tables.findBo(use.handle);
		if (!bo)
			return false;
		uint64_t offset = 0;
		if (bytes &&
		    (!binding || !switch_gpu_binding_offset(*binding, use.handle, bytes, offset)))
			return false;
		usesHostWritten |= (use.access & TegraGM20BSurfaceHostWritten) != 0;
		out.hostWritten |= usesHostWritten;
		const uint32_t access =
			use.access & (TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite);
		out.uses[out.useTotal++] = {bo->backing, access | claim, 0};
		if (access & TegraGM20BSurfaceWrite) {
			out.writes[out.writeCount++] = {bo->backing,
							bo->representation ? 0 : offset,
							bo->representation ? 0 : bytes};
			out.unlockedWrites |= bo->unlockedReads;
			out.written[out.writtenCount++] = bo->coh;
		}
		if (bo->representation)
			out.surfaces[out.surfaceCount++] = {
				bo->backing,
				bo->surface,
				bo->representation,
				&const_cast<FakeBo *>(bo)->representationValue,
				7,
				access,
				0,
				nullptr,
				nullptr,
				nullptr};
	}
	cache.save(chanUid, snapBind, snapBo, claim, setId, out.uses.data(), out.useTotal,
		   out.writes.data(), out.writeCount, out.surfaces.data(), out.surfaceCount,
		   out.written.data(), out.writtenCount, out.rangedUses, out.unlockedWrites,
		   usesHostWritten);
	return true;
}
}  // namespace

TEST(SwitchGpuUseCache, SecondJobWithSameUsesDoesZeroLookupsAndSurvivesInPlaceMutations)
{
	CountingTables tables;
	SwitchGpuUseCache<HostMemory> cache;
	uint64_t genWord = 1;
	FakeBo bos[6];
	FakeBinding binds[3];
	for (uint32_t i = 0; i < 6; ++i) {
		bos[i].handle = 10 + i;
		bos[i].backing = reinterpret_cast<switch_gpu_backing *>(uintptr_t(0x1000 + i * 64));
		bos[i].coh = reinterpret_cast<tgm_coh *>(uintptr_t(0x2000 + i * 64));
		if (i == 5) {
			bos[i].surface = reinterpret_cast<void *>(0x3000);
			bos[i].representation = &genWord;
			bos[i].unlockedReads = true;
		}
		ASSERT_TRUE(tables.boIndex.reserve());
		tables.boIndex.insert(bos[i].handle, &bos[i]);
	}
	for (uint32_t i = 0; i < 3; ++i)
		binds[i] = {0x100000ULL + i * 0x10000ULL, 0x10000ULL, &bos[i + 3], 0};
	auto *snap = SwitchGpuBindingTable<FakeBo, HostMemory>::prepare(3);
	ASSERT_NE(snap, nullptr);
	tables.bindTable.publish(snap, binds, 3);

	const std::vector<TegraGM20BAbiSurfaceUse> input = {
		{10, TegraGM20BSurfaceRead, 0},
		{11, TegraGM20BSurfaceRead | TegraGM20BSurfaceHostWritten, 0},
		{12, TegraGM20BSurfaceRead, 0},
		{0x100000ULL, TegraGM20BSurfaceRead | TegraGM20BSurfaceGpuAddress, 0},
		{0x110000ULL + 0x2000ULL,
		 TegraGM20BSurfaceWrite | TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRange,
		 0x1000},
		{0x120000ULL, TegraGM20BSurfaceWrite | TegraGM20BSurfaceGpuAddress, 0},
	};

	JobRun first(16);
	ASSERT_TRUE(resolveJob(tables, cache, 99, 0, 1, false, input, first));
	EXPECT_EQ(tables.boLookups, 3u);
	EXPECT_EQ(tables.bindLookups, 3u);
	EXPECT_EQ(first.useTotal, 6u);
	EXPECT_EQ(first.writeCount, 2u);
	EXPECT_EQ(first.surfaceCount, 1u);
	EXPECT_EQ(first.writtenCount, 2u);
	EXPECT_EQ(first.rangedUses, 1u);
	EXPECT_TRUE(first.unlockedWrites);
	EXPECT_TRUE(first.hostWritten);

	first.uses[0].backing = nullptr;
	first.uses[0].reserved = 1;
	first.surfaces[0].expected = 77;
	first.surfaces[0].descriptor = reinterpret_cast<void *>(0x4000);

	tables.boLookups = tables.bindLookups = 0;
	JobRun second(16);
	ASSERT_TRUE(resolveJob(tables, cache, 99, 0, 1, true, {}, second));
	EXPECT_EQ(tables.boLookups, 0u);
	EXPECT_EQ(tables.bindLookups, 0u);
	EXPECT_EQ(second.useTotal, 6u);
	EXPECT_EQ(second.writeCount, 2u);
	EXPECT_EQ(second.surfaceCount, 1u);
	EXPECT_EQ(second.writtenCount, 2u);
	EXPECT_EQ(second.rangedUses, 1u);
	EXPECT_TRUE(second.unlockedWrites);
	EXPECT_TRUE(second.hostWritten);
	EXPECT_EQ(second.uses[0].backing, bos[0].backing);
	EXPECT_EQ(second.uses[0].reserved, 0u);
	EXPECT_EQ(second.writes[0].offset, 0x2000u);
	EXPECT_EQ(second.writes[0].bytes, 0x1000u);
	EXPECT_EQ(second.surfaces[0].expected, 0u);
	EXPECT_EQ(second.surfaces[0].descriptor, nullptr);
	EXPECT_EQ(second.written[1], bos[5].coh);

	cache.release();
}

TEST(SwitchGpuUseCache, MutationsAndContextChangesInvalidateTheCache)
{
	CountingTables tables;
	SwitchGpuUseCache<HostMemory> cache;
	FakeBo bo = {42,      0x10000ULL, reinterpret_cast<switch_gpu_backing *>(0x1000),
		     nullptr, nullptr,    nullptr,
		     1,       false};
	ASSERT_TRUE(tables.boIndex.reserve());
	tables.boIndex.insert(bo.handle, &bo);
	const std::vector<TegraGM20BAbiSurfaceUse> input = {{42, TegraGM20BSurfaceRead, 0}};
	JobRun run(8);

	ASSERT_TRUE(resolveJob(tables, cache, 1, 0, 1, false, input, run));
	EXPECT_TRUE(resolveJob(tables, cache, 1, 0, 1, true, {}, run));

	++tables.bindGen;
	EXPECT_FALSE(resolveJob(tables, cache, 1, 0, 1, true, {}, run));
	ASSERT_TRUE(resolveJob(tables, cache, 1, 0, 1, false, input, run));

	++tables.boGen;
	EXPECT_FALSE(resolveJob(tables, cache, 1, 0, 1, true, {}, run));
	ASSERT_TRUE(resolveJob(tables, cache, 1, 0, 1, false, input, run));

	EXPECT_FALSE(resolveJob(tables, cache, 2, 0, 1, true, {}, run));
	ASSERT_TRUE(resolveJob(tables, cache, 1, 0, 1, false, input, run));
	EXPECT_FALSE(resolveJob(tables, cache, 1, 1, 1, true, {}, run));
	ASSERT_TRUE(resolveJob(tables, cache, 1, 0, 1, false, input, run));
	EXPECT_FALSE(resolveJob(tables, cache, 1, 0, 2, true, {}, run));

	cache.release();
}

TEST(SwitchGpuUseCacheSource, SubmitJobChecksGenerationsAndRecomputesScanoutWrites)
{
	const CodeText job = Block(SourceText("../TegraGM20BUserClientJobSubmit.cpp"),
				   "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.before("snapBindGen = __atomic_load_n(&bindGen_, __ATOMIC_ACQUIRE);",
			       "bindTable_.find(use.handle, bytes ? bytes : 1)"));
	EXPECT_TRUE(job.before("snapBoGen = __atomic_load_n(&boGen_, __ATOMIC_ACQUIRE);",
			       "findBo(use.handle)"));
	EXPECT_TRUE(job.has("SwitchGpuRefusal::StaleUses, kIOReturnNotReady"));
	EXPECT_TRUE(job.has("scanoutWrites |= switch_gpu_backing_scanned(job.writes[i].backing) "
			    "!= 0;"));
	EXPECT_TRUE(job.has("if (!sameUses && !nativeReads) { job.useCache.save("));

	EXPECT_TRUE(
		Block(SourceText("../SwitchGpuClientRetirement.h"),
		      "void AGXDeviceUserClient::publishBindings(")
			.before("bindTable_.publish(made, binds_, nbinds_);", "bumpBindGen();"));
	EXPECT_TRUE(Block(SourceText("../SwitchGpuClientRetirement.h"),
			  "IOReturn AGXDeviceUserClient::freeBo(Bo **p)")
			    .before("boIndex_.erase(bo->handle);", "bumpBoGen();"));
	EXPECT_TRUE(Block(SourceText("../TegraGM20BUserClientBo.cpp"),
			  "IOReturn AGXDeviceUserClient::publishBo(")
			    .before("boIndex_.insert(bo->handle, bo);", "bumpBoGen();"));
	EXPECT_TRUE(Block(SourceText("../TegraGM20BUserClientJob.cpp"),
			  "IOReturn AGXDeviceUserClient::attachSet(")
			    .has("bumpBoGen();"));
	EXPECT_TRUE(Block(SourceText("../TegraGM20BUserClientPark.cpp"),
			  "IOReturn AGXDeviceUserClient::boPark(")
			    .before("bo->backing = nullptr;", "bumpBoGen();"));
	EXPECT_TRUE(Block(SourceText("../TegraGM20BUserClientPark.cpp"),
			  "IOReturn AGXDeviceUserClient::boUnpark(")
			    .before("still->backing = still->parkedBacking;", "bumpBoGen();"));
}
