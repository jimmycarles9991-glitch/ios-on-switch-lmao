/* SPDX-License-Identifier: MIT */
/* Lock-free directory of live GM20B channels by uid, for submission paths that need a producer's
 * fence-pool offset, hardware channel id, or address space without taking tgm_live_lock. Writers
 * serialize under tgm_live_lock (channel creation and teardown); readers use per-slot sequence
 * counts with acquire/release atomic loads and never dereference a tgm_chan pointer. */
#pragma once
#include <stdint.h>
#include "SwitchGpuChanTable.h"

#define SWITCH_GPU_LIVE_SLOTS SWITCH_GPU_CHAN_SLOTS
#define SWITCH_GPU_LIVE_HINTS 1024u
#define SWITCH_GPU_LIVE_NO_FENCE UINT32_MAX

struct switch_gpu_live_info {
	uint64_t id;
	uint32_t fence_offset;
	uint32_t chid;
	const void *vm;
};

struct switch_gpu_live_slot {
	uint32_t seq;
	uint32_t fence_offset;
	uint64_t id;
	const void *vm;
};

struct switch_gpu_live_dir {
	struct switch_gpu_live_slot slots[SWITCH_GPU_LIVE_SLOTS];
	uint64_t live_mask[SWITCH_GPU_LIVE_SLOTS / 64u];
	uint16_t hint[SWITCH_GPU_LIVE_HINTS];
};

static inline void switch_gpu_live_dir_set(struct switch_gpu_live_dir *dir, uint32_t chid,
					   uint64_t id, uint32_t fence_offset, const void *vm)
{
	struct switch_gpu_live_slot *slot;
	uint32_t seq;

	if (chid >= SWITCH_GPU_LIVE_SLOTS)
		return;
	slot = &dir->slots[chid];
	seq = __atomic_load_n(&slot->seq, __ATOMIC_RELAXED);
	__atomic_store_n(&slot->seq, seq + 1u, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&slot->id, id, __ATOMIC_RELAXED);
	__atomic_store_n(&slot->fence_offset, fence_offset, __ATOMIC_RELAXED);
	__atomic_store_n(&slot->vm, vm, __ATOMIC_RELAXED);
	__atomic_store_n(&slot->seq, seq + 2u, __ATOMIC_RELEASE);
	if (id) {
		const uint64_t mask =
			__atomic_load_n(&dir->live_mask[chid / 64u], __ATOMIC_RELAXED) |
			(UINT64_C(1) << (chid % 64u));
		__atomic_store_n(&dir->live_mask[chid / 64u], mask, __ATOMIC_RELEASE);
		__atomic_store_n(&dir->hint[(uint32_t)id & (SWITCH_GPU_LIVE_HINTS - 1u)],
				 (uint16_t)(chid + 1u), __ATOMIC_RELEASE);
	} else {
		const uint64_t mask =
			__atomic_load_n(&dir->live_mask[chid / 64u], __ATOMIC_RELAXED) &
			~(UINT64_C(1) << (chid % 64u));
		__atomic_store_n(&dir->live_mask[chid / 64u], mask, __ATOMIC_RELEASE);
	}
}

static inline void switch_gpu_live_dir_clear(struct switch_gpu_live_dir *dir, uint32_t chid)
{
	switch_gpu_live_dir_set(dir, chid, 0, SWITCH_GPU_LIVE_NO_FENCE, 0);
}

static inline int switch_gpu_live_dir_read_slot(const struct switch_gpu_live_dir *dir,
						uint32_t chid, uint32_t uid,
						struct switch_gpu_live_info *out)
{
	const struct switch_gpu_live_slot *slot = &dir->slots[chid];

	for (;;) {
		const uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
		uint64_t id;
		uint32_t fence_offset;
		const void *vm;

		if (seq & 1u)
			continue;
		id = __atomic_load_n(&slot->id, __ATOMIC_RELAXED);
		fence_offset = __atomic_load_n(&slot->fence_offset, __ATOMIC_RELAXED);
		vm = __atomic_load_n(&slot->vm, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&slot->seq, __ATOMIC_RELAXED) != seq)
			continue;
		if (!id || (uint32_t)id != uid)
			return 0;
		out->id = id;
		out->fence_offset = fence_offset;
		out->chid = chid;
		out->vm = vm;
		return 1;
	}
}

static inline int switch_gpu_live_dir_find(const struct switch_gpu_live_dir *dir, uint32_t uid,
					   struct switch_gpu_live_info *out)
{
	const uint16_t hint =
		__atomic_load_n(&dir->hint[uid & (SWITCH_GPU_LIVE_HINTS - 1u)], __ATOMIC_ACQUIRE);
	uint32_t word;

	if (!uid)
		return 0;
	if (hint && hint <= SWITCH_GPU_LIVE_SLOTS &&
	    switch_gpu_live_dir_read_slot(dir, (uint32_t)(hint - 1u), uid, out))
		return 1;
	for (word = 0; word < SWITCH_GPU_LIVE_SLOTS / 64u; ++word) {
		uint64_t bits = __atomic_load_n(&dir->live_mask[word], __ATOMIC_ACQUIRE);

		while (bits) {
			const uint32_t chid = word * 64u + (uint32_t)__builtin_ctzll(bits);

			bits &= bits - 1u;
			if (switch_gpu_live_dir_read_slot(dir, chid, uid, out))
				return 1;
		}
	}
	return 0;
}
