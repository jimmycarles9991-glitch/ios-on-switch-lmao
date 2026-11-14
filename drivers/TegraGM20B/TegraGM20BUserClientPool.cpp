// SPDX-License-Identifier: MIT
// The pool BOs' orphans, a freed pool BO's zeroing, and the pool's breakdown.
#include "TegraGM20BUserClientInternal.h"

namespace tgm_uc
{

PoolOrphan *gPoolOrphans;
// Coherent BOs the pool did not serve (tgm_pool::FromSystem), since boot: count and bytes (the stats line's "system")
volatile uint64_t gSystemCoherent, gSystemCoherentBytes;
// Pool BOs of clients other than the compositor, and pool pages freed but still behind their view's release (orphans)
volatile uint64_t gClientPoolBytes, gOrphanBytes, gOrphans;
// Compressible BOs (pool chunks), every client's
volatile uint64_t gCompressibleBytes, gCompressibles;

void pushOrphan(PoolOrphan *orphan)
{
	orphan->next = __atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED);
	while (!__atomic_compare_exchange_n(&gPoolOrphans, &orphan->next, orphan, true,
					    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		;
}
}  // namespace tgm_uc

namespace
{

// A view only we hold is mapped nowhere (an IOMemoryMap holds its descriptor), and nothing can map it any more. But
// releasing it destroys its device pager's VM object, which first waits (uninterruptibly, vm_object_destroy ->
// vm_object_paging_wait) for every UPL on that object to be committed, and the retain count does not show those: an import
// of the view's mapping (BoImport of the WC pages, m2v's twin of a raw buffer) holds one until its own BO is released,
// maybe on another connection, maybe later on the very thread that freed the view. So no client thread releases a view:
//  - a dying task closes its connections one after the other on one thread; if the raw connection's view goes first,
//    the twin's connection, which would complete that UPL, never gets closed (the task's exit hangs in
//    vm_object_destroy under AGXDeviceUserClient::releaseBo <- clientClose until the watchdog panics);
//  - the same wait on a live task's thread: a BoFree of a raw buffer no task maps any more, or a BoAlloc that releases
//    orphans inline, with the buffer's twin import still live: the compositor's thread waits for an import only it
//    would free.
// Here a run that meets such a view waits for its import to go, with the rest of the batch it took behind it; orphans
// freed meanwhile get a run of their own (the call is not ONCE: XNU thread_call.c _pending_call_enqueue queues a running
// non-ONCE call again, and the kernel group is TCG_PARALLEL, so another thread runs it). A view whose twin a long-lived
// process keeps holds its batch's pages until that import is freed or the process exits.
void releaseOrphans()
{
	for (PoolOrphan *orphan = __atomic_exchange_n(&gPoolOrphans, nullptr, __ATOMIC_ACQUIRE),
			*next = nullptr;
	     orphan; orphan = next) {
		next = orphan->next;
		if (orphan->view->getRetainCount() > 1) {
			pushOrphan(orphan);
			continue;
		}
		orphan->view->release();
		__atomic_sub_fetch(&gOrphanBytes, orphan->bytes, __ATOMIC_RELAXED);
		__atomic_sub_fetch(&gOrphans, 1, __ATOMIC_RELAXED);
		if (!(orphan->zero && poolZeroLater(orphan->va, orphan->pa, orphan->bytes)))
			tgm_pool_free(orphan->va, orphan->bytes, orphan->pa);
		IODelete(orphan, PoolOrphan, 1);
	}
}

thread_call_t gOrphanCall;

void orphanCallMain(thread_call_param_t, thread_call_param_t)
{
	releaseOrphans();
}
}  // namespace

namespace tgm_uc
{

// releaseOrphans, on the thread call. Never blocks: callable with the connection's lock held.
void scheduleOrphanRelease()
{
	if (!__atomic_load_n(&gOrphanCall, __ATOMIC_ACQUIRE)) {
		// not ONCE: entered while a run is blocked on one view, it runs again (concurrently) for the others
		thread_call_t fresh = thread_call_allocate_with_options(
			orphanCallMain, nullptr, THREAD_CALL_PRIORITY_KERNEL, 0);
		thread_call_t expected = nullptr;
		if (fresh && !__atomic_compare_exchange_n(&gOrphanCall, &expected, fresh, false,
							  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			thread_call_free(fresh);
	}
	thread_call_t call = __atomic_load_n(&gOrphanCall, __ATOMIC_ACQUIRE);
	if (call)
		thread_call_enter(call);
}

// A freed pool BO's range (no task maps it) to be zeroed and given back clean: by the GPU's copy engine (gpu_scrub.c "pool
// scrub", no CPU time), else by the thread call: true if taken.
bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)
{
	const int scrubbed = tgm_pool_scrub(pa, bytes);
	if (!scrubbed)
		return true;
	if (scrubbed == -16) {
		tgm_pool_free(va, bytes, pa);
		return true;
	}
	SparePool *p = spares();
	PoolZero *z = p ? IONew(PoolZero, 1) : nullptr;
	if (!z)
		return false;
	*z = {nullptr, va, pa, bytes};
	IOLockLock(p->lock);
	z->next = p->poolDirty;
	p->poolDirty = z;
	p->poolZeroing += bytes;
	IOLockUnlock(p->lock);
	thread_call_enter(p->call);
	return true;
}
}  // namespace tgm_uc

extern "C" void tgm_pool_breakdown(uint64_t *client_bytes, uint64_t *orphan_bytes,
				   uint64_t *orphans, uint64_t *compressible_bytes,
				   uint64_t *compressibles, uint64_t *system_bytes,
				   uint64_t *systems)
{
	*client_bytes = __atomic_load_n(&gClientPoolBytes, __ATOMIC_RELAXED);
	*orphan_bytes = __atomic_load_n(&gOrphanBytes, __ATOMIC_RELAXED);
	*orphans = __atomic_load_n(&gOrphans, __ATOMIC_RELAXED);
	*compressible_bytes = __atomic_load_n(&gCompressibleBytes, __ATOMIC_RELAXED);
	*compressibles = __atomic_load_n(&gCompressibles, __ATOMIC_RELAXED);
	*system_bytes = __atomic_load_n(&gSystemCoherentBytes, __ATOMIC_RELAXED);
	*systems = __atomic_load_n(&gSystemCoherent, __ATOMIC_RELAXED);
}
