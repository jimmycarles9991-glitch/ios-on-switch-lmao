// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>
#include "SwitchGpuAdmission.h"
#include "SwitchGpuPathStats.h"
#include "SwitchGpuReady.h"
#include "SwitchGpuResourceClient.h"
#include "TegraGM20BViews.h"

struct SwitchGpuResourceClient::Watch {
	Watch *next;
	SwitchGpuResourceClient *owner;
	switch_gpu_ready_watch *watch;
	SwitchGpuResourceUse *uses;
	Callback callback;
	void *context;
	uint64_t cookie;
	uint32_t references, count;
	bool registering, finished, cancelled;
};

bool SwitchGpuResourceClient::initialize()
{
	lock_ = IOLockAlloc();
	return lock_ != nullptr;
}

void SwitchGpuResourceClient::drop(Watch *watch)
{
	if (__atomic_sub_fetch(&watch->references, 1u, __ATOMIC_ACQ_REL))
		return;
	for (uint32_t i = 0; i < watch->count; ++i) {
		switch_gpu_backing_release(watch->uses[i].backing);
		if (watch->uses[i].surface)
			watch->uses[i].surface->release();
	}
	IODelete(watch->uses, SwitchGpuResourceUse, watch->count);
	IODelete(watch, Watch, 1);
}

bool SwitchGpuResourceClient::unlink(Watch *watch)
{
	auto **at = &watches_;
	while (*at && *at != watch)
		at = &(*at)->next;
	if (!*at)
		return false;
	*at = watch->next;
	--count_;
	return true;
}

int SwitchGpuResourceClient::query(void *context)
{
	auto *watch = static_cast<Watch *>(context);
	for (uint32_t i = 0; i < watch->count; ++i) {
		const auto &use = watch->uses[i];
		int result = 1;
		if (use.surface && use.generation) {
			const auto generation = __atomic_load_n(use.generation, __ATOMIC_ACQUIRE);
			if (generation != use.expected ||
			    (generation > 1 && (use.access & SWITCH_GPU_BACKING_WRITE)))
				result = switch_gpu_surface_gpu_admission(use.surface, use.backing,
									  use.access);
		}
		if (result == 1)
			result = switch_gpu_backing_admission_ready(use.backing, use.access);
		if (result != 1)
			return result;
	}
	return 1;
}

void SwitchGpuResourceClient::completed(void *context, int result)
{
	auto *watch = static_cast<Watch *>(context);
	auto *owner = watch->owner;
	switch_gpu_ready_watch *native = nullptr;
	IOLockLock(owner->lock_);
	watch->finished = true;
	const bool removed = !watch->registering && owner->unlink(watch);
	if (!watch->registering) {
		native = watch->watch;
		watch->watch = nullptr;
	}
	IOLockUnlock(owner->lock_);
	switch_gpu_ready_cancel(&native);
	if (removed)
		drop(watch);
	const auto callback = watch->callback;
	void *argument = watch->context;
	const auto cookie = watch->cookie;
	drop(watch);
	callback(argument, cookie, result);
}

int SwitchGpuResourceClient::observe(const SwitchGpuResourceUse *uses, uint32_t count,
				     uint64_t cookie, Callback callback, void *context)
{
	if (!count || !uses || !callback || !cookie)
		return -22;
	auto *watch = IONew(Watch, 1);
	if (!watch)
		return -12;
	auto *copy = IONew(SwitchGpuResourceUse, count);
	if (!copy) {
		IODelete(watch, Watch, 1);
		return -12;
	}
	*watch = {nullptr, this, nullptr, copy, callback, context,
		  cookie,  3,    count,   true, false,    false};
	for (uint32_t i = 0; i < count; ++i) {
		copy[i] = uses[i];
		switch_gpu_backing_retain(copy[i].backing);
		if (copy[i].surface)
			copy[i].surface->retain();
	}
	IOLockLock(lock_);
	int result = closed_ ? -19 : count_ == 64 ? -28 : 0;
	for (auto *at = watches_; !result && at; at = at->next)
		if (at->cookie == cookie)
			result = -22;
	if (!result) {
		watch->next = watches_;
		watches_ = watch;
		++count_;
		switch_gpu_path_taken(SWITCH_GPU_PATH_READY_ARM, 1);
	}
	IOLockUnlock(lock_);
	if (result) {
		watch->references = 1;
		drop(watch);
		return result;
	}
	switch_gpu_ready_watch *native = nullptr;
	result = switch_gpu_ready_observe(query, completed, watch, &native);
	IOLockLock(lock_);
	watch->registering = false;
	const bool remove = result || watch->finished || watch->cancelled;
	const bool removed = remove && unlink(watch);
	if (!remove) {
		watch->watch = native;
		native = nullptr;
	}
	IOLockUnlock(lock_);
	switch_gpu_ready_cancel(&native);
	if (removed)
		drop(watch);
	if (result)
		drop(watch);
	drop(watch);
	return result;
}

int SwitchGpuResourceClient::cancel(uint64_t cookie)
{
	IOLockLock(lock_);
	auto *watch = watches_;
	while (watch && watch->cookie != cookie)
		watch = watch->next;
	switch_gpu_ready_watch *native = nullptr;
	if (watch) {
		unlink(watch);
		watch->cancelled = true;
		if (!watch->registering) {
			native = watch->watch;
			watch->watch = nullptr;
		}
	}
	IOLockUnlock(lock_);
	if (!watch)
		return 0;
	switch_gpu_ready_cancel(&native);
	drop(watch);
	return 0;
}

void SwitchGpuResourceClient::stop()
{
	if (!lock_)
		return;
	IOLockLock(lock_);
	closed_ = true;
	IOLockUnlock(lock_);
	for (;;) {
		IOLockLock(lock_);
		const uint64_t cookie = watches_ ? watches_->cookie : 0;
		IOLockUnlock(lock_);
		if (!cookie)
			break;
		cancel(cookie);
	}
}

void SwitchGpuResourceClient::destroy()
{
	stop();
	if (lock_)
		IOLockFree(lock_);
	lock_ = nullptr;
}
