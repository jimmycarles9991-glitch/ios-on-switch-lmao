// SPDX-License-Identifier: MIT
#include <string.h>
#include "SwitchGpuBackingInternal.h"
#include "SwitchGpuPathStats.h"
extern "C" void switch_gpu_surface_sweep(void) __attribute__((weak));

bool switch_gpu_backing_same_timeline(const switch_gpu_backing_token &a,
				      const switch_gpu_backing_token &b)
{
	return a.engine == b.engine && a.queue == b.queue &&
	       a.reset_generation == b.reset_generation;
}

bool switch_gpu_backing_later(const switch_gpu_backing_token &a, const switch_gpu_backing_token &b)
{
	const auto *ops = switch_gpu_backing_engine_ops(a.engine);
	return ops && (ops->value_bits == 32 ? int32_t(uint32_t(a.value - b.value)) > 0
					     : int64_t(a.value - b.value) > 0);
}

void switch_gpu_backing_drop_token(const switch_gpu_backing_token &token)
{
	const auto *ops = switch_gpu_backing_engine_ops(token.engine);
	if (ops)
		ops->release_token(&token);
}

void switch_gpu_backing_sort_uses(switch_gpu_backing_use *uses, uint32_t count)
{
	uint32_t sorted = 1;
	while (sorted < count && uses[sorted - 1].backing->id <= uses[sorted].backing->id)
		++sorted;
	if (sorted >= count)
		return;
	const auto swap = [&](uint32_t a, uint32_t b) {
		const auto value = uses[a];
		uses[a] = uses[b];
		uses[b] = value;
	};
	const auto sift = [&](uint32_t root, uint32_t end) {
		while (root < end / 2) {
			uint32_t child = root * 2 + 1;
			if (child + 1 < end &&
			    uses[child].backing->id < uses[child + 1].backing->id)
				++child;
			if (uses[root].backing->id >= uses[child].backing->id)
				break;
			swap(root, child);
			root = child;
		}
	};
	for (uint32_t root = count / 2; root; --root)
		sift(root - 1, count);
	for (uint32_t end = count; end > 1; --end) {
		swap(0, end - 1);
		sift(0, end - 1);
	}
}

namespace
{
uint64_t logged_producer;
}  // namespace

// What a failed write wrote is undefined, and its readers go on with it, as Metal's go on after a failed command buffer:
// the failure is logged, once per producer, and returned to nobody.
void switch_gpu_backing_note_failure_locked(const switch_gpu_backing *backing,
					    switch_gpu_backing_access &access, int status)
{
	if (!access.noted && (access.access & SWITCH_GPU_BACKING_WRITE) &&
	    !(access.access & SWITCH_GPU_BACKING_UNLISTED)) {
		const auto &token = access.token;
		const uint64_t producer = (uint64_t(token.engine) << 56) ^
					  (token.reset_generation << 32) ^ token.queue;
		if (__atomic_exchange_n(&logged_producer, producer, __ATOMIC_RELAXED) != producer)
			IOLog("SwitchGpu: surface %u (allocation %llu) holds undefined contents "
			      "from generation %llu (now %llu): engine %u queue %u reset %llu job "
			      "%llu "
			      "failed (%d); its users go on\n",
			      backing->surface_id, backing->id, access.content, backing->content,
			      token.engine, token.queue, token.reset_generation, token.value,
			      status);
	}
	access.noted = true;
}

void switch_gpu_backing_prune_locked(switch_gpu_backing *backing)
{
	if (backing->host_acquire.engine) {
		const int acquired = switch_gpu_backing_token_status(&backing->host_acquire);
		if (acquired) {
			if (acquired == 1 && backing->host_acquire_epoch == backing->host_epoch)
				backing->host_visible_epoch = backing->host_epoch;
			switch_gpu_backing_drop_token(backing->host_acquire);
			backing->host_acquire = {};
		}
	}
	bool pending = false;
	for (uint32_t i = 0; i < backing->access_count; ++i)
		pending |= switch_gpu_backing_token_status(&backing->accesses[i].token) == 0;
	for (uint32_t i = 0; i < backing->access_count;) {
		auto &access = backing->accesses[i];
		const int status = switch_gpu_backing_token_status(&access.token);
		if (status < 0)
			switch_gpu_backing_note_failure_locked(backing, access, status);
		// Pending consumers may still have a hardware acquire referencing a completed producer's fence allocation.
		const bool current_writer = (access.access & SWITCH_GPU_BACKING_WRITE) &&
					    access.content == backing->content;
		if (status != 0 && !pending && !current_writer) {
			switch_gpu_backing_drop_token(access.token);
			backing->accesses[i] = backing->accesses[--backing->access_count];
		} else {
			++i;
		}
	}
}

namespace
{
constexpr uint32_t kAccessMask = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kBesideCpu = SWITCH_GPU_BACKING_UNLISTED | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
constexpr uint32_t kUseMask = kAccessMask | kBesideCpu;

uint32_t access_kind(uint32_t access)
{
	return (access & SWITCH_GPU_BACKING_WRITE ? SWITCH_GPU_BACKING_WRITE
						  : SWITCH_GPU_BACKING_READ) |
	       (access & SWITCH_GPU_BACKING_UNLISTED);
}

// A backing used both listed and unlisted by one job is ordered by everything it may do, as a listed use.
uint32_t merge(uint32_t a, uint32_t b)
{
	uint32_t merged = (a | b) & ~kBesideCpu;
	if (a & b & SWITCH_GPU_BACKING_UNLISTED)
		merged |= SWITCH_GPU_BACKING_UNLISTED;
	else if ((a & kBesideCpu) && (b & kBesideCpu))
		merged |= SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
	return merged;
}

bool conflicts(uint32_t old_access, uint32_t access)
{
	return (old_access | access) & SWITCH_GPU_BACKING_WRITE;
}

// A read ordered on the GPU behind writer promises still pending reads what the promised writes leave (its content is
// past the backing's): the promised job waiting for it would wait for itself.
bool reads_promised(const switch_gpu_backing &backing, const switch_gpu_backing_access &access)
{
	return !(access.access & SWITCH_GPU_BACKING_WRITE) && access.content > backing.content &&
	       switch_gpu_backing_promise_pending_locked(&backing);
}

// What a committed read reads: the backing's content after the promised writes it orders behind, else 0 (the current).
uint64_t read_content(const switch_gpu_backing_transaction &transaction,
		      const switch_gpu_backing &backing)
{
	if (!transaction.promised || !backing.promise_count)
		return 0;
	const int pending = switch_gpu_backing_promises_locked(&backing, transaction.promise_first,
							       transaction.promise_end, nullptr, 0);
	return pending > 0 ? backing.content + uint64_t(pending) : 0;
}

uint32_t access_slot(switch_gpu_backing *backing, const switch_gpu_backing_token &token,
		     uint32_t access)
{
	for (uint32_t i = 0; i < backing->access_count; ++i)
		if (backing->accesses[i].access == access_kind(access) &&
		    switch_gpu_backing_same_timeline(backing->accesses[i].token, token))
			return i;
	return backing->access_count;
}

int reserve_access(switch_gpu_backing *backing)
{
	if (backing->access_count < backing->access_capacity)
		return 0;
	const uint32_t capacity = backing->access_capacity ? backing->access_capacity * 2 : 4;
	if (capacity < backing->access_capacity)
		return -12;
	auto *accesses = IONew(switch_gpu_backing_access, capacity);
	if (!accesses)
		return -12;
	if (backing->access_count)
		memcpy(accesses, backing->accesses, sizeof(*accesses) * backing->access_count);
	IODelete(backing->accesses, switch_gpu_backing_access, backing->access_capacity);
	backing->accesses = accesses;
	backing->access_capacity = capacity;
	return 0;
}

// A dead client's IOSurface lease can refuse or slow down only a job that uses its backing, and keep waiting only what
// waits for CPU holders to leave. With neither, the job takes no global lock for it.
bool sweep_wanted(const switch_gpu_backing_use *uses, uint32_t count)
{
	if (__atomic_load_n(&switch_gpu_cpu_sleepers, __ATOMIC_ACQUIRE) ||
	    switch_gpu_ready_waiting())
		return true;
	for (uint32_t i = 0; i < count; ++i)
		if (__atomic_load_n(&uses[i].backing->cpu_readers, __ATOMIC_RELAXED) ||
		    __atomic_load_n(&uses[i].backing->cpu_writers, __ATOMIC_RELAXED))
			return true;
	return false;
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
uint64_t uses_digest(const switch_gpu_backing_use *uses, uint32_t count)
{
	uint64_t digest = 0xcbf29ce484222325ull ^ uintptr_t(uses);
	for (uint32_t i = 0; i < count; ++i) {
		digest = (digest ^ uintptr_t(uses[i].backing)) * 0x100000001b3ull;
		digest = (digest ^ uses[i].access) * 0x100000001b3ull;
	}
	return digest | 1;
}
#endif

// Debug kernels: the use array still holds what begin left in it, or the transaction is not touched (its backings stay
// locked) rather than unlocked and recorded through whatever the array now holds.
bool uses_intact(const switch_gpu_backing_transaction *transaction, const char *step)
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	if (!transaction->uses_check ||
	    uses_digest(transaction->uses, transaction->count) == transaction->uses_check)
		return true;
	IOLog("SwitchGpu: engine %u queue %u job %llu: its %u uses changed between begin and %s "
	      "(the caller's array went "
	      "away); nothing is recorded or unlocked\n",
	      transaction->completion.engine, transaction->completion.queue,
	      transaction->completion.value, transaction->count, step);
	return false;
#else
	(void)transaction, (void)step;
	return true;
#endif
}

void unlock_transaction(switch_gpu_backing_transaction *transaction)
{
	while (transaction->locked)
		IOLockUnlock(transaction->uses[--transaction->locked].backing->lock);
	for (uint32_t i = 0; i < transaction->count; ++i)
		switch_gpu_backing_release(transaction->uses[i].backing);
	*transaction = {};
}
}  // namespace

static int begin(switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
		 uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
		 const switch_gpu_backing_token *completion,
		 switch_gpu_backing_transaction *transaction)
{
	if (!transaction)
		return -22;
	*transaction = {};
	if (!completion || (count && !uses) || (lease_count && !leases) || (guard_count && !guards))
		return -22;
	for (uint32_t i = 0; i < lease_count; ++i) {
		if (!leases[i])
			return -22;
		for (uint32_t j = 0; j < i; ++j)
			if (leases[i] == leases[j])
				return -22;
	}
	const auto *ops = switch_gpu_backing_engine_ops(completion->engine);
	if (!ops)
		return -19;
	for (uint32_t i = 0; i < count; ++i) {
		if (!uses[i].backing || !(uses[i].access & kAccessMask) ||
		    (uses[i].access & ~kUseMask) || uses[i].reserved)
			return -22;
	}
	switch_gpu_backing_sort_uses(uses, count);
	uint32_t unique = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (unique && uses[unique - 1].backing == uses[i].backing)
			uses[unique - 1].access = merge(uses[unique - 1].access, uses[i].access);
		else
			uses[unique++] = uses[i];
	}
	if (!lease_count && switch_gpu_surface_sweep && sweep_wanted(uses, unique))
		switch_gpu_surface_sweep();
	for (uint32_t i = 0; i < guard_count; ++i) {
		if (!guards[i].backing || !guards[i].generation ||
		    (uintptr_t(guards[i].generation) & 7) || !guards[i].expected)
			return -22;
		uint32_t first = 0, last = unique;
		while (first < last) {
			const uint32_t middle = first + (last - first) / 2;
			if (uses[middle].backing->id < guards[i].backing->id)
				first = middle + 1;
			else
				last = middle;
		}
		if (first == unique || uses[first].backing != guards[i].backing)
			return -22;
	}
	transaction->uses = uses;
	transaction->count = unique;
	transaction->completion = *completion;
	for (uint32_t i = 0; i < unique; ++i)
		switch_gpu_backing_retain(uses[i].backing);
	int result = 0;
	uint32_t matched_leases = 0;
	for (uint32_t i = 0; i < unique; ++i) {
		IOLockLock(uses[i].backing->lock);
		++transaction->locked;
	}
	for (uint32_t i = 0; !result && i < guard_count; ++i)
		if (__atomic_load_n(guards[i].generation, __ATOMIC_ACQUIRE) != guards[i].expected)
			result = SWITCH_GPU_BACKING_STALE;
	for (uint32_t i = 0; !result && i < unique; ++i) {
		auto *backing = uses[i].backing;
		uint32_t readers = switch_gpu_backing_exclusive_readers(backing),
			 writers = backing->cpu_writers;
		uint32_t owned = 0;
		for (uint32_t j = 0; j < lease_count; ++j) {
			const auto *lease =
				switch_gpu_backing_find_lease_locked(backing, leases[j]);
			if (!lease)
				continue;
			++matched_leases;
			auto &owners = lease->access & SWITCH_GPU_BACKING_WRITE ? writers : readers;
			if ((lease->producer.engine &&
			     (uses[i].access & SWITCH_GPU_BACKING_WRITE)) ||
			    ++owned > 1 || !owners ||
			    !(lease->access & SWITCH_GPU_BACKING_CPU_NO_CACHE) || lease->offset ||
			    lease->bytes != (uint64_t(backing->pages->count) << 12))
				result = -22;
			else
				--owners;
		}
		if (!result && !(uses[i].access & kBesideCpu) &&
		    (writers || (readers && (uses[i].access & SWITCH_GPU_BACKING_WRITE))))
			result = -16;
		if (!result)
			switch_gpu_backing_prune_locked(backing);
		if (completion->engine == SWITCH_GPU_ENGINE_GM20B &&
		    backing->host_visible_epoch != backing->host_epoch)
			transaction->host_written = 1;
		for (uint32_t j = 0; !result && j < backing->access_count; ++j)
			if (switch_gpu_backing_same_timeline(backing->accesses[j].token,
							     *completion) &&
			    !switch_gpu_backing_later(*completion, backing->accesses[j].token))
				result = -22;
		if (!result &&
		    access_slot(backing, *completion, uses[i].access) == backing->access_count)
			result = reserve_access(backing);
		if (!result)
			result = ops->retain_token(completion);
		if (result)
			break;
		++transaction->prepared;
		if (uses[i].access & SWITCH_GPU_BACKING_WRITE)
			result = switch_gpu_backing_writer_prepare_locked(backing, *completion);
		if (result)
			break;
		if (backing->dirty & SWITCH_GPU_BACKING_CPU_DIRTY) {
			transaction->host_written = 1;
			switch_gpu_backing_cache(
				backing->pages->addresses, backing->pages->count,
				backing->clean_start,
				(uint64_t(backing->pages->count) << 12) - backing->clean_start, 0);
			backing->clean_start = 0;
			// A CPU writer still holding the backing (a submission proceeds beside it) may write more before the
			// next job reads it: that job cleans again.
			if (!backing->cpu_writers)
				backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
			backing->initial_clean = true;
		}
	}
	if (!result && matched_leases != lease_count)
		result = -22;
	if (result)
		switch_gpu_backing_abort(transaction);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	else
		transaction->uses_check = uses_digest(transaction->uses, transaction->count);
#endif
	return result;
}

extern "C" int switch_gpu_backing_begin(switch_gpu_backing_use *uses, uint32_t count,
					const switch_gpu_backing_token *completion,
					switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, nullptr, 0, nullptr, 0, completion, transaction);
}

extern "C" int switch_gpu_backing_begin_conversion(switch_gpu_backing_use *uses, uint32_t count,
						   switch_gpu_cpu_lease *const *leases,
						   uint32_t lease_count,
						   const switch_gpu_backing_token *completion,
						   switch_gpu_backing_transaction *transaction)
{
	if (!lease_count)
		return -22;
	return begin(uses, count, leases, lease_count, nullptr, 0, completion, transaction);
}

extern "C" int switch_gpu_backing_begin_checked(switch_gpu_backing_use *uses, uint32_t count,
						const switch_gpu_backing_guard *guards,
						uint32_t guard_count,
						const switch_gpu_backing_token *completion,
						switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, nullptr, 0, guards, guard_count, completion, transaction);
}

extern "C" int switch_gpu_backing_begin_conversion_checked(
	switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
	uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
	const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, leases, lease_count, guards, guard_count, completion,
		     transaction);
}

extern "C" int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *transaction,
					       switch_gpu_backing_token *out, uint32_t capacity,
					       uint32_t *needed)
{
	if (!transaction || !needed || (capacity && !out) ||
	    transaction->locked != transaction->count)
		return -22;
	*needed = 0;
	// The kernel's other engines follow what a GM20B job did not list; GM20B jobs follow only listed work, as
	// TegraGM20BFences.h orders them.
	const bool follows_unlisted = transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto &use = transaction->uses[i];
		if (use.access & SWITCH_GPU_BACKING_UNLISTED)
			continue;
		for (uint32_t j = 0; j < use.backing->access_count; ++j) {
			auto &access = use.backing->accesses[j];
			if (((access.access & SWITCH_GPU_BACKING_UNLISTED) && !follows_unlisted) ||
			    !conflicts(access.access, use.access) ||
			    reads_promised(*use.backing, access))
				continue;
			const bool external =
				(access.access & SWITCH_GPU_BACKING_WRITE) &&
				transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B;
			const int status =
				external ? switch_gpu_backing_token_external_status(&access.token)
					 : switch_gpu_backing_token_status(&access.token);
			if (status < 0)
				switch_gpu_backing_note_failure_locked(use.backing, access, status);
			if (status != 0 ||
			    switch_gpu_backing_same_timeline(access.token, transaction->completion))
				continue;
			uint32_t existing = 0;
			while (existing < *needed && existing < capacity &&
			       !switch_gpu_backing_same_timeline(out[existing], access.token))
				++existing;
			if (existing < *needed && existing < capacity) {
				if (switch_gpu_backing_later(access.token, out[existing]))
					out[existing] = access.token;
				continue;
			}
			// An exhausted output still reports an upper bound; retrying with needed slots cannot truncate.
			if (*needed < capacity)
				out[*needed] = access.token;
			++*needed;
		}
	}
	return *needed > capacity ? -28 : 0;
}

extern "C" int switch_gpu_backing_promise_dependencies(switch_gpu_backing_transaction *transaction,
						       switch_gpu_backing_token *out,
						       uint32_t capacity, uint32_t *needed,
						       uint32_t own_first, uint32_t own_end)
{
	if (!transaction || !needed || *needed > capacity ||
	    transaction->locked != transaction->count)
		return -22;
	transaction->promised = 1;
	transaction->promise_first = own_first;
	transaction->promise_end = own_end;
	uint32_t extra = 0;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto *backing = transaction->uses[i].backing;
		if (!backing->promise_count)
			continue;
		const uint32_t at = *needed + extra;
		const int pending = switch_gpu_backing_promises_locked(
			backing, own_first, own_end, at < capacity ? out + at : nullptr,
			at < capacity ? capacity - at : 0);
		// past the room, the count alone: the caller grows by it, as for switch_gpu_backing_dependencies
		extra += pending >= 0 ? uint32_t(pending)
				      : uint32_t(switch_gpu_backing_promises_locked(
						backing, own_first, own_end, nullptr, 0));
	}
	*needed += extra;
	if (*needed > capacity)
		return -28;
	switch_gpu_path_taken(SWITCH_GPU_PATH_PROMISE_WAIT, extra ? 1 : 0);
	return 0;
}

extern "C" void switch_gpu_backing_commit(switch_gpu_backing_transaction *transaction)
{
	if (!transaction || transaction->locked != transaction->count ||
	    transaction->prepared != transaction->count || !uses_intact(transaction, "commit"))
		return;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		auto *backing = transaction->uses[i].backing;
		const uint32_t access = access_kind(transaction->uses[i].access);
		if (transaction->completion.engine == SWITCH_GPU_ENGINE_GM20B &&
		    backing->host_visible_epoch != backing->host_epoch &&
		    !backing->host_acquire.engine) {
			if (!switch_gpu_backing_token_retain(&transaction->completion)) {
				backing->host_acquire = transaction->completion;
				backing->host_acquire_epoch = backing->host_epoch;
			}
		}
		const uint32_t slot = access_slot(backing, transaction->completion, access);
		uint64_t content = access & SWITCH_GPU_BACKING_WRITE
					   ? backing->content + 1
					   : read_content(*transaction, *backing);
		if (slot < backing->access_count) {
			// a later read on the same timeline runs after the earlier one, so after what that one waits for
			if (!(access & SWITCH_GPU_BACKING_WRITE) &&
			    backing->accesses[slot].content > content)
				content = backing->accesses[slot].content;
			switch_gpu_backing_drop_token(backing->accesses[slot].token);
		} else {
			++backing->access_count;
		}
		backing->accesses[slot] = {transaction->completion, content, access, false};
		if (access & SWITCH_GPU_BACKING_WRITE) {
			switch_gpu_backing_writer_commit_locked(backing, transaction->completion);
			++backing->content;
			if (transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B)
				++backing->host_epoch;
			backing->dirty |= SWITCH_GPU_BACKING_GPU_DIRTY;
		}
	}
	unlock_transaction(transaction);
}

extern "C" void switch_gpu_backing_abort(switch_gpu_backing_transaction *transaction)
{
	if (!transaction || !uses_intact(transaction, "abort"))
		return;
	while (transaction->prepared) {
		switch_gpu_backing_writer_abort_locked(
			transaction->uses[transaction->prepared - 1].backing);
		switch_gpu_backing_drop_token(transaction->completion);
		--transaction->prepared;
	}
	unlock_transaction(transaction);
}

int switch_gpu_backing_adopt_locked(switch_gpu_backing *backing,
				    const switch_gpu_backing_token *jobs, uint32_t count,
				    uint32_t access)
{
	const uint32_t kind = access_kind(access);
	for (uint32_t i = 0; i < count; ++i) {
		const auto &job = jobs[i];
		const auto *ops = switch_gpu_backing_engine_ops(job.engine);
		// A finished writer whose bytes may still be in the GPU L2 stays a user, as commit keeps it.
		const int settled = kind & SWITCH_GPU_BACKING_WRITE
					    ? switch_gpu_backing_token_external_status(&job)
					    : switch_gpu_backing_token_status(&job);
		if (!ops || settled != 0)
			continue;
		const uint32_t slot = access_slot(backing, job, kind);
		if (slot < backing->access_count &&
		    !switch_gpu_backing_later(job, backing->accesses[slot].token))
			continue;
		if (slot == backing->access_count)
			if (const int result = reserve_access(backing))
				return result;
		if (const int result = ops->retain_token(&job))
			return result;
		if (slot < backing->access_count)
			switch_gpu_backing_drop_token(backing->accesses[slot].token);
		else
			++backing->access_count;
		const bool write = kind & SWITCH_GPU_BACKING_WRITE;
		backing->accesses[slot] = {job, write ? backing->content + 1 : 0, kind, false};
		if (write) {
			++backing->content;
			backing->dirty |= SWITCH_GPU_BACKING_GPU_DIRTY;
		}
	}
	return 0;
}
