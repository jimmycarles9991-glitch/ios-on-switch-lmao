// SPDX-License-Identifier: MIT
#include "SwitchGpuOwnershipInternal.h"
#include "SwitchGpuPathStats.h"
extern "C" void switch_gpu_surface_sweep(void) __attribute__((weak));

using namespace switch_gpu_ownership;

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
	switch_gpu_backing_assert_held(backing);
	if (backing->host_acquire.engine) {
		const int acquired = switch_gpu_backing_held_status(backing->host_acquire,
								    backing->host_acquire_handle);
		if (acquired) {
			if (acquired == 1 && backing->host_acquire_epoch == backing->host_epoch)
				backing->host_visible_epoch = backing->host_epoch;
			switch_gpu_backing_drop_token(backing->host_acquire);
			backing->host_acquire = {};
			backing->host_acquire_handle = nullptr;
		}
	}
	bool pending = false;
	for (uint32_t i = 0; i < backing->access_count; ++i)
		pending |= switch_gpu_backing_access_status(backing->accesses[i]) == 0;
	for (uint32_t i = 0; i < backing->access_count;) {
		auto &access = backing->accesses[i];
		const int status = switch_gpu_backing_access_status(access);
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

// An engine with timeline references takes the job's at once, after every use is checked: one for each record commit
// adds (one it replaces is on the same timeline and passes its reference on) and each host acquire. A job that only
// replaces records takes none and finds the timeline's handle in a record it replaces: its timeline's lock is taken when
// it first records on a backing, never on every job.
int retain_completion(switch_gpu_backing_transaction *transaction, uint32_t refs,
		      const void *replaced)
{
	if (!refs && replaced) {
		transaction->handle = replaced;
		return 0;
	}
	const int result =
		transaction->timeline->retain(&transaction->completion, refs, &transaction->handle);
	if (!result)
		transaction->refs = refs;
	return result;
}

int begin(switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
	  uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
	  const switch_gpu_backing_token *completion, bool held, bool ordered,
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
	transaction->held = held;
	transaction->timeline = switch_gpu_backing_timeline_ops(completion->engine);
	for (uint32_t i = 0; !held && i < unique; ++i)
		switch_gpu_backing_retain(uses[i].backing);
	int result = 0;
	uint32_t matched_leases = 0, refs = 0;
	const void *replaced = nullptr;
	if (unique && ordered)
		hold_ordered(transaction);
	else if (unique)
		hold_claimed(transaction);
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
		const uint32_t slot = access_slot(backing, *completion, uses[i].access);
		const bool fresh = slot == backing->access_count;
		if (!fresh && !replaced)
			replaced = backing->accesses[slot].handle;
		if (!result && fresh)
			result = reserve_access(backing);
		if (!result && !transaction->timeline)
			result = ops->retain_token(completion);
		if (result)
			break;
		refs += fresh + host_acquire_wanted(*backing, *completion);
		++transaction->prepared;
		if (uses[i].access & SWITCH_GPU_BACKING_WRITE)
			result = switch_gpu_backing_writer_prepare_locked(backing, *completion);
		if (result)
			break;
		if (backing->dirty & SWITCH_GPU_BACKING_CPU_DIRTY) {
			transaction->host_written = 1;
			const uint64_t size = uint64_t(backing->pages->count) << 12;
			if (!(uses[i].reserved & kUseCleaned) && backing->clean_start < size)
				switch_gpu_backing_cache(
					backing->pages->addresses, backing->pages->count,
					backing->clean_start, size - backing->clean_start, 0);
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
	if (!result && transaction->timeline)
		result = retain_completion(transaction, refs, replaced);
	if (result)
		switch_gpu_backing_abort(transaction);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
	else
		transaction->uses_check = uses_digest(transaction->uses, transaction->count);
#endif
	return result;
}
}  // namespace

extern "C" int switch_gpu_backing_begin(switch_gpu_backing_use *uses, uint32_t count,
					const switch_gpu_backing_token *completion,
					switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, nullptr, 0, nullptr, 0, completion, false, false, transaction);
}

extern "C" int switch_gpu_backing_begin_conversion(switch_gpu_backing_use *uses, uint32_t count,
						   switch_gpu_cpu_lease *const *leases,
						   uint32_t lease_count,
						   const switch_gpu_backing_token *completion,
						   switch_gpu_backing_transaction *transaction)
{
	if (!lease_count)
		return -22;
	return begin(uses, count, leases, lease_count, nullptr, 0, completion, false, false,
		     transaction);
}

extern "C" int switch_gpu_backing_begin_checked(switch_gpu_backing_use *uses, uint32_t count,
						const switch_gpu_backing_guard *guards,
						uint32_t guard_count,
						const switch_gpu_backing_token *completion,
						switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, nullptr, 0, guards, guard_count, completion, false, false,
		     transaction);
}

extern "C" int switch_gpu_backing_begin_conversion_checked(
	switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
	uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
	const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, leases, lease_count, guards, guard_count, completion, false,
		     false, transaction);
}

extern "C" int switch_gpu_backing_begin_held(
	switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
	uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
	const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, leases, lease_count, guards, guard_count, completion, true, true,
		     transaction);
}

extern "C" int switch_gpu_backing_begin_ordered(
	switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
	uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
	const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, leases, lease_count, guards, guard_count, completion, false, true,
		     transaction);
}
