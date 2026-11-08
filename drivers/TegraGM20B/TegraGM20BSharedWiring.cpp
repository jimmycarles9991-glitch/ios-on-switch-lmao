// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <string.h>
#include "TegraGM20BSharedWiring.h"

extern "C" {
uint64_t pmap_find_pa(pmap_t pmap, uint64_t va);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
pmap_t get_task_pmap(task_t task);
}

namespace
{
IOLock *gLock;
SharedWiring *gWirings;

IOLock *tableLock()
{
	IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
	if (lock)
		return lock;
	IOLock *fresh = IOLockAlloc();
	if (!fresh)
		return nullptr;
	if (!__atomic_compare_exchange_n(&gLock, &lock, fresh, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		IOLockFree(fresh);
		return lock;
	}
	return fresh;
}

bool same(const SharedWiring *w, task_t task, uint64_t address, uint64_t bytes, uint32_t surfaceId)
{
	return w->task == task && w->address == address && w->bytes == bytes &&
	       w->surfaceId == surfaceId;
}

// The range may have been unmapped and something else mapped at the same address since the wiring was made: the
// wired pages cannot be anyone else's while wired, so the task mapping them at both ends means it maps this wiring.
bool stillMapped(const SharedWiring *w)
{
	struct pmap *const pmap = get_task_pmap(w->task);
	const uint64_t last = w->bytes - 4096;
	return (pmap != nullptr) && pmap_find_pa(pmap, w->address) == w->pas[0] &&
	       pmap_find_pa(pmap, w->address + last) == w->pas[w->npages - 1];
}

void destroy(SharedWiring *w)
{
	tgm_mem_del(&w->mem);
	if (w->wired) {
		w->wired->complete();
		w->wired->release();
	}
	if (w->pas)
		IODelete(w->pas, uint64_t, w->npages);
	IODelete(w, SharedWiring, 1);
}
}  // namespace

SharedWiring *sharedWiringFind(task_t task, uint64_t address, uint64_t bytes, uint32_t surfaceId)
{
	IOLock *lock = tableLock();
	if (!lock || !surfaceId)
		return nullptr;
	IOLockLock(lock);
	SharedWiring *w = gWirings;
	while (w && !same(w, task, address, bytes, surfaceId))
		w = w->next;
	if (w && stillMapped(w)) {
		w->refs++;
	} else {
		w = nullptr;
	}
	IOLockUnlock(lock);
	return w;
}

SharedWiring *sharedWiringPublish(SharedWiring *made)
{
	IOLock *lock = tableLock();
	if (!lock) {
		made->refs = 1;
		return made;
	}
	IOLockLock(lock);
	SharedWiring *w = gWirings;
	while (w && !same(w, made->task, made->address, made->bytes, made->surfaceId))
		w = w->next;
	if (w && !memcmp(w->pas, made->pas, static_cast<size_t>(made->npages) * sizeof(uint64_t))) {
		w->refs++;
	} else {
		w = made;
		w->refs = 1;
		w->next = gWirings;
		gWirings = w;
	}
	IOLockUnlock(lock);
	if (w != made)
		destroy(made);
	return w;
}

void sharedWiringPut(SharedWiring *w)
{
	IOLock *lock = tableLock();
	bool last = true;
	if (lock) {
		IOLockLock(lock);
		last = ((--w->refs) == 0u);
		if (last) {
			for (SharedWiring **p = &gWirings; *p; p = &(*p)->next) {
				if (*p == w) {
					*p = w->next;
					break;
				}
			}
		}
		IOLockUnlock(lock);
	}
	if (last)
		destroy(w);
}
