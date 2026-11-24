// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "SwitchGpuCopyMappings.h"
#include "SwitchGpuBacking.h"
#include "TegraGM20BLayout.h"

extern "C" __attribute__((weak)) uint32_t tegra_gm20b_surface_kind(uint64_t)
{
	return 0;
}

extern "C" __attribute__((weak)) int
switch_gpu_backing_tgm_mem_existing(struct switch_gpu_backing *, uint32_t, struct tgm_mem **)
{
	return -2;
}

namespace
{
constexpr uint64_t Page = 4096;

void discard(SwitchGpuCopyMapping &entry)
{
	if (!entry.descriptor)
		return;
	switch_gpu_relayout_mapping_put(entry.mapping);
	entry.descriptor->complete();
	entry.descriptor->release();
	entry = {};
}

SwitchGpuCopyMapping *oldest(SwitchGpuCopyMappings *cache)
{
	SwitchGpuCopyMapping *found = nullptr;
	for (auto &entry : cache->descriptors)
		if (entry.descriptor && !entry.users && (!found || entry.lastUse < found->lastUse))
			found = &entry;
	return found;
}

bool contains(const SwitchGpuCopyMapping &entry, uint64_t descriptorOffset, uint64_t backingOffset,
	      uint64_t bytes)
{
	if (descriptorOffset < entry.descriptorStart || backingOffset < entry.backingStart)
		return false;
	const uint64_t relative = descriptorOffset - entry.descriptorStart;
	return relative == backingOffset - entry.backingStart && relative <= entry.length &&
	       bytes <= entry.length - relative;
}
}

int switch_gpu_copy_mapping(tgm_vm *vm, SwitchGpuCopyMappings *cache,
			    IOMemoryDescriptor *descriptor, uint64_t descriptorOffset,
			    switch_gpu_backing *backing, uint64_t backingOffset, uint64_t bytes,
			    bool tiled, SwitchGpuCopyMapping **out)
{
	*out = nullptr;
	if (!descriptor || !backing || !bytes || ((descriptorOffset ^ backingOffset) & (Page - 1)))
		return -22;
	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(backing, &info);
	const uint64_t backingBytes = uint64_t(info.page_count) * Page;
	const uint64_t descriptorBytes = descriptor->getLength();
	if (descriptorOffset > descriptorBytes || bytes > descriptorBytes - descriptorOffset ||
	    backingOffset > backingBytes || bytes > backingBytes - backingOffset)
		return -22;
	uint32_t surfaceKind = 0u;
	if (tiled && info.pages && info.page_count)
		surfaceKind = tegra_gm20b_surface_kind(info.pages[0]);
	const uint32_t wantKind = tiled ? (surfaceKind ? surfaceKind : TGM_LAYOUT_KIND) : 0u;
	tgm_mem *canonical = nullptr;
	const bool hasCanonical =
		switch_gpu_backing_tgm_mem_existing(backing, info.page_count, &canonical) == 0;
	for (auto &entry : cache->descriptors) {
		auto *map = entry.mapping;
		if (entry.descriptor == descriptor && map && map->backing == backing &&
		    map->allocation == info.allocation_id &&
		    map->generation == info.allocation_generation &&
		    (!tiled || (map->tiled && map->tiledKind == wantKind)) &&
		    (!hasCanonical || map->memory == canonical) &&
		    contains(entry, descriptorOffset, backingOffset, bytes)) {
			++entry.users;
			entry.lastUse = ++cache->clock;
			*out = &entry;
			return 0;
		}
	}
	for (auto &candidate : cache->descriptors) {
		auto *map = candidate.mapping;
		if (candidate.descriptor && !candidate.users && map && map->backing == backing &&
		    map->allocation == info.allocation_id &&
		    map->generation == info.allocation_generation &&
		    ((tiled && (!map->tiled || map->tiledKind != wantKind)) ||
		     (hasCanonical && map->memory != canonical)))
			discard(candidate);
	}
	SwitchGpuCopyMapping *entry = nullptr;
	for (auto &candidate : cache->descriptors)
		if (!candidate.descriptor) {
			entry = &candidate;
			break;
		}
	if (!entry)
		entry = oldest(cache);
	if (!entry)
		return -16;
	discard(*entry);
	if (descriptor->prepare() != kIOReturnSuccess)
		return -22;
	descriptor->retain();
	entry->descriptor = descriptor;
	entry->users = 1;
	entry->descriptorStart =
		descriptorOffset > backingOffset ? descriptorOffset - backingOffset : 0;
	entry->backingStart =
		backingOffset > descriptorOffset ? backingOffset - descriptorOffset : 0;
	entry->length = descriptorBytes - entry->descriptorStart;
	if (entry->length > backingBytes - entry->backingStart)
		entry->length = backingBytes - entry->backingStart;
	for (uint64_t offset = 0; offset < entry->length; offset += Page) {
		const uint64_t physical = descriptor->getPhysicalSegment(
			entry->descriptorStart + offset, nullptr, kIOMemoryMapperNone);
		if (!physical || physical != info.pages[(entry->backingStart + offset) / Page]) {
			discard(*entry);
			return -22;
		}
	}
	int result;
	while ((result = switch_gpu_relayout_mapping_backing(vm, &cache->allocations, backing,
							     tiled, &entry->mapping)) == -16) {
		auto *unused = oldest(cache);
		if (!unused || unused == entry)
			break;
		discard(*unused);
	}
	if (result) {
		discard(*entry);
		return result;
	}
	entry->users = 1;
	entry->lastUse = ++cache->clock;
	*out = entry;
	return 0;
}

void switch_gpu_copy_mapping_put(SwitchGpuCopyMapping *mapping)
{
	if (mapping)
		--mapping->users;
}

void switch_gpu_copy_mappings_clear(tgm_vm *vm, SwitchGpuCopyMappings *cache)
{
	for (auto &entry : cache->descriptors)
		if (!entry.users)
			discard(entry);
	switch_gpu_relayout_cache_clear(vm, &cache->allocations);
}
