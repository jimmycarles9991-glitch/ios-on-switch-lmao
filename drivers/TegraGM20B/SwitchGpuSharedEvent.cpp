// SPDX-License-Identifier: MIT
#include "SwitchGpuEventInternal.h"
#include "SwitchGpuEventStats.h"
#include "SwitchGpuEventWord.h"

namespace
{
SwitchGpuEvents *state;
constexpr uint32_t kReservations = 256;
uint32_t records;

void retire(thread_call_param_t, thread_call_param_t)
{
	auto *s = state;
	switch_gpu_event_signal *ready = nullptr;
	switch_gpu_event_waits *consumed = nullptr;
	bool progress = false;
	IOLockLock(s->lock);
	for (auto *event = s->events; event; event = event->next) {
		auto **at = &event->signals;
		while (*at) {
			auto *signal = *at;
			int result = signal->word ? switch_gpu_event_word_result_locked(signal) : 0;
			const bool released = result != 0;
			if (!released) {
				result = switch_gpu_backing_token_external_status(&signal->token);
				if (result > 0 && signal->dependencies)
					result = signal->dependencies->result;
			}
			signal->result = result;
			if (!result) {
				at = &signal->next;
				continue;
			}
			if (signal->word)
				switch_gpu_event_word_settle_locked(signal, result, released);
			if (result > 0) {
				if (signal->value > event->readyThrough)
					event->readyThrough = signal->value;
			} else if (signal->value >= event->failedThrough) {
				event->failedThrough = signal->value;
				event->failure = result;
			}
			*at = signal->next;
			signal->next = ready;
			ready = signal;
			progress = true;
			--event->reservations;
			__atomic_sub_fetch(&s->pendingSignals, 1u, __ATOMIC_RELEASE);
		}
	}
	auto **wait = &s->waits;
	switch_gpu_event_waits *previous = nullptr;
	while (*wait) {
		auto *batch = *wait;
		const int result = switch_gpu_event_waits_query_locked(batch);
		if (!result) {
			previous = batch;
			wait = &batch->next;
			continue;
		}
		__atomic_store_n(&batch->result, result, __ATOMIC_RELEASE);
		IOLockWakeup(s->lock, batch, false);
		progress = true;
		*wait = batch->next;
		if (s->waitsTail == batch)
			s->waitsTail = previous;
		batch->next = consumed;
		consumed = batch;
		__atomic_sub_fetch(&s->pendingSignals, 1u, __ATOMIC_RELEASE);
	}
	IOLockUnlock(s->lock);
	while (consumed) {
		auto *batch = consumed;
		consumed = batch->next;
		for (uint32_t i = 0; i < batch->count; ++i)
			switch_gpu_event_use_release(&batch->uses[i]);
		batch->count = 0;
		if (batch->previous) {
			switch_gpu_event_waits_drop(batch->previous);
			batch->previous = nullptr;
		}
		if (batch->completed)
			batch->completed(batch->context, batch->result > 0 ? 0 : batch->result);
		switch_gpu_event_waits_drop(batch);
	}
	while (ready) {
		auto *signal = ready;
		ready = signal->next;
		if (signal->result > 0)
			switch_gpu_event_native_signal(signal->event->native, signal->value, true);
		switch_gpu_event_release(&signal->event);
		switch_gpu_event_signal_drop(signal);
	}
	switch_gpu_event_watches_retire();
	switch_gpu_events_dispose();
	if (progress && __atomic_load_n(&s->pendingSignals, __ATOMIC_ACQUIRE))
		switch_gpu_events_schedule();
}
}

bool switch_gpu_events_reserve()
{
	uint32_t count = __atomic_load_n(&records, __ATOMIC_RELAXED);
	while (count < 4096)
		if (__atomic_compare_exchange_n(&records, &count, count + 1, false,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return true;
	return false;
}

void switch_gpu_events_unreserve()
{
	__atomic_sub_fetch(&records, 1u, __ATOMIC_RELAXED);
}

SwitchGpuEvents *switch_gpu_events_state()
{
	if (auto *s = __atomic_load_n(&state, __ATOMIC_ACQUIRE))
		return s;
	auto *s = IONewZero(SwitchGpuEvents, 1);
	if (!s)
		return nullptr;
	s->lock = IOLockAlloc();
	s->listener = switch_gpu_event_native_listener_create(0);
	s->work = thread_call_allocate_with_options(retire, nullptr, THREAD_CALL_PRIORITY_KERNEL,
						    THREAD_CALL_OPTIONS_ONCE);
	if (s->lock && s->listener && s->work) {
		SwitchGpuEvents *empty = nullptr;
		if (__atomic_compare_exchange_n(&state, &empty, s, false, __ATOMIC_RELEASE,
						__ATOMIC_ACQUIRE))
			return s;
	}
	if (s->work)
		thread_call_free(s->work);
	if (s->listener)
		s->listener->release();
	if (s->lock)
		IOLockFree(s->lock);
	IODelete(s, SwitchGpuEvents, 1);
	return __atomic_load_n(&state, __ATOMIC_ACQUIRE);
}

void switch_gpu_events_schedule()
{
	if (auto *s = __atomic_load_n(&state, __ATOMIC_ACQUIRE))
		thread_call_enter(s->work);
}

extern "C" void switch_gpu_events_completed()
{
	auto *s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
	if (s && __atomic_load_n(&s->pendingSignals, __ATOMIC_ACQUIRE))
		thread_call_enter(s->work);
}

extern "C" int switch_gpu_event_acquire(void *task, uint32_t port, switch_gpu_event **out)
{
	if (!out || !task || !port)
		return -22;
	*out = nullptr;
	auto *native = switch_gpu_event_native_lookup(task, port);
	if (!native)
		return -2;
	auto *s = switch_gpu_events_state();
	auto *fresh = s ? IONewZero(switch_gpu_event, 1) : nullptr;
	if (!fresh) {
		native->release();
		return -12;
	}
	IOLockLock(s->lock);
	for (auto *event = s->events; event; event = event->next) {
		if (event->native != native)
			continue;
		++event->refs;
		*out = event;
		break;
	}
	if (!*out) {
		fresh->native = native;
		fresh->refs = 1;
		fresh->next = s->events;
		s->events = fresh;
		*out = fresh;
	}
	IOLockUnlock(s->lock);
	if (*out != fresh) {
		native->release();
		IODelete(fresh, switch_gpu_event, 1);
	}
	return 0;
}

extern "C" void switch_gpu_event_retain(switch_gpu_event *event)
{
	if (!event)
		return;
	IOLockLock(state->lock);
	++event->refs;
	IOLockUnlock(state->lock);
}

extern "C" void switch_gpu_event_release(switch_gpu_event **owned)
{
	if (!owned || !*owned)
		return;
	auto *event = *owned;
	*owned = nullptr;
	IOLockLock(state->lock);
	const bool last = --event->refs == 0;
	if (last) {
		auto **at = &state->events;
		while (*at && *at != event)
			at = &(*at)->next;
		if (*at)
			*at = event->next;
	}
	IOLockUnlock(state->lock);
	if (last) {
		event->native->release();
		IODelete(event, switch_gpu_event, 1);
	}
}

extern "C" uint64_t switch_gpu_event_value(switch_gpu_event *event)
{
	return event ? switch_gpu_event_native_value(event->native) : 0;
}

int switch_gpu_event_resolve_locked(switch_gpu_event *event, uint64_t value,
				    switch_gpu_backing_token *token)
{
	if (switch_gpu_event_native_value(event->native) >= value || event->readyThrough >= value)
		return 1;
	switch_gpu_event_signal *chosen = nullptr;
	for (auto *signal = event->signals; signal; signal = signal->next)
		if (signal->value >= value && (!chosen || signal->value < chosen->value))
			chosen = signal;
	if (!chosen)
		return event->failure && value <= event->failedThrough ? event->failure : 0;
	const int result = chosen->result
				   ? chosen->result
				   : switch_gpu_backing_token_external_status(&chosen->token);
	if (result < 0)
		return result;
	if (token) {
		const int retained = switch_gpu_backing_token_retain(&chosen->token);
		if (retained)
			return retained;
		*token = chosen->token;
	}
	return 2;
}

extern "C" int switch_gpu_event_resolve(switch_gpu_event *event, uint64_t value,
					switch_gpu_backing_token *token)
{
	if (!event || !token)
		return -22;
	*token = {};
	IOLockLock(state->lock);
	const int result = switch_gpu_event_resolve_locked(event, value, token);
	IOLockUnlock(state->lock);
	return result;
}

extern "C" int switch_gpu_event_signal_prepare(switch_gpu_event *event, uint64_t value,
					       switch_gpu_event_signal **out)
{
	if (!event || !out)
		return -22;
	*out = nullptr;
	if (!switch_gpu_events_reserve())
		return -16;
	auto *signal = IONewZero(switch_gpu_event_signal, 1);
	if (!signal) {
		switch_gpu_events_unreserve();
		return -12;
	}
	signal->refs = 1;
	signal->kind = SwitchEventSignal;
	IOLockLock(state->lock);
	const bool full = event->reservations >= kReservations;
	if (!full) {
		++event->reservations;
		++event->refs;
	}
	IOLockUnlock(state->lock);
	if (full) {
		IODelete(signal, switch_gpu_event_signal, 1);
		switch_gpu_events_unreserve();
		return -16;
	}
	signal->event = event;
	signal->value = value;
	*out = signal;
	return 0;
}

extern "C" int switch_gpu_event_signal_bind(switch_gpu_event_signal *signal,
					    const switch_gpu_backing_token *token)
{
	if (!signal || !token || !token->engine)
		return -22;
	if (signal->bound)
		return signal->token.engine == token->engine &&
				       signal->token.queue == token->queue &&
				       signal->token.reset_generation == token->reset_generation &&
				       signal->token.value == token->value
			       ? 0
			       : -22;
	const int result = switch_gpu_backing_token_retain(token);
	if (result)
		return result;
	signal->token = *token;
	signal->bound = true;
	return 0;
}

extern "C" void switch_gpu_event_signal_commit(switch_gpu_event_signal **owned)
{
	if (!owned || !*owned)
		return;
	auto *signal = *owned;
	if (!signal->bound) {
		switch_gpu_event_signal_abort(owned);
		return;
	}
	*owned = nullptr;
	IOLockLock(state->lock);
	if (signal->word)
		switch_gpu_event_stats_released(1);
	signal->next = signal->event->signals;
	signal->event->signals = signal;
	switch_gpu_event_watches_bind_locked(signal->event);
	__atomic_add_fetch(&state->pendingSignals, 1u, __ATOMIC_RELEASE);
	IOLockUnlock(state->lock);
	switch_gpu_events_schedule();
}

extern "C" void switch_gpu_event_signal_abort(switch_gpu_event_signal **owned)
{
	if (!owned || !*owned)
		return;
	auto *signal = *owned;
	*owned = nullptr;
	IOLockLock(state->lock);
	--signal->event->reservations;
	if (signal->word)
		switch_gpu_event_word_aborted_locked(signal);
	IOLockUnlock(state->lock);
	switch_gpu_event_release(&signal->event);
	switch_gpu_event_signal_drop(signal);
}

void switch_gpu_event_signal_drop(switch_gpu_event_signal *signal)
{
	if (__atomic_sub_fetch(&signal->refs, 1u, __ATOMIC_ACQ_REL))
		return;
	switch_gpu_event_dispose(signal);
}

extern "C" int switch_gpu_event_signal_inherit(switch_gpu_event_signal *signal,
					       switch_gpu_event_waits *batch)
{
	if (!signal || !batch || (signal->dependencies && signal->dependencies != batch))
		return -22;
	if (!signal->dependencies) {
		__atomic_add_fetch(&batch->refs, 1u, __ATOMIC_RELAXED);
		signal->dependencies = batch;
	}
	return 0;
}

extern "C" int switch_gpu_event_resolve_use(switch_gpu_event *event, uint64_t value,
					    switch_gpu_backing_token *token,
					    switch_gpu_event_use **out)
{
	if (!event || !token || !out)
		return -22;
	*out = nullptr;
	*token = {};
	if (!switch_gpu_events_reserve())
		return -16;
	auto *use = IONewZero(switch_gpu_event_use, 1);
	if (!use) {
		switch_gpu_events_unreserve();
		return -12;
	}
	IOLockLock(state->lock);
	const int result = switch_gpu_event_resolve_locked(event, value, &use->token);
	if (result == 2) {
		switch_gpu_event_signal *chosen = nullptr;
		for (auto *signal = event->signals; signal; signal = signal->next)
			if (signal->value >= value && (!chosen || signal->value < chosen->value))
				chosen = signal;
		use->authority = chosen;
		use->refs = 1;
		use->kind = SwitchEventUse;
		__atomic_add_fetch(&chosen->refs, 1u, __ATOMIC_RELAXED);
		*out = use;
		*token = use->token;
	}
	IOLockUnlock(state->lock);
	if (result != 2) {
		IODelete(use, switch_gpu_event_use, 1);
		switch_gpu_events_unreserve();
	}
	return result;
}

extern "C" void switch_gpu_event_use_release(switch_gpu_event_use **owned)
{
	if (!owned || !*owned)
		return;
	auto *use = *owned;
	*owned = nullptr;
	if (__atomic_sub_fetch(&use->refs, 1u, __ATOMIC_ACQ_REL))
		return;
	switch_gpu_event_dispose(use);
}
