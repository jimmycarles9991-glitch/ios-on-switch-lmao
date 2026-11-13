// SPDX-License-Identifier: BSD-2-Clause-Patent
// An engine's mappings of other engines' backings in its own SMMU space, cached by allocation so memory that comes back
// (a decoder's picture pool, a swapchain) maps once: each held by its users, the least recently used idle one evicted
// for room, within a cap on the bytes mapped. A mapping retains its backing, so the pages stay until it is evicted.
// Space provides static uint64_t map(const uint64_t *pages, uint32_t count, bool writable), 0 when it cannot, and
// static void unmap(uint64_t address, uint32_t count). Pages are 4 KiB (switch_gpu_backing_info).
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdint.h>
#include "SwitchGpuBacking.h"

struct SwitchGpuBackingMap {
	switch_gpu_backing *backing = nullptr;
	uint64_t allocation = 0, generation = 0, address = 0, used = 0;
	uint32_t firstPage = 0, pages = 0, users = 0;
	bool writable = false;
};

template <typename Map, typename Space, uint32_t Slots, uint64_t CapBytes>
struct SwitchGpuBackingMaps {
	Map maps[Slots];
	IOLock *lock = nullptr;
	uint64_t clock = 0, bytes = 0, made = 0;

	bool start()
	{
		lock = IOLockAlloc();
		return lock != nullptr;
	}

	Map *acquire(switch_gpu_backing *backing, bool writable, uint32_t firstPage,
		     uint32_t pageCount)
	{
		if (!lock || !backing)
			return nullptr;
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(backing, &info);
		if (firstPage > info.page_count || pageCount > info.page_count - firstPage)
			return nullptr;
		const uint64_t size = static_cast<uint64_t>(pageCount) << 12;
		if (!size || size > CapBytes)
			return nullptr;
		IOLockLock(lock);
		for (auto &map : maps) {
			if (map.backing && map.allocation == info.allocation_id &&
			    map.generation == info.allocation_generation &&
			    map.writable == writable && map.firstPage == firstPage &&
			    map.pages == pageCount) {
				++map.users;
				map.used = ++clock;
				IOLockUnlock(lock);
				return &map;
			}
		}
		while (bytes + size > CapBytes) {
			Map *map = oldest(true);
			if (!map || !map->backing)
				break;
			evict(*map);
		}
		Map *map = bytes + size <= CapBytes ? oldest() : nullptr;
		if (map) {
			evict(*map);
			const uint64_t address =
				Space::map(info.pages + firstPage, pageCount, writable);
			if (address) {
				switch_gpu_backing_retain(backing);
				__atomic_add_fetch(&made, uint64_t(1), __ATOMIC_RELAXED);
				static_cast<SwitchGpuBackingMap &>(*map) = {
					backing,
					info.allocation_id,
					info.allocation_generation,
					address,
					++clock,
					firstPage,
					pageCount,
					1,
					writable};
				bytes += size;
			} else {
				map = nullptr;
			}
		}
		IOLockUnlock(lock);
		return map;
	}

	Map *acquire(switch_gpu_backing *backing, bool writable)
	{
		if (!backing)
			return nullptr;
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(backing, &info);
		return acquire(backing, writable, 0, info.page_count);
	}

	void release(Map *map)
	{
		if (!map)
			return;
		IOLockLock(lock);
		if (!map->users)
			panic("SwitchGpuBackingMaps: a mapping released twice");
		--map->users;
		map->used = ++clock;
		IOLockUnlock(lock);
	}

	uint64_t takeMade()
	{
		return __atomic_exchange_n(&made, uint64_t(0), __ATOMIC_RELAXED);
	}

	// Caller holds the lock, or owns every mapping (a test's teardown).
	void evict(Map &map)
	{
		if (!map.backing)
			return;
		Space::unmap(map.address, map.pages);
		switch_gpu_backing_release(map.backing);
		bytes -= static_cast<uint64_t>(map.pages) << 12;
		map = {};
	}

private:
	Map *oldest(bool occupied = false)
	{
		Map *candidate = nullptr;
		for (auto &map : maps) {
			if (!map.users && (!occupied || map.backing) &&
			    (!candidate || map.used < candidate->used))
				candidate = &map;
		}
		return candidate;
	}
};
