// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <string.h>
#include "SwitchGpuRelayoutCache.h"
#include "SwitchGpuBacking.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BLayout.h"

extern "C" __attribute__((weak)) uint32_t tegra_gm20b_surface_kind(uint64_t)
{
	return 0;
}

extern "C" __attribute__((weak)) int switch_gpu_backing_tgm_mem(struct switch_gpu_backing *,
								uint32_t, struct tgm_mem **)
{
	return -95;
}

extern "C" __attribute__((weak)) int
switch_gpu_backing_tgm_mem_existing(struct switch_gpu_backing *, uint32_t, struct tgm_mem **)
{
	return -2;
}

extern "C" __attribute__((weak)) int
switch_gpu_backing_tgm_mem_existing_or_direct(struct switch_gpu_backing *, uint32_t,
					      struct tgm_mem **)
{
	return -2;
}

namespace
{
constexpr uint64_t Base = 16ULL << 30;

void discard(tgm_vm *vm, SwitchGpuRelayoutCache *cache, SwitchGpuRelayoutMapping *entry)
{
	if (!entry->backing)
		return;
	switch_gpu_vm_unmap(vm, &entry->tiledMapping);
	switch_gpu_vm_unmap(vm, &entry->pitchMapping);
	if (entry->ownsMemory)
		tgm_mem_del(&entry->memory);
	else
		entry->memory = nullptr;
	cache->bytes -= entry->bytes;
	IODelete(entry->pages, uint64_t, entry->pageCount);
	switch_gpu_backing_release(entry->backing);
	*entry = {};
}

SwitchGpuRelayoutMapping *oldest(SwitchGpuRelayoutCache *cache)
{
	SwitchGpuRelayoutMapping *found = nullptr;
	for (auto &entry : cache->mappings) {
		if (!entry.users && entry.backing && (!found || entry.lastUse < found->lastUse))
			found = &entry;
	}
	return found;
}
}

static int mapping(tgm_vm *vm, SwitchGpuRelayoutCache *cache, switch_gpu_backing *backing,
		   const uint64_t *pages, uint32_t count, bool tiled,
		   SwitchGpuRelayoutMapping **out)
{
	*out = nullptr;
	const uint64_t bytes = uint64_t(count) << 12;
	if (!backing || !pages || !count || bytes > SwitchGpuRelayoutCache::MaximumBytes) {
		switch_gpu_backing_release(backing);
		return -22;
	}

	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(backing, &info);
	const uint32_t surfaceKind = tiled ? tegra_gm20b_surface_kind(pages[0]) : 0u;
	const uint32_t wantKind = tiled ? (surfaceKind ? surfaceKind : TGM_LAYOUT_KIND) : 0u;
	const bool compressed = wantKind != 0u && wantKind != TGM_LAYOUT_KIND;
	tgm_mem *canonical = nullptr;
	const bool hasCanonical =
		switch_gpu_backing_tgm_mem_existing(backing, count, &canonical) == 0;
	for (auto &entry : cache->mappings) {
		if (entry.backing == backing && entry.allocation == info.allocation_id &&
		    entry.generation == info.allocation_generation && entry.pageCount == count &&
		    entry.sourcePages == pages &&
		    (!tiled || (entry.tiled && entry.tiledKind == wantKind)) &&
		    (!hasCanonical || entry.memory == canonical)) {
			++entry.users;
			entry.lastUse = ++cache->clock;
			*out = &entry;
			switch_gpu_backing_release(backing);
			return 0;
		}
	}
	if (info.page_count < count || memcmp(info.pages, pages, size_t(count) * sizeof(*pages))) {
		switch_gpu_backing_release(backing);
		return -22;
	}
	for (auto &candidate : cache->mappings) {
		if (!candidate.users && candidate.backing == backing &&
		    candidate.allocation == info.allocation_id &&
		    candidate.generation == info.allocation_generation)
			discard(vm, cache, &candidate);
	}
	while (cache->bytes > SwitchGpuRelayoutCache::MaximumBytes - bytes) {
		auto *entry = oldest(cache);
		if (!entry) {
			switch_gpu_backing_release(backing);
			return -16;
		}
		discard(vm, cache, entry);
	}
	SwitchGpuRelayoutMapping *entry = nullptr;
	for (auto &candidate : cache->mappings) {
		if (!candidate.backing) {
			entry = &candidate;
			break;
		}
	}
	if (!entry) {
		entry = oldest(cache);
		if (!entry) {
			switch_gpu_backing_release(backing);
			return -16;
		}
		discard(vm, cache, entry);
	}
	entry->pages = IONew(uint64_t, count);
	if (!entry->pages) {
		switch_gpu_backing_release(backing);
		return -12;
	}
	memcpy(entry->pages, pages, size_t(count) * sizeof(*pages));
	entry->backing = backing;
	entry->sourcePages = pages;
	entry->allocation = info.allocation_id;
	entry->generation = info.allocation_generation;
	entry->bytes = bytes;
	entry->pageCount = count;
	cache->bytes += bytes;
	int result = 0;
	if (hasCanonical) {
		entry->memory = canonical;
		entry->ownsMemory = false;
	} else if (compressed) {
		result = switch_gpu_backing_tgm_mem(backing, count, &entry->memory);
		entry->ownsMemory = false;
	} else if (switch_gpu_backing_tgm_mem_existing_or_direct(backing, count, &entry->memory) ==
		   0) {
		entry->ownsMemory = false;
	} else {
		result = tgm_mem_new(vm, entry->pages, count, &entry->memory);
		entry->ownsMemory = result == 0;
	}
	const uint64_t pitch =
		Base + uint64_t(entry - cache->mappings) * SwitchGpuRelayoutCache::WindowBytes * 2;
	if (!result) {
		result = switch_gpu_vm_map(vm, pitch, bytes, entry->memory, 0, 0, 0,
					   &entry->pitchMapping);
		if (!result)
			entry->pitch = pitch;
	}
	if (!result && tiled) {
		const uint64_t address = pitch + SwitchGpuRelayoutCache::WindowBytes;
		result = switch_gpu_vm_map(vm, address, bytes, entry->memory, 0,
					   static_cast<uint8_t>(wantKind), 0, &entry->tiledMapping);
		if (!result) {
			entry->tiled = address;
			entry->tiledKind = wantKind;
		}
	}
	if (result) {
		discard(vm, cache, entry);
		return result;
	}
	entry->users = 1;
	entry->lastUse = ++cache->clock;
	*out = entry;
	return 0;
}

int switch_gpu_relayout_mapping(tgm_vm *vm, SwitchGpuRelayoutCache *cache, const uint64_t *pages,
				uint32_t count, bool tiled, SwitchGpuRelayoutMapping **out)
{
	*out = nullptr;
	if (!pages || !count)
		return -22;
	auto *backing = switch_gpu_backing_find_pa(pages[0]);
	if (!backing)
		return -2;
	return mapping(vm, cache, backing, pages, count, tiled, out);
}

int switch_gpu_relayout_mapping_backing(tgm_vm *vm, SwitchGpuRelayoutCache *cache,
					switch_gpu_backing *backing, bool tiled,
					SwitchGpuRelayoutMapping **out)
{
	*out = nullptr;
	if (!backing)
		return -22;
	switch_gpu_backing_info info{};
	switch_gpu_backing_retain(backing);
	switch_gpu_backing_describe(backing, &info);
	return mapping(vm, cache, backing, info.pages, info.page_count, tiled, out);
}

void switch_gpu_relayout_mapping_put(SwitchGpuRelayoutMapping *mapping)
{
	if (mapping)
		--mapping->users;
}

void switch_gpu_relayout_cache_clear(tgm_vm *vm, SwitchGpuRelayoutCache *cache)
{
	for (auto &entry : cache->mappings) {
		if (!entry.users)
			discard(vm, cache, &entry);
	}
}
