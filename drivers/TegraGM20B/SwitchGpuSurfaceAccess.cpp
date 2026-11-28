// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOUserClient.h>
#ifdef KERNEL
#include <IOKit/IOMessage.h>
#include <IOKit/IOService.h>
#include <kern/thread_call.h>
#endif
#include <kern/thread.h>
#include "SwitchGpuBacking.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BViews.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

extern "C" void switch_gpu_surface_sweep(void);

namespace
{
struct Lease {
	Lease *next;
	IOUserClient *client;
	switch_gpu_backing *backing;
	void *descriptor;
	uint64_t bytes;
	uint32_t id, flags;
	uint32_t pending[2], held[2];
};

struct Association {
	Association *next;
	void *descriptor;
	switch_gpu_backing *backing;
	bool adopted;
};

struct Pending {
	Pending *next;
	Lease *lease;
	void *thread;
	uint32_t index;
};

struct ClientWatch {
	ClientWatch *next;
	IOUserClient *client;
	IONotifier *notifier;
	uint32_t refs;
	bool registering;
};

IOLock *lock;
Lease *leases, *free_leases;
uint32_t lease_count;  // leases' length, read without the lock by switch_gpu_surface_sweep
Pending *pending, *free_pending;
Association *associations;
ClientWatch *watches;
#ifdef KERNEL
thread_call_t sweep_call;

void sweep_work(thread_call_param_t, thread_call_param_t)
{
	switch_gpu_surface_sweep();
}

void schedule_sweep()
{
	if (thread_call_t call = __atomic_load_n(&sweep_call, __ATOMIC_ACQUIRE))
		thread_call_enter(call);
}

IOReturn client_interest(void *, void *, UInt32 message_type, IOService *, void *, vm_size_t)
{
	if (message_type == kIOMessageServiceIsTerminated)
		schedule_sweep();
	return kIOReturnSuccess;
}
#else
void schedule_sweep()
{
	switch_gpu_surface_sweep();
}

int client_interest(void *, void *, uint32_t message_type, IOService *, void *, size_t)
{
	if (message_type == kIOMessageServiceIsTerminated)
		schedule_sweep();
	return 0;
}
#endif

bool initialize()
{
	if (__atomic_load_n(&lock, __ATOMIC_ACQUIRE))
		return true;
#ifdef KERNEL
	if (!__atomic_load_n(&sweep_call, __ATOMIC_ACQUIRE)) {
		thread_call_t call = thread_call_allocate_with_options(
			sweep_work, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
		if (!call)
			return false;
		thread_call_t no_call = nullptr;
		if (!__atomic_compare_exchange_n(&sweep_call, &no_call, call, false,
						 __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
			thread_call_free(call);
	}
#endif
	IOLock *made = IOLockAlloc(), *expected = nullptr;
	if (!made)
		return false;
	if (!__atomic_compare_exchange_n(&lock, &expected, made, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE))
		IOLockFree(made);
	return true;
}

ClientWatch *find_watch_locked(IOUserClient *client)
{
	for (auto *watch = watches; watch; watch = watch->next)
		if (watch->client == client)
			return watch;
	return nullptr;
}

void unlink_watch_locked(ClientWatch *target)
{
	for (auto **at = &watches; *at; at = &(*at)->next) {
		if (*at == target) {
			*at = target->next;
			return;
		}
	}
}

Lease **find(void *client, uint32_t id)
{
	auto **at = &leases;
	while (*at && ((*at)->client != client || (*at)->id != id))
		at = &(*at)->next;
	return at;
}

Association *association(void *descriptor)
{
	for (auto *entry = associations; entry; entry = entry->next)
		if (entry->descriptor == descriptor)
			return entry;
	return nullptr;
}

uint32_t access(uint32_t index)
{
	return index ? SWITCH_GPU_BACKING_WRITE : SWITCH_GPU_BACKING_READ;
}

bool take_pending(Lease *lease, uint32_t index)
{
	auto **at = &pending;
	while (*at && ((*at)->lease != lease || (*at)->index != index ||
		       (*at)->thread != current_thread()))
		at = &(*at)->next;
	if (!*at)
		return false;
	auto *item = *at;
	*at = item->next;
	item->next = free_pending;
	free_pending = item;
	return true;
}

void recycle(Lease *lease)
{
	IOUserClient *client = lease->client;
	switch_gpu_backing *backing = lease->backing;
	IONotifier *drop_notifier = nullptr;
	IOLockLock(lock);
	if (ClientWatch *watch = find_watch_locked(client)) {
		if (!--watch->refs && !watch->registering) {
			unlink_watch_locked(watch);
			drop_notifier = watch->notifier;
			IODelete(watch, ClientWatch, 1);
		}
	}
	lease->next = free_leases;
	free_leases = lease;
	IOLockUnlock(lock);
	if (drop_notifier)
		drop_notifier->remove();
	client->release();
	if (backing)
		switch_gpu_backing_release(backing);
}

void release_one(void *client, uint32_t id, uint32_t options, bool failed)
{
	if (!lock)
		return;
	const uint32_t index = options & 1 ? 0 : 1;
	IOLockLock(lock);
	auto **at = find(client, id);
	auto *lease = *at;
	uint32_t *counts = lease ? (failed ? lease->pending : lease->held) : nullptr;
	if (!counts || !counts[index] || (failed && !take_pending(lease, index))) {
		IOLockUnlock(lock);
		return;
	}
	--counts[index];
	auto *backing = lease->backing;
	void *descriptor = lease->descriptor;
	const uint64_t bytes = lease->bytes;
	const uint32_t flags = lease->flags | access(index) | SWITCH_GPU_BACKING_CPU_NO_CACHE;
	const bool last =
		!lease->pending[0] && !lease->pending[1] && !lease->held[0] && !lease->held[1];
	if (last) {
		*at = lease->next;
		__atomic_store_n(&lease_count, lease_count - 1u, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock);
	if (backing) {
		if (failed)
			switch_gpu_backing_cpu_cancel(backing, flags);
		else
			switch_gpu_backing_cpu_release(backing, 0, bytes, flags);
	}
	switch_gpu_surface_finish(descriptor);
	if (last)
		recycle(lease);
}
}  // namespace

extern "C" int switch_gpu_surface_adopt(void *descriptor, switch_gpu_backing *backing)
{
	if (!descriptor || !backing)
		return -22;
	if (!initialize())
		return -12;
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(backing, &info);
	IOLockLock(lock);
	auto *entry = association(descriptor);
	int result = entry && entry->backing != backing ? -95 : 0;
	if (!result && !entry && switch_gpu_surface_users(descriptor)) {
		entry = IONew(Association, 1);
		if (!entry) {
			result = -12;
		} else {
			switch_gpu_surface_descriptor_hold(descriptor, true);
			switch_gpu_backing_retain(backing);
			*entry = {associations, descriptor, backing, false};
			associations = entry;
		}
	}
	for (auto *lease = leases; !result && entry && !entry->adopted && lease;
	     lease = lease->next) {
		if (lease->descriptor != descriptor || lease->backing == backing)
			continue;
		if (lease->backing) {
			result = -95;
			break;
		}
		result = switch_gpu_backing_cpu_adopt(backing, lease->pending[0] + lease->held[0],
						      lease->pending[1] + lease->held[1]);
		if (!result) {
			switch_gpu_backing_retain(backing);
			lease->backing = backing;
			lease->bytes = uint64_t(info.page_count) << 12;
		}
	}
	if (!result && entry)
		entry->adopted = true;
	IOLockUnlock(lock);
	return result;
}

extern "C" void switch_gpu_surface_idle(void *descriptor)
{
	if (!lock || !descriptor)
		return;
	IOLockLock(lock);
	auto **at = &associations;
	while (*at && (*at)->descriptor != descriptor)
		at = &(*at)->next;
	auto *removed = *at;
	if (removed && !switch_gpu_surface_users(descriptor))
		*at = removed->next;
	else
		removed = nullptr;
	IOLockUnlock(lock);
	if (removed) {
		switch_gpu_backing_release(removed->backing);
		switch_gpu_surface_descriptor_hold(removed->descriptor, false);
		IODelete(removed, Association, 1);
	}
}

extern "C" void switch_gpu_surface_sweep(void)
{
	if (!lock || !__atomic_load_n(&lease_count, __ATOMIC_ACQUIRE))
		return;
	IOLockLock(lock);
	Lease *dead = nullptr;
	for (auto **at = &leases; *at;) {
		auto *lease = *at;
		if (!lease->client->isInactive()) {
			at = &lease->next;
			continue;
		}
		*at = lease->next;
		__atomic_store_n(&lease_count, lease_count - 1u, __ATOMIC_RELEASE);
		lease->next = dead;
		dead = lease;
		for (auto **item = &pending; *item;) {
			if ((*item)->lease != lease) {
				item = &(*item)->next;
				continue;
			}
			auto *removed = *item;
			*item = removed->next;
			removed->next = free_pending;
			free_pending = removed;
		}
	}
	IOLockUnlock(lock);
	while (dead) {
		auto *lease = dead;
		dead = dead->next;
		switch_gpu_backing_info info = {};
		if (lease->backing)
			switch_gpu_backing_describe(lease->backing, &info);
		if (auto *coherency = info.page_count ? tgm_coh_find_pa(info.pages[0]) : nullptr) {
			tgm_views_sweep(coherency);
			tgm_coherency_put(coherency);
		}
		for (uint32_t index = 0; index < 2; ++index) {
			while (lease->pending[index]) {
				--lease->pending[index];
				if (lease->backing)
					switch_gpu_backing_cpu_cancel(lease->backing,
								      lease->flags | access(index));
				switch_gpu_surface_finish(lease->descriptor);
			}
			while (lease->held[index]) {
				--lease->held[index];
				if (lease->backing)
					switch_gpu_backing_cpu_release(
						lease->backing, 0, lease->bytes,
						lease->flags | access(index));
				switch_gpu_surface_finish(lease->descriptor);
			}
		}
		recycle(lease);
	}
}

extern "C" int switch_gpu_surface_lock_will(void *opaque, uint32_t id, uint32_t options)
{
	if (!id || !initialize())
		return id ? -12 : 0;
	switch_gpu_surface_sweep();
	tgm_views_deferred_lock(id, options);
	switch_gpu_backing *backing = nullptr;
	switch_gpu_cpu_lease *owned = nullptr;
	void *descriptor = nullptr;
	const int resolved = switch_gpu_surface_prepare(id, current_task(), options, &backing,
							&owned, &descriptor);
	if (resolved || (!backing && !descriptor))
		return resolved;
acquire_backing:
	const int adopted =
		backing && descriptor ? switch_gpu_surface_adopt(descriptor, backing) : 0;
	if (adopted) {
		if (owned)
			switch_gpu_backing_cpu_release_owned(&owned, 0);
		switch_gpu_backing_release(backing);
		switch_gpu_surface_finish(descriptor);
		return adopted;
	}
	switch_gpu_backing_info info = {};
	if (backing)
		switch_gpu_backing_describe(backing, &info);
	uint32_t flags = SWITCH_GPU_BACKING_CPU_CONCURRENT;
	if (!owned && info.page_count &&
	    (tegra_gm20b_surface_tiled(info.pages[0]) ||
	     (info.layout.plane_count &&
	      info.layout.planes[0].layout == SWITCH_GPU_BACKING_BLOCK_LINEAR)))
		flags |= SWITCH_GPU_BACKING_CPU_NO_CACHE;
	const uint32_t index = options & 1 ? 0 : 1;
	const uint64_t bytes = uint64_t(info.page_count) << 12;
	const uint32_t requested = flags | access(index) |
				   (options & 2 ? uint32_t(SWITCH_GPU_BACKING_CPU_AVOID_SYNC) : 0u);
	int result = owned     ? switch_gpu_backing_cpu_transfer_owned(backing, &owned, requested)
		     : backing ? switch_gpu_backing_cpu_acquire(backing, 0, bytes, requested)
			       : 0;
	if (result) {
		if (owned)
			switch_gpu_backing_cpu_release_owned(&owned, 0);
		if (backing)
			switch_gpu_backing_release(backing);
		switch_gpu_surface_finish(descriptor);
		return result;
	}
	auto *client = static_cast<IOUserClient *>(opaque);
	IOLockLock(lock);
	if (!backing) {
		if (auto *entry = association(descriptor)) {
			backing = entry->backing;
			switch_gpu_backing_retain(backing);
			IOLockUnlock(lock);
			goto acquire_backing;
		}
	}
	auto *lease = *find(client, id);
	auto *item = free_pending;
	if (item)
		free_pending = item->next;
	else
		item = IONew(Pending, 1);
	if (!item)
		result = -12;
	if (client->isInactive() ||
	    (lease && (lease->backing != backing || lease->descriptor != descriptor)))
		result = -19;
	ClientWatch *register_watch = nullptr;
	if (!result && !lease) {
		lease = free_leases;
		if (lease)
			free_leases = lease->next;
		else
			lease = IONew(Lease, 1);
		ClientWatch *watch = lease ? find_watch_locked(client) : nullptr;
		if (lease && !watch) {
			watch = IONew(ClientWatch, 1);
			if (!watch) {
				lease->next = free_leases;
				free_leases = lease;
				lease = nullptr;
			} else {
				*watch = {watches, client, nullptr, 0, true};
				watches = watch;
				register_watch = watch;
				client->retain();
			}
		}
		if (!lease) {
			result = -12;
		} else {
			++watch->refs;
			*lease = {leases, client, backing, descriptor, bytes, id, flags, {}, {}};
			client->retain();
			if (backing)
				switch_gpu_backing_retain(backing);
			leases = lease;
			__atomic_store_n(&lease_count, lease_count + 1u, __ATOMIC_RELEASE);
		}
	}
	if (!result) {
		++lease->pending[index];
		*item = {pending, lease, current_thread(), index};
		pending = item;
	} else if (item) {
		item->next = free_pending;
		free_pending = item;
	}
	IOLockUnlock(lock);
	if (register_watch) {
		IONotifier *notifier = client->registerInterest(gIOGeneralInterest, client_interest,
								nullptr, nullptr);
		IONotifier *drop_notifier = nullptr;
		bool need_sweep = false;
		IOLockLock(lock);
		register_watch->registering = false;
		if (!register_watch->refs) {
			unlink_watch_locked(register_watch);
			IODelete(register_watch, ClientWatch, 1);
			drop_notifier = notifier;
		} else {
			register_watch->notifier = notifier;
			need_sweep = !notifier || client->isInactive();
		}
		IOLockUnlock(lock);
		if (drop_notifier)
			drop_notifier->remove();
		client->release();
		if (need_sweep)
			schedule_sweep();
	}
	if (result) {
		if (backing)
			switch_gpu_backing_cpu_cancel(backing, flags | access(index));
		switch_gpu_surface_finish(descriptor);
	}
	if (backing)
		switch_gpu_backing_release(backing);
	return result;
}

extern "C" void switch_gpu_surface_lock_did(void *client, uint32_t id, uint32_t options)
{
	if (!lock)
		return;
	const uint32_t index = options & 1 ? 0 : 1;
	IOLockLock(lock);
	if (auto *lease = *find(client, id);
	    lease && lease->pending[index] && take_pending(lease, index)) {
		--lease->pending[index];
		++lease->held[index];
	}
	IOLockUnlock(lock);
}

extern "C" void switch_gpu_surface_lock_failed(void *client, uint32_t id, uint32_t options)
{
	release_one(client, id, options, true);
}

extern "C" void switch_gpu_surface_unlock_will(void *client, uint32_t id, uint32_t options)
{
	if (!lock || (options & 1))
		return;
	IOLockLock(lock);
	const auto *lease = *find(client, id);
	switch_gpu_backing *backing =
		lease && lease->held[1] && !(lease->flags & SWITCH_GPU_BACKING_CPU_NO_CACHE)
			? lease->backing
			: nullptr;
	const uint64_t bytes = backing ? lease->bytes : 0;
	if (backing)
		switch_gpu_backing_retain(backing);
	IOLockUnlock(lock);
	if (backing) {
		switch_gpu_backing_cpu_publish(backing, 0, bytes);
		switch_gpu_backing_release(backing);
	}
}

extern "C" void switch_gpu_surface_unlock_did(void *client, uint32_t id, uint32_t options)
{
	release_one(client, id, options, false);
}

#if TGM_STATS
namespace
{
// Debug kernels (K18): the surfaces a submission cleaned whole while a CPU writer still held them, which their release
// cannot have cleaned. Lock-free: begin() notes them under the backing locks. The first eight surfaces keep a slot.
struct ConcurrentClean {
	uint32_t surface, cleans;
	uint64_t bytes;
};

ConcurrentClean concurrentCleans[8];
}  // namespace

extern "C" void switch_gpu_concurrent_clean_note(uint32_t surface, uint64_t bytes)
{
	for (ConcurrentClean &c : concurrentCleans) {
		uint32_t seen = __atomic_load_n(&c.surface, __ATOMIC_RELAXED);
		if (!seen && __atomic_compare_exchange_n(&c.surface, &seen, surface, false,
							 __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			seen = surface;
		if (seen != surface)
			continue;
		__atomic_add_fetch(&c.cleans, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&c.bytes, bytes, __ATOMIC_RELAXED);
		return;
	}
}

#ifdef KERNEL
// The window's notes, each surface with the processes that hold it locked now and how ("pid N, name": the user client's
// creator), then cleared; 0 when there were none
extern "C" unsigned long switch_gpu_concurrent_clean_line(char *out, unsigned long size)
{
	unsigned long at = 0;
	const auto advance = [&](int n) {
		if (n > 0)
			at = at + unsigned(n) < size ? at + unsigned(n) : size - 1;
	};
	out[0] = 0;
	for (ConcurrentClean &c : concurrentCleans) {
		const uint32_t surface = __atomic_load_n(&c.surface, __ATOMIC_RELAXED);
		const uint32_t cleans = __atomic_exchange_n(&c.cleans, 0u, __ATOMIC_RELAXED);
		const uint64_t bytes = __atomic_exchange_n(&c.bytes, 0ull, __ATOMIC_RELAXED);
		if (!surface || !cleans)
			continue;
		advance(snprintf(out + at, size - at,
				 "%sIOSurface %u: %u cleans, %llu KiB, locked by", at ? "; " : "",
				 surface, cleans, static_cast<unsigned long long>(bytes >> 10)));
		IOLockLock(lock);
		for (Lease *l = leases; l; l = l->next) {
			if (l->id != surface ||
			    !(l->held[0] + l->held[1] + l->pending[0] + l->pending[1]))
				continue;
			OSObject *creator =
				l->client ? l->client->copyProperty(kIOUserClientCreatorKey)
					  : nullptr;
			const OSString *name = OSDynamicCast(OSString, creator);
			advance(snprintf(out + at, size - at, " [%s: %u writing, %u reading]",
					 name ? name->getCStringNoCopy() : "?",
					 l->held[1] + l->pending[1], l->held[0] + l->pending[0]));
			OSSafeReleaseNULL(creator);
		}
		IOLockUnlock(lock);
	}
	return at;
}
#endif
#endif
