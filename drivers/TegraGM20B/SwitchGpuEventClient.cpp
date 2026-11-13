// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include "SwitchGpuEventClient.h"
#include "SwitchGpuEventWord.h"

struct SwitchGpuEventClient::Watch {
	Watch *next;
	SwitchGpuEventClient *owner;
	switch_gpu_event_watch *watch;
	Callback callback;
	void *context;
	uint64_t handle, cookie;
	uint32_t references;
	bool registering, finished, cancelled;
};

bool SwitchGpuEventClient::initialize()
{
	lock_ = IOLockAlloc();
	return lock_ != nullptr;
}

switch_gpu_event *SwitchGpuEventClient::find(uint64_t handle) const
{
	const auto &entry = entries_[handle % kHandles];
	return handle && entry.handle == handle ? entry.event : nullptr;
}

int SwitchGpuEventClient::import(void *task, uint32_t port, uint64_t *handle, uint64_t *value)
{
	switch_gpu_event *event = nullptr;
	int result = switch_gpu_event_acquire(task, port, &event);
	if (result)
		return result;
	const uint64_t currentValue = switch_gpu_event_value(event);
	IOLockLock(lock_);
	result = closed_ ? -19 : -28;
	for (uint32_t i = 0; !closed_ && i < kHandles; ++i) {
		if (entries_[i].event ||
		    (entries_[i].word && !switch_gpu_event_word_idle(entries_[i].word)))
			continue;
		if (next_ > UINT64_MAX / kHandles)
			break;
		*handle = next_++ * kHandles + i;
		entries_[i].handle = *handle;
		entries_[i].event = event;
		if (entries_[i].word)
			switch_gpu_event_word_disarm(entries_[i].word);
		*value = currentValue;
		event = nullptr;
		result = 0;
		break;
	}
	IOLockUnlock(lock_);
	switch_gpu_event_release(&event);
	return result;
}

int SwitchGpuEventClient::release(uint64_t handle)
{
	IOLockLock(lock_);
	switch_gpu_event *event = find(handle);
	if (event) {
		entries_[handle % kHandles].handle = 0;
		entries_[handle % kHandles].event = nullptr;
	}
	IOLockUnlock(lock_);
	if (!event)
		return -22;
	for (;;) {
		IOLockLock(lock_);
		Watch *watch = watches_;
		while (watch && watch->handle != handle)
			watch = watch->next;
		const uint64_t cookie = watch ? watch->cookie : 0;
		IOLockUnlock(lock_);
		if (!watch)
			break;
		cancel(handle, cookie);
	}
	switch_gpu_event_release(&event);
	return 0;
}

int SwitchGpuEventClient::capture(const TegraGM20BAbiEventUse *uses, uint32_t count,
				  switch_gpu_event **events)
{
	if (count > TegraGM20BMaxJobEvents)
		return -22;
	IOLockLock(lock_);
	int result = closed_ ? -19 : 0;
	uint32_t held = 0;
	for (; !result && held < count; ++held) {
		events[held] = find(uses[held].handle);
		if (!events[held]) {
			result = -22;
			break;
		}
		switch_gpu_event_retain(events[held]);
	}
	IOLockUnlock(lock_);
	if (result)
		while (held)
			switch_gpu_event_release(&events[--held]);
	return result;
}

int SwitchGpuEventClient::probe(uint64_t handle, uint64_t value, bool terminal)
{
	const TegraGM20BAbiEventUse use = {handle, value};
	switch_gpu_event *event = nullptr;
	int result = capture(&use, 1, &event);
	if (!result) {
		switch_gpu_backing_token token = {};
		result = terminal ? switch_gpu_event_terminal(event, value)
				  : switch_gpu_event_resolve(event, value, &token);
		if (result == 2)
			switch_gpu_backing_token_release(&token);
		result = result > 0 ? 0 : result ? result : -19;
	}
	switch_gpu_event_release(&event);
	return result;
}

bool SwitchGpuEventClient::unlink(Watch *watch)
{
	Watch **entry = &watches_;
	while (*entry && *entry != watch)
		entry = &(*entry)->next;
	if (!*entry)
		return false;
	*entry = watch->next;
	--count_;
	return true;
}

void SwitchGpuEventClient::drop(Watch *watch)
{
	if (!__atomic_sub_fetch(&watch->references, 1u, __ATOMIC_ACQ_REL))
		IODelete(watch, Watch, 1);
}

void SwitchGpuEventClient::completed(void *context, uint64_t cookie, int result)
{
	auto *watch = static_cast<Watch *>(context);
	auto *owner = watch->owner;
	switch_gpu_event_watch *native = nullptr;
	IOLockLock(owner->lock_);
	watch->finished = true;
	const bool removed = !watch->registering && owner->unlink(watch);
	if (!watch->registering) {
		native = watch->watch;
		watch->watch = nullptr;
	}
	IOLockUnlock(owner->lock_);
	switch_gpu_event_observe_cancel(&native);
	if (removed)
		drop(watch);
	const auto callback = watch->callback;
	void *argument = watch->context;
	drop(watch);
	callback(argument, cookie, result);
}

int SwitchGpuEventClient::observe(uint64_t handle, uint64_t value, uint64_t cookie,
				  Callback callback, void *context, bool terminal)
{
	const TegraGM20BAbiEventUse use = {handle, value};
	switch_gpu_event *event = nullptr;
	int result = capture(&use, 1, &event);
	if (result)
		return result;
	auto *watch = IONew(Watch, 1);
	if (!watch) {
		switch_gpu_event_release(&event);
		return -12;
	}
	*watch = {nullptr, this, nullptr, callback, context, handle, cookie, 3, true, false, false};
	IOLockLock(lock_);
	result = closed_ ? -19 : !find(handle) ? -22 : count_ >= kWatches ? -28 : 0;
	for (auto *entry = watches_; !result && entry; entry = entry->next)
		if (entry->handle == handle && entry->cookie == cookie)
			result = -22;
	if (!result) {
		watch->next = watches_;
		watches_ = watch;
		++count_;
	}
	IOLockUnlock(lock_);
	if (result) {
		switch_gpu_event_release(&event);
		IODelete(watch, Watch, 1);
		return result;
	}
	switch_gpu_event_watch *native = nullptr;
	result = terminal ? switch_gpu_event_observe_terminal(event, value, cookie, completed,
							      watch, &native)
			  : switch_gpu_event_observe(event, value, cookie, completed, watch,
						     &native);
	switch_gpu_event_release(&event);
	IOLockLock(lock_);
	watch->registering = false;
	const bool remove = result || watch->finished || watch->cancelled;
	const bool removed = remove && unlink(watch);
	if (!remove) {
		watch->watch = native;
		native = nullptr;
	}
	IOLockUnlock(lock_);
	switch_gpu_event_observe_cancel(&native);
	if (removed)
		drop(watch);
	if (result)
		drop(watch);
	drop(watch);
	return result;
}

int SwitchGpuEventClient::cancel(uint64_t handle, uint64_t cookie)
{
	IOLockLock(lock_);
	Watch *watch = watches_;
	while (watch && (watch->handle != handle || watch->cookie != cookie))
		watch = watch->next;
	switch_gpu_event_watch *native = nullptr;
	if (watch) {
		unlink(watch);
		watch->cancelled = true;
		native = watch->watch;
		watch->watch = nullptr;
	}
	IOLockUnlock(lock_);
	switch_gpu_event_observe_cancel(&native);
	if (watch)
		drop(watch);
	return 0;
}

void SwitchGpuEventClient::stop()
{
	if (!lock_)
		return;
	IOLockLock(lock_);
	closed_ = true;
	IOLockUnlock(lock_);
	for (;;) {
		IOLockLock(lock_);
		Watch *watch = watches_;
		const uint64_t handle = watch ? watch->handle : 0;
		const uint64_t cookie = watch ? watch->cookie : 0;
		IOLockUnlock(lock_);
		if (!watch)
			break;
		cancel(handle, cookie);
	}
	for (auto &entry : entries_) {
		IOLockLock(lock_);
		auto *event = entry.event;
		auto *word = entry.word;
		entry = {};
		IOLockUnlock(lock_);
		switch_gpu_event_release(&event);
		switch_gpu_event_word_release(&word);
	}
}

void SwitchGpuEventClient::destroy()
{
	stop();
	if (lock_)
		IOLockFree(lock_);
	lock_ = nullptr;
}
