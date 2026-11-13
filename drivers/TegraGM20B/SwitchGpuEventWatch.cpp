// SPDX-License-Identifier: MIT
#include "SwitchGpuEventInternal.h"

OSDefineMetaClassAndStructors(switch_gpu_event_watch, OSObject)

	bool switch_gpu_event_watch::initialize()
{
	return OSObject::init();
}

void switch_gpu_event_watch::free()
{
	if (authority)
		switch_gpu_event_signal_drop(authority);
	switch_gpu_event_release(&event);
	switch_gpu_events_unreserve();
	OSObject::free();
}

namespace
{
int terminalLocked(switch_gpu_event *event, uint64_t value, switch_gpu_event_signal **authority)
{
	if (authority && *authority)
		return (*authority)->result;
	if (switch_gpu_event_native_value(event->native) >= value || event->readyThrough >= value)
		return 1;
	switch_gpu_event_signal *chosen = nullptr;
	for (auto *signal = event->signals; signal; signal = signal->next)
		if (signal->value >= value && (!chosen || signal->value < chosen->value))
			chosen = signal;
	if (!chosen)
		return event->failure && value <= event->failedThrough ? event->failure : 0;
	if (authority) {
		__atomic_add_fetch(&chosen->refs, 1u, __ATOMIC_RELAXED);
		*authority = chosen;
	}
	return chosen->result;
}

void nativeReady(OSObject *, OSObject *, uint64_t, void *)
{
	switch_gpu_events_schedule();
}

void unlinkLocked(switch_gpu_event_watch *watch)
{
	auto **at = &watch->event->watches;
	while (*at && *at != watch)
		at = &(*at)->next;
	if (*at) {
		*at = watch->next;
		--watch->event->observers;
	}
	watch->done = true;
}

void deliver(switch_gpu_event_watch *watch)
{
	auto *waiter = watch->waiter;
	watch->waiter = nullptr;
	if (waiter) {
		switch_gpu_event_native_waiter_cancel(watch->event->native, waiter);
		waiter->release();
	}
	watch->completed(watch->context, watch->cookie, watch->result);
	watch->release();
}
}

static int observe(switch_gpu_event *event, uint64_t value, uint64_t cookie,
		   void (*completed)(void *, uint64_t, int), void *context,
		   switch_gpu_event_watch **out, bool terminal)
{
	if (!event || !completed || !out)
		return -22;
	*out = nullptr;
	if (!switch_gpu_events_reserve())
		return -16;
	auto *watch = new switch_gpu_event_watch;
	if (!watch || !watch->initialize()) {
		if (watch)
			watch->release();
		else
			switch_gpu_events_unreserve();
		return -12;
	}
	switch_gpu_event_retain(event);
	watch->event = event;
	watch->value = value;
	watch->cookie = cookie;
	watch->completed = completed;
	watch->context = context;
	watch->terminal = terminal;
	auto *s = switch_gpu_events_state();
	watch->waiter = switch_gpu_event_native_waiter_create(event->native, s->listener, value,
							      nullptr, watch, nativeReady);
	if (!watch->waiter) {
		watch->release();
		return -12;
	}
	IOLockLock(s->lock);
	if (event->observers >= 256) {
		IOLockUnlock(s->lock);
		watch->waiter->release();
		watch->waiter = nullptr;
		watch->release();
		return -16;
	}
	if (terminal)
		watch->terminalStatus = terminalLocked(event, value, &watch->authority);
	++event->observers;
	watch->retain();
	watch->next = event->watches;
	event->watches = watch;
	IOLockUnlock(s->lock);
	switch_gpu_event_native_waiter_arm(event->native, watch->waiter);
	IOLockLock(s->lock);
	watch->arming = false;
	IOLockUnlock(s->lock);
	*out = watch;
	switch_gpu_events_schedule();
	return 0;
}

extern "C" int switch_gpu_event_terminal(switch_gpu_event *event, uint64_t value)
{
	if (!event)
		return -22;
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	const int result = terminalLocked(event, value, nullptr);
	IOLockUnlock(s->lock);
	return result;
}

extern "C" int switch_gpu_event_observe(switch_gpu_event *event, uint64_t value, uint64_t cookie,
					void (*completed)(void *, uint64_t, int), void *context,
					switch_gpu_event_watch **out)
{
	return observe(event, value, cookie, completed, context, out, false);
}

extern "C" int switch_gpu_event_observe_terminal(switch_gpu_event *event, uint64_t value,
						 uint64_t cookie,
						 void (*completed)(void *, uint64_t, int),
						 void *context, switch_gpu_event_watch **out)
{
	return observe(event, value, cookie, completed, context, out, true);
}

void switch_gpu_event_watches_bind_locked(switch_gpu_event *event)
{
	for (auto *watch = event->watches; watch; watch = watch->next)
		if (watch->terminal && !watch->terminalStatus)
			watch->terminalStatus =
				terminalLocked(event, watch->value, &watch->authority);
}

extern "C" void switch_gpu_event_observe_cancel(switch_gpu_event_watch **owned)
{
	if (!owned || !*owned)
		return;
	auto *watch = *owned;
	*owned = nullptr;
	auto *s = switch_gpu_events_state();
	IOLockLock(s->lock);
	const bool cancelled = !watch->done;
	if (cancelled) {
		unlinkLocked(watch);
		watch->result = -125;
	}
	IOLockUnlock(s->lock);
	if (cancelled)
		deliver(watch);
	watch->release();
}

void switch_gpu_event_watches_retire()
{
	auto *s = switch_gpu_events_state();
	switch_gpu_event_watch *ready = nullptr;
	IOLockLock(s->lock);
	for (auto *event = s->events; event; event = event->next) {
		auto *watch = event->watches;
		while (watch) {
			auto *next = watch->next;
			const int result = watch->arming ? 0
					   : watch->terminal
						   ? (watch->terminalStatus
							      ? watch->terminalStatus
							      : terminalLocked(event, watch->value,
									       &watch->authority))
						   : switch_gpu_event_resolve_locked(
							     event, watch->value, nullptr);
			if (result) {
				unlinkLocked(watch);
				watch->result = result > 0 ? 0 : result;
				watch->next = ready;
				ready = watch;
			}
			watch = next;
		}
	}
	IOLockUnlock(s->lock);
	while (ready) {
		auto *watch = ready;
		ready = watch->next;
		deliver(watch);
	}
}
