// SPDX-License-Identifier: MIT
#include <string.h>
#include "SwitchGpuBackingInternal.h"
#include "SwitchGpuJobStats.h"

struct switch_gpu_resource_set {
	IOLock *lock;  // a leaving member's wait for the submissions inside
	uint32_t refs, generation, used, host_written, inside, waiting;
	volatile uint32_t *mirror;

	// one writer per slot (its channel's submitter); readers retry while seq is odd or moved
	struct {
		uint32_t seq;
		switch_gpu_backing_token job;
	} slots[SWITCH_GPU_SET_SLOTS];

	switch_gpu_set_member *members;
	uint32_t count, capacity;
};

namespace
{
void put(switch_gpu_resource_set *set)
{
	if (__atomic_sub_fetch(&set->refs, 1u, __ATOMIC_ACQ_REL))
		return;
	IODelete(set->members, switch_gpu_set_member, set->capacity);
	IOLockFree(set->lock);
	IODelete(set, switch_gpu_resource_set, 1);
}

uint32_t read_jobs(const switch_gpu_resource_set *set, switch_gpu_backing_token *jobs)
{
	const uint32_t used = __atomic_load_n(&set->used, __ATOMIC_ACQUIRE);
	uint32_t count = 0;
	for (uint32_t slot = 0; slot < SWITCH_GPU_SET_SLOTS; ++slot) {
		if (!(used & (1u << slot)))
			continue;
		const auto &entry = set->slots[slot];
		for (;;) {
			const uint32_t seq = __atomic_load_n(&entry.seq, __ATOMIC_ACQUIRE);
			jobs[count] = {
				__atomic_load_n(&entry.job.value, __ATOMIC_RELAXED),
				__atomic_load_n(&entry.job.reset_generation, __ATOMIC_RELAXED),
				__atomic_load_n(&entry.job.engine, __ATOMIC_RELAXED),
				__atomic_load_n(&entry.job.queue, __ATOMIC_RELAXED)};
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if (!(seq & 1) && __atomic_load_n(&entry.seq, __ATOMIC_RELAXED) == seq)
				break;
		}
		if (jobs[count].engine)
			++count;
	}
	return count;
}

// The set's jobs as the member's users: writers when they may write it, else readers. A CPU-tracked member's writers
// cover it whole, so CpuComplete maintains what any of them wrote.
int adopt_locked(switch_gpu_backing *backing, const switch_gpu_resource_set *set)
{
	switch_gpu_backing_token jobs[SWITCH_GPU_SET_SLOTS];
	const uint32_t count = read_jobs(set, jobs);
	const bool writes = backing->set_access & SWITCH_GPU_BACKING_WRITE;
	int result = switch_gpu_backing_adopt_locked(
		backing, jobs, count, writes ? SWITCH_GPU_BACKING_WRITE : SWITCH_GPU_BACKING_READ);
	for (uint32_t i = 0; !result && writes && backing->cpu_tracking && i < count; ++i) {
		result = switch_gpu_backing_writer_prepare_locked(backing, jobs[i]);
		if (!result)
			switch_gpu_backing_writer_commit_locked(backing, jobs[i]);
	}
	return result;
}

// Setup only (the set's submissions excluded): a member the set lets go keeps the set's pending jobs as its users.
void leave(switch_gpu_backing *backing, const switch_gpu_resource_set *set)
{
	IOLockLock(backing->lock);
	if (backing->set == set) {
		adopt_locked(backing, set);
		backing->set = nullptr;
		backing->set_access = 0;
	}
	IOLockUnlock(backing->lock);
}

void wait_inside(switch_gpu_resource_set *set)
{
	if (!__atomic_load_n(&set->inside, __ATOMIC_SEQ_CST))
		return;
	IOLockLock(set->lock);
	__atomic_add_fetch(&set->waiting, 1u, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&set->inside, __ATOMIC_SEQ_CST))
		IOLockSleep(set->lock, &set->inside, THREAD_UNINT);
	__atomic_sub_fetch(&set->waiting, 1u, __ATOMIC_SEQ_CST);
	IOLockUnlock(set->lock);
}

// Leaves the member list as it is: replace and destroy release what it holds.
void detach(switch_gpu_backing *backing, const switch_gpu_resource_set *set)
{
	IOLockLock(backing->lock);
	if (backing->set == set) {
		backing->set = nullptr;
		backing->set_access = 0;
	}
	IOLockUnlock(backing->lock);
}

// members: sorted by allocation id, one entry per backing
bool listed(const switch_gpu_set_member *members, uint32_t count, const switch_gpu_backing *backing)
{
	uint32_t first = 0, last = count;
	while (first < last) {
		const uint32_t middle = first + (last - first) / 2;
		if (members[middle].backing->id < backing->id)
			first = middle + 1;
		else
			last = middle;
	}
	return first < count && members[first].backing == backing;
}

// The caller's members by allocation id, a backing named twice once with both accesses; 0 when one is malformed.
uint32_t sorted_members(const switch_gpu_set_member *members, uint32_t count,
			switch_gpu_backing_use *uses)
{
	constexpr uint32_t kAccess = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE;
	for (uint32_t i = 0; i < count; ++i) {
		if (!members[i].backing || members[i].reserved || !(members[i].access & kAccess) ||
		    (members[i].access & ~kAccess))
			return 0;
		uses[i] = {members[i].backing, members[i].access, 0};
	}
	switch_gpu_backing_sort_uses(uses, count);
	uint32_t unique = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (unique && uses[unique - 1].backing == uses[i].backing)
			uses[unique - 1].access |= uses[i].access;
		else
			uses[unique++] = uses[i];
	}
	return unique;
}
}  // namespace

extern "C" int switch_gpu_set_create(volatile uint32_t *mirror, switch_gpu_resource_set **out)
{
	if (!out)
		return -22;
	auto *set = IONew(switch_gpu_resource_set, 1);
	if (!set)
		return -12;
	bzero(set, sizeof(*set));
	set->lock = IOLockAlloc();
	if (!set->lock) {
		IODelete(set, switch_gpu_resource_set, 1);
		return -12;
	}
	set->refs = 1;
	set->mirror = mirror;
	if (mirror)
		*mirror = 0;
	*out = set;
	return 0;
}

extern "C" uint32_t switch_gpu_set_generation(const switch_gpu_resource_set *set)
{
	return __atomic_load_n(&set->generation, __ATOMIC_SEQ_CST);
}

namespace
{
void bump(switch_gpu_resource_set *set)
{
	__atomic_add_fetch(&set->generation, 1u, __ATOMIC_SEQ_CST);
	if (!set->mirror)
		return;
	// Two bumps can race their stores: the word ends at the newest generation either way.
	for (uint32_t seen = 0;;) {
		const uint32_t now = __atomic_load_n(&set->generation, __ATOMIC_SEQ_CST);
		if (now == seen)
			break;
		__atomic_store_n(set->mirror, now, __ATOMIC_RELEASE);
		seen = now;
	}
}
}  // namespace

extern "C" void switch_gpu_set_invalidate(switch_gpu_resource_set *set, uint32_t cause)
{
	if (cause >= SWITCH_GPU_SET_CAUSES)
		return;
	bump(set);
	switch_gpu_job_invalidated(cause);
}

extern "C" int switch_gpu_set_attach(switch_gpu_resource_set *set,
				     const switch_gpu_set_member *members, uint32_t count,
				     uint32_t *generation, switch_gpu_backing **refused)
{
	if (refused)
		*refused = nullptr;
	if (!set || !generation || (count && !members))
		return -22;
	auto *uses = count ? IONew(switch_gpu_backing_use, count) : nullptr;
	if (count && !uses)
		return -12;
	const uint32_t unique = count ? sorted_members(members, count, uses) : 0;
	auto *kept = unique ? IONew(switch_gpu_set_member, unique) : nullptr;
	int result = count && !unique ? -22 : unique && !kept ? -12 : 0;
	uint32_t attached = 0;
	for (; !result && attached < unique; ++attached) {
		auto *backing = uses[attached].backing;
		IOLockLock(backing->lock);
		if (backing->set && backing->set != set) {
			result = -16;
		} else if (backing->shared || backing->surface_id || backing->cpu_reached) {
			result = -22;
		} else if (!backing->set) {
			backing->set = set;
			backing->set_access = uses[attached].access;
		}
		IOLockUnlock(backing->lock);
		if (result) {
			if (refused)
				*refused = backing;
			break;
		}
		switch_gpu_backing_retain(backing);
		kept[attached] = {backing, uses[attached].access, 0};
	}
	IODelete(uses, switch_gpu_backing_use, count);
	if (result) {
		for (uint32_t i = 0; i < attached; ++i) {
			if (!listed(set->members, set->count, kept[i].backing))
				detach(kept[i].backing, set);
			switch_gpu_backing_release(kept[i].backing);
		}
		IODelete(kept, switch_gpu_set_member, unique);
		return result;
	}
	for (uint32_t i = 0; i < unique; ++i) {
		IOLockLock(kept[i].backing->lock);
		if (kept[i].backing->set == set)
			kept[i].backing->set_access = kept[i].access;
		IOLockUnlock(kept[i].backing->lock);
	}
	for (uint32_t i = 0; i < set->count; ++i) {
		if (!listed(kept, unique, set->members[i].backing))
			leave(set->members[i].backing, set);
		switch_gpu_backing_release(set->members[i].backing);
	}
	IODelete(set->members, switch_gpu_set_member, set->capacity);
	set->members = kept;
	set->count = set->capacity = unique;
	if (switch_gpu_set_generation(set))
		switch_gpu_set_invalidate(set, SWITCH_GPU_SET_CAUSE_REPLACE);
	else
		bump(set);
	*generation = switch_gpu_set_generation(set);
	return 0;
}

extern "C" void switch_gpu_set_destroy(switch_gpu_resource_set *set)
{
	if (!set)
		return;
	for (uint32_t i = 0; i < set->count; ++i) {
		leave(set->members[i].backing, set);
		switch_gpu_backing_release(set->members[i].backing);
	}
	IODelete(set->members, switch_gpu_set_member, set->capacity);
	set->members = nullptr;
	set->count = set->capacity = 0;
	put(set);
}

extern "C" int switch_gpu_set_enter(switch_gpu_resource_set *set, uint32_t generation)
{
	__atomic_add_fetch(&set->inside, 1u, __ATOMIC_SEQ_CST);
	if (generation && __atomic_load_n(&set->generation, __ATOMIC_SEQ_CST) == generation)
		return 1;
	switch_gpu_set_leave(set);
	return 0;
}

extern "C" void switch_gpu_set_leave(switch_gpu_resource_set *set)
{
	if (__atomic_sub_fetch(&set->inside, 1u, __ATOMIC_SEQ_CST) ||
	    !__atomic_load_n(&set->waiting, __ATOMIC_SEQ_CST))
		return;
	IOLockLock(set->lock);
	IOLockWakeup(set->lock, &set->inside, false);
	IOLockUnlock(set->lock);
}

extern "C" void switch_gpu_set_note_job(switch_gpu_resource_set *set, uint32_t slot,
					const switch_gpu_backing_token *job)
{
	if (slot >= SWITCH_GPU_SET_SLOTS || !job)
		return;
	auto &entry = set->slots[slot];
	const uint32_t seq = entry.seq;
	__atomic_store_n(&entry.seq, seq + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&entry.job.value, job->value, __ATOMIC_RELAXED);
	__atomic_store_n(&entry.job.reset_generation, job->reset_generation, __ATOMIC_RELAXED);
	__atomic_store_n(&entry.job.engine, job->engine, __ATOMIC_RELAXED);
	__atomic_store_n(&entry.job.queue, job->queue, __ATOMIC_RELAXED);
	__atomic_store_n(&entry.seq, seq + 2, __ATOMIC_RELEASE);
	if (!(__atomic_load_n(&set->used, __ATOMIC_RELAXED) & (1u << slot)))
		__atomic_fetch_or(&set->used, 1u << slot, __ATOMIC_RELEASE);
}

extern "C" uint32_t switch_gpu_set_jobs(const switch_gpu_resource_set *set,
					switch_gpu_backing_token *jobs)
{
	return read_jobs(set, jobs);
}

extern "C" uint32_t switch_gpu_set_used(const switch_gpu_resource_set *set)
{
	return __atomic_load_n(&set->used, __ATOMIC_ACQUIRE);
}

extern "C" uint32_t switch_gpu_set_seqno(const switch_gpu_resource_set *set, uint32_t slot)
{
	return slot < SWITCH_GPU_SET_SLOTS
		       ? uint32_t(__atomic_load_n(&set->slots[slot].job.value, __ATOMIC_ACQUIRE))
		       : 0;
}

extern "C" uint32_t switch_gpu_set_waits(const switch_gpu_resource_set *set, uint32_t slot,
					 uint32_t live, const uint32_t *completed,
					 switch_gpu_set_wait *waits)
{
	const uint32_t others = switch_gpu_set_used(set) & live & ~(1u << slot);
	uint32_t count = 0;
	for (uint32_t other = 0; other < SWITCH_GPU_SET_SLOTS; ++other) {
		const uint32_t seqno = switch_gpu_set_seqno(set, other);
		if ((others & (1u << other)) && int32_t(completed[other] - seqno) < 0)
			waits[count++] = {other, seqno};
	}
	return count;
}

extern "C" void switch_gpu_set_killed(switch_gpu_resource_set *set, uint32_t slot)
{
	if (slot >= SWITCH_GPU_SET_SLOTS || !(switch_gpu_set_used(set) & (1u << slot)))
		return;
	__atomic_fetch_and(&set->used, ~(1u << slot), __ATOMIC_RELEASE);
	switch_gpu_set_invalidate(set, SWITCH_GPU_SET_CAUSE_KILL);
}

extern "C" void switch_gpu_set_host_written(switch_gpu_resource_set *set)
{
	__atomic_store_n(&set->host_written, 1u, __ATOMIC_RELEASE);
}

extern "C" int switch_gpu_set_take_host_written(switch_gpu_resource_set *set)
{
	return __atomic_load_n(&set->host_written, __ATOMIC_ACQUIRE) &&
	       __atomic_exchange_n(&set->host_written, 0u, __ATOMIC_ACQ_REL);
}

extern "C" switch_gpu_resource_set *switch_gpu_backing_set(switch_gpu_backing *backing)
{
	return backing ? __atomic_load_n(&backing->set, __ATOMIC_ACQUIRE) : nullptr;
}

extern "C" void switch_gpu_backing_member_cpu(switch_gpu_backing *backing)
{
	if (!backing)
		return;
	IOLockLock(backing->lock);
	backing->cpu_reached = true;
	const bool member = backing->set != nullptr;
	IOLockUnlock(backing->lock);
	if (member)
		switch_gpu_set_reached(backing, SWITCH_GPU_SET_CAUSE_CPU);
}

void switch_gpu_set_reached(switch_gpu_backing *backing, uint32_t cause)
{
	IOLockLock(backing->lock);
	auto *set = backing->set;
	if (set) {
		__atomic_add_fetch(&set->refs, 1u, __ATOMIC_ACQ_REL);
		switch_gpu_set_invalidate(set, cause);
	}
	IOLockUnlock(backing->lock);
	if (!set)
		return;
	wait_inside(set);
	IOLockLock(backing->lock);
	if (backing->set == set) {
		// A member that cannot take its users is left in the set: its generation already ended.
		if (!adopt_locked(backing, set)) {
			backing->set = nullptr;
			backing->set_access = 0;
		}
	}
	IOLockUnlock(backing->lock);
	put(set);
}

extern "C" void switch_gpu_backing_member_freed(switch_gpu_backing *backing)
{
	auto *set = switch_gpu_backing_set(backing);
	if (!set)
		return;
	switch_gpu_set_reached(backing, SWITCH_GPU_SET_CAUSE_FREE);
	// A member that could not take the set's jobs as its users is still the set's: it stays listed and held until a
	// replace or the set's destroy lets it go, or its set pointer would outlive the set.
	IOLockLock(backing->lock);
	const bool kept = backing->set == set;
	IOLockUnlock(backing->lock);
	if (kept)
		return;
	uint32_t at = 0;
	while (at < set->count && set->members[at].backing != backing)
		++at;
	if (at == set->count)
		return;
	memmove(&set->members[at], &set->members[at + 1],
		(set->count - at - 1) * sizeof(set->members[0]));
	--set->count;
	switch_gpu_backing_release(backing);
}
