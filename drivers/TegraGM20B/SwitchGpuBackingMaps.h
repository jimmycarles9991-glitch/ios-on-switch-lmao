// SPDX-License-Identifier: BSD-2-Clause-Patent
// An engine's mappings of other engines' backings in its own SMMU space, found again by allocation and page-list
// generation, so memory that comes back (a decoder's picture pool, a swapchain) maps once for the life of its
// allocation. Each is held by its users; the least recently used idle one is evicted only for room, within a cap on the
// bytes mapped. An entry holds no reference on its backing: every user holds one from acquire until its job is recorded
// on the backing, whose retirement then waits for the job, and the backing's destruction calls forget, which unmaps its
// allocation's entries before the pages go back.
// Space provides static uint64_t map(const uint64_t *pages, uint32_t count, bool writable), 0 when it cannot, and
// static void unmap(uint64_t address, uint32_t count). Pages are 4 KiB (switch_gpu_backing_info).
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdint.h>
#include "SwitchGpuBacking.h"

struct SwitchGpuBackingMap {
	uint64_t allocation = 0, generation = 0, address = 0, used = 0;
	uint32_t firstPage = 0, pages = 0, users = 0;
	bool writable = false;
	// its allocation was destroyed while a user had not released it yet: unmapped, found by nobody, freed at the release
	bool orphan = false;
};

struct SwitchGpuBackingMapKey {
	uint64_t allocation, generation;
	uint32_t firstPage, pages;
	bool writable;
};

// Debug kernels: mappings made since boot, and of them remakes of one evicted for room.
struct SwitchGpuBackingMapTotals {
	uint64_t made, remade;
};

template <typename Map, typename Space, uint32_t Engine, uint32_t Slots, uint64_t CapBytes>
struct SwitchGpuBackingMaps {
	static constexpr uint32_t kEvictedKeys = 64;
	Map maps[Slots];
	IOLock *lock = nullptr;
	uint64_t clock = 0, bytes = 0, made = 0;
	SwitchGpuBackingMapTotals totals = {};
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	SwitchGpuBackingMapKey evicted[kEvictedKeys] = {};
	uint32_t evictedNext = 0;
#endif

	bool start(void (*forget)(uint64_t))
	{
		lock = IOLockAlloc();
		return lock && !switch_gpu_backing_register_forget(Engine, forget);
	}

	// info: the caller's switch_gpu_backing_describe of `backing`.
	Map *acquire(switch_gpu_backing *backing, const switch_gpu_backing_info &info,
		     bool writable, uint32_t firstPage, uint32_t pageCount)
	{
		if (!lock || !backing)
			return nullptr;
		if (firstPage > info.page_count || pageCount > info.page_count - firstPage)
			return nullptr;
		const uint64_t size = static_cast<uint64_t>(pageCount) << 12;
		if (!size || size > CapBytes)
			return nullptr;
		const SwitchGpuBackingMapKey key = {info.allocation_id, info.allocation_generation,
						    firstPage, pageCount, writable};
		IOLockLock(lock);
		for (auto &map : maps) {
			if (holds(map, key)) {
				++map.users;
				map.used = ++clock;
				IOLockUnlock(lock);
				return &map;
			}
		}
		for (auto &map : maps) {
			if (map.address && !map.users && map.allocation == key.allocation &&
			    map.generation != key.generation)
				evict(map);
		}
		while (bytes + size > CapBytes) {
			Map *map = oldest(true);
			if (!map)
				break;
			evictForRoom(*map);
		}
		Map *map = bytes + size <= CapBytes ? oldest() : nullptr;
		if (map) {
			evictForRoom(*map);
			const uint64_t address =
				Space::map(info.pages + firstPage, pageCount, writable);
			if (address) {
				switch_gpu_backing_note_mapped(backing, Engine);
				__atomic_add_fetch(&made, uint64_t(1), __ATOMIC_RELAXED);
				noteMade(key);
				static_cast<SwitchGpuBackingMap &>(*map) = {
					key.allocation, key.generation, address, ++clock,
					firstPage,      pageCount,      1,       writable};
				bytes += size;
			} else {
				map = nullptr;
			}
		}
		IOLockUnlock(lock);
		return map;
	}

	Map *acquire(switch_gpu_backing *backing, bool writable, uint32_t firstPage,
		     uint32_t pageCount)
	{
		if (!backing)
			return nullptr;
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(backing, &info);
		return acquire(backing, info, writable, firstPage, pageCount);
	}

	Map *acquire(switch_gpu_backing *backing, bool writable)
	{
		if (!backing)
			return nullptr;
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(backing, &info);
		return acquire(backing, info, writable, 0, info.page_count);
	}

	void release(Map *map)
	{
		if (!map)
			return;
		IOLockLock(lock);
		if (!map->users)
			panic("SwitchGpuBackingMaps: a mapping released twice");
		if (!--map->users && map->orphan)
			*map = {};
		else
			map->used = ++clock;
		IOLockUnlock(lock);
	}

	// The allocation's backing is being destroyed: none of its jobs is pending, and nobody can acquire it again.
	void forget(uint64_t allocation)
	{
		if (!lock || !allocation)
			return;
		IOLockLock(lock);
		for (auto &map : maps) {
			if (!map.address || map.allocation != allocation)
				continue;
			const uint32_t users = map.users;
			evict(map);
			if (users) {
				map.users = users;
				map.orphan = true;
			}
		}
		IOLockUnlock(lock);
	}

	uint64_t takeMade()
	{
		return __atomic_exchange_n(&made, uint64_t(0), __ATOMIC_RELAXED);
	}

	SwitchGpuBackingMapTotals readTotals()
	{
		if (!lock)
			return {};
		IOLockLock(lock);
		const SwitchGpuBackingMapTotals now = totals;
		IOLockUnlock(lock);
		return now;
	}

	// Caller holds the lock, or owns every mapping (a test's teardown).
	void evict(Map &map)
	{
		if (!map.address)
			return;
		Space::unmap(map.address, map.pages);
		bytes -= static_cast<uint64_t>(map.pages) << 12;
		map = {};
	}

private:
	static bool holds(const Map &map, const SwitchGpuBackingMapKey &key)
	{
		return map.address && map.allocation == key.allocation &&
		       map.generation == key.generation && map.writable == key.writable &&
		       map.firstPage == key.firstPage && map.pages == key.pages;
	}

	void evictForRoom(Map &map)
	{
		if (!map.address)
			return;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
		evicted[evictedNext++ % kEvictedKeys] = {map.allocation, map.generation,
							 map.firstPage, map.pages, map.writable};
#endif
		evict(map);
	}

	void noteMade(const SwitchGpuBackingMapKey &key)
	{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
		++totals.made;
		for (auto &old : evicted) {
			if (old.allocation == key.allocation && old.generation == key.generation &&
			    old.firstPage == key.firstPage && old.pages == key.pages &&
			    old.writable == key.writable) {
				old = {};
				++totals.remade;
				return;
			}
		}
#else
		(void)key;
#endif
	}

	Map *oldest(bool occupied = false)
	{
		Map *candidate = nullptr;
		for (auto &map : maps) {
			if (!map.users && (!occupied || map.address) &&
			    (!candidate || map.used < candidate->used))
				candidate = &map;
		}
		return candidate;
	}
};
