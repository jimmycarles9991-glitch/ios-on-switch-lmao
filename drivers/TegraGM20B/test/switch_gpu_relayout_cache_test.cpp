// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuRelayoutCache.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"

struct tgm_vm {};

struct tgm_mem {
	const uint64_t *pages;
};

struct switch_gpu_mapping {
	uint64_t address;
};

struct switch_gpu_backing {
	std::vector<uint64_t> pages;
	uint64_t id = 0, generation = 1;
	unsigned references = 1;
};

namespace
{
std::map<uint64_t, switch_gpu_backing *> backings;
std::map<uint64_t, uint32_t> surfaceKinds;
std::map<switch_gpu_backing *, tgm_mem *> canonicalMemories;
int creates, maps, unmaps, deletes, failMap, directPaLatches;
uint8_t lastMappedKind;
}

extern "C" uint32_t tegra_gm20b_surface_kind(uint64_t page)
{
	const auto found = surfaceKinds.find(page);
	return found == surfaceKinds.end() ? 0u : found->second;
}

extern "C" int switch_gpu_backing_tgm_mem(switch_gpu_backing *backing, uint32_t, tgm_mem **out)
{
	const auto found = canonicalMemories.find(backing);
	if (found == canonicalMemories.end())
		return -95;
	*out = found->second;
	return 0;
}

extern "C" int switch_gpu_backing_tgm_mem_existing(switch_gpu_backing *backing, uint32_t,
						   tgm_mem **out)
{
	const auto found = canonicalMemories.find(backing);
	if (found == canonicalMemories.end())
		return -2;
	*out = found->second;
	return 0;
}

extern "C" int switch_gpu_backing_tgm_mem_existing_or_direct(switch_gpu_backing *backing, uint32_t,
							     tgm_mem **out)
{
	const auto found = canonicalMemories.find(backing);
	if (found == canonicalMemories.end()) {
		++directPaLatches;
		return -2;
	}
	*out = found->second;
	return 0;
}

extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t page)
{
	const auto found = backings.find(page);
	if (found == backings.end())
		return nullptr;
	++found->second->references;
	return found->second;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
	++backing->references;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
	if (backing)
		--backing->references;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing,
					    switch_gpu_backing_info *info)
{
	*info = {};
	info->allocation_id = backing->id;
	info->allocation_generation = backing->generation;
	info->pages = backing->pages.data();
	info->page_count = uint32_t(backing->pages.size());
}

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *pages, uint32_t, tgm_mem **memory)
{
	++creates;
	*memory = new tgm_mem{pages};
	return 0;
}

extern "C" void tgm_mem_del(tgm_mem **memory)
{
	if (*memory) {
		EXPECT_NE((*memory)->pages[0], 0u);
		++deletes;
		delete *memory;
		*memory = nullptr;
	}
}

extern "C" int switch_gpu_vm_map(tgm_vm *, uint64_t address, uint64_t, tgm_mem *, uint64_t,
				 uint8_t kind, int, switch_gpu_mapping **mapping)
{
	if (++maps == failMap)
		return -12;
	lastMappedKind = kind;
	*mapping = new switch_gpu_mapping{address};
	return 0;
}

extern "C" void switch_gpu_vm_unmap(tgm_vm *, switch_gpu_mapping **mapping)
{
	if (*mapping) {
		++unmaps;
		delete *mapping;
		*mapping = nullptr;
	}
}

namespace
{
class RelayoutCache : public ::testing::Test
{
protected:
	tgm_vm vm;
	SwitchGpuRelayoutCache cache{};
	std::vector<std::unique_ptr<switch_gpu_backing>> allocations;

	void SetUp() override
	{
		backings.clear();
		surfaceKinds.clear();
		canonicalMemories.clear();
		creates = maps = unmaps = deletes = failMap = directPaLatches = 0;
		lastMappedKind = 0;
	}

	void TearDown() override
	{
		switch_gpu_relayout_cache_clear(&vm, &cache);
		for (const auto &allocation : allocations)
			EXPECT_EQ(allocation->references, 1u);
		EXPECT_EQ(creates, deletes);
	}

	switch_gpu_backing *allocation(uint32_t pages = 4)
	{
		auto value = std::make_unique<switch_gpu_backing>();
		value->id = allocations.size() + 1;
		for (uint32_t i = 0; i < pages; ++i)
			value->pages.push_back((value->id << 30) + uint64_t(i) * 4096);
		auto *result = value.get();
		backings[result->pages[0]] = result;
		allocations.push_back(std::move(value));
		return result;
	}

	SwitchGpuRelayoutMapping *get(switch_gpu_backing *allocation, bool tiled = true)
	{
		SwitchGpuRelayoutMapping *result = nullptr;
		EXPECT_EQ(switch_gpu_relayout_mapping(&vm, &cache, allocation->pages.data(),
						      uint32_t(allocation->pages.size()), tiled,
						      &result),
			  0);
		return result;
	}
};

TEST_F(RelayoutCache, WarmSurfaceAndScratchReuseAllMemoryObjectsAndMappings)
{
	auto *surface = allocation(), *scratch = allocation();
	for (unsigned i = 0; i < 10000; ++i) {
		auto *a = get(surface), *b = get(scratch, false);
		ASSERT_NE(a, nullptr);
		ASSERT_NE(b, nullptr);
		EXPECT_NE(a->pitch, a->tiled);
		EXPECT_EQ(b->tiled, 0u);
		switch_gpu_relayout_mapping_put(a);
		switch_gpu_relayout_mapping_put(b);
	}
	EXPECT_EQ(creates, 2);
	EXPECT_EQ(maps, 3);
	EXPECT_EQ(unmaps, 0);
}

TEST_F(RelayoutCache, AllocationGenerationChangeCannotReuseOldPageTable)
{
	auto *owner = allocation();
	auto *first = get(owner);
	switch_gpu_relayout_mapping_put(first);
	++owner->generation;
	auto *second = get(owner);
	EXPECT_NE(first, second);
	EXPECT_EQ(creates, 2);
	switch_gpu_relayout_mapping_put(second);
}

TEST_F(RelayoutCache, EntriesRemainPinnedThroughEvictionPressure)
{
	auto *protectedEntry = get(allocation());
	const auto address = protectedEntry->tiled;
	for (unsigned i = 0; i < SwitchGpuRelayoutCache::Capacity * 2; ++i)
		switch_gpu_relayout_mapping_put(get(allocation()));
	EXPECT_EQ(protectedEntry->tiled, address);
	EXPECT_EQ(protectedEntry->users, 1u);
	switch_gpu_relayout_mapping_put(protectedEntry);
	EXPECT_GT(unmaps, 0);
}

TEST_F(RelayoutCache, ByteBudgetEvictsOnlyUnusedAllocations)
{
	auto *first = get(allocation(16384));
	auto *second = get(allocation(16384));
	EXPECT_EQ(cache.bytes, SwitchGpuRelayoutCache::MaximumBytes);
	auto *third = allocation();
	SwitchGpuRelayoutMapping *result = nullptr;
	EXPECT_EQ(switch_gpu_relayout_mapping(&vm, &cache, third->pages.data(), 4, true, &result),
		  -16);
	EXPECT_EQ(result, nullptr);
	switch_gpu_relayout_mapping_put(second);
	result = get(third);
	EXPECT_LE(cache.bytes, SwitchGpuRelayoutCache::MaximumBytes);
	switch_gpu_relayout_mapping_put(result);
	switch_gpu_relayout_mapping_put(first);
}

TEST_F(RelayoutCache, FailedSecondAliasUnwindsEveryPinAndPublishedMapping)
{
	auto *owner = allocation();
	failMap = 2;
	SwitchGpuRelayoutMapping *result = nullptr;
	EXPECT_EQ(switch_gpu_relayout_mapping(&vm, &cache, owner->pages.data(), 4, true, &result),
		  -12);
	EXPECT_EQ(owner->references, 1u);
	EXPECT_EQ(cache.bytes, 0u);
	EXPECT_EQ(unmaps, 1);
	EXPECT_EQ(creates, deletes);
}

TEST_F(RelayoutCache, PhysicalPrefixMustMatchAuthoritativeAllocation)
{
	auto *owner = allocation();
	auto pages = owner->pages;
	pages[1] += 4096;
	SwitchGpuRelayoutMapping *result = nullptr;
	EXPECT_EQ(switch_gpu_relayout_mapping(&vm, &cache, pages.data(), 4, true, &result), -22);
	EXPECT_EQ(maps, 0);
	EXPECT_EQ(owner->references, 1u);
}

TEST_F(RelayoutCache, CacheOwnsStablePageVectorUntilMemoryObjectIsDestroyed)
{
	auto *owner = allocation();
	auto *entry = get(owner);
	EXPECT_NE(entry->pages, owner->pages.data());
	EXPECT_EQ(entry->memory->pages, entry->pages);
	switch_gpu_relayout_cache_clear(&vm, &cache);
	EXPECT_EQ(unmaps, 0);
	switch_gpu_relayout_mapping_put(entry);
}
}

TEST_F(RelayoutCache, ExplicitBackingIdentityDoesNotNeedFirstPageRegistryLookup)
{
	auto *backing = allocation();
	backings.clear();
	SwitchGpuRelayoutMapping *first = nullptr, *second = nullptr;
	ASSERT_EQ(switch_gpu_relayout_mapping_backing(&vm, &cache, backing, true, &first), 0);
	ASSERT_EQ(switch_gpu_relayout_mapping_backing(&vm, &cache, backing, true, &second), 0);
	EXPECT_EQ(first, second);
	EXPECT_EQ(creates, 1);
	switch_gpu_relayout_mapping_put(first);
	switch_gpu_relayout_mapping_put(second);
}

TEST_F(RelayoutCache, CompressedSurfaceUsesCanonicalBackingMemoryAndCompressedKind)
{
	auto *backing = allocation(16);
	tgm_mem canonical{backing->pages.data()};
	surfaceKinds[backing->pages[0]] = 0xdbu;
	canonicalMemories[backing] = &canonical;
	auto *entry = get(backing, true);
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->memory, &canonical);
	EXPECT_FALSE(entry->ownsMemory);
	EXPECT_EQ(entry->tiledKind, 0xdbu);
	EXPECT_EQ(lastMappedKind, 0xdbu);
	EXPECT_EQ(creates, 0);
	EXPECT_EQ(maps, 2);
	auto *again = get(backing, true);
	EXPECT_EQ(again, entry);
	EXPECT_EQ(maps, 2);
	switch_gpu_relayout_mapping_put(entry);
	switch_gpu_relayout_mapping_put(again);
	switch_gpu_relayout_cache_clear(&vm, &cache);
	EXPECT_EQ(deletes, 0);
	EXPECT_EQ(unmaps, 2);
}

TEST_F(RelayoutCache, UncompressedMappingReusesCanonicalMemoryAndKindTransitionEvictsIdleSlot)
{
	auto *backing = allocation(16);
	tgm_mem canonical{backing->pages.data()};
	canonicalMemories[backing] = &canonical;
	auto *uncompressed = get(backing, true);
	ASSERT_NE(uncompressed, nullptr);
	EXPECT_EQ(uncompressed->memory, &canonical);
	EXPECT_FALSE(uncompressed->ownsMemory);
	EXPECT_EQ(uncompressed->tiledKind, 0xfeu);
	EXPECT_EQ(creates, 0);
	switch_gpu_relayout_mapping_put(uncompressed);

	surfaceKinds[backing->pages[0]] = 0xdbu;
	auto *compressed = get(backing, true);
	ASSERT_NE(compressed, nullptr);
	EXPECT_EQ(compressed->memory, &canonical);
	EXPECT_EQ(compressed->tiledKind, 0xdbu);
	EXPECT_EQ(unmaps, 2);
	EXPECT_EQ(cache.bytes, 16u * 4096u);
	EXPECT_EQ(directPaLatches, 0);
	switch_gpu_relayout_mapping_put(compressed);
}

TEST_F(RelayoutCache, DirectFallbackLatchesDirectPaBeforeAllocatingMem)
{
	auto *backing = allocation(16);
	auto *entry = get(backing, false);
	ASSERT_NE(entry, nullptr);
	EXPECT_TRUE(entry->ownsMemory);
	EXPECT_EQ(creates, 1);
	EXPECT_EQ(directPaLatches, 1);
	switch_gpu_relayout_mapping_put(entry);
}
