// SPDX-License-Identifier: MIT
#include "SwitchGpuEventInternal.h"
#include <kern/clock.h>
#include <kern/thread.h>

namespace
{
bool sameUse(const switch_gpu_event_use *a, const switch_gpu_event_use *b)
{
	if (a->authority || b->authority)
		return a->authority == b->authority;
	if (a->batchAuthority || b->batchAuthority)
		return a->batchAuthority == b->batchAuthority;
	return a->token.engine == b->token.engine && a->token.queue == b->token.queue &&
	       a->token.reset_generation == b->token.reset_generation &&
	       a->token.value == b->token.value;
}
}

extern "C" int switch_gpu_event_waits_prepare(switch_gpu_event_use *const *uses, uint32_t count,
					      switch_gpu_event_waits **out)
{
	if (!out || count > SWITCH_GPU_EVENT_MAX_SOURCES || (count && !uses))
		return -22;
	*out = nullptr;
	if (!switch_gpu_events_state())
		return -12;
	if (!switch_gpu_events_reserve())
		return -16;
	auto *batch = IONewZero(switch_gpu_event_waits, 1);
	if (!batch) {
		switch_gpu_events_unreserve();
		return -12;
	}
	batch->refs = 1;
	batch->kind = SwitchEventWaits;
	batch->capacity = count;
	batch->uses = count > SWITCH_GPU_EVENT_MAX_JOB_USES
			      ? IONewZero(switch_gpu_event_use *, count)
			      : batch->inlineUses;
	if (!batch->uses) {
		switch_gpu_event_waits_abort(&batch);
		return -12;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (!uses[i]) {
			switch_gpu_event_waits_abort(&batch);
			return -22;
		}
		__atomic_add_fetch(&uses[i]->refs, 1u, __ATOMIC_RELAXED);
		batch->uses[batch->count++] = uses[i];
	}
	*out = batch;
	return 0;
}

extern "C" int switch_gpu_event_waits_bind(switch_gpu_event_waits *batch,
					   const switch_gpu_backing_token *consumer)
{
	if (!batch || !consumer || !consumer->engine)
		return -22;
	if (batch->bound)
		return batch->consumer.engine == consumer->engine &&
				       batch->consumer.queue == consumer->queue &&
				       batch->consumer.reset_generation ==
					       consumer->reset_generation &&
				       batch->consumer.value == consumer->value
			       ? 0
			       : -22;
	const int result = switch_gpu_backing_token_retain(consumer);
	if (result)
		return result;
	batch->consumer = *consumer;
	batch->bound = true;
	return 0;
}

extern "C" void switch_gpu_event_waits_commit(switch_gpu_event_waits **owned)
{
	if (!owned || !*owned)
		return;
	auto *batch = *owned;
	if (!batch->bound) {
		switch_gpu_event_waits_abort(owned);
		return;
	}
	*owned = nullptr;
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	if (batch->result || batch->committed) {
		IOLockUnlock(s->lock);
		switch_gpu_event_waits_drop(batch);
		return;
	}
	batch->committed = true;
	batch->next = nullptr;
	if (s->waitsTail)
		s->waitsTail->next = batch;
	else
		s->waits = batch;
	s->waitsTail = batch;
	__atomic_add_fetch(&s->pendingSignals, 1u, __ATOMIC_RELEASE);
	IOLockUnlock(s->lock);
	switch_gpu_events_schedule();
}

extern "C" void switch_gpu_event_waits_abort(switch_gpu_event_waits **owned)
{
	if (!owned || !*owned)
		return;
	auto *batch = *owned;
	*owned = nullptr;
	auto *s = switch_gpu_events_state();
	void (*completed)(void *, int) = nullptr;
	IOLockLock(s->lock);
	if (!batch->committed && !batch->result) {
		__atomic_store_n(&batch->result, -125, __ATOMIC_RELEASE);
		IOLockWakeup(s->lock, batch, false);
		completed = batch->completed;
		batch->completed = nullptr;
	}
	IOLockUnlock(s->lock);
	if (completed)
		completed(batch->context, -125);
	switch_gpu_event_waits_drop(batch);
}

void switch_gpu_event_waits_drop(switch_gpu_event_waits *batch)
{
	if (__atomic_sub_fetch(&batch->refs, 1u, __ATOMIC_ACQ_REL))
		return;
	switch_gpu_event_dispose(batch);
}

int switch_gpu_event_waits_query_locked(switch_gpu_event_waits *batch)
{
	const int consumer = switch_gpu_backing_token_physical_status(&batch->consumer, 0);
	return consumer <= 0 ? consumer : switch_gpu_event_waits_producers_locked(batch);
}

int switch_gpu_event_waits_producers_locked(const switch_gpu_event_waits *batch)
{
	bool pending = batch->previous && !batch->previous->result;
	int failure = batch->previous && batch->previous->result < 0 ? batch->previous->result : 0;
	for (uint32_t i = 0; i < batch->count; ++i) {
		const auto *use = batch->uses[i];
		const int result = use->authority ? use->authority->result
				   : use->batchAuthority
					   ? use->batchAuthority->result
					   : switch_gpu_backing_token_status(&use->token);
		pending |= result == 0;
		if (result < 0)
			failure = result;
	}
	return failure ? failure : pending ? 0 : 1;
}

extern "C" int switch_gpu_event_waits_completion(switch_gpu_event_waits *batch,
						 void (*completed)(void *, int), void *context)
{
	if (!batch || !completed || batch->completed)
		return -22;
	batch->completed = completed;
	batch->context = context;
	return 0;
}

extern "C" int switch_gpu_event_waits_inherit(switch_gpu_event_waits *batch,
					      switch_gpu_event_waits *previous)
{
	if (!batch || !previous || batch == previous ||
	    (batch->previous && batch->previous != previous))
		return -22;
	if (!batch->previous) {
		switch_gpu_event_waits_retain(previous);
		batch->previous = previous;
	}
	return 0;
}

extern "C" void switch_gpu_event_waits_retain(switch_gpu_event_waits *batch)
{
	if (batch)
		__atomic_add_fetch(&batch->refs, 1u, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_event_waits_release(switch_gpu_event_waits **owned)
{
	if (!owned || !*owned)
		return;
	auto *batch = *owned;
	*owned = nullptr;
	switch_gpu_event_waits_drop(batch);
}

extern "C" int switch_gpu_event_waits_status(switch_gpu_event_waits *batch)
{
	if (!batch)
		return -22;
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	const int result = batch->result;
	IOLockUnlock(s->lock);
	return result;
}

extern "C" int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t deadline_ns)
{
	if (!batch)
		return -22;
	uint64_t deadline = 0;
	if (deadline_ns)
		nanoseconds_to_absolutetime(deadline_ns, &deadline);
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	while (!batch->result) {
		if (deadline && mach_absolute_time() >= deadline) {
			IOLockUnlock(s->lock);
			return -110;
		}
		if (deadline)
			IOLockSleepDeadline(s->lock, batch, deadline, THREAD_UNINT);
		else
			IOLockSleep(s->lock, batch, THREAD_UNINT);
	}
	const int result = batch->result;
	IOLockUnlock(s->lock);
	return result;
}

extern "C" int switch_gpu_event_waits_prepare_dependencies(const switch_gpu_backing_token *tokens,
							   uint32_t count,
							   switch_gpu_event_waits **out)
{
	if (!out || count > SWITCH_GPU_EVENT_MAX_SOURCES || (count && !tokens))
		return -22;
	*out = nullptr;
	if (!count || !switch_gpu_backing_has_semantics())
		return 0;
	auto **uses = IONewZero(switch_gpu_event_use *, count);
	if (!uses)
		return -12;
	uint32_t used = 0;
	int error = 0;
	for (uint32_t i = 0; i < count && !error; ++i) {
		switch_gpu_event_waits *authority = nullptr;
		const int result = switch_gpu_backing_semantic_snapshot(&tokens[i], &authority);
		if (result < 0) {
			error = result;
			break;
		}
		if (!result)
			continue;
		bool duplicate = false;
		for (uint32_t j = 0; j < used; ++j)
			duplicate |= uses[j]->batchAuthority == authority;
		if (duplicate) {
			switch_gpu_event_waits_release(&authority);
			continue;
		}
		if (!switch_gpu_events_reserve()) {
			switch_gpu_event_waits_release(&authority);
			error = -16;
			break;
		}
		auto *use = IONewZero(switch_gpu_event_use, 1);
		if (!use) {
			switch_gpu_events_unreserve();
			switch_gpu_event_waits_release(&authority);
			error = -12;
			break;
		}
		error = switch_gpu_backing_token_retain(&tokens[i]);
		if (error) {
			IODelete(use, switch_gpu_event_use, 1);
			switch_gpu_events_unreserve();
			switch_gpu_event_waits_release(&authority);
			break;
		}
		use->refs = 1;
		use->kind = SwitchEventUse;
		use->token = tokens[i];
		use->batchAuthority = authority;
		uses[used++] = use;
	}
	if (!error && used)
		error = switch_gpu_event_waits_prepare(uses, used, out);
	for (uint32_t i = 0; i < used; ++i)
		switch_gpu_event_use_release(&uses[i]);
	IODelete(uses, switch_gpu_event_use *, count);
	return error;
}

extern "C" int switch_gpu_event_waits_merge_uncommitted(switch_gpu_event_waits *destination,
							switch_gpu_event_waits *source)
{
	if (!destination || !source || destination == source)
		return -22;
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	const bool invalid = destination->committed || source->committed || destination->result ||
			     source->result || source->previous;
	uint32_t capacity = destination->count;
	for (uint32_t i = 0; !invalid && i < source->count; ++i) {
		bool duplicate = false;
		for (uint32_t j = 0; j < destination->count; ++j)
			duplicate |= sameUse(destination->uses[j], source->uses[i]);
		for (uint32_t j = 0; j < i; ++j)
			duplicate |= sameUse(source->uses[j], source->uses[i]);
		capacity += !duplicate;
	}
	IOLockUnlock(s->lock);
	if (invalid || capacity > SWITCH_GPU_EVENT_MAX_SOURCES)
		return invalid ? -22 : -28;
	if (capacity == destination->count)
		return 0;
	auto **replacement = IONewZero(switch_gpu_event_use *, capacity);
	if (!replacement)
		return -12;
	IOLockLock(s->lock);
	if (destination->committed || source->committed || destination->result || source->result) {
		IOLockUnlock(s->lock);
		IODelete(replacement, switch_gpu_event_use *, capacity);
		return -22;
	}
	uint32_t count = destination->count;
	for (uint32_t i = 0; i < count; ++i)
		replacement[i] = destination->uses[i];
	for (uint32_t i = 0; i < source->count; ++i) {
		auto *use = source->uses[i];
		bool duplicate = false;
		for (uint32_t j = 0; j < count; ++j)
			duplicate |= sameUse(replacement[j], use);
		if (duplicate)
			continue;
		__atomic_add_fetch(&use->refs, 1u, __ATOMIC_RELAXED);
		replacement[count++] = use;
	}
	auto **old = destination->uses;
	const uint32_t oldCapacity = destination->capacity;
	destination->uses = replacement;
	destination->count = count;
	destination->capacity = capacity;
	IOLockUnlock(s->lock);
	if (old != destination->inlineUses)
		IODelete(old, switch_gpu_event_use *, oldCapacity);
	return 0;
}

extern "C" int switch_gpu_event_waits_covers(switch_gpu_event_waits *batch,
					     const switch_gpu_backing_token *token)
{
	if (!token)
		return -22;
	switch_gpu_event_waits *authority = nullptr;
	const int captured = switch_gpu_backing_semantic_snapshot(token, &authority);
	if (captured <= 0)
		return captured ? captured : 1;
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	bool covered = authority->result == 1;
	const int failure = authority->result < 0 ? authority->result : 0;
	if (batch) {
		covered |= batch->previous == authority;
		for (uint32_t i = 0; i < batch->count && !covered; ++i) {
			const auto *use = batch->uses[i];
			covered = use->batchAuthority == authority ||
				  (use->authority && use->authority->dependencies == authority);
		}
	}
	IOLockUnlock(s->lock);
	switch_gpu_event_waits_release(&authority);
	return failure ? failure : covered ? 1 : 0;
}

extern "C" int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *batch)
{
	return batch ? __atomic_load_n(&batch->result, __ATOMIC_ACQUIRE) : -22;
}

extern "C" int switch_gpu_event_waits_settled_status(const switch_gpu_event_waits *batch)
{
	if (!batch)
		return 0;
	const int result = __atomic_load_n(&batch->result, __ATOMIC_ACQUIRE);
	if (result)
		return result > 0 ? 1 : 0;
	const switch_gpu_event_waits *previous =
		__atomic_load_n(&batch->previous, __ATOMIC_ACQUIRE);
	return !__atomic_load_n(&batch->count, __ATOMIC_ACQUIRE) &&
			       (!previous ||
				__atomic_load_n(&previous->result, __ATOMIC_ACQUIRE) > 0)
		       ? 1
		       : 0;
}

extern "C" int switch_gpu_event_waits_prepare_tokens(const switch_gpu_backing_token *tokens,
						     uint32_t count, switch_gpu_event_waits **out)
{
	if (!out || count > SWITCH_GPU_EVENT_MAX_SOURCES || (count && !tokens))
		return -22;
	*out = nullptr;
	if (!count)
		return 0;
	auto **uses = IONewZero(switch_gpu_event_use *, count);
	if (!uses)
		return -12;
	uint32_t used = 0;
	int error = 0;
	for (uint32_t i = 0; i < count && !error; ++i) {
		const int status = switch_gpu_backing_token_status(&tokens[i]);
		if (status == 1)
			continue;
		if (status < 0) {
			error = status;
			break;
		}
		if (!switch_gpu_events_reserve()) {
			error = -16;
			break;
		}
		auto *use = IONewZero(switch_gpu_event_use, 1);
		if (!use) {
			switch_gpu_events_unreserve();
			error = -12;
			break;
		}
		error = switch_gpu_backing_token_retain(&tokens[i]);
		if (error) {
			IODelete(use, switch_gpu_event_use, 1);
			switch_gpu_events_unreserve();
			break;
		}
		use->refs = 1;
		use->kind = SwitchEventUse;
		use->token = tokens[i];
		uses[used++] = use;
	}
	if (!error && used)
		error = switch_gpu_event_waits_prepare(uses, used, out);
	for (uint32_t i = 0; i < used; ++i)
		switch_gpu_event_use_release(&uses[i]);
	IODelete(uses, switch_gpu_event_use *, count);
	return error;
}

extern "C" int switch_gpu_event_waits_covers_token(switch_gpu_event_waits *batch,
						   const switch_gpu_backing_token *token)
{
	if (!token || !token->engine)
		return -22;
	const int status = switch_gpu_backing_token_status(token);
	if (status)
		return status;
	if (!batch)
		return 0;
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	bool covered = false;
	for (uint32_t i = 0; i < batch->count && !covered; ++i) {
		const auto &held = batch->uses[i]->token;
		covered = held.engine == token->engine && held.queue == token->queue &&
			  held.reset_generation == token->reset_generation &&
			  held.value == token->value;
	}
	IOLockUnlock(s->lock);
	return covered ? 1 : 0;
}
