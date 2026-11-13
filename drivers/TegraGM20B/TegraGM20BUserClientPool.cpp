// SPDX-License-Identifier: MIT
// The pool BOs' orphans, the spare-page pool and its zeroing, and the owner counters.
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
}  // namespace tgm_uc

namespace
{
constexpr uint64_t kSpareCapBytes = 96ULL << 20, kLearnMaxBytes = 16ULL << 20;

struct WarmClass {
	uint64_t bytes;
	uint16_t ready;
};

constexpr WarmClass kWarm[] = {
	{64ULL << 10, 8}, {4ULL << 20, 2}, {8ULL << 20, 1}, {16ULL << 20, 2}};
}  // namespace

namespace tgm_uc
{

uint64_t classBytes(uint32_t c)
{
	return c < 5 ? 4096ULL << c : ((64ULL << 10) << ((c - 5) / 4)) * (5 + (c - 5) % 4) / 4;
}

// The smallest class of at least `bytes`; kClasses past the largest.
uint32_t classUp(uint64_t bytes)
{
	uint32_t c = 0;
	while (c < kClasses && classBytes(c) < bytes)
		c++;
	return c;
}
}  // namespace tgm_uc

namespace
{

SparePool *gSpares;
// XNU's vm_pressure_level_t: 0 normal, 1 warning, 2 urgent, 4 critical.
extern "C" int memorystatus_vm_pressure_level;

bool memoryPressure()
{
	return __atomic_load_n(&memorystatus_vm_pressure_level, __ATOMIC_RELAXED) != 0;
}

// The class a spare of `bytes` serves: the largest class not past it.
uint32_t spareClass(uint64_t bytes)
{
	const uint32_t c = classUp(bytes);
	return c < kClasses && classBytes(c) == bytes ? c : c - 1;
}

void spareFree(Spare *s)
{
	switch_gpu_cached_mapping_drop(&s->mapping);
	s->bmd->complete();
	s->bmd->release();
	IODelete(s->pas, uint64_t, s->bytes >> 12);
	IODelete(s, Spare, 1);
}
}  // namespace

namespace tgm_uc
{

// New VM pages contain CPU-authored zeros; publish them before exposing any GPU alias.
void cleanZeros(void *va, uint64_t bytes)
{
	switch_dcache_clean_invalidate(va, bytes);
}
}  // namespace tgm_uc

namespace
{

// The page list of a wired kernel buffer.
bool kernelPages(const void *va, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes; off += 4096) {
		const uint64_t pa = ml_vtophys(reinterpret_cast<vm_offset_t>(va) + off);
		if (!pa || (pa & 0xfff) || pa + 4096 > (1ULL << 34))
			return false;
		pas[off >> 12] = pa;
	}
	return true;
}

// A wired, zeroed, clean cached BO of `bytes` (what BoAlloc's own path makes), or nullptr.
Spare *spareMake(uint64_t bytes)
{
	Spare *s = IONew(Spare, 1);
	uint64_t *pas = IONew(uint64_t, bytes >> 12);
	IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task,
		static_cast<IOOptionBits>(kIODirectionInOut) |
			static_cast<IOOptionBits>(kIOMemoryMapperNone),
		bytes, 0x3ffffffffULL);
	if (s && pas && bmd && bmd->prepare() == kIOReturnSuccess) {
		// KMA_ZERO zero-filled it through the cacheable aperture: clean the zeros out
		cleanZeros(bmd->getBytesNoCopy(), bytes);
		if (kernelPages(bmd->getBytesNoCopy(), bytes, pas)) {
			*s = {nullptr, bmd, pas, bytes, nullptr};
			return s;
		}
		bmd->complete();
	}
	if (bmd)
		bmd->release();
	if (pas)
		IODelete(pas, uint64_t, bytes >> 12);
	if (s)
		IODelete(s, Spare, 1);
	return nullptr;
}

void spareZeroed(void *context, int result)
{
	auto *s = static_cast<Spare *>(context);
	SparePool *p = gSpares;
	const uint32_t c = spareClass(s->bytes);
	IOLockLock(p->lock);
	--p->zeroing[c];
	p->cleaningBytes -= s->bytes;
	const bool keep = !result && !memoryPressure();
	if (keep) {
		s->next = p->ready[c];
		p->ready[c] = s;
		++p->count[c];
	} else {
		p->clean -= s->bytes;
	}
	IOLockUnlock(p->lock);
	if (!keep)
		spareFree(s);
	thread_call_enter(p->call);
}

void spareWork(thread_call_param_t param, thread_call_param_t)
{
	SparePool *p = static_cast<SparePool *>(param);
	for (;;) {
		IOLockLock(p->lock);
		if (PoolZero *z = p->poolDirty) {  // first: pool pages are the scarcer
			p->poolDirty = z->next;
			IOLockUnlock(p->lock);
			tgm_pool_free_zeroed(z->va, z->bytes, z->pa);
			IOLockLock(p->lock);
			p->poolZeroing -= z->bytes;
			IOLockUnlock(p->lock);
			IODelete(z, PoolZero, 1);
			continue;
		}
		const bool pressure = memoryPressure();
		if (Spare *s = p->dirty) {
			p->dirty = s->next;
			p->dirtyBytes -= s->bytes;
			const bool keep = !pressure && p->clean + s->bytes <= kSpareCapBytes;
			if (keep) {
				p->clean += s->bytes;
				p->cleaningBytes += s->bytes;
				++p->zeroing[spareClass(s->bytes)];
			}
			IOLockUnlock(p->lock);
			if (!keep) {
				spareFree(
					s);  // past the cap or under pressure: back to the VM, unzeroed
				continue;
			}
			const int result = switch_gpu_cached_zero(s->pas, uint32_t(s->bytes >> 12),
								  s->bmd->getBytesNoCopy(),
								  &s->mapping, spareZeroed, s);
			if (result == -16) {
				// every scrub slot busy: a retiring scrub's completion runs this again
				IOLockLock(p->lock);
#if TGM_STATS
				++p->zeroBusy;
#endif
				--p->zeroing[spareClass(s->bytes)];
				p->cleaningBytes -= s->bytes;
				p->clean -= s->bytes;
				s->next = p->dirty;
				p->dirty = s;
				p->dirtyBytes += s->bytes;
				IOLockUnlock(p->lock);
				return;
			}
			if (result) {
#if TGM_STATS
				__atomic_add_fetch(&p->zeroFailed, 1, __ATOMIC_RELAXED);
#endif
				spareZeroed(s, result);
			}
			continue;
		}
		if (pressure) {  // give every spare back; refill nothing while it lasts
			Spare *all = nullptr;
			for (uint32_t c = 0; c < kClasses; c++) {
				while (Spare *s = p->ready[c]) {
					p->ready[c] = s->next;
					s->next = all;
					all = s;
				}
				p->count[c] = 0;
			}
			p->clean = p->cleaningBytes;
			IOLockUnlock(p->lock);
			while (all) {
				Spare *next = all->next;
				spareFree(all);
				all = next;
			}
			return;
		}
		uint32_t c = 0;
		while (c < kClasses && (p->count[c] + p->zeroing[c] >= p->target[c] ||
					p->clean + classBytes(c) > kSpareCapBytes))
			c++;
		if (c == kClasses) {
			IOLockUnlock(p->lock);
			return;
		}
		p->clean += classBytes(c);
		IOLockUnlock(p->lock);
		Spare *s = spareMake(classBytes(c));
		IOLockLock(p->lock);
		if (!s) {  // the VM could not give it: try again at the next event, not now
			p->clean -= classBytes(c);
#if TGM_STATS
			++p->refillRefused;
#endif
			IOLockUnlock(p->lock);
			return;
		}
		s->next = p->ready[c];
		p->ready[c] = s;
		p->count[c]++;
		IOLockUnlock(p->lock);
	}
}
}  // namespace

namespace tgm_uc
{

SparePool *spares()
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	if (p)
		return p;
	SparePool *n = IONew(SparePool, 1);
	if (!n)
		return nullptr;
	bzero(n, sizeof(*n));
	for (const WarmClass &w : kWarm)
		n->target[classUp(w.bytes)] = w.ready;
	n->lock = IOLockAlloc();
	n->call =
		n->lock ? thread_call_allocate_with_options(spareWork, n, THREAD_CALL_PRIORITY_USER,
							    THREAD_CALL_OPTIONS_ONCE)
			: nullptr;
	if (!n->call) {
		if (n->lock)
			IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return nullptr;
	}
	if (!__atomic_compare_exchange_n(&gSpares, &p, n, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		thread_call_free(n->call);
		IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return p;
	}
	thread_call_enter(n->call);  // the warm classes, made before anyone waits for them
	return n;
}

// A ready spare for a cached BO of `bytes`: of its class, or of a larger one up to about twice its size; nullptr when
// none is ready, which marks the class to keep warm. Either way a class below its mark wakes the thread call.
Spare *spareTake(uint64_t bytes)
{
	SparePool *p = spares();
	const uint32_t c = classUp(bytes);
	if (!p || c >= kClasses)
		return nullptr;
	const uint32_t last = c + (c < 5 ? 1 : 4);
	IOLockLock(p->lock);
	Spare *s = nullptr;
	for (uint32_t d = c; d < kClasses && d <= last && !s; d++) {
		s = p->ready[d];
		if (s) {
			p->ready[d] = s->next;
			p->count[d]--;
			p->clean -= s->bytes;
		}
	}
	if (!s) {
		const switch_gpu_spare_miss why =
			switch_gpu_spare_miss_of(p->target[c], p->zeroing[c], memoryPressure());
		p->target[c] = switch_gpu_spare_depth_after_miss(
			p->target[c], classBytes(c) <= kLearnMaxBytes, why);
#if TGM_STATS
		++p->misses[why];
#endif
	}
	const bool kick = !memoryPressure() ? p->count[c] < p->target[c] : p->clean != 0;
	IOLockUnlock(p->lock);
	if (kick)
		thread_call_enter(p->call);
	return s;
}

// Debug kernels: a cached BoAlloc that no spare can serve by its flags (contiguous, write-combined).
void spareIneligible()
{
#if TGM_STATS
	if (SparePool *p = spares()) {
		IOLockLock(p->lock);
		++p->misses[SWITCH_GPU_SPARE_INELIGIBLE];
		IOLockUnlock(p->lock);
	}
#endif
}

// A freed cached BO (its prepared descriptor, which nothing else holds, with its `bytes` and page list) to the spares:
// true if taken, then the caller must not touch bmd, pas or its persistent mapping again.
bool spareKeep(IOBufferMemoryDescriptor *bmd, uint64_t *pas, uint64_t bytes,
	       switch_gpu_cached_mapping *mapping)
{
	SparePool *p = spares();
	Spare *s = p && bmd->getRetainCount() == 1 ? IONew(Spare, 1) : nullptr;
	if (!s)
		return false;
	*s = {nullptr, bmd, pas, bytes, mapping};
	IOLockLock(p->lock);
	s->next = p->dirty;
	p->dirty = s;
	p->dirtyBytes += bytes;
	IOLockUnlock(p->lock);
	thread_call_enter(p->call);
	return true;
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

extern "C" void switch_gpu_cached_retire(IOMemoryDescriptor *memory, uint64_t *pages,
					 uint32_t count, switch_gpu_cached_mapping *mapping)
{
	auto *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer && spareKeep(buffer, pages, uint64_t(count) << 12, mapping))
		return;
	switch_gpu_cached_mapping_drop(&mapping);
	memory->complete();
	memory->release();
	IODelete(pages, uint64_t, count);
}

extern "C" void tgm_spare_counts(uint64_t *kept_bytes, uint64_t *ready,
				 uint64_t *pool_zeroing_bytes)
{
	const SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	uint64_t n = 0;
	if (p) {
		IOLockLock(p->lock);
		for (const uint16_t count : p->count)
			n += count;
		*kept_bytes = p->clean + p->dirtyBytes;
		*pool_zeroing_bytes = p->poolZeroing;
		IOLockUnlock(p->lock);
	} else {
		*kept_bytes = *pool_zeroing_bytes = 0;
	}
	*ready = n;
}

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

// The window's BoAllocs by process ("name n (KiB, mean/max us, made m)"), then cleared; 0 when there were none.
extern "C" unsigned long tgm_owner_stats(char *out, unsigned long size)
{
#if TGM_STATS
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	unsigned long at = 0;
	if (!TGM_STATS || !p || !size)
		return 0;
	out[0] = 0;
	IOLockLock(p->lock);
	for (uint32_t i = 0; i < kOwners && p->owners[i].n; i++) {
		const OwnerStat &o = p->owners[i];
		const int n = snprintf(out + at, size - at,
				       "%s%s %llu (%llu KiB, %llu/%llu us, made %llu)",
				       at ? "; " : "", o.name, o.n, o.bytes >> 10,
				       o.ns / o.n / 1000, o.max_ns / 1000, o.made);
		if (n > 0)
			at = at + n < size ? at + n : size - 1;
	}
	uint32_t missed = p->zeroBusy + p->zeroFailed + p->refillRefused;
	for (const uint32_t m : p->misses)
		missed += m;
	const int n =
		missed ? snprintf(out + at, size - at,
				  "%sspare misses: first %u, zeroing %u, drained %u, pressure %u, "
				  "ineligible %u; copy-engine zero busy %u, failed %u; refill "
				  "refused %u",
				  at ? "; " : "", p->misses[SWITCH_GPU_SPARE_FIRST],
				  p->misses[SWITCH_GPU_SPARE_ZEROING],
				  p->misses[SWITCH_GPU_SPARE_DRAINED],
				  p->misses[SWITCH_GPU_SPARE_PRESSURE],
				  p->misses[SWITCH_GPU_SPARE_INELIGIBLE], p->zeroBusy,
				  p->zeroFailed, p->refillRefused)
		       : 0;
	if (n > 0)
		at = at + n < size ? at + n : size - 1;
	bzero(p->owners, sizeof(p->owners));
	bzero(p->misses, sizeof(p->misses));
	p->zeroBusy = p->zeroFailed = p->refillRefused = 0;
	IOLockUnlock(p->lock);
	return at;
#else
	(void)out;
	(void)size;
	return 0;
#endif
}
