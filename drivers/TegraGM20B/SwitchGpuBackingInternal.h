/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchGpuBacking.h"
#include "SwitchGpuWriterRanges.h"
#include "SwitchGpuResourceSet.h"
#include "SwitchGpuWriterPromise.h"

struct switch_gpu_backing_pages {
	switch_gpu_backing_pages *older;
	uint64_t *addresses;
	uint32_t count;
};

struct switch_gpu_backing_access {
	switch_gpu_backing_token token;
	uint64_t content;
	uint32_t access;
	bool noted;  // its failure was already logged, or needs no log
};

struct switch_gpu_backing {
	switch_gpu_backing *next;
	switch_gpu_backing *retiring_next, *expiry_next, *expiry_prev;
	IOLock *lock;
	uint64_t id, generation, content, host_epoch, host_visible_epoch;
	uint64_t clean_start, retirement_deadline, host_acquire_epoch;
	switch_gpu_backing_token host_acquire;
	uint32_t refs, surface_id;
	switch_gpu_backing_pages *pages;
	void *owner;
	void (*release_owner)(void *);
	void *gpu_memory;
	void (*release_gpu_memory)(void *);
	uint32_t gpu_memory_count, gpu_memory_flags;
	uint64_t *cpu_current;
	uint64_t cpu_current_content;
	uint32_t cpu_current_pages;
	switch_gpu_writer_range *writers, *writer_next;
	uint32_t writer_count, writer_capacity, writer_next_count, writer_next_capacity;
	bool cpu_tracking, writers_pending, writers_precise;
	switch_gpu_backing_layout layout;
	switch_gpu_backing_access *accesses;
	switch_gpu_cpu_lease *cpu_leases;
	uint32_t access_count, access_capacity;
	uint32_t cpu_readers, cpu_writers, concurrent_readers, concurrent_writers, dirty;
	uint32_t gpu_admission_readers, gpu_admission_writers;
	bool initial_clean, retiring;
	// Appended so every older field keeps its offset. set and set_access change under `lock`; shared is sticky.
	switch_gpu_resource_set *set;
	uint32_t set_access;
	bool shared;
	bool cpu_reached;  // a CPU lease or CpuComplete named it: sticky, and it is never a set member again
	uint32_t scanout_holds;  // atomic: switch_gpu_backing_scanout
	// writer promises on it (SwitchGpuWriterPromise.h), and the display leases taken behind them, which no admission
	// waits for
	switch_gpu_promise_ref promises[SWITCH_GPU_BACKING_PROMISES];
	uint32_t promise_count, promised_readers;
	// CPU threads asleep on it behind a pending promise (SwitchGpuPromiseSleepers.cpp, under its lock)
	switch_gpu_backing *promise_sleep_next, *promise_sleep_prev;
	uint32_t promise_sleepers, promise_woken;
};

struct switch_gpu_cpu_lease {
	switch_gpu_cpu_lease *next;
	switch_gpu_backing *backing;
	uint64_t offset, bytes;
	uint32_t access;
	switch_gpu_backing_token producer;
};

const switch_gpu_backing_engine *switch_gpu_backing_engine_ops(uint32_t engine);
bool switch_gpu_backing_same_timeline(const switch_gpu_backing_token &a,
				      const switch_gpu_backing_token &b);
bool switch_gpu_backing_later(const switch_gpu_backing_token &a, const switch_gpu_backing_token &b);
void switch_gpu_backing_prune_locked(switch_gpu_backing *backing);
void switch_gpu_backing_note_failure_locked(const switch_gpu_backing *backing,
					    switch_gpu_backing_access &access, int status);
void switch_gpu_backing_drop_token(const switch_gpu_backing_token &token);
extern "C" void switch_gpu_backing_cache(const uint64_t *pages, uint32_t count, uint64_t offset,
					 uint64_t bytes, int operation);
extern "C" int switch_gpu_backing_prepare_retirement(void);
extern "C" void switch_gpu_backing_schedule_retirement(void);
extern "C" void switch_gpu_backing_retire(void);

extern "C" uint64_t switch_gpu_backing_time_ns(void);
extern "C" void switch_gpu_backing_arm_expiry(uint64_t deadline_ns);
extern "C" void switch_gpu_backing_expire(void);

const switch_gpu_cpu_lease *switch_gpu_backing_find_lease_locked(const switch_gpu_backing *backing,
								 const switch_gpu_cpu_lease *lease);

void switch_gpu_backing_sort_uses(switch_gpu_backing_use *uses, uint32_t count);
/* Records already-published jobs as the backing's users, as commit records a job; pending ones only. */
int switch_gpu_backing_adopt_locked(switch_gpu_backing *backing,
				    const switch_gpu_backing_token *jobs, uint32_t count,
				    uint32_t access);
/* Something outside the set reaches a member, or its layout changes: the set's jobs become its users and it leaves. */
void switch_gpu_set_reached(switch_gpu_backing *backing, uint32_t cause);

// Threads asleep until a backing's CPU holders leave. A holder whose IOSurface client died leaves only at a sweep
// (switch_gpu_surface_sweep), which GPU jobs run while any thread or ready watch (switch_gpu_ready_waiting) waits.
extern uint32_t switch_gpu_cpu_sleepers;
void switch_gpu_backing_sleep_cpu_locked(switch_gpu_backing *backing);

#define SWITCH_GPU_PROMISE_PARKED (2 * SWITCH_GPU_BACKING_PROMISES)

// The promises a CPU reader parked on (SwitchGpuPromiseGiveUp.h), once each for its whole wait
struct switch_gpu_promise_parked {
	uint32_t count;
	uint32_t offset[SWITCH_GPU_PROMISE_PARKED], value[SWITCH_GPU_PROMISE_PARKED];
};

// The same where a pending writer promise may be what it waits for: a promise given up on wakes it too. A CPU reader
// (`who`; null for one that orders behind the promise on the GPU) parks on each pending promise into `parked`, and
// leaves them all with switch_gpu_promise_parked_leave once its wait is over. A wait across more promises than `parked`
// holds leaves the ones no longer pending first, so it parks on every promise it waits behind.
void switch_gpu_backing_sleep_promised_locked(switch_gpu_backing *backing,
					      switch_gpu_promise_parked *parked, const char *who);
void switch_gpu_promise_parked_leave(switch_gpu_promise_parked *parked);
extern "C" int tgm_promise_abandoned(uint32_t offset, uint32_t value);
extern "C" int switch_gpu_ready_waiting(void);

// Writer promises (SwitchGpuWriterPromise.h), under the backing's lock: the pending ones but the asking connection's own
// as tokens (at most capacity, -28 past it), or with no tokens their count. One given up on is not pending: its users go
// on without its writes. A CPU reader cannot acquire one on the GPU: it sleeps as for a CPU writer until the promised
// job is submitted or the promise is given back or up.
inline int switch_gpu_backing_promises_locked(const switch_gpu_backing *backing, uint32_t own_first,
					      uint32_t own_end, switch_gpu_backing_token *tokens,
					      uint32_t capacity)
{
	uint32_t offsets[SWITCH_GPU_BACKING_PROMISES], values[SWITCH_GPU_BACKING_PROMISES];
	const int found = switch_gpu_promise_ref_pending(backing->promises, backing->promise_count,
							 own_first, own_end, offsets, values,
							 SWITCH_GPU_BACKING_PROMISES);
	int pending = 0;
	for (int i = 0; i < found; ++i) {
		if (tgm_promise_abandoned(offsets[i], values[i]))
			continue;
		offsets[pending] = offsets[i];
		values[pending++] = values[i];
	}
	if (found < 0)
		return found;
	if (pending <= 0 || !tokens)
		return pending;
	if (uint32_t(pending) > capacity)
		return -28;
	for (int i = 0; i < pending; ++i)
		tokens[i] = {values[i], 0, SWITCH_GPU_ENGINE_PROMISE, offsets[i]};
	return pending;
}

inline bool switch_gpu_backing_promise_pending_locked(const switch_gpu_backing *backing)
{
	return backing->promise_count &&
	       switch_gpu_backing_promises_locked(backing, 0, 0, nullptr, 0) > 0;
}

// CPU readers a GPU write is held off by: a display lease taken behind writer promises is not one, as its reader orders
// behind the promised job on the GPU.
inline uint32_t switch_gpu_backing_exclusive_readers(const switch_gpu_backing *backing)
{
	return backing->cpu_readers - backing->promised_readers;
}

bool switch_gpu_backing_cpu_current_prepare_locked(switch_gpu_backing *);
void switch_gpu_backing_cpu_cache_locked(switch_gpu_backing *, uint64_t offset, uint64_t bytes,
					 int operation);
void switch_gpu_backing_cpu_current_written_locked(switch_gpu_backing *, uint64_t offset,
						   uint64_t bytes);
void switch_gpu_backing_cpu_written_locked(switch_gpu_backing *, uint64_t offset, uint64_t bytes);
int switch_gpu_backing_writer_prepare_locked(switch_gpu_backing *,
					     const switch_gpu_backing_token &);
void switch_gpu_backing_writer_commit_locked(switch_gpu_backing *,
					     const switch_gpu_backing_token &);
void switch_gpu_backing_writer_abort_locked(switch_gpu_backing *);
void switch_gpu_backing_writer_free_locked(switch_gpu_backing *);
bool switch_gpu_backing_writer_prefix_locked(const switch_gpu_backing *,
					     const switch_gpu_backing_token &, uint64_t offset,
					     uint64_t bytes);
