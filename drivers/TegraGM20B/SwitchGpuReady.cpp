// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include "SwitchGpuReady.h"

struct switch_gpu_ready_watch {
	switch_gpu_ready_watch *next;
	switch_gpu_ready_query query;
	switch_gpu_ready_callback callback;
	void *context;
	uint64_t seen;
	uint32_t references;
	bool checking, cancelled, finished;
};

namespace
{
constexpr uint32_t maximum = 256, batch = 16;

struct State {
	IOLock *lock;
	thread_call_t work;
	switch_gpu_ready_watch *watches;
	uint64_t generation;
	uint32_t count;
};

State *state;

void drop(switch_gpu_ready_watch *watch)
{
	if (!__atomic_sub_fetch(&watch->references, 1u, __ATOMIC_ACQ_REL))
		IODelete(watch, switch_gpu_ready_watch, 1);
}

void unlink(State *owner, switch_gpu_ready_watch *watch)
{
	auto **at = &owner->watches;
	while (*at != watch)
		at = &(*at)->next;
	*at = watch->next;
	watch->finished = true;
	__atomic_sub_fetch(&owner->count, 1u, __ATOMIC_RELEASE);
}

void complete(switch_gpu_ready_watch *watch, int result)
{
	watch->callback(watch->context, result);
	drop(watch);
}

void work(thread_call_param_t context, thread_call_param_t)
{
	auto *owner = static_cast<State *>(context);
	const uint64_t generation = __atomic_load_n(&owner->generation, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < batch; ++i) {
		IOLockLock(owner->lock);
		auto *watch = owner->watches;
		while (watch && watch->seen == generation)
			watch = watch->next;
		if (!watch) {
			IOLockUnlock(owner->lock);
			return;
		}
		watch->seen = generation;
		watch->checking = true;
		__atomic_add_fetch(&watch->references, 1u, __ATOMIC_RELAXED);
		IOLockUnlock(owner->lock);
		int result = watch->query(watch->context);
		IOLockLock(owner->lock);
		watch->checking = false;
		if (watch->cancelled)
			result = -125;
		if (result) {
			unlink(owner, watch);
		} else {
			auto **at = &owner->watches;
			while (*at != watch)
				at = &(*at)->next;
			*at = watch->next;
			while (*at)
				at = &(*at)->next;
			*at = watch;
			watch->next = nullptr;
		}
		IOLockUnlock(owner->lock);
		if (result)
			complete(watch, result == 1 ? 0 : result);
		drop(watch);
	}
	IOLockLock(owner->lock);
	bool more = false;
	for (auto *watch = owner->watches; watch; watch = watch->next)
		more |= watch->seen != generation;
	IOLockUnlock(owner->lock);
	if (more)
		thread_call_enter(owner->work);
}

State *initialize()
{
	auto *owner = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
	if (owner)
		return owner;
	auto *made = IONew(State, 1);
	if (!made)
		return nullptr;
	*made = {};
	made->lock = IOLockAlloc();
	made->generation = 1;
	made->work = thread_call_allocate_with_options(work, made, THREAD_CALL_PRIORITY_KERNEL,
						       THREAD_CALL_OPTIONS_ONCE);
	if (!made->lock || !made->work) {
		if (made->lock)
			IOLockFree(made->lock);
		if (made->work)
			thread_call_free(made->work);
		IODelete(made, State, 1);
		return nullptr;
	}
	if (__atomic_compare_exchange_n(&state, &owner, made, false, __ATOMIC_RELEASE,
					__ATOMIC_ACQUIRE))
		return made;
	thread_call_free(made->work);
	IOLockFree(made->lock);
	IODelete(made, State, 1);
	return owner;
}
}

int switch_gpu_ready_observe(switch_gpu_ready_query query, switch_gpu_ready_callback callback,
			     void *context, switch_gpu_ready_watch **out)
{
	if (!out || !query || !callback)
		return -22;
	*out = nullptr;
	auto *owner = initialize();
	if (!owner)
		return -12;
	auto *watch = IONew(switch_gpu_ready_watch, 1);
	if (!watch)
		return -12;
	*watch = {nullptr, query, callback, context, 0, 2, false, false, false};
	IOLockLock(owner->lock);
	const bool full = __atomic_load_n(&owner->count, __ATOMIC_RELAXED) == maximum;
	if (!full) {
		auto **at = &owner->watches;
		while (*at)
			at = &(*at)->next;
		*at = watch;
		__atomic_add_fetch(&owner->count, 1u, __ATOMIC_RELEASE);
		*out = watch;
	}
	IOLockUnlock(owner->lock);
	if (full) {
		IODelete(watch, switch_gpu_ready_watch, 1);
		return -28;
	}
	thread_call_enter(owner->work);
	return 0;
}

void switch_gpu_ready_cancel(switch_gpu_ready_watch **input)
{
	if (!input || !*input)
		return;
	auto *watch = *input;
	*input = nullptr;
	auto *owner = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
	IOLockLock(owner->lock);
	const bool notify = !watch->finished && !watch->checking;
	watch->cancelled = true;
	if (notify)
		unlink(owner, watch);
	IOLockUnlock(owner->lock);
	if (notify)
		complete(watch, -125);
	drop(watch);
}

extern "C" int switch_gpu_ready_waiting(void)
{
	auto *owner = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
	return owner && __atomic_load_n(&owner->count, __ATOMIC_ACQUIRE);
}

extern "C" void switch_gpu_admission_changed(void)
{
	auto *owner = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
	if (!owner || !__atomic_load_n(&owner->count, __ATOMIC_ACQUIRE))
		return;
	__atomic_add_fetch(&owner->generation, uint64_t(1), __ATOMIC_RELEASE);
	thread_call_enter(owner->work);
}
