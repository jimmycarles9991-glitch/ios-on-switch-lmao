/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <limits.h>

#define SWITCH_GPU_POOL_EXTENTS 64u
#define SWITCH_GPU_POOL_CONTROL_READY_BYTES (16ULL << 20)

struct switch_gpu_pool_extent {
	unsigned long start, count;
	int clean;
};

struct switch_gpu_pool_extents {
	struct switch_gpu_pool_extent entries[SWITCH_GPU_POOL_EXTENTS];
	unsigned next;
};

static inline void switch_gpu_pool_remember(struct switch_gpu_pool_extents *cache,
					    unsigned long start, unsigned long count, int clean)
{
	if (!count)
		return;
	for (unsigned i = 0; i < SWITCH_GPU_POOL_EXTENTS; ++i) {
		struct switch_gpu_pool_extent *entry = &cache->entries[i];
		if (entry->count && entry->clean == clean && entry->start <= start + count &&
		    start <= entry->start + entry->count) {
			const unsigned long end = entry->start + entry->count > start + count
							  ? entry->start + entry->count
							  : start + count;
			start = entry->start < start ? entry->start : start;
			count = end - start;
			entry->count = 0;
		}
	}
	unsigned slot = SWITCH_GPU_POOL_EXTENTS;
	for (unsigned i = 0; i < SWITCH_GPU_POOL_EXTENTS; ++i) {
		if (!cache->entries[i].count) {
			slot = i;
			break;
		}
	}
	if (slot == SWITCH_GPU_POOL_EXTENTS) {
		slot = cache->next;
		cache->next = (cache->next + 1) % SWITCH_GPU_POOL_EXTENTS;
	}
	cache->entries[slot] = (struct switch_gpu_pool_extent){start, count, clean};
}

/* Every bitmap claim removes overlapping cached ranges, including nvkm and scrubber claims. */
static inline void switch_gpu_pool_claim(struct switch_gpu_pool_extents *cache, unsigned long start,
					 unsigned long count)
{
	const unsigned long end = start + count;
	struct switch_gpu_pool_extent tails[SWITCH_GPU_POOL_EXTENTS];
	unsigned ntails = 0;
	for (unsigned i = 0; i < SWITCH_GPU_POOL_EXTENTS; ++i) {
		struct switch_gpu_pool_extent *entry = &cache->entries[i];
		const unsigned long oldEnd = entry->start + entry->count;
		if (!entry->count || entry->start >= end || oldEnd <= start)
			continue;
		if (oldEnd > end)
			tails[ntails++] =
				(struct switch_gpu_pool_extent){end, oldEnd - end, entry->clean};
		entry->count = entry->start < start ? start - entry->start : 0;
	}
	for (unsigned i = 0; i < ntails; ++i)
		switch_gpu_pool_remember(cache, tails[i].start, tails[i].count, tails[i].clean);
}

static inline long switch_gpu_pool_cached(const struct switch_gpu_pool_extents *cache,
					  unsigned long count, unsigned long align, int clean)
{
	if (!count || !align || (align & (align - 1)))
		return -1;
	long found = -1;
	unsigned long waste = ULONG_MAX;
	for (unsigned i = 0; i < SWITCH_GPU_POOL_EXTENTS; ++i) {
		const struct switch_gpu_pool_extent *entry = &cache->entries[i];
		if (!entry->count || (clean && !entry->clean))
			continue;
		const unsigned long start = (entry->start + align - 1) & ~(align - 1);
		const unsigned long prefix = start - entry->start;
		if (prefix <= entry->count && count <= entry->count - prefix &&
		    entry->count - count < waste) {
			found = (long)start;
			waste = entry->count - count;
		}
	}
	return found;
}

static inline long switch_gpu_pool_find(const unsigned long *used, const unsigned long *dirty,
					unsigned long pages, unsigned long count,
					unsigned long align, int clean)
{
	if (!count || count > pages || !align || (align & (align - 1)))
		return -1;
	const unsigned long bits = sizeof(unsigned long) * CHAR_BIT;
	unsigned long start = 0;
	while (start <= pages - count) {
		unsigned long at = start;
		const unsigned long end = start + count;
		while (at < end) {
			const unsigned long word = at / bits, offset = at % bits;
			const unsigned long remaining = end - at;
			const unsigned long take =
				remaining < bits - offset ? remaining : bits - offset;
			const unsigned long mask = (take == bits ? ULONG_MAX : (1UL << take) - 1)
						   << offset;
			const unsigned long blocked =
				(used[word] | (clean ? dirty[word] : 0)) & mask;
			if (blocked) {
				start = (word * bits + (unsigned long)__builtin_ctzl(blocked) +
					 align) &
					~(align - 1);
				break;
			}
			at += take;
		}
		if (at == end)
			return (long)start;
	}
	return -1;
}

static inline unsigned long switch_gpu_pool_clean_count(const unsigned long *dirty,
							unsigned long start, unsigned long count)
{
	const unsigned long bits = sizeof(unsigned long) * CHAR_BIT;
	unsigned long clean = 0;
	while (count) {
		const unsigned long offset = start % bits;
		const unsigned long take = count < bits - offset ? count : bits - offset;
		const unsigned long mask = (take == bits ? ULONG_MAX : (1UL << take) - 1) << offset;
		clean += (unsigned long)__builtin_popcountl(~dirty[start / bits] & mask);
		start += take;
		count -= take;
	}
	return clean;
}
