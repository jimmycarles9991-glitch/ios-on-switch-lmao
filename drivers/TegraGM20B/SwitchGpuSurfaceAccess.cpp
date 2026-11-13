// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <kern/thread.h>
#include "SwitchGpuBacking.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BViews.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

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

IOLock *lock;
Lease *leases, *free_leases;
uint32_t lease_count;  // leases' length, read without the lock by switch_gpu_surface_sweep
Pending *pending, *free_pending;
Association *associations;

bool initialize()
{
	if (__atomic_load_n(&lock, __ATOMIC_ACQUIRE))
		return true;
	IOLock *made = IOLockAlloc(), *expected = nullptr;
	if (!made)
		return false;
	if (!__atomic_compare_exchange_n(&lock, &expected, made, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE))
		IOLockFree(made);
	return true;
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
	lease->client->release();
	if (lease->backing)
		switch_gpu_backing_release(lease->backing);
	IOLockLock(lock);
	lease->next = free_leases;
	free_leases = lease;
	IOLockUnlock(lock);
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
		__atomic_sub_fetch(&lease_count, 1u, __ATOMIC_RELEASE);
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
		__atomic_sub_fetch(&lease_count, 1u, __ATOMIC_RELEASE);
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
	if (!result && !lease) {
		lease = free_leases;
		if (lease)
			free_leases = lease->next;
		else
			lease = IONew(Lease, 1);
		if (!lease) {
			result = -12;
		} else {
			*lease = {leases, client, backing, descriptor, bytes, id, flags, {}, {}};
			client->retain();
			if (backing)
				switch_gpu_backing_retain(backing);
			leases = lease;
			__atomic_add_fetch(&lease_count, 1u, __ATOMIC_RELEASE);
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
