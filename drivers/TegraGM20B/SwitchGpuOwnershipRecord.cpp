// SPDX-License-Identifier: MIT
// A begun transaction's dependencies, and the records its commit leaves on its uses' backings (SwitchGpuOwnership.cpp
// begins it).
#include "SwitchGpuOwnershipInternal.h"
#include "SwitchGpuPathStats.h"

using namespace switch_gpu_ownership;

namespace
{
bool conflicts(uint32_t old_access, uint32_t access)
{
	return (old_access | access) & SWITCH_GPU_BACKING_WRITE;
}

bool promise_pending_on(const switch_gpu_backing &backing, uint32_t offset, uint32_t value)
{
	for (uint32_t i = 0; i < backing.promise_count; ++i) {
		const auto &ref = backing.promises[i];
		if (ref.offset == offset && ref.value == value &&
		    !switch_gpu_promise_ref_released(&ref) &&
		    !tgm_promise_abandoned(ref.offset, ref.value))
			return true;
	}
	return false;
}

void append_promise_wait(switch_gpu_promise_wait out[SWITCH_GPU_BACKING_PROMISES], uint32_t &count,
			 uint32_t offset, uint32_t value)
{
	for (uint32_t i = 0; i < count; ++i)
		if (out[i].offset == offset && out[i].value == value)
			return;
	if (count < SWITCH_GPU_BACKING_PROMISES)
		out[count++] = {offset, value};
}

// A read ordered on the GPU behind writer promises still pending on this backing reads what the
// promised writes leave: any write before those promises drop waiting for it would deadlock.
bool reads_promised(const switch_gpu_backing &backing, const switch_gpu_backing_access &access)
{
	if ((access.access & SWITCH_GPU_BACKING_WRITE) || !access.promise_count ||
	    !backing.promise_count)
		return false;
	for (uint32_t i = 0; i < access.promise_count; ++i)
		if (promise_pending_on(backing, access.promises[i].offset,
				       access.promises[i].value))
			return true;
	return false;
}

uint32_t collect_read_promises(const switch_gpu_backing_transaction &transaction,
			       const switch_gpu_backing &backing,
			       const switch_gpu_backing_access *previous,
			       switch_gpu_promise_wait out[SWITCH_GPU_BACKING_PROMISES])
{
	if (!backing.promise_count)
		return 0;
	uint32_t count = 0;
	if (transaction.promised) {
		switch_gpu_backing_token found[SWITCH_GPU_BACKING_PROMISES];
		const int pending = switch_gpu_backing_promises_locked(
			&backing, transaction.promise_first, transaction.promise_end, found,
			SWITCH_GPU_BACKING_PROMISES);
		for (int i = 0; i < pending; ++i)
			append_promise_wait(out, count, found[i].queue, uint32_t(found[i].value));
	}
	if (previous && previous->promise_count &&
	    switch_gpu_backing_access_status(*previous) == 0) {
		for (uint32_t i = 0; i < previous->promise_count; ++i)
			if (promise_pending_on(backing, previous->promises[i].offset,
					       previous->promises[i].value))
				append_promise_wait(out, count, previous->promises[i].offset,
						    previous->promises[i].value);
	}
	return count;
}

void unlock_transaction(switch_gpu_backing_transaction *transaction)
{
	if (transaction->ordered)
		switch_gpu_backing_order_unlock();
	if (transaction->claimed) {
		switch_gpu_backing_unclaim_uses(transaction->uses, transaction->count);
		transaction->locked = 0;
	}
	for (uint32_t i = transaction->count; transaction->locked && i; --i) {
		if (!(transaction->uses[i - 1].reserved & kUseLocked))
			continue;
		transaction->uses[i - 1].reserved = 0;
		IOLockUnlock(transaction->uses[i - 1].backing->lock);
		--transaction->locked;
	}
	for (uint32_t i = 0; !transaction->held && i < transaction->count; ++i)
		switch_gpu_backing_release(transaction->uses[i].backing);
	*transaction = {};
}

// Commit's records and host acquires against the references begin took for them: any it did not use go back.
void settle_refs(const switch_gpu_backing_transaction &transaction, uint32_t used)
{
	const auto *timeline = transaction.timeline;
	if (used < transaction.refs) {
		timeline->release(&transaction.completion, transaction.refs - used);
	} else if (used > transaction.refs) {
		const void *handle = nullptr;
		(void)timeline->retain(&transaction.completion, used - transaction.refs, &handle);
	}
}
}  // namespace

extern "C" int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *transaction,
					       switch_gpu_backing_token *out, uint32_t capacity,
					       uint32_t *needed)
{
	if (!transaction || !needed || (capacity && !out) || !holds(transaction))
		return -22;
	*needed = 0;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto &use = transaction->uses[i];
		for (uint32_t j = 0; j < use.backing->access_count; ++j) {
			auto &access = use.backing->accesses[j];
			if (!conflicts(access.access, use.access) ||
			    reads_promised(*use.backing, access))
				continue;
			const bool external =
				(access.access & SWITCH_GPU_BACKING_WRITE) &&
				transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B;
			const int status =
				external ? switch_gpu_backing_token_external_status(&access.token)
					 : switch_gpu_backing_access_status(access);
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
	if (!transaction || !needed || *needed > capacity || !holds(transaction))
		return -22;
	transaction->promised = 1;
	transaction->promise_first = own_first;
	transaction->promise_end = own_end;
	const uint32_t first = *needed;
	uint32_t extra = 0;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto *backing = transaction->uses[i].backing;
		if (!backing->promise_count)
			continue;
		switch_gpu_backing_token found[SWITCH_GPU_BACKING_PROMISES];
		const int pending = switch_gpu_backing_promises_locked(
			backing, own_first, own_end, found, SWITCH_GPU_BACKING_PROMISES);
		if (pending < 0)
			return pending;
		// One promise names every surface its job writes, as many as its maker chooses: its word is waited
		// for once, at the later value. Past the room, the count alone: the caller grows by it, as for
		// switch_gpu_backing_dependencies.
		for (int k = 0; k < pending; ++k) {
			const uint32_t kept = first + extra < capacity ? first + extra : capacity;
			uint32_t j = first;
			while (j < kept && out[j].queue != found[k].queue)
				++j;
			if (j < kept) {
				if (switch_gpu_backing_later(found[k], out[j]))
					out[j] = found[k];
				continue;
			}
			if (first + extra < capacity)
				out[first + extra] = found[k];
			++extra;
		}
	}
	*needed += extra;
	if (*needed > capacity)
		return -28;
	switch_gpu_path_taken(SWITCH_GPU_PATH_PROMISE_WAIT, extra ? 1 : 0);
	return 0;
}

extern "C" int switch_gpu_backing_seal(switch_gpu_backing_transaction *transaction)
{
	if (transaction == nullptr)
		return -22;
	if (transaction->ordered != 0)
		return 0;
	if (transaction->count != 0)
		return -22;
	switch_gpu_backing_order_lock();
	transaction->ordered = 1;
	return 0;
}

namespace
{
bool publish_transaction(switch_gpu_backing_transaction *transaction)
{
	if (!transaction || !holds(transaction) || transaction->prepared != transaction->count ||
	    !uses_intact(transaction, "commit"))
		return false;
	const auto *timeline = transaction->timeline;
	uint32_t used = 0;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		auto *backing = transaction->uses[i].backing;
		const uint32_t access = access_kind(transaction->uses[i].access);
		if (host_acquire_wanted(*backing, transaction->completion) &&
		    (timeline || !switch_gpu_backing_token_retain(&transaction->completion))) {
			used += timeline != nullptr;
			backing->host_acquire = transaction->completion;
			backing->host_acquire_handle = transaction->handle;
			backing->host_acquire_epoch = backing->host_epoch;
		}
		const uint32_t slot = access_slot(backing, transaction->completion, access);
		const bool write = (access & SWITCH_GPU_BACKING_WRITE) != 0;
		const switch_gpu_backing_access *previous =
			slot < backing->access_count ? &backing->accesses[slot] : nullptr;
		switch_gpu_promise_wait read_promises[SWITCH_GPU_BACKING_PROMISES] = {};
		const uint32_t promise_count =
			write ? 0u
			      : collect_read_promises(*transaction, *backing, previous,
						      read_promises);
		if (slot < backing->access_count) {
			if (!timeline)
				switch_gpu_backing_drop_token(backing->accesses[slot].token);
		} else {
			++backing->access_count;
			used += timeline != nullptr;
		}
		backing->accesses[slot] = {transaction->completion,
					   write ? backing->content + 1 : 0,
					   access,
					   promise_count,
					   {},
					   false,
					   transaction->handle};
		for (uint32_t p = 0; p < promise_count; ++p)
			backing->accesses[slot].promises[p] = read_promises[p];
		if (write) {
			switch_gpu_backing_writer_commit_locked(backing, transaction->completion);
			++backing->content;
			if (transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B)
				++backing->host_epoch;
			backing->dirty |= SWITCH_GPU_BACKING_GPU_DIRTY;
		}
	}
	if (timeline)
		settle_refs(*transaction, used);
	return true;
}
}  // namespace

extern "C" void switch_gpu_backing_publish(switch_gpu_backing_transaction *transaction)
{
	if (!publish_transaction(transaction))
		return;
	if (transaction->ordered == 0)
		return;
	for (uint32_t i = transaction->count; transaction->locked != 0 && i != 0; --i) {
		if ((transaction->uses[i - 1].reserved & kUseLocked) == 0)
			continue;
		transaction->uses[i - 1].reserved = 0;
		IOLockUnlock(transaction->uses[i - 1].backing->lock);
		--transaction->locked;
	}
}

extern "C" void switch_gpu_backing_finish(switch_gpu_backing_transaction *transaction)
{
	if (transaction != nullptr)
		unlock_transaction(transaction);
}

extern "C" void switch_gpu_backing_commit(switch_gpu_backing_transaction *transaction)
{
	if (publish_transaction(transaction))
		unlock_transaction(transaction);
}

extern "C" void switch_gpu_backing_abort(switch_gpu_backing_transaction *transaction)
{
	if (!transaction || !uses_intact(transaction, "abort"))
		return;
	while (transaction->prepared) {
		switch_gpu_backing_writer_abort_locked(
			transaction->uses[transaction->prepared - 1].backing);
		if (!transaction->timeline)
			switch_gpu_backing_drop_token(transaction->completion);
		--transaction->prepared;
	}
	if (transaction->refs)
		transaction->timeline->release(&transaction->completion, transaction->refs);
	unlock_transaction(transaction);
}

int switch_gpu_backing_adopt_locked(switch_gpu_backing *backing,
				    const switch_gpu_backing_token *jobs, uint32_t count,
				    uint32_t access)
{
	switch_gpu_backing_assert_held(backing);
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
		backing->accesses[slot] = {job, write ? backing->content + 1 : 0, kind, 0, {}, false,
					   nullptr};
		if (write) {
			++backing->content;
			backing->dirty |= SWITCH_GPU_BACKING_GPU_DIRTY;
		}
	}
	return 0;
}
