/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

struct tgm_vm;
struct tgm_mem;
struct switch_gpu_mapping;
struct switch_gpu_backing;

struct SwitchGpuRelayoutMapping {
	switch_gpu_backing *backing;
	tgm_mem *memory;
	switch_gpu_mapping *pitchMapping, *tiledMapping;
	uint64_t *pages;
	const uint64_t *sourcePages;
	uint64_t allocation, generation, bytes, pitch, tiled, lastUse;
	uint32_t pageCount, users;
};

struct SwitchGpuRelayoutCache {
	static constexpr uint32_t Capacity = 16;
	static constexpr uint64_t MaximumBytes = 128ULL << 20;
	static constexpr uint64_t WindowBytes = 512ULL << 20;
	SwitchGpuRelayoutMapping mappings[Capacity];
	uint64_t bytes, clock;
};

/* Caller serializes cache access and keeps each mapping acquired through actual GPU retirement. */
int switch_gpu_relayout_mapping(tgm_vm *vm, SwitchGpuRelayoutCache *cache, const uint64_t *pages,
				uint32_t count, bool tiled, SwitchGpuRelayoutMapping **out);
int switch_gpu_relayout_mapping_backing(tgm_vm *vm, SwitchGpuRelayoutCache *cache,
					switch_gpu_backing *backing, bool tiled,
					SwitchGpuRelayoutMapping **out);
void switch_gpu_relayout_mapping_put(SwitchGpuRelayoutMapping *mapping);
void switch_gpu_relayout_cache_clear(tgm_vm *vm, SwitchGpuRelayoutCache *cache);
