// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <string.h>
#include "SwitchGpuRelayoutCache.h"
#include "SwitchGpuBacking.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BLayout.h"

namespace
{
constexpr uint64_t Base = 16ULL << 30;

void discard(tgm_vm *vm, SwitchGpuRelayoutCache *cache, SwitchGpuRelayoutMapping *entry)
{
	if (!entry->backing)
		return;
	switch_gpu_vm_unmap(vm, &entry->tiledMapping);
	switch_gpu_vm_unmap(vm, &entry->pitchMapping);
	tgm_mem_del(&entry->memory);
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
	for (auto &entry : cache->mappings) {
		if (entry.backing == backing && entry.allocation == info.allocation_id &&
		    entry.generation == info.allocation_generation && entry.pageCount == count &&
		    entry.sourcePages == pages && (!tiled || entry.tiled)) {
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
	int result = tgm_mem_new(vm, entry->pages, count, &entry->memory);
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
		result = switch_gpu_vm_map(vm, address, bytes, entry->memory, 0, TGM_LAYOUT_KIND, 0,
					   &entry->tiledMapping);
		if (!result)
			entry->tiled = address;
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
