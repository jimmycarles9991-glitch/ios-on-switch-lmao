// SPDX-License-Identifier: MIT
#include <string.h>
#include "SwitchGpuBackingInternal.h"
#include "SwitchGpuDependencyTrace.h"
#include "SwitchGpuBackingMemory.h"

namespace
{
constexpr uint32_t kBuckets = 256;
IOLock *registry;
switch_gpu_backing *buckets[kBuckets];
switch_gpu_backing *retiring, **retiring_tail = &retiring;
switch_gpu_backing *expiry_first, *expiry_last;
uint32_t retirement_pending, retirement_count, scan_remaining;
uint64_t completion_generation, scan_generation, armed_deadline;
constexpr uint32_t kRetirementBatch = 64;
constexpr uint64_t kRetirementTimeoutNs = 10000000000ULL;
switch_gpu_backing_engine engines[SWITCH_GPU_ENGINE_COUNT];
uint32_t engine_ready[SWITCH_GPU_ENGINE_COUNT];
switch_gpu_backing_visibility visibility[SWITCH_GPU_ENGINE_COUNT];
uint32_t visibility_ready[SWITCH_GPU_ENGINE_COUNT];
switch_gpu_backing_filter_status filter_status[SWITCH_GPU_ENGINE_COUNT];
switch_gpu_backing_semantics semantics[SWITCH_GPU_ENGINE_COUNT];
uint32_t semantics_ready[SWITCH_GPU_ENGINE_COUNT];
uint64_t next_id, next_generation;

IOLock *registry_lock()
{
	IOLock *lock = __atomic_load_n(&registry, __ATOMIC_ACQUIRE);
	if (lock)
		return lock;
	IOLock *made = IOLockAlloc();
	if (!made)
		return nullptr;
	if (!__atomic_compare_exchange_n(&registry, &lock, made, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE)) {
		IOLockFree(made);
		return lock;
	}
	return made;
}

uint32_t bucket(uint64_t pa)
{
	return uint32_t((pa >> 12) ^ (pa >> 20)) & (kBuckets - 1);
}

switch_gpu_backing_pages *copy_pages(const uint64_t *pages, uint32_t count)
{
	if (!pages || !count)
		return nullptr;
	for (uint32_t i = 0; i < count; ++i)
		if (!pages[i] || (pages[i] & 4095))
			return nullptr;
	auto *copy = IONew(switch_gpu_backing_pages, 1);
	if (!copy)
		return nullptr;
	copy->older = nullptr;
	copy->count = count;
	copy->addresses = IONew(uint64_t, count);
	if (!copy->addresses) {
		IODelete(copy, switch_gpu_backing_pages, 1);
		return nullptr;
	}
	memcpy(copy->addresses, pages, uint64_t(count) * sizeof(*pages));
	return copy;
}

void free_pages(switch_gpu_backing_pages *pages)
{
	while (pages) {
		auto *older = pages->older;
		IODelete(pages->addresses, uint64_t, pages->count);
		IODelete(pages, switch_gpu_backing_pages, 1);
		pages = older;
	}
}

uint64_t next_deadline()
{
	const uint64_t now = switch_gpu_backing_time_ns();
	return now > UINT64_MAX - kRetirementTimeoutNs ? UINT64_MAX : now + kRetirementTimeoutNs;
}

void arm_expiry_locked()
{
	const uint64_t deadline = expiry_first ? expiry_first->retirement_deadline : 0;
	if (deadline == armed_deadline)
		return;
	armed_deadline = deadline;
	switch_gpu_backing_arm_expiry(deadline);
}

void expiry_remove(switch_gpu_backing *backing)
{
	if (backing->expiry_prev)
		backing->expiry_prev->expiry_next = backing->expiry_next;
	else
		expiry_first = backing->expiry_next;
	if (backing->expiry_next)
		backing->expiry_next->expiry_prev = backing->expiry_prev;
	else
		expiry_last = backing->expiry_prev;
	backing->expiry_next = backing->expiry_prev = nullptr;
}

void expiry_append(switch_gpu_backing *backing)
{
	backing->retirement_deadline = next_deadline();
	backing->expiry_prev = expiry_last;
	backing->expiry_next = nullptr;
	if (expiry_last)
		expiry_last->expiry_next = backing;
	else
		expiry_first = backing;
	expiry_last = backing;
}

void retire_append(switch_gpu_backing *backing)
{
	backing->retiring_next = nullptr;
	*retiring_tail = backing;
	retiring_tail = &backing->retiring_next;
}

switch_gpu_backing *retire_pop()
{
	auto *backing = retiring;
	retiring = backing->retiring_next;
	if (!retiring)
		retiring_tail = &retiring;
	backing->retiring_next = nullptr;
	return backing;
}

// registry held: what is found again by pages or surface is no longer private to one connection
bool reached_locked(switch_gpu_backing *backing)
{
	if (__atomic_load_n(&backing->shared, __ATOMIC_ACQUIRE) &&
	    !__atomic_load_n(&backing->set, __ATOMIC_ACQUIRE))
		return false;
	IOLockLock(backing->lock);
	__atomic_store_n(&backing->shared, true, __ATOMIC_RELEASE);
	const bool member = backing->set != nullptr;
	IOLockUnlock(backing->lock);
	return member;
}

void destroy(switch_gpu_backing *backing)
{
	switch_gpu_backing_writer_free_locked(backing);
	if (backing->host_acquire.engine)
		switch_gpu_backing_drop_token(backing->host_acquire);
	for (uint32_t i = 0; i < backing->access_count; ++i)
		switch_gpu_backing_drop_token(backing->accesses[i].token);
	IODelete(backing->accesses, switch_gpu_backing_access, backing->access_capacity);
	IODelete(backing->cpu_current, uint64_t, backing->cpu_current_pages);
	if (backing->gpu_memory)
		backing->release_gpu_memory(backing->gpu_memory);
	if (backing->owner && backing->release_owner)
		backing->release_owner(backing->owner);
	free_pages(backing->pages);
	IOLockFree(backing->lock);
	IODelete(backing, switch_gpu_backing, 1);
}
}  // namespace

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *pages, uint32_t count,
							   uint32_t surface_id, void *owner,
							   void (*release_owner)(void *))
{
	if ((owner != nullptr) != (release_owner != nullptr))
		return nullptr;
	auto *copy = copy_pages(pages, count);
	IOLock *lock = registry_lock();
	if (!copy || !lock) {
		free_pages(copy);
		return nullptr;
	}
	auto *made = IONew(switch_gpu_backing, 1);
	if (!made) {
		free_pages(copy);
		return nullptr;
	}
	bzero(made, sizeof(*made));
	made->lock = IOLockAlloc();
	if (!made->lock) {
		free_pages(copy);
		IODelete(made, switch_gpu_backing, 1);
		return nullptr;
	}
	IOLockLock(lock);
	if (switch_gpu_backing_prepare_retirement()) {
		IOLockUnlock(lock);
		free_pages(copy);
		IOLockFree(made->lock);
		IODelete(made, switch_gpu_backing, 1);
		return nullptr;
	}
	switch_gpu_backing *found = buckets[bucket(pages[0])];
	while (found && found->pages->addresses[0] != pages[0])
		found = found->next;
	if (found) {
		IOLockLock(found->lock);
		const uint32_t common = count < found->pages->count ? count : found->pages->count;
		if (memcmp(pages, found->pages->addresses, uint64_t(common) * sizeof(*pages)) ||
		    (count > found->pages->count &&
		     (found->gpu_memory_flags & SWITCH_GPU_BACKING_MEMORY_FIXED)) ||
		    (count > found->pages->count && found->owner && !owner) ||
		    (count > found->pages->count &&
		     (found->cpu_readers || found->cpu_writers || found->gpu_admission_readers ||
		      found->gpu_admission_writers)) ||
		    (surface_id && found->surface_id && surface_id != found->surface_id)) {
			IOLockUnlock(found->lock);
			IOLockUnlock(lock);
			free_pages(copy);
			IOLockFree(made->lock);
			IODelete(made, switch_gpu_backing, 1);
			return nullptr;
		}
		void *old_owner = nullptr;
		void (*old_release)(void *) = nullptr;
		if (count > found->pages->count) {
			if (found->initial_clean)
				found->clean_start = uint64_t(found->pages->count) << 12;
			copy->older = found->pages;
			found->pages = copy;
			copy = nullptr;
			found->generation = ++next_generation;
			const bool cpu_current = found->cpu_current_content == found->content;
			++found->content;
			if (cpu_current)
				found->cpu_current_content = found->content;
			++found->host_epoch;
			found->dirty |= SWITCH_GPU_BACKING_CPU_DIRTY;
			found->initial_clean = false;
			old_owner = found->owner;
			old_release = found->release_owner;
			found->owner = owner;
			found->release_owner = release_owner;
			owner = nullptr;
		} else if (!found->owner && owner && count == found->pages->count) {
			found->owner = owner;
			found->release_owner = release_owner;
			owner = nullptr;
		}
		if (surface_id)
			found->surface_id = surface_id;
		// A retiring backing found again is its pages handed out anew, not shared.
		if (!found->retiring)
			__atomic_store_n(&found->shared, true, __ATOMIC_RELEASE);
		++found->refs;
		const bool member = found->set != nullptr;
		IOLockUnlock(found->lock);
		IOLockUnlock(lock);
		if (member)
			switch_gpu_set_reached(found, SWITCH_GPU_SET_CAUSE_SHARED);
		if (old_owner && old_release)
			old_release(old_owner);
		if (owner)
			release_owner(owner);
		free_pages(copy);
		IOLockFree(made->lock);
		IODelete(made, switch_gpu_backing, 1);
		return found;
	}
	made->id = ++next_id;
	made->generation = ++next_generation;
	made->refs = 1;
	made->pages = copy;
	made->surface_id = surface_id;
	made->owner = owner;
	made->release_owner = release_owner;
	made->dirty = SWITCH_GPU_BACKING_CPU_DIRTY;
	made->host_epoch = 1;
	made->next = buckets[bucket(pages[0])];
	buckets[bucket(pages[0])] = made;
	IOLockUnlock(lock);
	return made;
}

extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t first_page)
{
	IOLock *lock = registry_lock();
	if (!lock)
		return nullptr;
	IOLockLock(lock);
	auto *found = buckets[bucket(first_page)];
	while (found && found->pages->addresses[0] != first_page)
		found = found->next;
	const bool member = found && reached_locked(found);
	if (found)
		++found->refs;
	IOLockUnlock(lock);
	if (member)
		switch_gpu_set_reached(found, SWITCH_GPU_SET_CAUSE_SHARED);
	return found;
}

extern "C" switch_gpu_backing *switch_gpu_backing_find_surface(uint32_t surface_id)
{
	IOLock *lock = registry_lock();
	if (!surface_id || !lock)
		return nullptr;
	IOLockLock(lock);
	switch_gpu_backing *found = nullptr;
	for (uint32_t i = 0; i < kBuckets; ++i)
		for (auto *at = buckets[i]; at; at = at->next)
			if (at->surface_id == surface_id &&
			    (!found || at->pages->count > found->pages->count))
				found = at;
	const bool member = found && reached_locked(found);
	if (found)
		++found->refs;
	IOLockUnlock(lock);
	if (member)
		switch_gpu_set_reached(found, SWITCH_GPU_SET_CAUSE_SHARED);
	return found;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
	if (!backing)
		return;
	IOLockLock(registry);
	++backing->refs;
	IOLockUnlock(registry);
}

extern "C" void switch_gpu_backing_scanout(switch_gpu_backing *backing, int add)
{
	if (!backing)
		return;
	if (add)
		__atomic_add_fetch(&backing->scanout_holds, 1u, __ATOMIC_ACQ_REL);
	else
		__atomic_sub_fetch(&backing->scanout_holds, 1u, __ATOMIC_ACQ_REL);
}

extern "C" int switch_gpu_backing_scanned(const switch_gpu_backing *backing)
{
	return backing && __atomic_load_n(&backing->scanout_holds, __ATOMIC_ACQUIRE) != 0;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
	if (!backing)
		return;
	IOLockLock(registry);
	const bool last = --backing->refs == 0;
	if (last) {
		backing->refs = 1;
		backing->retiring = true;
		retire_append(backing);
		expiry_append(backing);
		++retirement_count;
		__atomic_add_fetch(&completion_generation, 1u, __ATOMIC_RELEASE);
		__atomic_store_n(&retirement_pending, 1u, __ATOMIC_RELEASE);
		arm_expiry_locked();
	}
	IOLockUnlock(registry);
	if (last)
		switch_gpu_backing_schedule_retirement();
}

extern "C" void switch_gpu_bridge_completed(void) __attribute__((weak));
extern "C" void switch_gpu_events_completed(void) __attribute__((weak));
extern "C" void switch_gpu_copy_completed(void) __attribute__((weak));

extern "C" void switch_gpu_backing_completed(void)
{
	if (switch_gpu_bridge_completed)
		switch_gpu_bridge_completed();
	if (switch_gpu_events_completed)
		switch_gpu_events_completed();
	if (switch_gpu_copy_completed)
		switch_gpu_copy_completed();
	if (__atomic_load_n(&retirement_pending, __ATOMIC_ACQUIRE)) {
		__atomic_add_fetch(&completion_generation, 1u, __ATOMIC_RELEASE);
		switch_gpu_backing_schedule_retirement();
	}
}

extern "C" void switch_gpu_backing_retire(void)
{
	if (!registry)
		return;
	switch_gpu_backing *ready = nullptr;
	IOLockLock(registry);
	if (!scan_remaining) {
		scan_remaining = retirement_count;
		scan_generation = __atomic_load_n(&completion_generation, __ATOMIC_ACQUIRE);
	}
	for (uint32_t scanned = 0; retiring && scan_remaining && scanned < kRetirementBatch;
	     ++scanned) {
		auto *backing = retire_pop();
		--scan_remaining;
		IOLockLock(backing->lock);
		bool pending = false;
		for (uint32_t i = 0; backing->refs == 1 && i < backing->access_count; ++i)
			pending |=
				switch_gpu_backing_token_status(&backing->accesses[i].token) == 0;
		if (pending) {
			retire_append(backing);
			IOLockUnlock(backing->lock);
			continue;
		}
		--retirement_count;
		expiry_remove(backing);
		backing->retiring = false;
		if (!--backing->refs) {
			auto **entry = &buckets[bucket(backing->pages->addresses[0])];
			while (*entry && *entry != backing)
				entry = &(*entry)->next;
			if (*entry)
				*entry = backing->next;
			backing->retiring_next = ready;
			ready = backing;
		}
		IOLockUnlock(backing->lock);
	}
	__atomic_store_n(&retirement_pending, retiring != nullptr, __ATOMIC_RELEASE);
	const bool again = retiring && (scan_remaining ||
					scan_generation != __atomic_load_n(&completion_generation,
									   __ATOMIC_ACQUIRE));
	arm_expiry_locked();
	IOLockUnlock(registry);
	while (ready) {
		auto *next = ready->retiring_next;
		destroy(ready);
		ready = next;
	}
	if (again)
		switch_gpu_backing_schedule_retirement();
}

extern "C" void switch_gpu_backing_expire(void)
{
	if (!registry)
		return;
	switch_gpu_backing_token expired = {};
	const uint64_t now = switch_gpu_backing_time_ns();
	IOLockLock(registry);
	armed_deadline = 0;
	for (uint32_t scanned = 0; expiry_first && !expired.engine && scanned < kRetirementBatch;
	     ++scanned) {
		auto *backing = expiry_first;
		if (backing->retirement_deadline > now)
			break;
		IOLockLock(backing->lock);
		expiry_remove(backing);
		expiry_append(backing);
		for (uint32_t i = 0; backing->refs == 1 && i < backing->access_count; ++i) {
			const auto &token = backing->accesses[i].token;
			if (switch_gpu_backing_token_status(&token) != 0)
				continue;
			const auto *ops = switch_gpu_backing_engine_ops(token.engine);
			if (!ops->retain_token(&token))
				expired = token;
			break;
		}
		IOLockUnlock(backing->lock);
	}
	arm_expiry_locked();
	IOLockUnlock(registry);
	// Recovery may sleep; its separate call never delays reclamation on other engines.
	if (expired.engine) {
		(void)switch_gpu_backing_token_wait(&expired);
		switch_gpu_backing_drop_token(expired);
	}
	switch_gpu_backing_completed();
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing,
					    switch_gpu_backing_info *info)
{
	if (!backing || !info)
		return;
	IOLockLock(backing->lock);
	*info = {backing->id,
		 backing->generation,
		 backing->content,
		 backing->pages->addresses,
		 backing->pages->count,
		 backing->surface_id,
		 backing->cpu_readers + backing->cpu_writers,
		 backing->dirty,
		 backing->layout};
	IOLockUnlock(backing->lock);
}

extern "C" int switch_gpu_backing_set_layout(switch_gpu_backing *backing,
					     const switch_gpu_backing_layout *layout)
{
	if (!backing || !layout || layout->plane_count > SWITCH_GPU_BACKING_MAX_PLANES)
		return -22;
	if (switch_gpu_backing_set(backing))
		switch_gpu_set_reached(backing, SWITCH_GPU_SET_CAUSE_LAYOUT);
	IOLockLock(backing->lock);
	int result = 0;
	const uint64_t bytes = uint64_t(backing->pages->count) << 12;
	for (uint32_t i = 0; i < layout->plane_count; ++i) {
		const auto &plane = layout->planes[i];
		if (!plane.width || !plane.height || !plane.pitch || !plane.bytes_per_element ||
		    plane.offset > bytes || plane.bytes > bytes - plane.offset ||
		    plane.layout > SWITCH_GPU_BACKING_BLOCK_LINEAR || plane.block_height_log2 > 5 ||
		    uint64_t(plane.width) * plane.bytes_per_element > plane.pitch ||
		    uint64_t(plane.pitch) * plane.height > plane.bytes ||
		    (plane.layout == SWITCH_GPU_BACKING_BLOCK_LINEAR &&
		     ((plane.pitch & 63) ||
		      uint64_t(plane.pitch) * ((uint64_t(plane.height) +
						(8u << plane.block_height_log2) - 1) &
					       ~uint64_t((8u << plane.block_height_log2) - 1)) >
			      plane.bytes)))
			result = -22;
	}
	if (!result && memcmp(layout, &backing->layout, sizeof(*layout))) {
		switch_gpu_backing_prune_locked(backing);
		if ((backing->access_count || backing->cpu_readers || backing->cpu_writers ||
		     backing->gpu_admission_readers || backing->gpu_admission_writers))
			result = -16;
		if (!result)
			backing->layout = *layout;
	}
	IOLockUnlock(backing->lock);
	return result;
}

const switch_gpu_backing_engine *switch_gpu_backing_engine_ops(uint32_t engine)
{
	return engine < SWITCH_GPU_ENGINE_COUNT &&
			       __atomic_load_n(&engine_ready[engine], __ATOMIC_ACQUIRE)
		       ? &engines[engine]
		       : nullptr;
}

extern "C" int switch_gpu_backing_register_engine(uint32_t engine,
						  const switch_gpu_backing_engine *operations)
{
	if (!engine || engine >= SWITCH_GPU_ENGINE_COUNT || !operations || !operations->status ||
	    !operations->wait || !operations->retain_token || !operations->release_token ||
	    (operations->value_bits != 32 && operations->value_bits != 64))
		return -22;
	IOLock *lock = registry_lock();
	if (!lock)
		return -12;
	IOLockLock(lock);
	const int result = engine_ready[engine] ? -16 : 0;
	if (!result) {
		engines[engine] = *operations;
		__atomic_store_n(&engine_ready[engine], 1u, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *token)
{
	const auto *ops = token ? switch_gpu_backing_engine_ops(token->engine) : nullptr;
	return ops ? ops->status(token) : -19;
}

extern "C" int switch_gpu_backing_token_wait(const switch_gpu_backing_token *token)
{
	SwitchGpuDependencyTrace dependency;
	const auto *ops = token ? switch_gpu_backing_engine_ops(token->engine) : nullptr;
	return ops ? ops->wait(token) : -19;
}

extern "C" int switch_gpu_backing_token_retain(const switch_gpu_backing_token *token)
{
	const auto *ops = token ? switch_gpu_backing_engine_ops(token->engine) : nullptr;
	return ops ? ops->retain_token(token) : -19;
}

extern "C" void switch_gpu_backing_token_release(const switch_gpu_backing_token *token)
{
	if (const auto *ops = token ? switch_gpu_backing_engine_ops(token->engine) : nullptr)
		ops->release_token(token);
}

extern "C" int switch_gpu_backing_register_filter_status(uint32_t engine,
							 switch_gpu_backing_filter_status status)
{
	if (!engine || engine >= SWITCH_GPU_ENGINE_COUNT || !status)
		return -22;
	switch_gpu_backing_filter_status expected = nullptr;
	return __atomic_compare_exchange_n(&filter_status[engine], &expected, status, false,
					   __ATOMIC_RELEASE, __ATOMIC_RELAXED)
		       ? 0
		       : -16;
}

extern "C" int switch_gpu_backing_token_filter_status(const switch_gpu_backing_token *token)
{
	const switch_gpu_backing_filter_status status =
		token && token->engine < SWITCH_GPU_ENGINE_COUNT
			? __atomic_load_n(&filter_status[token->engine], __ATOMIC_ACQUIRE)
			: nullptr;
	return status ? status(token) : 0;
}

extern "C" int
switch_gpu_backing_register_visibility(uint32_t engine,
				       const switch_gpu_backing_visibility *operations)
{
	if (!engine || engine >= SWITCH_GPU_ENGINE_COUNT || !operations || !operations->status ||
	    !operations->wait)
		return -22;
	IOLock *lock = registry_lock();
	if (!lock)
		return -12;
	IOLockLock(lock);
	const int result = visibility_ready[engine] ? -16 : 0;
	if (!result) {
		visibility[engine] = *operations;
		__atomic_store_n(&visibility_ready[engine], 1u, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *token)
{
	if (token && token->engine < SWITCH_GPU_ENGINE_COUNT &&
	    __atomic_load_n(&visibility_ready[token->engine], __ATOMIC_ACQUIRE))
		return visibility[token->engine].status(token);
	return switch_gpu_backing_token_status(token);
}

extern "C" int switch_gpu_backing_token_external_wait(const switch_gpu_backing_token *token)
{
	SwitchGpuDependencyTrace dependency;
	if (token && token->engine < SWITCH_GPU_ENGINE_COUNT &&
	    __atomic_load_n(&visibility_ready[token->engine], __ATOMIC_ACQUIRE))
		return visibility[token->engine].wait(token);
	return switch_gpu_backing_token_wait(token);
}

extern "C" int switch_gpu_backing_register_semantics(uint32_t engine,
						     const switch_gpu_backing_semantics *operations)
{
	if (!engine || engine >= SWITCH_GPU_ENGINE_COUNT || !operations || !operations->snapshot ||
	    !operations->physical_status || !operations->pending)
		return -22;
	IOLock *lock = registry_lock();
	if (!lock)
		return -12;
	IOLockLock(lock);
	const int result = semantics_ready[engine] ? -16 : 0;
	if (!result) {
		semantics[engine] = *operations;
		__atomic_store_n(&semantics_ready[engine], 1u, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_backing_has_semantics(void)
{
	for (uint32_t i = 1; i < SWITCH_GPU_ENGINE_COUNT; ++i)
		if (__atomic_load_n(&semantics_ready[i], __ATOMIC_ACQUIRE) &&
		    semantics[i].pending())
			return 1;
	return 0;
}

extern "C" int switch_gpu_backing_semantic_snapshot(const switch_gpu_backing_token *token,
						    switch_gpu_event_waits **waits)
{
	if (!token || !waits)
		return -22;
	*waits = nullptr;
	if (token->engine < SWITCH_GPU_ENGINE_COUNT &&
	    __atomic_load_n(&semantics_ready[token->engine], __ATOMIC_ACQUIRE))
		return semantics[token->engine].snapshot(token, waits);
	return 0;
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token,
							int external)
{
	if (token && token->engine < SWITCH_GPU_ENGINE_COUNT &&
	    __atomic_load_n(&semantics_ready[token->engine], __ATOMIC_ACQUIRE))
		return semantics[token->engine].physical_status(token, external);
	return external ? switch_gpu_backing_token_external_status(token)
			: switch_gpu_backing_token_status(token);
}
