// SPDX-License-Identifier: MIT
// AGXDeviceUserClient: one connection = one GPU address space, implementing scratchpad/gpu/CONTRACT.md v1 over the wire
// records of TegraGM20BABI.h. Transport modelled on drivers/NativeAdreno/NativeAdrenoPublish.cpp: Apple user-client
// class name (sandbox profiles admit it), one open type, struct-in/struct-out methods with exact sizes, the same
// methods tunnelled through any admitted selector, and clientMemoryForType views cached per token so
// IOConnectUnmapMemory64 finds the same descriptor. GPU work goes through gpu.c (TegraGM20BGpu.h). Fence waits sleep
// until the GPU's nonstall interrupt wakes them; a fence found complete without that wake panics (no polled mode), and
// so does one that never completes.
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <mach/vm_region.h>
#include <stddef.h>
#include <string.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BExecSurfaces.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BLayout.h"
#include "TegraGM20BPoolPolicy.h"
#include "TegraGM20BViews.h"
#include "TegraGM20BPushScan.h"
#include "TegraGM20BSchedPolicy.h"
#include "TegraGM20BSharedWiring.h"
#include "shim/include/switch_tiled_cache.h"
using tgm_scan::PushCounts;
using tgm_scan::PushParse;
using tgm_scan::scanPush;
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

namespace
{
constexpr uint32_t kMaxChannels = 8;  // fence slots: 16 bytes each in the connection's fence page
constexpr uint64_t kMaxBoBytes = 256ULL << 20;
constexpr uint32_t kFenceToken = 0xfe000001u;
// Connections system-wide: 16, or 64 with every process on the GPU (gpu.c TGM_FENCE_USER_PAGES: one fence page each).
constexpr uint32_t kMaxConnections = 16, kMaxConnectionsAll = 64;
// GPU pool pages are outside the VM (arm_vm_init.c switch_ram_init), so the kernel reports them as device
// memory, and for a physical descriptor IOKit fixes the user mapping's memory type from that default when the descriptor
// is made: kIOMapWriteCombineCache at map time does not change it. Without this the client gets Device-nGnRE and its
// first unaligned memmove into a BO is an alignment fault (backboardd SIGBUS in Texture::replace_region).
constexpr IOOptionBits kPoolCacheMode = static_cast<IOOptionBits>(kIOWriteCombineCache)
					<< kIOMemoryBufferCacheShift;
volatile uint32_t gConnections;

extern "C" void proc_selfname(char *buf, int size);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" vm_offset_t ml_vtophys(vm_offset_t vaddr);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" task_t current_task(void);

// A cached BO's view up to this size is mapped into its task with every page entered at once (kIOMapPrefault: one
// vm_map_enter_mem_object_prefault over the wired page list), not one page fault per 4 KiB page as the task first touches
// it. NVK fills such BOs (64 KiB command, upload and stream memory; small descriptor and query pools); larger ones (the
// runtime's pooled blocks, mapped whole and touched only where it places CPU-visible resources) keep lazy faults.
constexpr uint64_t kPrefaultMaxBytes = 256ULL << 10;

// Freed pool BOs (see releaseBo): every one's view is released and its pages given back here, on a thread call, never on
// the freeing thread. A view a task still maps keeps its pages until the last such mapping is gone, so the task can never
// write pages the pool has handed out again (GPU page tables among them); IOKit keeps a task's mappings past its
// connection's close, until the task unmaps them or dies, so the list is global and any connection's BoAlloc and close
// kick the thread call to look at it again.
struct PoolOrphan {
	PoolOrphan *next;
	IOMemoryDescriptor *view;
	void *va;
	uint64_t pa, bytes;
	bool zero;  // a BO a client had: its pages are zeroed before they go back (poolZeroLater)
	// a coherent BO from ordinary pages (tgm_pool::FromSystem): its pages, released after the view instead of va's, to
	// the spares when a client had it (spareKeep), with their page list
	IOBufferMemoryDescriptor *backing;
	uint64_t *backingPas;
	uint64_t backingBytes;
};

PoolOrphan *gPoolOrphans;
// Coherent BOs the pool did not serve (tgm_pool::FromSystem), since boot: count and bytes (the stats line's "system")
volatile uint64_t gSystemCoherent, gSystemCoherentBytes;
// Pool BOs of clients other than the compositor, and pool pages freed but still behind their view's release (orphans)
volatile uint64_t gClientPoolBytes, gOrphanBytes, gOrphans;

void pushOrphan(PoolOrphan *orphan)
{
	orphan->next = __atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED);
	while (!__atomic_compare_exchange_n(&gPoolOrphans, &orphan->next, orphan, true,
					    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		;
}

bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes);
bool spareKeep(IOBufferMemoryDescriptor *bmd, uint64_t *pas, uint64_t bytes);

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
		if (orphan->va) {
			__atomic_sub_fetch(&gOrphanBytes, orphan->bytes, __ATOMIC_RELAXED);
			__atomic_sub_fetch(&gOrphans, 1, __ATOMIC_RELAXED);
		}
		if (orphan->backing) {
			// The view was its pages' only user mapping and import, so they go to the spares as a freed cached BO's do
			if (!(orphan->zero && spareKeep(orphan->backing, orphan->backingPas,
							orphan->backingBytes))) {
				orphan->backing->complete();
				orphan->backing->release();
				IODelete(orphan->backingPas, uint64_t, orphan->backingBytes >> 12);
			}
		} else if (!(orphan->zero &&
			     poolZeroLater(orphan->va, orphan->pa, orphan->bytes))) {
			tgm_pool_free(orphan->va, orphan->bytes, orphan->pa);
		}
		IODelete(orphan, PoolOrphan, 1);
	}
}

thread_call_t gOrphanCall;

void orphanCallMain(thread_call_param_t, thread_call_param_t)
{
	releaseOrphans();
}

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

#if TGM_STATS
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" vm_map_t get_task_map(task_t task);
extern "C" kern_return_t vm_map_region_recurse_64(vm_map_t map, uint64_t *address, uint64_t *size,
						  natural_t *depth, vm_region_submap_info_64_t info,
						  mach_msg_type_number_t *count);

// What `task` maps at `address`, innermost submap's region, for a refused import's line: why IOKit could not wire it
// (protections, sharing, the allocator's tag, the object) is in these.
void describeRegion(task_t task, uint64_t address, char *out, size_t size)
{
	uint64_t at = address;
	uint64_t bytes = 0;
	natural_t depth = 64;
	vm_region_submap_info_data_64_t info = {};
	mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
	const kern_return_t kr =
		vm_map_region_recurse_64(get_task_map(task), &at, &bytes, &depth, &info, &count);
	if (kr != KERN_SUCCESS || at > address) {
		snprintf(out, size, "; nothing mapped there (%d)", kr);
		return;
	}
	snprintf(out, size,
		 "; region %#llx+%#llx depth %u prot %d/%d share %d tag %u object %#x "
		 "ref %u shadow %u resident %u wired %u%s%s",
		 at, bytes, depth, info.protection, info.max_protection, info.share_mode,
		 info.user_tag, info.object_id, info.ref_count, info.shadow_depth,
		 info.pages_resident, info.user_wired_count,
		 info.external_pager ? " file-backed" : "", info.is_submap ? " submap" : "");
}
#endif

uint64_t nowNs()
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

// The GPU pool for clients other than the compositor. Every coherent BO comes from
// the pool (NVK's coherent event heap, Metal's Shared storage through m2v), and so do nvkm's own page tables and channels,
// unpoliced. Tiers, so the compositor is never starved and nobody else is refused while there is room:
//  - a BO of up to kPoolSmallBytes (what a device needs to exist: NVK's event heap, upload chunks) is refused only when it
//    would leave less than kPoolFloorBytes free: one more of the compositor's 16 MiB m2v blocks, and with it the room
//    nvkm's page tables and channels for new clients take;
//  - a larger one also leaves kPoolReserveBytes (one of the compositor's 16 MiB m2v blocks, and the raw path's ~7 MiB of
//    rings and tables should backboardd restart, with margin) free, and keeps the client within kClientPoolBytes of pool
//    BOs, a fairness share, not a limit on GPU memory (cached BOs have none).
// Was: any BO kept the 32 MiB, so at 97 of 128 MiB used every poster extension's first 4 KiB was refused and its device
// never opened (r7j). A refused BoAlloc is kIOReturnNoMemory, logged once per connection with the rule that refused it.
constexpr uint64_t kPoolSmallBytes = 1ULL << 20, kPoolFloorBytes = 16ULL << 20;
constexpr uint64_t kClientPoolBytes = 64ULL << 20, kPoolReserveBytes = 32ULL << 20;

uint64_t clientPoolBytes()
{
	return kClientPoolBytes;
}

uint64_t poolReserveBytes()
{
	return kPoolReserveBytes;
}

bool poolZeroNow();

// Why a pool BO of `bytes` stays out of the pool while its client holds `held` (the tiers above; the compositor only the
// floor), or nullptr: it then comes from system memory (tgm_pool::coherentSource). Freed ranges the thread call has not
// zeroed yet are room too: when short, they are zeroed now rather than the BO sent elsewhere.
const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)
{
	// (the compositor zeroes pending ranges on its thread only where the floor would refuse it)
	uint64_t avail = tgm_pool_avail();
	if (avail < bytes + (compositor ? kPoolFloorBytes : poolReserveBytes()) && poolZeroNow())
		avail = tgm_pool_avail();
	return tgm_pool::refusal(
		bytes, held, avail,
		{kPoolSmallBytes, kPoolFloorBytes, poolReserveBytes(), clientPoolBytes()},
		compositor);
}

// BoAlloc without waiting: spare BOs. Made on the caller's thread, a cached BO is allocated and zero-filled by the VM
// and then cleaned to DRAM line by line (0.34 ms per MiB): 18-26 ms for backboardd's 16 MiB m2v blocks, which Core
// Animation makes on its render thread; a pool BO's memset takes 4.5 ms for 16 MiB. So:
//  - Cached BOs come from spares in size classes (4-64 KiB by powers of two, then quarter steps to 256 MiB, so a BO
//    wastes at most a quarter of its spare): a BoAlloc takes the smallest ready spare of its class or up to twice its size
//    (classUp, spareTake) and maps it: no allocation, no zeroing, no clean on the caller's thread. A BO smaller than its
//    spare is a view of the spare's first pages. The class of a size missed is kept warm from then on, and a few classes
//    from the start (kWarm: what NVK and m2v make most: 64 KiB pieces, m2v's 4 MiB blocks, backboardd's 8 and 16 MiB
//    ones, two of the 16 MiB so that a pair of them in one frame waits for neither).
//  - A freed cached BO no task maps any more goes to the spares as it is (a list push: BoFree does not unwire and
//    free it); a thread call zeroes it through its kernel mapping and cleans it to DRAM once, then files it by class,
//    or gives it back to the VM past kSpareCapBytes. The thread call runs on a free, a miss or a take below a class's
//    mark: never on a timer. Under memory pressure (memorystatus' level, the one apps' memory warnings follow) it gives
//    every spare back and keeps none.
//  - A freed pool BO is zeroed by the GPU's copy engine (tgm_pool_scrub; by the same thread call where the scrubber cannot
//    take it) and given back clean, so the next pool BoAlloc of those pages does no memset (shim nvxnu_pool_alloc_clean
//    zeroes only pages a plain free left dirty).
// Each window's BoAllocs by process go to the "mem3" line (tgm_owner_stats).
constexpr uint32_t kClasses = 53;  // 4 KiB .. 256 MiB (kMaxBoBytes)
constexpr uint64_t kSpareCapBytes = 96ULL << 20, kLearnMaxBytes = 16ULL << 20;

struct WarmClass {
	uint64_t bytes;
	uint16_t ready;
};

constexpr WarmClass kWarm[] = {
	{64ULL << 10, 8}, {4ULL << 20, 2}, {8ULL << 20, 1}, {16ULL << 20, 2}};
constexpr uint32_t kOwners = 16;

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

struct Spare {
	Spare *next;
	IOBufferMemoryDescriptor *bmd;  // prepared; ours alone (no view, no task mapping)
	uint64_t *pas;
	uint64_t bytes;
};

struct PoolZero {
	PoolZero *next;
	void *va;
	uint64_t pa, bytes;
};

struct OwnerStat {
	char name[17];
	uint64_t n, bytes, ns, max_ns,
		made;  // made: cached BOs made on the caller's thread (a miss)
};

struct SparePool {
	IOLock *lock;
	thread_call_t call;
	Spare *dirty;         // freed, not yet zeroed
	PoolZero *poolDirty;  // freed pool BOs, not yet zeroed
	Spare *ready
		[kClasses];  // zeroed and clean, by class (classUp(bytes) <= class of a spare's bytes)
	uint16_t count[kClasses], target[kClasses];
	uint64_t clean, dirtyBytes,
		poolZeroing;  // bytes ready, freed and waiting, pool bytes waiting
	OwnerStat owners[kOwners];
};

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
	s->bmd->complete();
	s->bmd->release();
	IODelete(s->pas, uint64_t, s->bytes >> 12);
	IODelete(s, Spare, 1);
}

// Zero `bytes` through a cached BO's kernel mapping (when `zero`), then clean and invalidate its lines: the GPU and the
// next owner's view see zeros, and no dirty line is written back later over what the GPU writes.
void cleanZeros(void *va, uint64_t bytes, bool zero)
{
	if (zero)
		bzero(va, bytes);
	switch_dcache_clean_invalidate(va, bytes);
}

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
		cleanZeros(bmd->getBytesNoCopy(), bytes, false);
		if (kernelPages(bmd->getBytesNoCopy(), bytes, pas)) {
			*s = {nullptr, bmd, pas, bytes};
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
			if (keep)
				p->clean += s->bytes;  // held for it while it is zeroed
			IOLockUnlock(p->lock);
			if (!keep) {
				spareFree(
					s);  // past the cap or under pressure: back to the VM, unzeroed
				continue;
			}
			cleanZeros(s->bmd->getBytesNoCopy(), s->bytes, true);
			const uint32_t c = spareClass(s->bytes);
			IOLockLock(p->lock);
			s->next = p->ready[c];
			p->ready[c] = s;
			p->count[c]++;
			IOLockUnlock(p->lock);
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
			p->clean = 0;
			IOLockUnlock(p->lock);
			while (all) {
				Spare *next = all->next;
				spareFree(all);
				all = next;
			}
			return;
		}
		uint32_t c = 0;
		while (c < kClasses &&
		       (p->count[c] >= p->target[c] || p->clean + classBytes(c) > kSpareCapBytes))
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
			IOLockUnlock(p->lock);
			return;
		}
		s->next = p->ready[c];
		p->ready[c] = s;
		p->count[c]++;
		IOLockUnlock(p->lock);
	}
}

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
	n->call = n->lock ? thread_call_allocate_with_priority(spareWork, n,
							       THREAD_CALL_PRIORITY_USER)
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
	if (!s && classBytes(c) <= kLearnMaxBytes && !p->target[c])
		p->target[c] = 1;
	const bool kick = !memoryPressure() ? p->count[c] < p->target[c] : p->clean != 0;
	IOLockUnlock(p->lock);
	if (kick)
		thread_call_enter(p->call);
	return s;
}

// A freed cached BO (its prepared descriptor, which nothing else holds, with its `bytes` and page list) to the spares:
// true if taken, then the caller must not touch bmd or pas again. Zeroing, cleaning and the cap are the thread call's.
bool spareKeep(IOBufferMemoryDescriptor *bmd, uint64_t *pas, uint64_t bytes)
{
	SparePool *p = spares();
	Spare *s = p && bmd->getRetainCount() == 1 ? IONew(Spare, 1) : nullptr;
	if (!s)
		return false;
	*s = {nullptr, bmd, pas, bytes};
	IOLockLock(p->lock);
	s->next = p->dirty;
	p->dirty = s;
	p->dirtyBytes += bytes;
	IOLockUnlock(p->lock);
	thread_call_enter(p->call);
	return true;
}

// A freed pool BO's range (no task maps it) to be zeroed and given back clean: by the GPU's copy engine (gpu.c "pool
// scrub", no CPU time), else by the thread call: true if taken.
bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)
{
	if (!tgm_pool_scrub(pa, bytes))
		return true;
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

// The freed pool ranges still waiting, zeroed and given back now: a pool BoAlloc that found no room tries again after.
bool poolZeroNow()
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	if (!p)
		return false;
	IOLockLock(p->lock);
	PoolZero *all = p->poolDirty;
	p->poolDirty = nullptr;
	IOLockUnlock(p->lock);
	if (!all)
		return false;
	while (all) {
		PoolZero *next = all->next;
		tgm_pool_free_zeroed(all->va, all->bytes, all->pa);
		IOLockLock(p->lock);
		p->poolZeroing -= all->bytes;
		IOLockUnlock(p->lock);
		IODelete(all, PoolZero, 1);
		all = next;
	}
	return true;
}

// One BoAlloc of `owner`'s for the window's "mem3" line; `made`: a cached BO made on the caller's thread.
void ownerNote(const char *owner, uint64_t bytes, uint64_t ns, bool made)
{
	SparePool *p = spares();
	if (!TGM_STATS || !p)
		return;
	IOLockLock(p->lock);
	OwnerStat *o = nullptr;
	for (uint32_t i = 0; i < kOwners && !o; i++) {
		if (!p->owners[i].n || !strncmp(p->owners[i].name, owner, sizeof(o->name) - 1))
			o = &p->owners[i];
	}
	if (!o) {  // more processes than slots this window: the last slot takes the rest
		o = &p->owners[kOwners - 1];
		strlcpy(o->name, "(others)", sizeof(o->name));
	} else if (!o->n) {
		strlcpy(o->name, owner, sizeof(o->name));
	}
	o->n++;
	o->bytes += bytes;
	o->ns += ns;
	o->made += static_cast<uint64_t>(made);
	if (ns > o->max_ns)
		o->max_ns = ns;
	IOLockUnlock(p->lock);
}

// With the stats (also gpu.c's stats lines): the method stream of every kScanEvery-th submission of each
// connection is read back and what it asks of the GPU is counted, one line per process every 5 s ("tegra-gm20b: push
// <process>", the window's first kPushOwners - 1 processes by name, the rest as "(others)"), per submission: render passes
// (and of those, the ones with a depth target and the multisampled ones), bound color targets (RTs) and the pitch-linear
// ones, draws, clears; every 3D wait-for-idle (WFI), shader cache invalidate that waits for idle and flushes (INVALIDATE_SHADER_CACHES,
// NVK's FLUSH_SHADER_DATA barrier) and the ones that do not (_NO_WFI), texture data cache invalidate, host SET_REFERENCE
// (a host wait for idle: NVK's HOST_WFI_FLUSH_SYSMEM and MME-data barriers), MEM_OP_D L2 system-memory invalidate and
// host WFI; copy-engine copies with their bytes and the zero-length LAUNCH_DMAs NVK uses as a copy-engine WFI, 2D blits,
// compute dispatches and semaphore acquires; "bad" counts streams with an opcode NVK does not emit. Pushes in host
// imports (no kernel mapping) are not read; the line says what share of the push bytes was. The reader:
// TegraGM20BPushScan.h. Every kProfileEvery-th submission read is also profiled (gpu.c "push profile").
constexpr uint32_t kScanEvery = 4, kPushOwners = 4,
		   kProfileEvery = 16;  // 1 submission in 64 profiled (its pushes WFI-stamped)
constexpr uint64_t kScanMaxBytes = 512 << 10, kScanWindowNs = 5000000000ULL;

struct PushOwner {
	char name[17];
	PushCounts c;
};

PushOwner gPushOwners[kPushOwners];
uint64_t gPushSince;
IOLock *gPushLock;

constexpr bool pushScanOn()
{
	return TGM_STATS != 0;
}

IOReturn ioreturn(int err)
{
	switch (err) {
	case 0:
		return kIOReturnSuccess;
	case -12:
		return kIOReturnNoMemory;  // ENOMEM
	case -22:
		return kIOReturnBadArgument;  // EINVAL
	case -16:
		return kIOReturnBusy;  // EBUSY
	case -5:
		return kIOReturnNotResponding;  // EIO
	case -110:
		return kIOReturnTimeout;  // ETIMEDOUT
	case -19:
		return kIOReturnNotReady;  // ENODEV
	case -28:
		return kIOReturnNoResources;  // ENOSPC
	default:
		return kIOReturnError;
	}
}
}  // namespace

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
				   uint64_t *orphans, uint64_t *system_bytes, uint64_t *systems)
{
	*client_bytes = __atomic_load_n(&gClientPoolBytes, __ATOMIC_RELAXED);
	*orphan_bytes = __atomic_load_n(&gOrphanBytes, __ATOMIC_RELAXED);
	*orphans = __atomic_load_n(&gOrphans, __ATOMIC_RELAXED);
	*system_bytes = __atomic_load_n(&gSystemCoherentBytes, __ATOMIC_RELAXED);
	*systems = __atomic_load_n(&gSystemCoherent, __ATOMIC_RELAXED);
}

// The window's BoAllocs by process ("name n (KiB, mean/max us, made m)"), then cleared; 0 when there were none.
extern "C" unsigned long tgm_owner_stats(char *out, unsigned long size)
{
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
	bzero(p->owners, sizeof(p->owners));
	IOLockUnlock(p->lock);
	return at;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AGXDeviceUserClient : public IOUserClient
{
	// NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
	OSDeclareDefaultStructors(AGXDeviceUserClient)

		struct Bo {
		Bo *next;
		uint64_t handle, bytes;
		uint32_t token, flags, npages;
		uint32_t
			backing_pages;  // pas' length: npages, or a spare's pages when the BO is a view of its first ones
		bool readOnly;  // imports of pages the task maps read-only: the GPU maps them read-only
		uint64_t *pas;  // one per 4 KiB page; tgm_mem keeps a pointer to it
		void *pool_va;  // Coherent BOs: pool memory (write-combined, contiguous)
		uint64_t pool_pa;
		IOBufferMemoryDescriptor *bmd;  // cached BOs
		IOMemoryDescriptor *wired;      // imports
		IOMemoryDescriptor *view;       // cached clientMemoryForType descriptor
		bool system;  // a coherent BO from ordinary pages (tgm_pool::FromSystem): bmd's pages under a physical view
		bool compositorPool;  // a pool BO counted as the compositor's (critical_ when it was made)
		tgm_mem *mem;
		tgm_coh *coh;  // imports: the surface's coherency registry entry (gpu.c), held
		SharedWiring *
			wiring;  // a surface import's pages, mem and wiring, shared with the task's other imports
		uint32_t snapshot[kMaxChannels];  // submitted seqnos when freed (zombies only)
	};

	struct Binding {
		uint64_t va, range;
		Bo *bo;
		uint64_t bo_offset;
		uint8_t pageShift;
	};

	// A mapping method 12 (Map) made into the client's task: what IOKit's io_connect_map_memory_into_task keeps in the
	// client's mapping set, kept here (that set is IOKit's own). Unmap and close release it, and its release unmaps it.
	struct Mapping {
		Mapping *next;
		IOMemoryMap *map;
		uint32_t token;
		uint64_t address;
	};

	IOLock *lock_ = nullptr;
	task_t task_ = nullptr;
	tgm_vm *vm_ = nullptr;
	volatile uint32_t *fence_va_ =
		nullptr;  // tgm_fence_page_get: the GPU present maps every connection's
	uint64_t fence_pa_ = 0, fence_gpu_ = 0;
	tgm_mem *fence_mem_ = nullptr;
	IOMemoryDescriptor *fence_view_ = nullptr;
	tgm_chan *chans_[kMaxChannels] = {};
	Bo *bos_ = nullptr, *zombies_ = nullptr;
	tgm_fences::ExecSurfaces
		surfaces_;  // the imports of bos_ with a coherency entry, and method 17's lists
	Mapping *maps_ = nullptr;  // under lock_
	Binding *binds_ = nullptr;
	uint32_t nbinds_ = 0, cap_binds_ = 0;
	uint64_t next_handle_ = 1;
	uint32_t next_token_ = 1;
	bool counted_ = false;
	bool closing_ = false;  // clientClose/free: BOs still mapped are expected (a dying task's)
	char owner_[17] = {};   // the opening process
	uint32_t pushSubmits_ =
		0;  // the stats: its submissions, every kScanEvery-th read (scanPushes)
	uint32_t pushScans_ =
		0;  // the ones read, every kProfileEvery-th also profiled (gpu.c "push profile")
	// the compositor's: its queues asked for the top GPU priority (method 18); its channels are high, it has no pool quota
	bool critical_ = false;
	// what the connection's queues asked for (method 18), whether runningboardd denies it the GPU (then the background
	// priority counts) and the runlist level they give its channels
	uint32_t priority_ = 2, backgroundPriority_ = 2;
	bool backgroundRole_ = false;
	uint32_t level_ = TGM_LEVEL_FOREGROUND;
	bool importsWritten_ =
		false;  // TegraGM20BPushImportsWritten: every submission marks every writable import
	bool quotaLogged_ = false;
	bool systemLogged_ =
		false;  // a coherent BoAlloc served from system memory (tgm_pool::FromSystem), logged once
	uint64_t pool_bytes_ = 0;  // pool BOs held
	// the thread about to take a hang's line while it holds lock_ (tgm_chan_hung, tgm_chan_del), so hungInterlocks
	// reads the binds without taking it again
	thread_t hangHolder_ = nullptr;

	Bo *findBo(uint64_t handle);
	Bo *findToken(uint32_t token);
	void releaseBo(Bo *bo);
	void reap(bool force);
	void unbindOverlaps(uint64_t va, uint64_t range, Bo *only);
	void scanPushes(const tgm_push *pushes, uint32_t count, tgm_prof *prof);
	const uint32_t *pushView(uint64_t va, uint64_t bytes, const Binding *&hit) const;
	IOReturn run(uint32_t method, const void *in, uint32_t inSize, void *out,
		     const uint32_t *outSize);

	static IOReturn getInfo(TegraGM20BAbiInfo *out);
	IOReturn boAlloc(const TegraGM20BAbiBoAlloc *in, TegraGM20BAbiBoAllocReply *out);
	IOReturn boImport(const TegraGM20BAbiBoImport *in, TegraGM20BAbiBoAllocReply *out);
	static void adoptWiring(Bo *bo, SharedWiring *w);
	void publishWiring(Bo *bo, const TegraGM20BAbiBoImport *in);
	IOReturn boFree(const TegraGM20BAbiBoFree *in);
	IOReturn vmBind(const TegraGM20BAbiVmBind *in, uint32_t count);
	IOReturn channelCreate(const TegraGM20BAbiChannelCreate *in,
			       TegraGM20BAbiChannelCreateReply *out);
	IOReturn channelDestroy(const TegraGM20BAbiChannel *in);
	IOReturn exec(const TegraGM20BAbiExec *in, uint32_t count, TegraGM20BAbiExecReply *out);
	IOReturn waitFence(const TegraGM20BAbiWaitFence *in);
	IOReturn getStatus(const TegraGM20BAbiChannel *in, TegraGM20BAbiStatus *out);
	IOReturn mapToken(const TegraGM20BAbiMap *in, TegraGM20BAbiMapReply *out);
	IOReturn order(const TegraGM20BAbiOrder *in, TegraGM20BAbiOrderReply *out);
	IOReturn surfaceLayout(const TegraGM20BAbiSurfaceLayout *in,
			       TegraGM20BAbiSurfaceLayoutReply *out);
	void applyLevel();
	IOReturn execSurfaces(const TegraGM20BAbiExecSurfaces *in);
	IOReturn unmapToken(const TegraGM20BAbiUnmap *in);
	void releaseMappings();

public:
	bool setUp(task_t task);
	int hungInterlocks(const uint64_t *va, const uint32_t *dwords, uint32_t n,
			   uint64_t *interlocks);

	void countConnection()
	{
		counted_ = true;
	}

	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
				     IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
				IOExternalMethodDispatch *dispatch, OSObject *target,
				void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)

	bool AGXDeviceUserClient::setUp(task_t task)
{
	task_ = task;
	proc_selfname(owner_, sizeof(owner_));

	lock_ = IOLockAlloc();
	if (!lock_ || tgm_vm_new(&vm_))
		return false;
	tgm_vm_claim(vm_, this);
	// fence page: pool memory (write-combined), GPU-mapped in the kernel range, read-only to the client
	if (tgm_fence_page_get(&fence_va_, &fence_pa_) ||
	    tgm_mem_new(vm_, &fence_pa_, 1, &fence_mem_) ||
	    tgm_vm_kmap(vm_, fence_mem_, 4096, &fence_gpu_))
		return false;
	// the whole fence slot (gpu.c TGM_FENCE_SLOT): one VM page
	IOAddressRange range = {fence_pa_, static_cast<IOByteCount>(PAGE_SIZE)};
	fence_view_ = IOMemoryDescriptor::withAddressRanges(
		&range, 1,
		static_cast<IOOptionBits>(kIODirectionIn) |
			static_cast<IOOptionBits>(kIOMemoryTypePhysical64) | kPoolCacheMode,
		TASK_NULL);
	return fence_view_ != nullptr;
}

IOReturn AGXDeviceUserClient::clientClose()
{
	releaseMappings();  // first: a view still mapped would keep its pool pages as an orphan (releaseBo)
	IOLockLock(lock_);
	closing_ = true;
	hangHolder_ = current_thread();
	for (auto &chan : chans_) {
		tgm_chan_del(
			&chan);  // bounded wait, then nvkm preempts and removes it from the runlist
	}
	hangHolder_ = nullptr;
	while (nbinds_)
		unbindOverlaps(binds_[0].va, binds_[0].range, nullptr);
	surfaces_.clear();
	while (bos_) {
		Bo *bo = bos_;
		bos_ = bo->next;
		bo->next = zombies_;
		zombies_ = bo;
	}
	reap(true);  // no channel is left, so nothing can touch the pages any more
	IOLockUnlock(lock_);
	scheduleOrphanRelease();  // not here: an import of one of these views may be on a connection not closed yet
	terminate();
	return kIOReturnSuccess;
}

void AGXDeviceUserClient::free()
{
	if (lock_) {
		releaseMappings();
		closing_ = true;
		for (auto &chan : chans_)
			tgm_chan_del(&chan);
		surfaces_.clear();
		while (bos_) {
			Bo *bo = bos_;
			bos_ = bo->next;
			releaseBo(bo);
		}
		reap(true);
		scheduleOrphanRelease();
		if (binds_)
			IODelete(binds_, Binding, cap_binds_);
		if (fence_view_)
			fence_view_->release();
		tgm_mem_del(&fence_mem_);
		tgm_vm_del(&vm_);
		tgm_fence_page_put(fence_va_);
		IOLockFree(lock_);
		lock_ = nullptr;
	}
	if (counted_)
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
	IOUserClient::free();
}

AGXDeviceUserClient::Bo *AGXDeviceUserClient::findBo(uint64_t handle)
{
	for (Bo *bo = bos_; bo; bo = bo->next) {
		if (bo->handle == handle)
			return bo;
	}
	return nullptr;
}

AGXDeviceUserClient::Bo *AGXDeviceUserClient::findToken(uint32_t token)
{
	for (Bo *bo = bos_; bo; bo = bo->next) {
		if (bo->token && bo->token == token)
			return bo;
	}
	return nullptr;
}

void AGXDeviceUserClient::releaseBo(Bo *bo)
{
	if (bo->wiring)
		bo->mem = nullptr;
	tgm_mem_del(&bo->mem);
	if (bo->pool_va && bo->handle) {
		pool_bytes_ -= bo->bytes;
		if (bo->compositorPool) {
			tgm_stats_compositor_pool(-static_cast<int64_t>(bo->bytes));
		} else {
			__atomic_sub_fetch(&gClientPoolBytes, bo->bytes, __ATOMIC_RELAXED);
		}
	}
	// A pool BO's view and pages go to the thread call (releaseOrphans: releasing the view can wait for an import on
	// another connection), which keeps the pages while a task still maps the view. Cached BOs need no such care: their
	// mappings hold the buffer itself, and releasing one waits for nothing.
	PoolOrphan *orphan = nullptr;
	if (bo->system && bo->handle) {
		__atomic_sub_fetch(&gSystemCoherent, 1, __ATOMIC_RELAXED);
		__atomic_sub_fetch(&gSystemCoherentBytes, bo->bytes, __ATOMIC_RELAXED);
	}
	// (a system BO's view is physical as a pool BO's is, and goes the same way, its backing pages released after it)
	if ((bo->pool_va || bo->system) && bo->view)
		orphan = IONew(PoolOrphan, 1);
	if (orphan) {
		if (!closing_ && bo->view->getRetainCount() > 1) {
			IOLog("tegra-gm20b: BoFree of BO %llu (%llu bytes) still mapped by its "
			      "task: pages kept until unmapped\n",
			      bo->handle, bo->bytes);
		}
		*orphan = {nullptr,
			   bo->view,
			   bo->pool_va,
			   bo->pool_pa,
			   bo->bytes,
			   bo->handle != 0,
			   bo->system ? bo->bmd : nullptr,
			   bo->system ? bo->pas : nullptr,
			   bo->system ? static_cast<uint64_t>(bo->backing_pages) << 12 : 0};
		if (bo->pool_va) {
			__atomic_add_fetch(&gOrphanBytes, bo->bytes, __ATOMIC_RELAXED);
			__atomic_add_fetch(&gOrphans, 1, __ATOMIC_RELAXED);
		}
		if (bo->system) {  // the orphan's now
			bo->bmd = nullptr;
			bo->pas = nullptr;
		}
		pushOrphan(orphan);
		scheduleOrphanRelease();
	} else {
		if (bo->view)
			bo->view->release();
		// a pool BO's pages are zeroed off this thread and go back clean; a cached BO no task maps any more goes to
		// the spares as it is (spareKeep: zeroed and cleaned by the thread call, or given back to the VM there)
		if (bo->pool_va &&
		    !(bo->handle && poolZeroLater(bo->pool_va, bo->pool_pa, bo->bytes)))
			tgm_pool_free(bo->pool_va, bo->bytes, bo->pool_pa);
	}
	if (bo->bmd && bo->handle && !bo->system && !(bo->flags & TegraGM20BBoContiguous) &&
	    spareKeep(bo->bmd, bo->pas, static_cast<uint64_t>(bo->backing_pages) << 12)) {
		bo->bmd = nullptr;
		bo->pas = nullptr;
	}
	if (bo->bmd) {
		bo->bmd->complete();
		bo->bmd->release();
	}
	if (bo->wired) {
		if (bo->handle) {
			tgm_stats_import(-static_cast<int64_t>(bo->bytes), bo->pas[0]);
			tgm_vm_import_note(vm_, bo->pas[0], 0);
		}
		tgm_coherency_put(bo->coh);
		if (bo->wiring) {
			sharedWiringPut(bo->wiring);
			bo->pas = nullptr;
		} else {
			bo->wired->complete();
			bo->wired->release();
		}
	}
	if (bo->pas)
		IODelete(bo->pas, uint64_t, bo->backing_pages);
	IODelete(bo, Bo, 1);
}

// CONTRACT §4.1: a freed BO's pages go back only once every channel finished what it had submitted at BoFree time
// (or is dead and stopped). force: no channels remain.
void AGXDeviceUserClient::reap(bool force)
{
	for (Bo **p = &zombies_; *p;) {
		Bo *bo = *p;
		bool idle = true;
		for (uint32_t i = 0; i < kMaxChannels && !force; i++) {
			if (chans_[i] && !tgm_chan_error(chans_[i]) &&
			    static_cast<int32_t>(tgm_chan_completed(chans_[i]) - bo->snapshot[i]) <
				    0)
				idle = false;
		}
		if (idle) {
			*p = bo->next;
			releaseBo(bo);
		} else {
			p = &bo->next;
		}
	}
}

// The page list of a prepared descriptor: one getPhysicalSegment per physically contiguous run (each call is a walk of the
// descriptor's DMA state; per 4 KiB page that would be 900 walks for a 1280x720 surface).
static IOReturn collectPages(IOMemoryDescriptor *md, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes;) {
		IOByteCount len = 0;
		const addr64_t pa = md->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		len &= ~0xfffULL;
		if (!len)
			len = 4096;  // a run shorter than a page: that page alone
		if (len > bytes - off)
			len = bytes - off;
		// GM20B on Tegra210 addresses 34-bit physical memory (nvgpu platform_gk20a_tegra.c dma_mask DMA_BIT_MASK(34),
		// nouveau tegra iommu_bit 34): all of the Switch's RAM, including the bank above 4 GiB
		if (!pa || (pa & 0xfff) || pa + len > (1ULL << 34))
			return kIOReturnUnsupported;
		for (uint64_t at = 0; at < len; at += 4096)
			pas[(off + at) >> 12] = pa + at;
		off += len;
	}
	return kIOReturnSuccess;
}

// The page list of a wired kernel buffer (a cached BO), from its kernel mapping: one address translation per page.
static IOReturn collectKernelPages(const void *va, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes; off += 4096) {
		const uint64_t pa = ml_vtophys(reinterpret_cast<vm_offset_t>(va) + off);
		if (!pa || (pa & 0xfff) || pa + 4096 > (1ULL << 34))
			return kIOReturnUnsupported;
		pas[off >> 12] = pa;
	}
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::boAlloc(const TegraGM20BAbiBoAlloc *in,
				      TegraGM20BAbiBoAllocReply *out)
{
	if (!in->bytes || (in->bytes & 0xfff) || in->bytes > kMaxBoBytes || in->align_log2 < 12 ||
	    in->align_log2 > 21 ||
	    (in->flags & ~static_cast<uint32_t>(TegraGM20BBoCoherent | TegraGM20BBoContiguous)))
		return kIOReturnBadArgument;
	if (__atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED))
		scheduleOrphanRelease();  // not here: releasing a view can wait for another connection (releaseOrphans)
	const uint64_t start = nowNs();
	uint64_t phase[TGM_PHASES] = {};
	uint64_t mark = start;  // the stats: kernel allocation and wiring, clean, pages, nvkm
	const auto lap = [&](int i) {
		const uint64_t now = nowNs();
		phase[i] += now - mark;
		mark = now;
	};
	Bo *bo = IONew(Bo, 1);
	if (!bo)
		return kIOReturnNoMemory;
	bzero(bo, sizeof(*bo));
	bo->bytes = in->bytes;
	bo->flags = in->flags;
	bo->npages = static_cast<uint32_t>(in->bytes >> 12);
	bo->backing_pages = bo->npages;
	bo->pas = IONew(uint64_t, bo->npages);
	IOReturn ret = bo->pas ? kIOReturnSuccess : kIOReturnNoMemory;
	int kind = TGM_BO_CACHED;
	Spare *spare = nullptr;
	const char *refused = nullptr;
	// A coherent BO the pool does not serve (its tiers keep it out, or the pool has no room) is not refused: ordinary pages,
	// a spare's or made below as a cached BO's are, mapped write-combined (tgm_pool::FromSystem). A refusal would abort
	// clients such as VectorKit (on Maps' first 4 MiB buffer). A spare is in the state the made path's cleanZeros leaves:
	// zero in DRAM and no line in its cacheable kernel alias, which nothing writes through while the BO lives.
	bool system = false;
	if (!ret && (in->flags & TegraGM20BBoCoherent)) {
		refused = poolRefusal(in->bytes, pool_bytes_, critical_);
		if (!refused) {
			// write-combined pool memory: zeroed by the allocator, no cacheable alias (CONTRACT §3). No room: the freed
			// ranges the thread call has not zeroed yet are zeroed now, and the allocation tried again.
			const uint64_t align =
				(in->flags & TegraGM20BBoContiguous) ? 1ULL << in->align_log2 : 0;
			bo->pool_va = tgm_pool_alloc(in->bytes, align, &bo->pool_pa);
			if (!bo->pool_va && poolZeroNow())
				bo->pool_va = tgm_pool_alloc(in->bytes, align, &bo->pool_pa);
		}
		system = tgm_pool::coherentSource(refused, bo->pool_va != nullptr) ==
			 tgm_pool::FromSystem;
		if (system && !systemLogged_) {
			IOLog("tegra-gm20b: %s: coherent BoAlloc of %llu KiB from system memory, "
			      "write-combined: %s "
			      "(client holds %llu MiB of the pool, %llu MiB free); further ones "
			      "not logged\n",
			      owner_, in->bytes >> 10, refused ? refused : "the pool has no room",
			      pool_bytes_ >> 20, tgm_pool_avail() >> 20);
		}
		systemLogged_ = systemLogged_ || system;
	}
	if (!ret && !bo->pool_va && !(in->flags & TegraGM20BBoContiguous))
		spare = spareTake(in->bytes);
	if (!ret && bo->pool_va) {
		for (uint32_t i = 0; i < bo->npages; i++)
			bo->pas[i] = bo->pool_pa + (static_cast<uint64_t>(i) << 12);
		IOAddressRange range = {bo->pool_pa, bo->bytes};
		bo->view = IOMemoryDescriptor::withAddressRanges(
			&range, 1,
			static_cast<IOOptionBits>(kIODirectionInOut) |
				static_cast<IOOptionBits>(kIOMemoryTypePhysical64) | kPoolCacheMode,
			TASK_NULL);
		ret = bo->view ? kIOReturnSuccess : kIOReturnNoMemory;
	} else if (spare) {
		// a spare: zeroed and cleaned already, its page list made; the BO is its first pages
		IODelete(bo->pas, uint64_t, bo->npages);
		bo->pas = spare->pas;
		bo->backing_pages = static_cast<uint32_t>(spare->bytes >> 12);
		bo->bmd = spare->bmd;
		IODelete(spare, Spare, 1);
		kind = TGM_BO_SPARE;
	} else if (!ret) {
		IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
				       static_cast<IOOptionBits>(kIOMemoryMapperNone);
		// Made here only when no spare was ready (the class is kept warm from now on), at its class's size so that it
		// serves the class again once freed; the BO is its first pages.
		const uint32_t c =
			(in->flags & TegraGM20BBoContiguous) ? kClasses : classUp(in->bytes);
		const uint64_t backing = c < kClasses ? classBytes(c) : in->bytes;
		if (backing != in->bytes) {
			IODelete(bo->pas, uint64_t, bo->npages);
			bo->backing_pages = static_cast<uint32_t>(backing >> 12);
			bo->pas = IONew(uint64_t, bo->backing_pages);
		}
		if (in->flags & TegraGM20BBoContiguous)
			options |= kIOMemoryPhysicallyContiguous;
		// 34-bit, not 32: a mask below 4 GiB on a non-contiguous buffer makes IOKit use KMA_LOMEM, and arm64 XNU keeps
		// no low-page pool, so every such BO fails (NVK "device refused: OutOfMemory(Device)")
		// The alignment goes into the mask only for Contiguous BOs (ABI: "physical alignment only matters with
		// Contiguous"): IOBufferMemoryDescriptor takes the mask's low zero bits as the alignment, and any alignment
		// above the page size makes IOKernelAllocateWithPhysicalRestrict use kmem_alloc_contig, a physically
		// contiguous search that takes up to 130 ms per BO for NVK's 64 KiB-aligned buffers. The GMMU maps
		// these page by page (bo->pas), so ordinary BOs need no physical alignment.
		const uint64_t mask = (in->flags & TegraGM20BBoContiguous)
					      ? 0x3ffffffffULL & ~((1ULL << in->align_log2) - 1)
					      : 0x3ffffffffULL;
		bo->bmd = bo->pas ? IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
					    kernel_task, options, backing, mask)
				  : nullptr;
		if (!bo->bmd || bo->bmd->prepare() != kIOReturnSuccess) {
			if (bo->bmd)
				bo->bmd->release();
			bo->bmd = nullptr;
			ret = kIOReturnNoMemory;
		} else {
			lap(0);
			// The pages come zeroed: the mask makes IOKit allocate with KMA_ZERO (IOKernelAllocateWithPhysicalRestrict),
			// and vm_page_alloc_list zero-fills each page through the cacheable physical aperture. Clean those zeros to
			// DRAM and drop the lines, so the GPU and a fresh client view see zeros and no dirty line is written back
			// later over what the GPU wrote. (A bzero here would be a second pass over the whole buffer.)
			void *va = bo->bmd->getBytesNoCopy();
			cleanZeros(va, backing, false);
			lap(1);
			ret = collectKernelPages(va, backing, bo->pas);
			lap(2);
		}
	}
	// A cached BO's view, which its task maps: the descriptor itself, or its first in->bytes when it is a spare's or
	// a class's larger buffer.
	if (!ret && bo->bmd && system) {
		// a coherent BO from ordinary pages: its task's view is a physical descriptor over them, write-combined as a pool
		// BO's (kPoolCacheMode: IOKit takes a physical descriptor's user cache mode from the descriptor), one range a run
		// of contiguous pages. The pages' cacheable kernel mapping holds no line of them (cleanZeros dropped the zeros',
		// here or when the spare was made or recycled), and nothing writes through it while the BO lives. Released as a
		// pool view is (releaseBo: orphan, then its backing to the spares).
		uint32_t runs = 0;
		for (uint32_t i = 0; i < bo->npages; i++) {
			runs += static_cast<uint32_t>((i == 0u) ||
						      bo->pas[i] != bo->pas[i - 1] + 4096);
		}
		IOAddressRange *ranges = IONew(IOAddressRange, runs);
		if (ranges) {
			uint32_t r = 0;
			for (uint32_t i = 0; i < bo->npages; i++) {
				if (i && bo->pas[i] == bo->pas[i - 1] + 4096) {
					ranges[r - 1].length += 4096;
					continue;
				}
				ranges[r++] = {bo->pas[i], 4096};
			}
			bo->view = IOMemoryDescriptor::withAddressRanges(
				ranges, runs,
				static_cast<IOOptionBits>(kIODirectionInOut) |
					static_cast<IOOptionBits>(kIOMemoryTypePhysical64) |
					kPoolCacheMode,
				TASK_NULL);
			IODelete(ranges, IOAddressRange, runs);
		}
		if (!bo->view)
			ret = kIOReturnNoMemory;
	} else if (!ret && bo->bmd) {
		bo->view = bo->backing_pages == bo->npages
				   ? static_cast<IOMemoryDescriptor *>(bo->bmd)
				   : IOSubMemoryDescriptor::withSubRange(bo->bmd, 0, in->bytes,
									 kIODirectionInOut);
		if (bo->view == bo->bmd) {
			bo->bmd->retain();
		} else if (!bo->view) {
			ret = kIOReturnNoMemory;
		}
	}
	if (!ret)
		ret = ioreturn(tgm_mem_new_exclusive(vm_, bo->pas, bo->npages, &bo->mem));
	lap(3);
	if (ret) {
		if (!quotaLogged_) {
			IOLog("tegra-gm20b: BoAlloc %llu bytes flags %#x align 2^%u refused: %#x\n",
			      in->bytes, in->flags, in->align_log2, ret);
		}
		releaseBo(bo);
		return ret;
	}
	if (bo->pool_va) {
		pool_bytes_ += bo->bytes;
		kind = TGM_BO_POOL;
		bo->compositorPool = critical_;
		if (critical_) {
			tgm_stats_compositor_pool(static_cast<int64_t>(bo->bytes));
		} else {
			__atomic_add_fetch(&gClientPoolBytes, bo->bytes, __ATOMIC_RELAXED);
		}
	}
	if (system) {
		bo->system = true;
		__atomic_add_fetch(&gSystemCoherent, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&gSystemCoherentBytes, bo->bytes, __ATOMIC_RELAXED);
	}
	tgm_stats_bo_alloc(bo->bytes, kind, mark - start, phase, owner_);
	ownerNote(owner_, bo->bytes, mark - start, kind == TGM_BO_CACHED);
	bo->handle = next_handle_++;
	bo->token = next_token_++;
	bo->next = bos_;
	bos_ = bo;
	out->handle = bo->handle;
	out->map_token = bo->token;
	out->reserved = 0;
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::boImport(const TegraGM20BAbiBoImport *in,
				       TegraGM20BAbiBoAllocReply *out)
{
	if (!in->bytes || ((in->address | in->bytes) & 0xfff) || in->bytes > kMaxBoBytes)
		return kIOReturnBadArgument;
	if (!surfaces_.reserve())
		return kIOReturnNoMemory;
	const uint64_t start = nowNs();
	uint64_t phase[TGM_PHASES] = {};
	uint64_t mark = start;  // the stats: wiring, pages, nvkm, clean
	const auto lap = [&](int i) {
		const uint64_t now = nowNs();
		phase[i] += now - mark;
		mark = now;
	};
	Bo *bo = IONew(Bo, 1);
	if (!bo)
		return kIOReturnNoMemory;
	bzero(bo, sizeof(*bo));
	bo->bytes = in->bytes;
	bo->npages = static_cast<uint32_t>(in->bytes >> 12);
	bo->backing_pages = bo->npages;
	if (SharedWiring *w = sharedWiringFind(task_, in->address, in->bytes, in->surface_id)) {
		adoptWiring(bo, w);
	} else {
		bo->pas = IONew(uint64_t, bo->npages);
	}
	IOReturn ret = bo->pas ? kIOReturnSuccess : kIOReturnNoMemory;
	IOReturn wire[2] = {kIOReturnNoMemory, kIOReturnNoMemory};
	tgm_import_cost cost = {};
	const uint64_t cpuBefore = tgm_stats_cpu_ns();
	const uint64_t zeroFillsBefore = tgm_stats_zero_fills();
	if (!ret && !bo->wiring) {
		// Wired for the GPU to write where the task's mapping allows that, else for it to read only (Core Animation
		// hands over image bytes it maps read-only); a read-only BO is mapped read-only in the GPU, so a GPU write to
		// it is an MMU fault (a panic naming it), never a store into memory the task cannot write.
		const IOOptionBits directions[2] = {kIODirectionInOut, kIODirectionOut};
		for (int i = 0; i < 2 && !bo->wired; i++) {
			bo->wired = IOMemoryDescriptor::withAddressRange(in->address, in->bytes,
									 directions[i], task_);
			wire[i] = bo->wired ? bo->wired->prepare() : kIOReturnNoMemory;
			if (wire[i] != kIOReturnSuccess)
				OSSafeReleaseNULL(bo->wired);
			bo->readOnly = i == 1;
		}
		lap(0);
		cost.wire_cpu_ns = tgm_stats_cpu_ns() - cpuBefore;
		cost.zero_fills = tgm_stats_zero_fills() - zeroFillsBefore;
		ret = bo->wired ? collectPages(bo->wired, in->bytes, bo->pas)
				: kIOReturnBadArgument;
		lap(1);
	}
	if (!ret && !bo->wiring) {
		ret = ioreturn(
			tgm_mem_new_waited(vm_, bo->pas, bo->npages, &bo->mem, &cost.lock_ns));
		if (!ret && in->surface_id)
			publishWiring(bo, in);
	}
	lap(2);
	if (ret) {
		char region[192] = "";
#if TGM_STATS
		describeRegion(task_, in->address, region, sizeof(region));
#endif
		IOLog("tegra-gm20b: BoImport %llu bytes at %#llx refused: %#x (wiring for GPU "
		      "writes %#x, reads %#x)%s\n",
		      in->bytes, in->address, ret, wire[0], wire[1], region);
		releaseBo(bo);
		return ret;
	}
	// Registered for the coherency every process's IOSurfaceLock/Unlock of the surface drives (TegraGM20BCoherency.cpp):
	// writable (the GPU may write it) unless the task maps the pages read-only.
	int covered = 0;
	const int gpuWrites =
		static_cast<int>(!bo->readOnly && (in->flags & TegraGM20BImportGpuReads) == 0u);
	bo->coh = tgm_coherency_add(bo->pas[0], in->surface_id, bo->pas, bo->npages,
				    static_cast<int>(!bo->readOnly), gpuWrites, &covered);
	const uint64_t added = TGM_STATS ? nowNs() : 0;
	// The GPU does not snoop the CPU caches: what the task wrote must be in DRAM before the GPU reads it, and no dirty
	// line (fresh pages are zeroed through the cache) may be written back later over what the GPU wrote.
	if (!covered) {
		tgm_cpu_clean_pages(bo->pas, bo->npages);
		tgm_coherency_cleaned(bo->coh);
	}
	lap(3);
	tgm_stats_import(static_cast<int64_t>(bo->bytes), bo->pas[0]);
	tgm_stats_bo_import(bo->bytes, static_cast<int>(bo->readOnly), covered, mark - start,
			    phase);
	if (TGM_STATS) {
		cost.bytes = bo->bytes;
		cost.total_ns = mark - start;
		cost.wire_ns = phase[0];
		cost.pages_ns = phase[1];
		cost.mem_ns = phase[2];
		cost.coh_ns = added - (mark - phase[3]);
		cost.clean_ns = mark - added;
		cost.surface_id = in->surface_id;
		cost.read_only = static_cast<int>(bo->readOnly);
		cost.owner = owner_;
		tgm_stats_import_cost(&cost);
	}
	tgm_vm_import_note(
		vm_, bo->pas[0],
		1);  // this connection may draw into what it imports (the GPU present asks)
	bo->handle = next_handle_++;
	bo->next = bos_;
	bos_ = bo;
	if (bo->coh)
		surfaces_.add(bo->handle, bo->coh, !bo->readOnly);
	out->handle = bo->handle;
	out->map_token = 0;
	out->reserved = 0;
	return kIOReturnSuccess;
}

void AGXDeviceUserClient::adoptWiring(Bo *bo, SharedWiring *w)
{
	bo->wiring = w;
	bo->wired = w->wired;
	bo->pas = w->pas;
	bo->mem = w->mem;
	bo->readOnly = w->readOnly;
}

// The import's own wiring becomes the task's for this range; when another import published one meanwhile, the import
// takes that one and its own goes.
void AGXDeviceUserClient::publishWiring(Bo *bo, const TegraGM20BAbiBoImport *in)
{
	SharedWiring *made = IONew(SharedWiring, 1);
	if (!made)
		return;
	*made = {nullptr,   task_,   in->address, in->bytes, in->surface_id, 0,
		 bo->wired, bo->pas, bo->npages,  bo->mem,   bo->readOnly};
	adoptWiring(bo, sharedWiringPublish(made));
}

IOReturn AGXDeviceUserClient::boFree(const TegraGM20BAbiBoFree *in)
{
	const uint64_t start = nowNs();
	Bo **p = &bos_;
	while (*p && (*p)->handle != in->handle)
		p = &(*p)->next;
	if (!*p)
		return kIOReturnBadArgument;
	Bo *bo = *p;
	*p = bo->next;
	surfaces_.remove(bo->handle);
	for (uint32_t i = 0; i < nbinds_;) {
		if (binds_[i].bo == bo) {
			unbindOverlaps(binds_[i].va, binds_[i].range, bo);
		} else {
			i++;
		}
	}
	for (uint32_t i = 0; i < kMaxChannels; i++)
		bo->snapshot[i] = chans_[i] ? tgm_chan_submitted(chans_[i]) : 0;
	bo->next = zombies_;
	zombies_ = bo;
	reap(false);
	tgm_stats_bo_free(nowNs() - start);
	return kIOReturnSuccess;
}

// Binds are tracked whole. A Map or Unmap that overlaps a recorded bind removes that whole bind first (NVK binds and
// unbinds whole VA ranges); this keeps nvkm's raw get/put page-table references balanced.
void AGXDeviceUserClient::unbindOverlaps(uint64_t va, uint64_t range, Bo *only)
{
	for (uint32_t i = 0; i < nbinds_;) {
		const Binding &b = binds_[i];
		if (b.va < va + range && va < b.va + b.range && (!only || b.bo == only)) {
			tgm_vm_unmap_pages(vm_, b.va, b.range, b.pageShift);
			binds_[i] = binds_[--nbinds_];
		} else {
			i++;
		}
	}
}

IOReturn AGXDeviceUserClient::vmBind(const TegraGM20BAbiVmBind *in, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		const TegraGM20BAbiBindOp &op = in->ops[i];
		if (((op.va | op.range | op.bo_offset) & 0xfff) || !op.range ||
		    op.va < TGM_USER_VA_START || op.range > TGM_KERNEL_VA ||
		    op.va > TGM_KERNEL_VA - op.range || op.kind > 0xff)
			return kIOReturnBadArgument;
		if (op.op == TegraGM20BBindMapSparse)
			return kIOReturnUnsupported;  // InfoSparse is not advertised in v1
		const uint64_t start = nowNs();
		if (op.op == TegraGM20BBindUnmap) {
			unbindOverlaps(op.va, op.range, nullptr);
			tgm_stats_bind(0, op.range >> 12, nowNs() - start);
			continue;
		}
		if (op.op != TegraGM20BBindMap)
			return kIOReturnBadArgument;
		Bo *bo = findBo(op.handle);
		if (!bo || op.bo_offset > bo->bytes || op.range > bo->bytes - op.bo_offset)
			return kIOReturnBadArgument;
		unbindOverlaps(op.va, op.range, nullptr);
		if (nbinds_ == cap_binds_) {
			const uint32_t cap = cap_binds_ ? cap_binds_ * 2 : 64;
			Binding *grown = IONew(Binding, cap);
			if (!grown)
				return kIOReturnNoMemory;
			if (binds_) {
				memcpy(grown, binds_, sizeof(Binding) * nbinds_);
				IODelete(binds_, Binding, cap_binds_);
			}
			binds_ = grown;
			cap_binds_ = cap;
		}
		uint8_t pageShift = 0;
		const int err = tgm_vm_map_ordered(vm_, op.va, op.range, bo->mem, op.bo_offset,
						   static_cast<uint8_t>(op.kind),
						   static_cast<int>(bo->readOnly), &pageShift);
		if (err)
			return ioreturn(err);
		binds_[nbinds_++] = {op.va, op.range, bo, op.bo_offset, pageShift};
		tgm_stats_bind(1, op.range >> 12, nowNs() - start);
	}
	return kIOReturnSuccess;
}

// The decision may tile the surface on the copy engine and sleep on its fence; the connection's lock is held meanwhile,
// as for a BoImport's wiring.
IOReturn AGXDeviceUserClient::surfaceLayout(const TegraGM20BAbiSurfaceLayout *in,
					    TegraGM20BAbiSurfaceLayoutReply *out)
{
	const Bo *bo = findBo(in->handle);
	if (!bo || !bo->wired || !bo->coh)
		return kIOReturnBadArgument;
	const bool writes = ((in->flags & TegraGM20BLayoutWrites) != 0u) &&
			    (tgm_surface_client_memory(tgm_coh_surface_id(bo->coh), task_) == 0);
	const int layout = tgm_coherency_layout(bo->coh, in->width, in->height, in->bpp,
						in->bytes_per_row, static_cast<int>(writes));
	out->layout = 0;
	if (layout > 0) {
		out->layout = TegraGM20BLayoutTiled;
	} else if (layout < 0) {
		out->layout = TegraGM20BLayoutOtherShape;
	}
	out->reserved = 0;
	return kIOReturnSuccess;
}

// lock_ held. The connection's level from its priority and GPU denial (TegraGM20BSchedPolicy.h), for every channel it has.
void AGXDeviceUserClient::applyLevel()
{
	level_ = tgm_priority_level(priority_, backgroundPriority_,
				    static_cast<int>(backgroundRole_));
	critical_ = level_ == TGM_LEVEL_HIGH;
	for (auto &chan : chans_) {
		if (chan)
			tgm_chan_set_level(chan, level_);
	}
}

IOReturn AGXDeviceUserClient::channelCreate(const TegraGM20BAbiChannelCreate *in,
					    TegraGM20BAbiChannelCreateReply *out)
{
	(void)in;  // engines is advisory: every channel is a full GR channel (CONTRACT §2)
	uint32_t slot = 0;
	while (slot < kMaxChannels && chans_[slot])
		slot++;
	if (slot == kMaxChannels)
		return kIOReturnNoResources;
	// not reset: a slot's seqno only moves forward (tgm_chan_new continues it)
	volatile uint32_t *fence = fence_va_ + static_cast<size_t>(slot) * 4;
	tgm_fence_kill_clear(fence);
	const IOReturn ret = ioreturn(
		tgm_chan_new(vm_, this, fence, fence_gpu_ + static_cast<uint64_t>(slot) * 16,
			     owner_, static_cast<int>(critical_), &chans_[slot]));
	if (ret)
		return ret;
	tgm_chan_set_group(chans_[slot], task_);  // Order relates the channels of one task
	if (level_ != TGM_LEVEL_FOREGROUND && !critical_)
		tgm_chan_set_level(chans_[slot], level_);
	out->channel = slot + 1;
	out->fence_token = kFenceToken;
	out->fence_offset = slot * 16;
	out->reserved = 0;
	return kIOReturnSuccess;
}

// Method 14: GPU-side order between this channel and the task's other channels (gpu.c tgm_chan_order); the peers are found by
// task in gpu.c, only the caller's own channel is named here.
IOReturn AGXDeviceUserClient::order(const TegraGM20BAbiOrder *in, TegraGM20BAbiOrderReply *out)
{
	static_assert(TegraGM20BOrderAfterTask == TGM_ORDER_AFTER &&
			      TegraGM20BOrderBeforeTask == TGM_ORDER_BEFORE &&
			      TegraGM20BMaxOrderWaits == TGM_ORDER_MAX,
		      "order flags");
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	uint32_t queued = 0;
	const IOReturn ret =
		ioreturn(tgm_chan_order(chans_[in->channel - 1], in->flags, in->seqno, &queued));
	out->waits = queued;
	out->reserved = 0;
	return ret;
}

// Method 17: what the channel's next Exec touches (TegraGM20BExecSurfaces.h).
IOReturn AGXDeviceUserClient::execSurfaces(const TegraGM20BAbiExecSurfaces *in)
{
	static_assert(tgm_fences::kChannels == kMaxChannels, "channel slots");
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	return surfaces_.set(in->channel - 1, in) ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn AGXDeviceUserClient::channelDestroy(const TegraGM20BAbiChannel *in)
{
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	hangHolder_ = current_thread();
	tgm_chan_del(&chans_[in->channel - 1]);
	hangHolder_ = nullptr;
	surfaces_.forget(in->channel - 1);
	reap(false);
	return kIOReturnSuccess;
}

// The kernel's view of the push at [va, va + bytes) through the binding that holds it (`hit`: the last one found, tried
// first): pool BOs write-combined, cached BOs their kernel mapping. Null for a host import (no kernel mapping) or a system
// coherent BO (its task writes it write-combined, and a read through its cacheable kernel mapping could fill lines a later
// read would find stale). lock_ held.
const uint32_t *AGXDeviceUserClient::pushView(uint64_t va, uint64_t bytes,
					      const Binding *&hit) const
{
	if (!hit || va < hit->va || va + bytes > hit->va + hit->range) {
		hit = nullptr;
		for (uint32_t b = 0; b < nbinds_ && !hit; b++) {
			if (va >= binds_[b].va && va + bytes <= binds_[b].va + binds_[b].range)
				hit = &binds_[b];
		}
	}
	const void *base = nullptr;
	if (hit && !hit->bo->system) {
		if (hit->bo->pool_va) {
			base = hit->bo->pool_va;
		} else if (hit->bo->bmd) {
			base = hit->bo->bmd->getBytesNoCopy();
		}
	}
	return base ? reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(base) +
							 hit->bo_offset + (va - hit->va))
		    : nullptr;
}

// A hang's line (gpu.c chan_hung_note): the hung submission's pushes read as the stats read them, for its interlocked
// states. From a GPU panic path on any thread: the binds are read under lock_, taken only if it is free (a thread that
// holds it while a GPU wait hangs would never let it go), unless this thread is the one holding it (hangHolder_).
int AGXDeviceUserClient::hungInterlocks(const uint64_t *va, const uint32_t *dwords, uint32_t n,
					uint64_t *interlocks)
{
	const bool held = hangHolder_ == current_thread();
	if (!held && !IOLockTryLock(lock_))
		return -1;
	PushCounts c = {};
	PushParse p = {};
	const Binding *hit = nullptr;
	int read = 0;
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t *words = pushView(va[i], static_cast<uint64_t>(dwords[i]) * 4, hit);
		if (!words) {
			p = {};  // a method left open by the push not read ends with it
			continue;
		}
		tgm_scan::pushParse(c, p, words, dwords[i]);
		read++;
	}
	if (!held)
		IOLockUnlock(lock_);
	*interlocks = c.interlocks;
	return read;
}

extern "C" int tgm_client_interlocks(const void *client, const uint64_t *va, const uint32_t *dwords,
				     uint32_t n, uint64_t *interlocks)
{
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the C caller holds the client as const void *
	return static_cast<AGXDeviceUserClient *>(const_cast<void *>(client))
		->hungInterlocks(va, dwords, n, interlocks);
}

// The stats (see pushScanOn): count what this submission's pushes ask of the GPU, through the kernel's view of the
// BOs they lie in (pool BOs write-combined, cached BOs their kernel mapping); every 5 s the window's line.
void AGXDeviceUserClient::scanPushes(const tgm_push *pushes, uint32_t count, tgm_prof *prof)
{
	if (!TGM_STATS)
		return;
	PushCounts c = {};
	PushParse p = {};
	const Binding *hit = nullptr;
	uint64_t budget = kScanMaxBytes;
	bool whole = true;
	for (uint32_t i = 0; i < count; i++) {
		if (prof && i < TGM_PROF_MAX) {
			prof->cls[i] = TGM_PC_SKIP;
			prof->draws[i] = prof->sampleDraws[i] = 0;
		}
		const uint64_t va = pushes[i].va;
		const uint64_t bytes = static_cast<uint64_t>(pushes[i].dwords) * 4;
		c.bytes += bytes;
		const uint32_t *words = pushView(va, bytes, hit);
		if (!words || bytes > budget) {
			scanPush(c, p, whole, nullptr, 0);  // not read: no later push is stamped
			continue;
		}
		budget -= bytes;
		c.readBytes += bytes;
		const uint64_t drawsBefore = c.draws;
		const uint64_t sampleBefore = c.sampleDraws;
		const uint8_t cls = scanPush(c, p, whole, words, bytes / 4);
		if (prof && i < TGM_PROF_MAX) {
			prof->cls[i] = cls;
			const uint64_t draws = c.draws - drawsBefore;
			const uint64_t sample = c.sampleDraws - sampleBefore;
			prof->draws[i] = static_cast<uint16_t>(draws > 0xffff ? 0xffff : draws);
			prof->sampleDraws[i] =
				static_cast<uint16_t>(sample > 0xffff ? 0xffff : sample);
		}
	}
	c.scanned = 1;
	c.submits = kScanEvery;  // this connection's submissions since its last one read
	if (!gPushLock) {
		IOLock *fresh = IOLockAlloc();
		// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap writes the current value back into it
		IOLock *expected = nullptr;
		if (!__atomic_compare_exchange_n(&gPushLock, &expected, fresh, false,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			IOLockFree(fresh);
		if (!gPushLock)
			return;
	}
	const uint64_t now = nowNs();
	// static: only the submission that ends a window (one per 5 s, chosen under the lock) logs from them
	static PushOwner window[kPushOwners];
	static char line[700];
	bool log = false;
	IOLockLock(gPushLock);
	PushOwner *o = nullptr;
	for (uint32_t i = 0; i < kPushOwners - 1 && !o; i++) {
		if (!gPushOwners[i].c.scanned ||
		    !strncmp(gPushOwners[i].name, owner_, sizeof(gPushOwners[i].name) - 1))
			o = &gPushOwners[i];
	}
	if (!o) {  // more processes than lines this window: the last line takes the rest
		o = &gPushOwners[kPushOwners - 1];
		strlcpy(o->name, "(others)", sizeof(o->name));
	} else if (!o->c.scanned) {
		strlcpy(o->name, owner_, sizeof(o->name));
	}
	for (size_t k = 0; k < sizeof(c) / sizeof(uint64_t); k++)
		reinterpret_cast<uint64_t *>(&o->c)[k] += reinterpret_cast<const uint64_t *>(&c)[k];
	if (!gPushSince) {
		gPushSince = now;
	} else if (now - gPushSince >= kScanWindowNs) {
		gPushSince = now;
		memcpy(window, gPushOwners, sizeof(window));
		bzero(gPushOwners, sizeof(gPushOwners));
		log = true;
	}
	IOLockUnlock(gPushLock);
	if (!log)
		return;
	for (auto &i : window) {
		const PushCounts &w = i.c;
		if (!w.scanned)
			continue;
		const uint64_t n = w.scanned;
		const uint64_t per[] = {w.bytes >> 10, w.passes,           w.depthPasses,
					w.msaaPasses,  w.targets,          w.linearTargets,
					w.draws,       w.clears,           w.wfi,
					w.shaderInval, w.shaderInvalNoWfi, w.texInval,
					w.setRef,      w.l2Inval,          w.hostWfi,
					w.ceCopies,    w.ceBytes >> 10,    w.ceWfi,
					w.blits,       w.dispatches,       w.acquires,
					w.psBarriers,  w.interlocks};
		constexpr size_t kFields = sizeof(per) / sizeof(per[0]);
		char v[kFields][24];
		for (size_t k = 0; k < kFields; k++)
			snprintf(v[k], sizeof(v[k]), "%llu.%llu", per[k] / n, per[k] * 10 / n % 10);
		// one string argument: os_log drops a message whose scalar arguments outgrow its buffer (gpu.c stats_tick)
		snprintf(line, sizeof(line),
			 "%s: %llu of %llu submits read, %s KiB each (%llu%% readable); per "
			 "submit: %s passes "
			 "(%s depth, %s MSAA), %s RTs (%s pitch), %s draws, %s clears; %s WFI, %s "
			 "shinv+WFI, %s "
			 "shinv, %s texinv, %s setref, %s L2 inv, %s host WFI; %s CE (%s KiB), %s "
			 "CE WFI, %s 2D, "
			 "%s disp, %s acq; %s PS barrier, %s interlock; %llu bad",
			 i.name, w.scanned, w.submits, v[0],
			 w.bytes ? w.readBytes * 100 / w.bytes : 0, v[1], v[2], v[3], v[4], v[5],
			 v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16],
			 v[17], v[18], v[19], v[20], v[21], v[22], w.unparsed);
		os_log(OS_LOG_DEFAULT, "tegra-gm20b: push %s", line); /* log only */
	}
}

IOReturn AGXDeviceUserClient::exec(const TegraGM20BAbiExec *in, uint32_t count,
				   TegraGM20BAbiExecReply *out)
{
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	tgm_chan *chan = chans_[in->channel - 1];
	// runningboardd's GPU denial, as IOKit reads it for kIOUCForegroundOnly methods (task_is_gpu_denied), at each
	// submission: a denied process runs at its background priority
	const bool background = clientHasPrivilege(current_task(), kIOClientPrivilegeForeground) !=
				kIOReturnSuccess;
	if (background != backgroundRole_) {
		backgroundRole_ = background;
		applyLevel();
	}
	// Taken whatever happens to the Exec. The writers' acquires are queued for this submission's stream before it is made
	// (one this Exec then does not make leaves them to the channel's next: each still names earlier work).
	const bool listed = surfaces_.before(in->channel - 1, chan, count);
	// TegraGM20BAbiPush and tgm_push share one layout; `in` is our private copy, so normalise flags in place
	static_assert(sizeof(tgm_push) == sizeof(TegraGM20BAbiPush) &&
			      offsetof(tgm_push, no_prefetch) == offsetof(TegraGM20BAbiPush, flags),
		      "push layouts");
	// `in` is our private copy (above).
	// NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
	tgm_push *pushes =
		reinterpret_cast<tgm_push *>(const_cast<TegraGM20BAbiPush *>(in->pushes));
	// NOLINTEND(cppcoreguidelines-pro-type-const-cast)
	for (uint32_t i = 0; i < count; i++) {
		const TegraGM20BAbiPush &p = in->pushes[i];
		if ((p.va & 3) || !p.dwords || p.dwords >= (1u << 21) ||
		    (p.flags & ~static_cast<uint32_t>(TegraGM20BPushNoPrefetch |
						      TegraGM20BPushImportsWritten)) ||
		    p.va < TGM_USER_VA_START ||
		    p.va + static_cast<uint64_t>(p.dwords) * 4 > TGM_KERNEL_VA)
			return kIOReturnBadArgument;
		if (p.flags & TegraGM20BPushImportsWritten)
			importsWritten_ = true;
		pushes[i].no_prefetch = (p.flags & TegraGM20BPushNoPrefetch) ? 1u : 0u;
	}
	tgm_prof prof;
	bool profiled = false;
	if (count && pushScanOn() && pushSubmits_++ % kScanEvery == 0) {
		profiled = count <= TGM_PROF_MAX && pushScans_++ % kProfileEvery == 0;
		scanPushes(pushes, count, profiled ? &prof : nullptr);
	}
	uint32_t seqno = 0;
	const IOReturn ret =
		ioreturn(tgm_chan_exec(chan, pushes, count, &seqno, profiled ? &prof : nullptr));
	reap(false);
	if (ret)
		return ret;
	if (count) {
		// The host imports this submission may have written, marked GPU-dirty with the submission as their writer
		// (atomic stores, no cache maintenance): a CPU IOSurfaceLock of one waits for this seqno, then invalidates
		// (tgm_coherency_*, the XNU hooks), and a listed Exec of another channel waits for it on the GPU.
		surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);
	}
	out->seqno = seqno;
	out->reserved = 0;
	return kIOReturnSuccess;
}

// Sleeps without the connection lock (a waiter must not block submitters), so the channel can be destroyed meanwhile:
// it is looked up again, under the lock, before every look at it.
IOReturn AGXDeviceUserClient::waitFence(const TegraGM20BAbiWaitFence *in)
{
	if (!in->channel || in->channel > kMaxChannels)
		return kIOReturnBadArgument;
	const uint64_t limit_ns = in->timeout_ns == UINT64_MAX ? 10000000000ULL : in->timeout_ns;
	uint64_t start = nowNs();
	uint64_t deadline = limit_ns > UINT64_MAX - start ? UINT64_MAX : start + limit_ns;
	uint32_t waiter = 0;
	uint32_t seq = 0;
	bool woken = false;
	for (bool slept = false;; slept = true) {
		const uint32_t sleptOn = waiter;
		IOReturn ret = kIOReturnNotReady;
		IOLockLock(lock_);
		tgm_chan *chan = chans_[in->channel - 1];
		if (chan) {
			waiter = tgm_chan_waiter(chan);
			seq = tgm_waiter_seq(waiter);
		}
		const uint64_t now = nowNs();
		if (!chan) {
			ret = slept ? kIOReturnNotResponding : kIOReturnBadArgument;
		} else if (tgm_chan_error(chan)) {
			ret = kIOReturnNotResponding;
		} else if (static_cast<int32_t>(tgm_chan_completed(chan) - in->seqno) >= 0) {
			ret = kIOReturnSuccess;
		} else if (in->timeout_ns == UINT64_MAX && now - start >= limit_ns) {
			// CONTRACT: UINT64_MAX means 10 s; then what the GPU is stuck on is killed (this channel's error
			// ends the wait) and the wait goes on
			hangHolder_ = current_thread();
			tgm_chan_stalled(chan, "fence wait", in->seqno, now - start);
			hangHolder_ = nullptr;
			start = now;
			deadline = now + limit_ns;
		} else if (!limit_ns || now - start >= limit_ns) {
			hangHolder_ = current_thread();
			if (limit_ns)
				tgm_chan_check_progress(chan, "fence wait", in->seqno);
			hangHolder_ = nullptr;
			ret = kIOReturnTimeout;
		}
		IOLockUnlock(lock_);
		if (slept) {
			tgm_stats_wait(sleptOn, static_cast<int>(woken),
				       static_cast<int>(ret == kIOReturnSuccess));
		}
		if (ret != kIOReturnNotReady)
			return ret;
		woken = (tgm_waiter_idle(waiter, seq, deadline) != 0);
		if (!woken) {
			IOLockLock(lock_);
			chan = chans_[in->channel - 1];
			const bool unwoken =
				(chan != nullptr) && tgm_chan_waiter(chan) == waiter &&
				static_cast<int32_t>(tgm_chan_completed(chan) - in->seqno) >= 0;
			IOLockUnlock(lock_);
			if (unwoken)
				tgm_waiter_unwoken(waiter, seq, in->seqno);
		}
	}
}

IOReturn AGXDeviceUserClient::getStatus(const TegraGM20BAbiChannel *in, TegraGM20BAbiStatus *out)
{
	if (!in->channel || in->channel > kMaxChannels || !chans_[in->channel - 1])
		return kIOReturnBadArgument;
	tgm_chan *chan = chans_[in->channel - 1];
	bzero(out, sizeof(*out));
	out->completed = tgm_chan_completed(chan);
	out->submitted = tgm_chan_submitted(chan);
	out->error = tgm_chan_error(chan);
	out->fault_va = tgm_chan_fault_va(chan);
	return kIOReturnSuccess;
}

// CPU mappings (methods 12, 13): what io_connect_map_memory_into_task and io_connect_unmap_memory_from_task do for a
// task mapping its own connection's memory (IOUserClient::mapClientMemory64 over clientMemoryForType, the same options,
// the map kept until unmapped or the connection closes), reached through io_connect_method, which WebKit's GPU process
// profile admits where it refuses those two MIG routines (21H461 syscall-mig list). The same kernel work as
// IOConnectMapMemory64: nothing is slower.
IOReturn AGXDeviceUserClient::mapToken(const TegraGM20BAbiMap *in, TegraGM20BAbiMapReply *out)
{
	const uint32_t known = TegraGM20BMapReadOnly | TegraGM20BMapFixed;
	const bool fixed = (in->flags & TegraGM20BMapFixed) != 0u;
	if (!in->token || (in->flags & ~known) || fixed != (in->address != 0))
		return kIOReturnBadArgument;
	if (current_task() != task_)
		return kIOReturnNotPermitted;  // only the opener maps its connection's memory, into itself
	Mapping *mapping = IONew(Mapping, 1);
	if (!mapping)
		return kIOReturnNoMemory;
	IOOptionBits options = fixed ? kIOMapOverwrite : kIOMapAnywhere;
	if (in->flags & TegraGM20BMapReadOnly)
		options |= kIOMapReadOnly;
	// clientMemoryForType takes lock_: called without it, as IOKit's own map path calls it
	IOMemoryMap *map = mapClientMemory64(in->token, task_, options, in->address);
	if (!map) {
		IODelete(mapping, Mapping, 1);
		return kIOReturnBadArgument;  // IOConnectMapMemory64's answer
	}
	out->address = map->getAddress();
	out->bytes = map->getSize();
	IOLockLock(lock_);
	*mapping = {maps_, map, in->token, out->address};
	maps_ = mapping;
	IOLockUnlock(lock_);
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::unmapToken(const TegraGM20BAbiUnmap *in)
{
	Mapping *found = nullptr;
	IOLockLock(lock_);
	for (Mapping **p = &maps_; *p; p = &(*p)->next) {
		if ((*p)->token == in->token && (*p)->address == in->address) {
			found = *p;
			*p = found->next;
			break;
		}
	}
	IOLockUnlock(lock_);
	if (!found)
		return kIOReturnBadArgument;
	found->map->unmap();
	found->map->release();
	IODelete(found, Mapping, 1);
	return kIOReturnSuccess;
}

// Every mapping method 12 made, unmapped (close and free: the task's mappings go with its connection).
void AGXDeviceUserClient::releaseMappings()
{
	IOLockLock(lock_);
	Mapping *list = maps_;
	maps_ = nullptr;
	IOLockUnlock(lock_);
	while (list) {
		Mapping *next = list->next;
		list->map->unmap();
		list->map->release();
		IODelete(list, Mapping, 1);
		list = next;
	}
}

IOReturn AGXDeviceUserClient::getInfo(TegraGM20BAbiInfo *out)
{
	tgm_info info;
	tgm_get_info(&info);
	bzero(out, sizeof(*out));
	out->abi_version = TegraGM20BAbiVersion;
	out->flags = static_cast<uint32_t>(TegraGM20BInfoHostImport) | TegraGM20BInfoKernelMap |
		     TegraGM20BInfoOrder | TegraGM20BInfoSurfaceWrites |
		     TegraGM20BInfoSurfaceLayout | TegraGM20BInfoExecSurfaces |
		     TegraGM20BInfoPriority | TegraGM20BInfoKillRecord;
	if (info.comptag_lines && switch_gpu_compression_on())
		out->flags |= TegraGM20BInfoCompression;
	if (switch_tiled_cache_on())
		out->flags |= TegraGM20BInfoTiledCache;
	out->chipset = info.chipset;
	out->device_id = 0;
	out->gpc_count = info.gpc_count;
	out->tpc_count = info.tpc_count;
	out->cls_copy = 0xb0b5;
	out->cls_eng2d = 0x902d;
	out->cls_eng3d = 0xb197;
	out->cls_m2mf = 0xa140;
	out->cls_compute = 0xb1c0;
	out->cls_gpfifo = 0xb06f;
	out->va_start = TGM_USER_VA_START;
	out->va_end = TGM_KERNEL_VA;
	out->page_size = 4096;
	out->big_page_size = 0x20000;
	out->max_bo_bytes = kMaxBoBytes;
	strlcpy(out->chip_name, "GM20B", sizeof(out->chip_name));
	strlcpy(out->device_name, "NVIDIA Tegra X1 (GM20B)", sizeof(out->device_name));
	return kIOReturnSuccess;
}

static IOReturn zbcAdd(const TegraGM20BAbiZbc *in, TegraGM20BAbiZbcReply *out)
{
	if (in->type != TegraGM20BZbcColor && in->type != TegraGM20BZbcDepth)
		return kIOReturnBadArgument;
	const int index = tgm_zbc_add(static_cast<int>(in->type == TegraGM20BZbcDepth), in->format,
				      in->ds, in->l2);
	if (index < 0)
		return ioreturn(index);
	out->index = static_cast<uint32_t>(index);
	out->reserved = 0;
	return kIOReturnSuccess;
}

template <typename T> static bool reservedZero(const T &v)
{
	return v == 0;
}

IOReturn AGXDeviceUserClient::run(uint32_t method, const void *in, uint32_t inSize, void *out,
				  const uint32_t *outSize)
{
	// exact record sizes; VmBind/Exec also accept just `count` entries
	uint32_t wantIn = 0;
	uint32_t wantOut = 0;
	uint32_t count = 0;
	switch (method) {
	case TegraGM20BMethodGetInfo:
		wantOut = sizeof(TegraGM20BAbiInfo);
		break;
	case TegraGM20BMethodBoAlloc:
		wantIn = sizeof(TegraGM20BAbiBoAlloc);
		wantOut = sizeof(TegraGM20BAbiBoAllocReply);
		break;
	case TegraGM20BMethodBoImport:
		wantIn = sizeof(TegraGM20BAbiBoImport);
		wantOut = sizeof(TegraGM20BAbiBoAllocReply);
		break;
	case TegraGM20BMethodBoFree:
		wantIn = sizeof(TegraGM20BAbiBoFree);
		break;
	case TegraGM20BMethodVmBind:
		if (inSize < offsetof(TegraGM20BAbiVmBind, ops))
			return kIOReturnBadArgument;
		count = static_cast<const TegraGM20BAbiVmBind *>(in)->count;
		if (!count || count > TegraGM20BMaxBindOps)
			return kIOReturnBadArgument;
		wantIn = inSize == sizeof(TegraGM20BAbiVmBind)
				 ? inSize
				 : static_cast<uint32_t>(offsetof(TegraGM20BAbiVmBind, ops) +
							 count * sizeof(TegraGM20BAbiBindOp));
		break;
	case TegraGM20BMethodChannelCreate:
		wantIn = sizeof(TegraGM20BAbiChannelCreate);
		wantOut = sizeof(TegraGM20BAbiChannelCreateReply);
		break;
	case TegraGM20BMethodChannelDestroy:
		wantIn = sizeof(TegraGM20BAbiChannel);
		break;
	case TegraGM20BMethodExec:
		if (inSize < offsetof(TegraGM20BAbiExec, pushes))
			return kIOReturnBadArgument;
		count = static_cast<const TegraGM20BAbiExec *>(in)->count;
		if (count > TegraGM20BMaxPushes)
			return kIOReturnBadArgument;
		wantIn = inSize == sizeof(TegraGM20BAbiExec)
				 ? inSize
				 : static_cast<uint32_t>(offsetof(TegraGM20BAbiExec, pushes) +
							 count * sizeof(TegraGM20BAbiPush));
		wantOut = sizeof(TegraGM20BAbiExecReply);
		break;
	case TegraGM20BMethodWaitFence:
		wantIn = sizeof(TegraGM20BAbiWaitFence);
		break;
	case TegraGM20BMethodGetTimestamp:
		wantOut = sizeof(TegraGM20BAbiTimestamp);
		break;
	case TegraGM20BMethodGetStatus:
		wantIn = sizeof(TegraGM20BAbiChannel);
		wantOut = sizeof(TegraGM20BAbiStatus);
		break;
	case TegraGM20BMethodMap:
		wantIn = sizeof(TegraGM20BAbiMap);
		wantOut = sizeof(TegraGM20BAbiMapReply);
		break;
	case TegraGM20BMethodUnmap:
		wantIn = sizeof(TegraGM20BAbiUnmap);
		break;
	case TegraGM20BMethodOrder:
		wantIn = sizeof(TegraGM20BAbiOrder);
		wantOut = sizeof(TegraGM20BAbiOrderReply);
		break;
	case TegraGM20BMethodSurfaceWrites:
		wantIn = sizeof(TegraGM20BAbiSurfaceWrites);
		break;
	case TegraGM20BMethodSurfaceLayout:
		wantIn = sizeof(TegraGM20BAbiSurfaceLayout);
		wantOut = sizeof(TegraGM20BAbiSurfaceLayoutReply);
		break;
	case TegraGM20BMethodExecSurfaces:
		if (inSize < offsetof(TegraGM20BAbiExecSurfaces, uses))
			return kIOReturnBadArgument;
		count = static_cast<const TegraGM20BAbiExecSurfaces *>(in)->count;
		if (count > TegraGM20BMaxExecSurfaces)
			return kIOReturnBadArgument;
		wantIn = inSize == sizeof(TegraGM20BAbiExecSurfaces)
				 ? inSize
				 : static_cast<uint32_t>(offsetof(TegraGM20BAbiExecSurfaces, uses) +
							 count * sizeof(TegraGM20BAbiSurfaceUse));
		break;
	case TegraGM20BMethodPriority:
		wantIn = sizeof(TegraGM20BAbiPriority);
		break;
	case TegraGM20BMethodZbc:
		wantIn = sizeof(TegraGM20BAbiZbc);
		wantOut = sizeof(TegraGM20BAbiZbcReply);
		break;
	default:
		return kIOReturnUnsupported;
	}
	if (inSize != wantIn || *outSize != wantOut || (wantIn && !in) || (wantOut && !out))
		return kIOReturnBadArgument;
	if (tgm_stage() < 2)
		return kIOReturnNotReady;

	switch (method) {
	case TegraGM20BMethodBoImport:
		if (static_cast<const TegraGM20BAbiBoImport *>(in)->flags &
		    ~static_cast<uint32_t>(TegraGM20BImportGpuReads))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodSurfaceWrites:
		if (!reservedZero(static_cast<const TegraGM20BAbiSurfaceWrites *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodSurfaceLayout:
		if (!reservedZero(static_cast<const TegraGM20BAbiSurfaceLayout *>(in)->reserved) ||
		    (static_cast<const TegraGM20BAbiSurfaceLayout *>(in)->flags &
		     ~static_cast<uint32_t>(TegraGM20BLayoutWrites)))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodVmBind:
		if (!reservedZero(static_cast<const TegraGM20BAbiVmBind *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodChannelCreate:
		if (!reservedZero(static_cast<const TegraGM20BAbiChannelCreate *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodChannelDestroy:
	case TegraGM20BMethodGetStatus:
		if (!reservedZero(static_cast<const TegraGM20BAbiChannel *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodUnmap:
		if (!reservedZero(static_cast<const TegraGM20BAbiUnmap *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodOrder:
		if (!reservedZero(static_cast<const TegraGM20BAbiOrder *>(in)->reserved))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodExecSurfaces:
		if (!tgm_fences::ExecSurfaces::valid(
			    static_cast<const TegraGM20BAbiExecSurfaces *>(in)))
			return kIOReturnBadArgument;
		break;
	case TegraGM20BMethodPriority:
		if (static_cast<const TegraGM20BAbiPriority *>(in)->priority >
			    TegraGM20BMaxPriority ||
		    static_cast<const TegraGM20BAbiPriority *>(in)->background_priority >
			    TegraGM20BMaxPriority)
			return kIOReturnBadArgument;
		break;
	default:
		break;
	}

	if (method == TegraGM20BMethodWaitFence)
		return waitFence(static_cast<const TegraGM20BAbiWaitFence *>(in));
	// Map and Unmap take lock_ themselves (Map's clientMemoryForType must run without it).
	if (method == TegraGM20BMethodMap) {
		return mapToken(static_cast<const TegraGM20BAbiMap *>(in),
				static_cast<TegraGM20BAbiMapReply *>(out));
	}
	if (method == TegraGM20BMethodUnmap)
		return unmapToken(static_cast<const TegraGM20BAbiUnmap *>(in));
	if (method == TegraGM20BMethodGetTimestamp) {
		static_cast<TegraGM20BAbiTimestamp *>(out)->ns = tgm_timestamp_ns();
		return kIOReturnSuccess;
	}
	if (method == TegraGM20BMethodZbc) {
		return zbcAdd(static_cast<const TegraGM20BAbiZbc *>(in),
			      static_cast<TegraGM20BAbiZbcReply *>(out));
	}
	IOLockLock(lock_);
	// A channel killed for a GPU fault or hang (client fault isolation) refuses only what names it (Exec, Order and
	// WaitFence answer kIOReturnNotResponding, GetStatus its error); the connection's memory, binds and other channels
	// go on, and ChannelCreate makes it a new one, as a GPU restart leaves a Metal device on iOS.
	IOReturn ret = 0;
	switch (method) {
	case TegraGM20BMethodGetInfo:
		ret = getInfo(static_cast<TegraGM20BAbiInfo *>(out));
		break;
	case TegraGM20BMethodBoAlloc:
		ret = boAlloc(static_cast<const TegraGM20BAbiBoAlloc *>(in),
			      static_cast<TegraGM20BAbiBoAllocReply *>(out));
		break;
	case TegraGM20BMethodBoImport:
		ret = boImport(static_cast<const TegraGM20BAbiBoImport *>(in),
			       static_cast<TegraGM20BAbiBoAllocReply *>(out));
		break;
	case TegraGM20BMethodBoFree:
		ret = boFree(static_cast<const TegraGM20BAbiBoFree *>(in));
		break;
	case TegraGM20BMethodVmBind:
		ret = vmBind(static_cast<const TegraGM20BAbiVmBind *>(in), count);
		break;
	case TegraGM20BMethodChannelCreate:
		ret = channelCreate(static_cast<const TegraGM20BAbiChannelCreate *>(in),
				    static_cast<TegraGM20BAbiChannelCreateReply *>(out));
		break;
	case TegraGM20BMethodChannelDestroy:
		ret = channelDestroy(static_cast<const TegraGM20BAbiChannel *>(in));
		break;
	case TegraGM20BMethodExec:
		ret = exec(static_cast<const TegraGM20BAbiExec *>(in), count,
			   static_cast<TegraGM20BAbiExecReply *>(out));
		break;
	case TegraGM20BMethodGetStatus:
		ret = getStatus(static_cast<const TegraGM20BAbiChannel *>(in),
				static_cast<TegraGM20BAbiStatus *>(out));
		break;
	case TegraGM20BMethodOrder:
		ret = order(static_cast<const TegraGM20BAbiOrder *>(in),
			    static_cast<TegraGM20BAbiOrderReply *>(out));
		break;
	case TegraGM20BMethodSurfaceWrites:
		tgm_coherency_gpu_writes(
			static_cast<const TegraGM20BAbiSurfaceWrites *>(in)->surface_id);
		ret = kIOReturnSuccess;
		break;
	case TegraGM20BMethodSurfaceLayout:
		ret = surfaceLayout(static_cast<const TegraGM20BAbiSurfaceLayout *>(in),
				    static_cast<TegraGM20BAbiSurfaceLayoutReply *>(out));
		break;
	case TegraGM20BMethodExecSurfaces:
		ret = execSurfaces(static_cast<const TegraGM20BAbiExecSurfaces *>(in));
		break;
	case TegraGM20BMethodPriority:
		priority_ = static_cast<const TegraGM20BAbiPriority *>(in)->priority;
		backgroundPriority_ =
			static_cast<const TegraGM20BAbiPriority *>(in)->background_priority;
		applyLevel();
		ret = kIOReturnSuccess;
		break;
	default:
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(lock_);
	return ret;
}

IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector,
					     IOExternalMethodArguments *arguments,
					     IOExternalMethodDispatch *dispatch, OSObject *target,
					     void *reference)
{
	(void)dispatch;
	(void)target;
	(void)reference;
	if (!arguments || arguments->structureInputDescriptor ||
	    arguments->structureOutputDescriptor || arguments->scalarInputCount ||
	    arguments->scalarOutputCount)
		return kIOReturnBadArgument;  // every record is inline (<= 4096 bytes)
	const uint8_t *in = static_cast<const uint8_t *>(arguments->structureInput);
	uint32_t inSize = arguments->structureInputSize;
	const uint32_t outSize = arguments->structureOutputSize;
	uint32_t method = selector;
	// tunnel: {magic, method, record...} on any selector the sandbox admits
	if (in && inSize >= 8 &&
	    reinterpret_cast<const uint32_t *>(in)[0] == TegraGM20BTunnelMagic) {
		method = reinterpret_cast<const uint32_t *>(in)[1];
		if (method == TegraGM20BTunnelProbe)
			return kIOReturnSuccess;
		in += 8;
		inSize -= 8;
	}
	// copy the input: records are naturally aligned structs and the caller's buffer may not be. Small records use the
	// stack; VmBind/Exec (up to 4 KiB) a heap buffer, keeping kernel stack use small.
	uint64_t small[64];
	void *copy = small;
	if (inSize > TegraGM20BMaxPushes * sizeof(TegraGM20BAbiPush) + 8)
		return kIOReturnBadArgument;
	if (inSize > sizeof(small)) {
		copy = IOMallocData(inSize);
		if (!copy)
			return kIOReturnNoMemory;
	}
	if (inSize) {
		// NOLINTNEXTLINE(clang-analyzer-unix.cstring.NullArg): a non-zero inSize comes with its buffer
		memcpy(copy, in, inSize);
	}
	const IOReturn ret =
		run(method, inSize ? copy : nullptr, inSize, arguments->structureOutput, &outSize);
	if (copy != small)
		IOFreeData(copy, inSize);
	arguments->structureOutputSize = ret ? 0 : outSize;
	return ret;
}

IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
						  IOMemoryDescriptor **memory)
{
	IOLockLock(lock_);
	IOReturn ret = kIOReturnBadArgument;
	if (type == kFenceToken && fence_view_) {
		fence_view_->retain();
		*memory = fence_view_;
		*options = kIOMapAnywhere | kIOMapReadOnly | kIOMapWriteCombineCache;
		ret = kIOReturnSuccess;
	} else if (const Bo *bo = findToken(type)) {
		bo->view->retain();
		*memory = bo->view;
		*options = kIOMapAnywhere |
			   ((bo->flags & TegraGM20BBoCoherent) ? kIOMapWriteCombineCache
							       : kIOMapDefaultCache);
		// Pool views are physical: IOKit puts their pages in the mapping's device pager at map time
		// (populateDevicePager), but enters them in the pmap only for kernel_map, so a task's first CPU touch of each
		// 4 KiB still faults (the "device" class on the kprof faults line), and IOKit drops kIOMapPrefault for device
		// pager memory. A cached BO's buffer is prepared (BoAlloc), which kIOMapPrefault requires.
		if (bo->bmd && bo->view == bo->bmd && bo->bytes <= kPrefaultMaxBytes) {
			*options |= kIOMapPrefault;
			tgm_stats_prefault(bo->bytes);
		}
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(lock_);
	return ret;
}

extern "C" IOReturn tegra_gm20b_new_device_client(IOService *, task_t, void *, UInt32,
						  OSDictionary *, IOUserClient **);

// Called by AGXAccelerator::newUserClient (TegraGM20BAccelerator.cpp).
extern "C" IOReturn tegra_gm20b_new_user_client(IOService *provider, task_t task, void *securityID,
						UInt32 type, OSDictionary *properties,
						IOUserClient **handler)
{
	if ((type & 0xffff) == 1) {  // IOGPU's device (IOGPUDeviceCreate), the API in the high half
		return tegra_gm20b_new_device_client(provider, task, securityID, type, properties,
						     handler);
	}
	if (type != TegraGM20BOpenType)
		return kIOReturnBadArgument;
	if (tgm_stage() < 2)
		return kIOReturnNotReady;
	if (__atomic_add_fetch(&gConnections, 1, __ATOMIC_SEQ_CST) >
	    (tgm_all_clients() ? kMaxConnectionsAll : kMaxConnections)) {
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
		return kIOReturnNoResources;
	}
	AGXDeviceUserClient *client = OSTypeAlloc(AGXDeviceUserClient);
	if (!client) {
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(task, securityID, type, properties)) {
		client->release();
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
		return kIOReturnError;
	}
	client->countConnection();  // from here free() gives the connection slot back
	if (!client->setUp(task) || !client->attach(provider)) {
		client->release();
		return kIOReturnNoMemory;
	}
	if (!client->start(provider)) {
		client->detach(provider);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	static volatile int opened;
	if (!__atomic_exchange_n(&opened, 1,
				 __ATOMIC_SEQ_CST)) {  // the one "Metal reached the kernel" line
		char name[17] = {};
		proc_selfname(name, sizeof(name));
		IOLog("tegra-gm20b: AGXDeviceUserClient opened by %s\n", name);
	}
	return kIOReturnSuccess;
}

// GPU frame copy for the display (TegraScanoutCopy.h). The display offers each swapped surface; gpu.c's present channel
// copies it, after the compositor's GPU work, into the block-linear target, and calls native_legacy_gpu_complete from the
// GPU interrupt pass. Offers come one at a time under the display's gate: nothing here waits for the GPU.
//   targets   the display's scanout buffers (three: latest-frame-wins flips), physically contiguous, mapped once
//             each at kPresentTarget + i * 4 MiB, kind 0xfe
//   surfaces  the last four swapped surfaces, like the display's own surface maps: each keeps a retained and prepared
//             (wired) descriptor and its GPU mapping at kPresentSurface + i * 16 MiB; the least recently offered one goes
//             when a fifth surface comes. Its CPU caches are cleaned once, when it is imported (the GPU, which wrote it,
//             does not snoop them, and a dirty line written back later would land on its pixels).
// native_legacy_gpu_complete (NativeLegacyDisplay.cpp, declared by TegraScanoutCopy.h) is a plain reference: the kernel's static
// link refuses undefined weak ones, and the display is in every kernel that has this driver.
namespace
{
constexpr uint64_t kPresentTarget = 0x10000000, kPresentTargetSpan = 4ULL << 20;
constexpr uint64_t kPresentSurface = 0x40000000, kPresentSurfaceSpan = 16ULL << 20;

struct PresentTarget {
	uint64_t pa, bytes;
	uint64_t *pas;
	tgm_mem *mem;
};

struct PresentSurface {
	IOMemoryDescriptor *memory;
	uint64_t bytes, used;
	uint32_t npages, kind;
	uint64_t *pas;
	tgm_mem *mem;
};

// TegraScanoutCopy.h: the boot framebuffer and the display's two other buffers
constexpr unsigned kPresentTargets = 3;
PresentTarget gTargets[kPresentTargets];
PresentSurface gSurfaces[4];
uint64_t gSurfaceUses;
IOLock *gPresentLock;

void presentDone(void *display)
{
	native_legacy_gpu_complete(display, 0);
}

void dropSurface(PresentSurface &s, unsigned slot)
{
	if (!s.memory)
		return;
	tgm_present_unmap(kPresentSurface + slot * kPresentSurfaceSpan, s.bytes);
	tgm_mem_del(&s.mem);
	s.memory->complete();
	s.memory->release();
	IODelete(s.pas, uint64_t, s.npages);
	s = {};
}

// The GPU address of the target at pa (one of the display's scanout buffers), mapped on first use.
uint64_t presentTarget(uint64_t pa, uint64_t bytes)
{
	unsigned slot = kPresentTargets;
	for (unsigned i = 0; i < kPresentTargets; i++) {
		if (gTargets[i].mem && gTargets[i].pa == pa && gTargets[i].bytes == bytes)
			return kPresentTarget + i * kPresentTargetSpan;
		if (!gTargets[i].mem && slot == kPresentTargets)
			slot = i;
	}
	if (slot == kPresentTargets || bytes > kPresentTargetSpan || (pa | bytes) & 0xfff ||
	    pa + bytes > (1ULL << 32))
		return 0;  // a fourth address: not the contract (TegraScanoutCopy.h)
	PresentTarget &t = gTargets[slot];
	const uint32_t npages = static_cast<uint32_t>(bytes >> 12);
	t.pas = IONew(uint64_t, npages);
	if (!t.pas)
		return 0;
	for (uint32_t i = 0; i < npages; i++)
		t.pas[i] = pa + (static_cast<uint64_t>(i) << 12);
	// The copy engine only places whole GOBs; the 16-byte sector order inside a GOB comes from the page kind. With kind 0
	// (pitch) every GOB's inside comes out scrambled (jagged edges, sliced text) while GOBs sit in the right
	// places. L4T maps display surfaces as 0xfe (generic 16Bx2, uncompressed), which the display reads.
	constexpr uint32_t kind = 0xfe;
	tgm_vm *vm = nullptr;
	if (tgm_present_open(&vm) || tgm_mem_new(vm, t.pas, npages, &t.mem) ||
	    tgm_present_map(kPresentTarget + slot * kPresentTargetSpan, t.mem, bytes,
			    static_cast<uint8_t>(kind))) {
		tgm_mem_del(&t.mem);
		IODelete(t.pas, uint64_t, npages);
		t = {};
		return 0;
	}
	t.pa = pa;
	t.bytes = bytes;
	return kPresentTarget + slot * kPresentTargetSpan;
}

// The GPU address of the surface, imported on first use (or again after it fell out of the four, or became block-linear);
// *pa its first page. A block-linear surface is mapped with TGM_LAYOUT_KIND, the kind its texels are written under.
uint64_t presentSurface(IOMemoryDescriptor *memory, uint64_t bytes, bool blockLinear, uint64_t *pa)
{
	bytes = (bytes + 0xfff) & ~0xfffULL;
	const uint32_t kind = blockLinear ? TGM_LAYOUT_KIND : 0;
	unsigned slot = 0;
	for (unsigned i = 0; i < 4; i++) {
		if (gSurfaces[i].memory == memory && gSurfaces[i].bytes >= bytes &&
		    gSurfaces[i].kind == kind) {
			gSurfaces[i].used = ++gSurfaceUses;
			*pa = gSurfaces[i].pas[0];
			return kPresentSurface + i * kPresentSurfaceSpan;
		}
		if (gSurfaces[i].used < gSurfaces[slot].used)
			slot = i;
	}
	if (bytes > kPresentSurfaceSpan || bytes > ((memory->getLength() + 0xfff) & ~0xfffULL))
		return 0;
	PresentSurface &s = gSurfaces[slot];
	dropSurface(s, slot);
	s.npages = static_cast<uint32_t>(bytes >> 12);
	s.pas = IONew(uint64_t, s.npages);
	if (!s.pas)
		return 0;
	memory->retain();
	s.memory = memory;
	s.bytes = bytes;
	s.kind = kind;
	tgm_vm *vm = nullptr;
	if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
		memory->release();
		IODelete(s.pas, uint64_t, s.npages);
		s = {};
		return 0;
	}
	if (collectPages(memory, bytes, s.pas) || tgm_present_open(&vm) ||
	    tgm_mem_new(vm, s.pas, s.npages, &s.mem) ||
	    tgm_present_map(kPresentSurface + slot * kPresentSurfaceSpan, s.mem, bytes,
			    static_cast<uint8_t>(kind))) {
		tgm_mem_del(&s.mem);
		memory->complete();
		memory->release();
		IODelete(s.pas, uint64_t, s.npages);
		s = {};
		return 0;
	}
	tgm_cpu_clean_pages(s.pas, s.npages);
	s.used = ++gSurfaceUses;
	*pa = s.pas[0];
	return kPresentSurface + slot * kPresentSurfaceSpan;
}

}  // namespace

extern "C" int tegra_gm20b_scanout_copy(const struct TegraScanoutCopy *copy)
{
	if (!copy || copy->version != TEGRA_SCANOUT_COPY_VERSION || !copy->display ||
	    !copy->source || tgm_stage() < 3)
		return kIOReturnNotReady;
	IOMemoryDescriptor *memory =
		OSDynamicCast(IOMemoryDescriptor, static_cast<OSObject *>(copy->source));
	const bool fenceOnly = (copy->flags & TEGRA_SCANOUT_COPY_FENCE_ONLY) != 0u;
	const bool sourceBlock = (copy->flags & TEGRA_SCANOUT_COPY_SOURCE_BLOCK) != 0u;
	const uint64_t sourceLines =
		sourceBlock ? (copy->height + (8u << TEGRA_SCANOUT_TILED_BLOCK_HEIGHT_LOG2) - 1) &
				      ~((8ULL << TEGRA_SCANOUT_TILED_BLOCK_HEIGHT_LOG2) - 1)
			    : copy->height;
	if (!memory || !copy->width || !copy->height || copy->width > 16384 ||
	    copy->sourceStride < uint64_t(copy->width) * 4 || copy->sourceStride > UINT32_MAX ||
	    copy->sourceBytes < copy->sourceStride * sourceLines ||
	    (sourceBlock && (copy->width * 4) % 64) ||
	    (!fenceOnly && (copy->targetPitch < copy->width * 4 || copy->blockHeightLog2 > 5)))
		return kIOReturnBadArgument;
	// the block-linear target must hold the frame: whole block rows of targetPitch bytes x (8 << blockHeightLog2) lines
	const uint64_t blockLines = 8ULL << copy->blockHeightLog2;
	if (!fenceOnly && copy->targetBytes < (copy->height + blockLines - 1) / blockLines *
						      blockLines * copy->targetPitch)
		return kIOReturnBadArgument;
	if (!gPresentLock) {
		IOLock *fresh = IOLockAlloc();
		// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap writes the current value back into it
		IOLock *expected = nullptr;
		if (!__atomic_compare_exchange_n(&gPresentLock, &expected, fresh, false,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			IOLockFree(fresh);
		if (!gPresentLock)
			return kIOReturnNoMemory;
	}
	IOLockLock(gPresentLock);
	uint64_t sourcePa = 0;
	// fence only: no target; the source is still imported (its first page names the channels that write it)
	const uint64_t target = fenceOnly ? 0 : presentTarget(copy->target, copy->targetBytes);
	const uint64_t source = target || fenceOnly ? presentSurface(memory, copy->sourceBytes,
								     sourceBlock, &sourcePa)
						    : 0;
	const int ret =
		(target || fenceOnly) && source
			? tgm_present_copy(source, sourcePa, copy->sourceStride,
					   static_cast<int>(sourceBlock), copy->width, copy->height,
					   target, copy->targetPitch, copy->blockHeightLog2,
					   presentDone, copy->display)
			: -12;  // ENOMEM: the target or the surface could not be mapped
	IOLockUnlock(gPresentLock);
	return ret ? ioreturn(ret) : 0;
}
