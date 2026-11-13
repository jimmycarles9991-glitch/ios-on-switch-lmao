// SPDX-License-Identifier: MIT
// Host imports kept coherent with the CPU caches, which the GM20B does not snoop on Tegra. The render path pays nothing:
// whoever touches an imported IOSurface on the CPU pays, at its IOSurfaceLock/Unlock. Those are IOKit traps
// (IOConnectTrap3 index 2 lock, 3 unlock, p1 the surface ID, p2 the lock options), which XNU's iokit_user_client_trap
// hands to tegra_gm20b_iosurface_trap after the trap ran.
//  - GPU -> CPU: Exec marks each writable import GPU-dirty (tgm_coherency_wrote), if the GPU may write the surface at
//    all: an import without TegraGM20BImportGpuReads, or a SurfaceWrites naming it (the Metal frontends send one before
//    a surface is first rendered, blitted or stored into). The next lock of the surface, in any process, invalidates
//    its lines once and clears the mark, so the CPU reads what the GPU wrote rather than lines it cached before (a
//    fresh surface's zeros came back as 16x1 black runs in a GPU-rendered launch snapshot). A surface the GPU only
//    samples (an app's backing store in the render server) is never marked, so the app's lock to redraw it costs no
//    invalidate.
//  - Lock and GPU work in flight: IOSurfaceLock is where a CPU reader synchronises with the GPU (Apple's: a lock without
//    kIOSurfaceLockAvoidSync waits for the GPU's pending writes; with it, a lock that would have to wait fails with
//    kIOReturnCannotLock), and readers count on it: a snapshot's reader locks its surface right after the render
//    server's reply, its render maybe still running. So Exec also records each marking submission (channel, seqno) on
//    the entry, and a lock (tegra_gm20b_iosurface_lock_will, before the trap) sleeps on those channels' fence
//    interrupts until they completed, then the trap invalidates. Only a lock of a surface whose writers are still
//    running waits.
//  - CPU -> GPU: an unlock that was not read-only cleans the surface, so the GPU reads what the CPU wrote. Read-only
//    imports included: the GPU samples what an app redraws into them. The clean runs before IOSurface's own unlock, which
//    moves the seed: Core Animation samples a surface again once its seed moved, and the raw path keeps a YCbCr
//    surface's converted image until it moves again, so a clean after it let the render server convert lines still
//    dirty in the writer's cache and keep that image.
//  - Import: the importer cleans the pages for what the CPU wrote before. Memory imported without a surface ID (no-copy
//    buffers, Core Animation's shared-memory images) never sees a lock, so the import clean is all it gets.
// Entries are keyed by the surface's first physical page (the display asks by that, tegra_gm20b_surface_tiled) and,
// when the importer named the IOSurface, by its ID too. An import holds a reference to its entry, so Exec's dirty
// mark is one store through the import's own pointer. Cache maintenance runs under the entry's own lock, never the
// registry's, so a large surface's clean does not hold up other surfaces' locks and imports.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BRecord.h"
#include "TegraGM20BTiled.h"
#include "SwitchGpuBacking.h"
#include "SwitchGpuPathStats.h"
#if TGM_STATS
#include <os/log.h>
#endif
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

struct tgm_coh {
	tgm_coh *next;    // first-page chain
	tgm_coh *idNext;  // surface-ID chain, while surfaceId != 0
	uint64_t pa;      // the surface's first physical page
	uint32_t surfaceId;
	uint32_t refs;    // registry lock
	uint32_t askers;  // registry lock: refs held across a layout decision (tgm_coh_hold_asker)
	bool parked;
	// registry lock: an import with the surface noted the entry; it was the entry's only import and the first GPU import
	// of memory nothing had written (TegraGM20BRecord.h); its last import went while it was block-linear and its one
	// reference is the record's (the record's memory lives on: it may still be read through a lock or imported again)
	bool noted, fresh, deferred;
	// registry lock; valid while `surface` is held or the entry is deferred on it
	tgm_record *record;
	switch_gpu_backing *backing;
	void *surface;
	void (*releaseSurface)(void *);
	IOLock *lock;   // pas, npages: the cache ops and a larger import's list
	uint64_t *pas;  // the largest page list imported for it
	uint32_t npages;
	int writable;  // some import lets the GPU write it
	int gpuWrites;  // atomic: an import or a SurfaceWrites says the GPU may write it, so submissions mark it
	int cleaned;       // atomic: an import of it finished its own clean
	int gpuDirty;      // atomic: the GPU may have written it since the last CPU lock
	uint64_t written;  // atomic: submissions that may have written it
	// atomic: the surface's layout once decided (TegraGM20BTiled.cpp), null before
	tgm_tiled *tiled;
	uint32_t
		cpuLocks;  // atomic: CPU locks held now (a surface is not made block-linear under one)
	uint64_t writers
		[TGM_COH_WRITERS];  // atomic: marking submissions, channel uid << 32 | seqno (0: free slot)
	int writersLost;  // atomic: one found no slot: a lock waits for every channel's submitted work
	// atomic: the writers whose submission listed the surface as written (ExecSurfaces), what another channel's listed
	// Exec waits for on the GPU; a mark without a list names every import, written or not, so it is a writer above only
	uint64_t listed[TGM_COH_WRITERS];
	uint32_t
		listedLost;  // atomic: listed writers that found no slot since tgm_coherency_lost_seen cleared it
	uint32_t traced;  // atomic: CPU lock and unlock lines logged for it (debug kernels)
};

namespace
{
constexpr uint32_t kBuckets = 256;
constexpr uint32_t kTrapLock = 2, kTrapUnlock = 3, kLockReadOnly = 0x1, kLockAvoidSync = 0x2;
constexpr uint32_t kBatch = 4;

IOLock *gRegistryLock;
tgm_coh *gByPage[kBuckets], *gById[kBuckets];
uint32_t gWithId;  // atomic: entries reachable by surface ID; 0 lets every trap return at once
uint32_t
	gDeferred;  // atomic: entries deferred on their records; 0 lets every import and engine go on at once

#if TGM_STATS
struct {
	uint64_t invalN, invalBytes, cleanN, cleanBytes, waitedN, cannotLockN;
} gStats;  // registry lock
#endif

void count(bool unlock, uint64_t n, uint64_t bytes)  // registry lock held
{
#if TGM_STATS
	(unlock ? gStats.cleanN : gStats.invalN) += n;
	(unlock ? gStats.cleanBytes : gStats.invalBytes) += bytes;
#else
	(void)unlock;
	(void)n;
	(void)bytes;
#endif
}

tgm_coh **pageSlot(uint64_t pa)
{
	tgm_coh **p = &gByPage[((pa >> 12) ^ (pa >> 20)) & (kBuckets - 1)];
	while (*p && (*p)->pa != pa)
		p = &(*p)->next;
	return p;
}

tgm_coh **idBucket(uint32_t surfaceId)
{
	return &gById[(surfaceId ^ (surfaceId >> 8)) & (kBuckets - 1)];
}

void idUnlink(const tgm_coh *c)
{
	for (tgm_coh **p = idBucket(c->surfaceId); *p; p = &(*p)->idNext) {
		if (*p == c) {
			*p = c->idNext;
			__atomic_sub_fetch(&gWithId, 1, __ATOMIC_RELAXED);
			return;
		}
	}
}

void entryFree(tgm_coh *c)
{
	switch_gpu_backing_release(c->backing);
	if (c->releaseSurface)
		c->releaseSurface(c->surface);
	tgm_tiled_free(c->tiled);
	IOLockFree(c->lock);
	IODelete(c->pas, uint64_t, c->npages);
	IODelete(c, tgm_coh, 1);
}

bool release(tgm_coh *c)  // registry lock held
{
	if (--c->refs)
		return false;
	tgm_coh **p = pageSlot(c->pa);
	if (*p == c)
		*p = c->next;
	if (c->surfaceId)
		idUnlink(c);
	return true;
}

bool tiledNow(const tgm_coh *c)
{
	const tgm_tiled *t = __atomic_load_n(&c->tiled, __ATOMIC_ACQUIRE);
	return (t != nullptr) && (t->tiled != 0);
}

// registry lock held: the entry keeps its texels and its one reference passes to its memory's record, giving up its
// hold on the surface (*surface, for the caller to release outside the lock), so the surface can go
bool defer(tgm_coh *c, void **surface, void (**releaseSurface)(void *))
{
	if (!c->record || !c->surface || c->parked)
		return false;
	c->deferred = true;
	__atomic_add_fetch(&gDeferred, 1, __ATOMIC_RELAXED);
	tgm_record_park(c->record, c);
	*surface = c->surface;
	*releaseSurface = c->releaseSurface;
	c->surface = nullptr;
	c->releaseSurface = nullptr;
	tgm_tiled_count(TGM_RELAYOUT_DEFERRED);
	return true;
}

// Drops one reference to each of `n` entries (at most kBatch). An entry whose last reference this is while its surface is
// block-linear stays block-linear on its record when it has one (the move back to rows waits for a CPU lock that asks
// for rows, TegraGM20BViews.cpp, and never comes if the memory goes first); without one it is made pitch again
// (TegraGM20BTiled.cpp: its texels back to rows), outside the registry lock, since a CPU lock would find no entry.
void drop(tgm_coh *const *cs, uint32_t n)
{
	tgm_coh *last[kBatch], *dead[kBatch];
	void *surfaces[kBatch];
	void (*releases[kBatch])(void *);
	uint32_t nlast = 0, ndead = 0, ndeferred = 0;
	IOLockLock(gRegistryLock);
	for (uint32_t i = 0; i < n; i++) {
		auto *c = cs[i];
		if (c->parked && c->refs == 2) {
			c->parked = false;
			--c->refs;
		}
		if (c->refs == 1 && tiledNow(c)) {
			if (defer(c, &surfaces[ndeferred], &releases[ndeferred]))
				ndeferred++;
			else
				last[nlast++] = c;
		} else if (release(c)) {
			dead[ndead++] = c;
		}
	}
	IOLockUnlock(gRegistryLock);
	// the surface's last reference may be this one: its record goes with its memory, and the entry with the record
	for (uint32_t i = 0; i < ndeferred; ++i)
		releases[i](surfaces[i]);
	for (uint32_t i = 0; i < ndead; ++i)
		entryFree(dead[i]);
	for (uint32_t i = 0; i < nlast; i++) {
		const int result = tgm_tiled_untile(last[i]);
		IOLockLock(gRegistryLock);
		bool destroy = false;
		if (result && last[i]->refs == 1)
			last[i]->parked = true;
		else
			destroy = release(last[i]);
		IOLockUnlock(gRegistryLock);
		if (destroy)
			entryFree(last[i]);
	}
}

uint64_t maintain(const tgm_coh *c)
{
	IOLockLock(c->lock);
	tgm_cpu_clean_pages(c->pas, c->npages);
	const uint64_t bytes = static_cast<uint64_t>(c->npages) << 12;
	IOLockUnlock(c->lock);
	return bytes;
}

// The entries of `surfaceId` from the `skip`th on, at most kBatch, each with a reference the caller drops. A surface has
// one entry unless its imports began at different pages.
uint32_t findById(uint32_t surfaceId, uint32_t skip, tgm_coh **out)
{
	uint32_t n = 0;
	uint32_t seen = 0;
	IOLockLock(gRegistryLock);
	for (tgm_coh *c = *idBucket(surfaceId); c && n < kBatch; c = c->idNext) {
		if (c->surfaceId == surfaceId && seen++ >= skip) {
			c->refs++;
			out[n++] = c;
		}
	}
	IOLockUnlock(gRegistryLock);
	return n;
}

// A lock invalidates the entries the GPU may have written since the last lock; an unlock that wrote cleans them all.
#if TGM_STATS
extern "C" void proc_selfname(char *buf, int size);
extern "C" int proc_selfpid(void);

constexpr uint32_t kTraced = 6, kTracedFullScreen = 64;
constexpr uint32_t kFullScreenPages =
	900;  // 3.5 MiB of 4 KiB pages: a 720x1280 BGRA surface (the framebuffers' 3840 KiB)

// The first CPU locks and writing unlocks of a GPU-imported surface after its entry was made, with the process and what
// the trap did: who writes a surface the GPU samples, and whether through IOSurfaceLock/Unlock at all (a writer that is
// not, a hardware engine or a CPU writer without locks, never shows here). Full-screen surfaces get more, so a periodic
// reader of the framebuffers shows its cadence.
void trace(tgm_coh *c, uint32_t surfaceId, bool unlock, bool readOnly, bool maintained)
{
	const uint32_t most = c->npages >= kFullScreenPages ? kTracedFullScreen : kTraced;
	if (__atomic_fetch_add(&c->traced, 1u, __ATOMIC_RELAXED) >= most)
		return;
	char name[32];
	proc_selfname(name, sizeof(name));
	os_log(OS_LOG_DEFAULT, "tegra-gm20b: IOSurface %u (%u KiB) CPU %s by %s[%d]%s", surfaceId,
	       c->npages * 4,
	       unlock     ? "unlock after a write"
	       : readOnly ? "lock read-only"
			  : "lock for writing",
	       name, proc_selfpid(),
	       maintained ? (unlock ? ", cleaned" : ", invalidated (GPU-written)") : "");
}
#endif

void surfaceTrapped(uint32_t surfaceId, bool unlock, bool readOnly)
{
	tgm_coh *found[kBatch];
	uint32_t n = kBatch;
	for (uint32_t skip = 0; n == kBatch; skip += n) {
		n = findById(surfaceId, skip, found);
		if (!n)
			break;
		uint64_t bytes = 0;
		uint64_t done = 0;
		for (uint32_t i = 0; i < n; i++) {
			if (!unlock) {
				__atomic_add_fetch(&found[i]->cpuLocks, 1, __ATOMIC_ACQ_REL);
			} else if (__atomic_load_n(&found[i]->cpuLocks, __ATOMIC_ACQUIRE)) {
				__atomic_sub_fetch(&found[i]->cpuLocks, 1, __ATOMIC_ACQ_REL);
			}
			// a block-linear surface's CPU locks read and write a scratch (TegraGM20BViews.cpp), never its pages
			if (tiledNow(found[i]))
				continue;
			const bool maintained =
				(unlock && !readOnly) ||
				(!unlock && __atomic_exchange_n(&found[i]->gpuDirty, 0,
								__ATOMIC_ACQ_REL) != 0);
			auto *backing = switch_gpu_backing_find_pa(found[i]->pa);
			if (maintained && !backing) {
				bytes += maintain(found[i]);
				done++;
			}
			if (backing)
				switch_gpu_backing_release(backing);
#if TGM_STATS
			trace(found[i], surfaceId, unlock, readOnly, maintained);
#else
			(void)readOnly;
#endif
		}
		IOLockLock(gRegistryLock);
		count(unlock, done, bytes);
		IOLockUnlock(gRegistryLock);
		drop(found, n);
	}
}

// A larger import of a surface already registered: its list replaces the entry's (entry lock held).
void adopt(tgm_coh *c, const uint64_t *pas, uint32_t npages)
{
	uint64_t *copy = IONew(uint64_t, npages);
	if (!copy)
		return;
	memcpy(copy, pas, static_cast<size_t>(npages) * sizeof(uint64_t));
	IODelete(c->pas, uint64_t, c->npages);
	c->pas = copy;
	c->npages = npages;
}

tgm_coh *entryNew(uint64_t pa, const uint64_t *pas, uint32_t npages)
{
	tgm_coh *c = IONew(tgm_coh, 1);
	if (!c)
		return nullptr;
	bzero(c, sizeof(*c));
	c->pa = pa;
	c->lock = IOLockAlloc();
	c->pas = IONew(uint64_t, npages);
	if (!c->lock || !c->pas) {
		if (c->lock)
			IOLockFree(c->lock);
		if (c->pas)
			IODelete(c->pas, uint64_t, npages);
		IODelete(c, tgm_coh, 1);
		return nullptr;
	}
	memcpy(c->pas, pas, static_cast<size_t>(npages) * sizeof(uint64_t));
	c->npages = npages;
	return c;
}
}  // namespace

extern "C" int tgm_coherency_init(void)
{
	if (!gRegistryLock)
		gRegistryLock = IOLockAlloc();
	return gRegistryLock && !tgm_relayout_init() ? 0 : -12;
}

extern "C" struct tgm_coh *tgm_coherency_add(uint64_t pa, uint32_t surface_id, const uint64_t *pas,
					     uint32_t npages, int writable, int gpu_writes,
					     int *covered)
{
	*covered = 0;
	if (!npages || !gRegistryLock)
		return nullptr;
	tgm_coh *fresh = entryNew(pa, pas, npages);
	IOLockLock(gRegistryLock);
	tgm_coh **p = pageSlot(pa);
	tgm_coh *c = *p;
	// A live import of the same surface that finished its own clean: every CPU write since came through an
	// IOSurfaceUnlock that cleaned these pages, so they are clean already.
	if (c && surface_id && c->surfaceId == surface_id && c->npages >= npages &&
	    __atomic_load_n(&c->cleaned, __ATOMIC_ACQUIRE))
		*covered = 1;
	if (!c && fresh) {
		c = *p = fresh;
		fresh = nullptr;
	}
	if (c) {
		if (c->parked) {
			c->parked = false;
		} else if (c->deferred) {
			c->deferred = false;
			__atomic_sub_fetch(&gDeferred, 1, __ATOMIC_RELAXED);
			tgm_record_take(c->record);
		} else {
			c->refs++;
		}
		if (writable)
			c->writable = 1;
		if (gpu_writes)
			__atomic_store_n(&c->gpuWrites, 1, __ATOMIC_RELAXED);
		if (surface_id && !c->surfaceId) {
			c->surfaceId = surface_id;
			tgm_coh **b = idBucket(surface_id);
			c->idNext = *b;
			*b = c;
			__atomic_add_fetch(&gWithId, 1, __ATOMIC_RELAXED);
		}
	}
	IOLockUnlock(gRegistryLock);
	if (fresh)
		entryFree(fresh);
	if (*covered) {
		IOLockLock(c->lock);
		*covered = static_cast<int>(
			memcmp(c->pas, pas, size_t(npages) * sizeof(uint64_t)) == 0);
		IOLockUnlock(c->lock);
	}
	if (c && npages > c->npages) {
		IOLockLock(c->lock);
		if (npages > c->npages)
			adopt(c, pas, npages);
		IOLockUnlock(c->lock);
	}
	return c;
}

extern "C" int tgm_coherency_pin(struct tgm_coh *c, struct switch_gpu_backing *backing,
				 void *surface, void (*release_surface)(void *))
{
	if (!c || !backing || (surface != nullptr) != (release_surface != nullptr))
		return -22;
	IOLockLock(c->lock);
	const int result = c->backing && c->backing != backing ? -22 : 0;
	if (!result && !c->backing) {
		switch_gpu_backing_retain(backing);
		c->backing = backing;
	}
	if (!result && surface && !c->surface) {
		c->surface = surface;
		c->releaseSurface = release_surface;
		surface = nullptr;
	}
	IOLockUnlock(c->lock);
	if (!result && surface)
		release_surface(surface);
	return result;
}

extern "C" int tgm_coh_pinned(const struct tgm_coh *c)
{
	return c->backing != nullptr;
}

extern "C" void tgm_coherency_cleaned(struct tgm_coh *c)
{
	if (c)
		__atomic_store_n(&c->cleaned, 1, __ATOMIC_RELEASE);
}

extern "C" void tgm_coherency_put(struct tgm_coh *c)
{
	if (!c)
		return;
	if (tiledNow(c))
		tgm_views_sweep(c);
	drop(&c, 1);
}

// One submission of channel `uid` that may write the surface, into `slots`: a later one of the same channel takes its
// slot. False: no slot left for it.
// NOLINTNEXTLINE(readability-non-const-parameter): the compare-and-swap below writes through slots
static bool noteWriter(uint64_t *slots, uint32_t uid, uint32_t seqno)
{
	const uint64_t mine = static_cast<uint64_t>(uid) << 32 | seqno;
	for (uint32_t i = 0; i < TGM_COH_WRITERS; i++) {
		uint64_t seen = __atomic_load_n(&slots[i], __ATOMIC_ACQUIRE);
		while (seen && static_cast<uint32_t>(seen >> 32) == uid) {
			if (static_cast<int32_t>(seqno - static_cast<uint32_t>(seen)) <= 0 ||
			    __atomic_compare_exchange_n(&slots[i], &seen, mine, false,
							__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				return true;
		}
	}
	for (uint32_t i = 0; i < TGM_COH_WRITERS; i++) {
		uint64_t free = 0;
		if (__atomic_compare_exchange_n(&slots[i], &free, mine, false, __ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return true;
	}
	// Slots are freed only by a CPU lock, and a surface the CPU never locks keeps every channel that ever wrote it.
	for (uint32_t i = 0; i < TGM_COH_WRITERS; i++) {
		uint64_t seen = __atomic_load_n(&slots[i], __ATOMIC_ACQUIRE);
		if (seen &&
		    tgm_writer_done(static_cast<uint32_t>(seen >> 32),
				    static_cast<uint32_t>(seen)) &&
		    __atomic_compare_exchange_n(&slots[i], &seen, mine, false, __ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return true;
	}
	return false;
}

static void wrote(tgm_coh *c, uint32_t uid, uint32_t seqno)
{
	if (!noteWriter(c->writers, uid, seqno))
		__atomic_store_n(&c->writersLost, 1, __ATOMIC_RELEASE);
	__atomic_add_fetch(&c->written, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&c->gpuDirty, 1, __ATOMIC_RELEASE);
}

extern "C" void tgm_coherency_wrote(struct tgm_coh *c, int always, uint32_t uid, uint32_t seqno)
{
	if (c && (always || __atomic_load_n(&c->gpuWrites, __ATOMIC_RELAXED)))
		wrote(c, uid, seqno);
}

extern "C" void tgm_coherency_wrote_listed(struct tgm_coh *c, uint32_t uid, uint32_t seqno)
{
	if (!c)
		return;
	wrote(c, uid, seqno);
	if (!noteWriter(c->listed, uid, seqno))
		__atomic_add_fetch(&c->listedLost, 1, __ATOMIC_ACQ_REL);
}

extern "C" uint32_t tgm_coherency_writers(const struct tgm_coh *c, uint64_t out[TGM_COH_WRITERS],
					  uint32_t *lost, uint64_t *pa)
{
	uint32_t n = 0;
	for (const uint64_t &slot : c->listed) {
		const uint64_t w = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
		if (w)
			out[n++] = w;
	}
	*lost = __atomic_load_n(&c->listedLost, __ATOMIC_ACQUIRE);
	*pa = c->pa;
	return n;
}

extern "C" void tgm_coherency_lost_seen(struct tgm_coh *c, uint32_t lost)
{
	__atomic_compare_exchange_n(&c->listedLost, &lost, 0u, false, __ATOMIC_ACQ_REL,
				    __ATOMIC_RELAXED);
}

// Whether `c` has a writer still running; with `wait`, sleeps until none has, and frees the completed slots.
static bool writersPending(tgm_coh *c, bool wait)
{
	bool pending = false;
	if (__atomic_load_n(&c->writersLost, __ATOMIC_ACQUIRE)) {
		if (!wait)
			return tgm_writers_all_done() == 0;
		__atomic_store_n(&c->writersLost, 0, __ATOMIC_RELAXED);
		tgm_writers_wait_all();
		pending = true;
	}
	for (uint64_t &writer : c->writers) {
		uint64_t seen = __atomic_load_n(&writer, __ATOMIC_ACQUIRE);
		if (!seen || tgm_writer_done(static_cast<uint32_t>(seen >> 32),
					     static_cast<uint32_t>(seen))) {
			if (seen) {
				__atomic_compare_exchange_n(&writer, &seen, 0, false,
							    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
			}
			continue;
		}
		if (!wait)
			return true;
		tgm_writer_wait(static_cast<uint32_t>(seen >> 32), static_cast<uint32_t>(seen));
		__atomic_compare_exchange_n(&writer, &seen, 0, false, __ATOMIC_ACQ_REL,
					    __ATOMIC_RELAXED);
		pending = true;
	}
	return pending;
}

extern "C" int tegra_gm20b_iosurface_lock_will(uint32_t surface_id, uint32_t options)
{
	if (!surface_id || !__atomic_load_n(&gWithId, __ATOMIC_RELAXED))
		return 0;
	const bool avoidSync = (options & kLockAvoidSync) != 0u;
	tgm_coh *found[kBatch];
	uint32_t n = kBatch;
	bool waited = false;
	bool refused = false;
	for (uint32_t skip = 0; n == kBatch && !refused; skip += n) {
		n = findById(surface_id, skip, found);
		if (!n)
			break;
		for (uint32_t i = 0; i < n && !refused; i++) {
			if (tiledNow(found[i]))
				continue;  // its view's copy waits for the writers on the GPU (TegraGM20BViews.cpp)
			if (avoidSync) {
				refused = writersPending(found[i], false);
			} else {
				waited |= writersPending(found[i], true);
			}
		}
		drop(found, n);
	}
#if TGM_STATS
	if (waited || refused) {
		IOLockLock(gRegistryLock);
		gStats.waitedN += static_cast<uint64_t>(waited);
		gStats.cannotLockN += static_cast<uint64_t>(refused);
		IOLockUnlock(gRegistryLock);
	}
#else
	(void)waited;
#endif
	return static_cast<int>(refused);
}

extern "C" void tgm_coherency_gpu_writes(uint32_t surface_id)
{
	if (!surface_id || !gRegistryLock)
		return;
	IOLockLock(gRegistryLock);
	for (tgm_coh *c = *idBucket(surface_id); c; c = c->idNext) {
		if (c->surfaceId == surface_id)
			__atomic_store_n(&c->gpuWrites, 1, __ATOMIC_RELAXED);
	}
	IOLockUnlock(gRegistryLock);
}

extern "C" int tegra_gm20b_surface_tiled(uint64_t pa)
{
	if (!gRegistryLock)
		return 0;
	IOLockLock(gRegistryLock);
	const tgm_coh *c = *pageSlot(pa);
	const tgm_tiled *t = c ? __atomic_load_n(&c->tiled, __ATOMIC_ACQUIRE) : nullptr;
	const int tiled = static_cast<int>((t != nullptr) && t->tiled != 0);
	IOLockUnlock(gRegistryLock);
	return tiled;
}

// The display's hold is one more reference on the entry: the last import's release no longer finds it alone, so nothing
// moves its texels until the display lets go (tgm_coherency_put, which moves them then if the surface is still block-linear).
extern "C" void *tegra_gm20b_surface_hold(uint64_t pa, int *tiled)
{
	*tiled = 0;
	if (!gRegistryLock)
		return nullptr;
	IOLockLock(gRegistryLock);
	tgm_coh *c = *pageSlot(pa);
	const tgm_tiled *t = c ? __atomic_load_n(&c->tiled, __ATOMIC_ACQUIRE) : nullptr;
	if (t) {
		c->refs++;
		*tiled = t->tiled != 0 ? 1 : 0;
	}
	IOLockUnlock(gRegistryLock);
	return t ? c : nullptr;
}

extern "C" void tegra_gm20b_surface_unhold(void *hold)
{
	tgm_coherency_put(static_cast<tgm_coh *>(hold));
}

extern "C" int tegra_gm20b_surface_layout(uint64_t pa, struct tgm_layout *out)
{
	if (!gRegistryLock || !out)
		return 0;
	IOLockLock(gRegistryLock);
	const tgm_coh *c = *pageSlot(pa);
	const tgm_tiled *t = c ? __atomic_load_n(&c->tiled, __ATOMIC_ACQUIRE) : nullptr;
	const int tiled = static_cast<int>((t != nullptr) && t->tiled != 0);
	if (tiled)
		*out = t->layout;
	IOLockUnlock(gRegistryLock);
	return tiled;
}

// NOLINTNEXTLINE(misc-const-correctness): the host tests' stubs and the C callers share this signature
extern "C" void tgm_coh_lock(struct tgm_coh *c)
{
	IOLockLock(c->lock);
}

// NOLINTNEXTLINE(misc-const-correctness): the host tests' stubs and the C callers share this signature
extern "C" void tgm_coh_unlock(struct tgm_coh *c)
{
	IOLockUnlock(c->lock);
}

extern "C" struct tgm_tiled **tgm_coh_tiled(struct tgm_coh *c)
{
	return &c->tiled;
}

extern "C" int tgm_coh_alone(struct tgm_coh *c, uint32_t refs)
{
	IOLockLock(gRegistryLock);
	const int alone = static_cast<int>(c->refs == refs &&
					   (__atomic_load_n(&c->cpuLocks, __ATOMIC_ACQUIRE)) == 0u);
	IOLockUnlock(gRegistryLock);
	return alone;
}

extern "C" int tgm_coh_unshared(const struct tgm_coh *c)
{
	IOLockLock(gRegistryLock);
	const int unshared =
		static_cast<int>(c->refs - c->askers == 1 &&
				 (__atomic_load_n(&c->cpuLocks, __ATOMIC_ACQUIRE)) == 0u);
	IOLockUnlock(gRegistryLock);
	return unshared;
}

extern "C" struct tgm_relayout_pages tgm_coh_pages(const struct tgm_coh *c)
{
	return {c->pas, c->npages};
}

extern "C" struct tgm_coh *tgm_coh_find_tiled(uint32_t id)
{
	if (!tgm_tiled_any())
		return nullptr;
	tgm_coh *found[kBatch];
	tgm_coh *tiled = nullptr;
	const uint32_t n = id && gRegistryLock ? findById(id, 0, found) : 0;
	// a deferred entry of a surface that is going may still carry an ID a new surface has taken
	for (uint32_t i = 0; i < n; i++) {
		if (tiledNow(found[i]) && (!tiled || (tiled->deferred && !found[i]->deferred)))
			tiled = found[i];
	}
	tgm_coh *others[kBatch];
	uint32_t m = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (found[i] != tiled)
			others[m++] = found[i];
	}
	drop(others, m);
	return tiled;
}

extern "C" struct tgm_coh *tgm_coh_find_tiled_pa(uint64_t pa, uint64_t *offset)
{
	if (!gRegistryLock || !tgm_tiled_any())
		return nullptr;
	IOLockLock(gRegistryLock);
	tgm_coh *found = nullptr;
	for (uint32_t b = 0; b < kBuckets && !found; b++) {
		for (tgm_coh *c = gByPage[b]; c && !found; c = c->next) {
			if (!tiledNow(c))
				continue;
			for (uint32_t i = 0; i < c->npages; i++) {
				if (c->pas[i] == (pa & ~0xfffULL)) {
					found = c;
					*offset = (uint64_t(i) << 12) | (pa & 0xfff);
					break;
				}
			}
		}
	}
	if (found)
		found->refs++;
	IOLockUnlock(gRegistryLock);
	return found;
}

extern "C" struct tgm_coh *tgm_coh_find_pa(uint64_t pa)
{
	if (!gRegistryLock)
		return nullptr;
	IOLockLock(gRegistryLock);
	tgm_coh *c = *pageSlot(pa & ~0xfffULL);
	if (c)
		c->refs++;
	IOLockUnlock(gRegistryLock);
	return c;
}

extern "C" void tgm_coh_hold(struct tgm_coh *c)
{
	IOLockLock(gRegistryLock);
	c->refs++;
	IOLockUnlock(gRegistryLock);
}

extern "C" void tgm_coh_hold_asker(struct tgm_coh *c)
{
	IOLockLock(gRegistryLock);
	c->refs++;
	c->askers++;
	IOLockUnlock(gRegistryLock);
}

// The count drops before the reference, which may be the entry's last: in between, the asker counts as another user of
// the surface (a decision then says pitch), never the other way round.
extern "C" void tgm_coh_put_asker(struct tgm_coh *c)
{
	IOLockLock(gRegistryLock);
	c->askers--;
	IOLockUnlock(gRegistryLock);
	tgm_coherency_put(c);
}

extern "C" uint32_t tgm_coh_surface_id(const struct tgm_coh *c)
{
	return c->surfaceId;
}

extern "C" uint64_t tgm_coh_written(const struct tgm_coh *c)
{
	switch_gpu_backing_info info = {};
	if (c->backing)
		switch_gpu_backing_describe(c->backing, &info);
	return __atomic_load_n(&c->written, __ATOMIC_ACQUIRE) + info.content_generation;
}

extern "C" void tgm_coh_wait_visible(struct tgm_coh *c)
{
	if (!c->backing)
		return;
	switch_gpu_path_taken(SWITCH_GPU_PATH_VISIBLE_WAIT, 1);
	(void)switch_gpu_backing_wait_visible(c->backing);
}

extern "C" uint32_t tgm_coh_writers(struct tgm_coh *c, struct tgm_relayout_writer *out,
				    uint32_t max)
{
	if (__atomic_exchange_n(&c->writersLost, 0, __ATOMIC_ACQ_REL)) {
		tgm_writers_wait_all();
		return 0;
	}
	uint32_t n = 0;
	for (uint64_t &writer : c->writers) {
		const uint64_t seen = __atomic_load_n(&writer, __ATOMIC_ACQUIRE);
		if (!seen || tgm_writer_done(uint32_t(seen >> 32), uint32_t(seen)))
			continue;
		if (n == max) {
			tgm_writer_wait(uint32_t(seen >> 32), uint32_t(seen));
			continue;
		}
		out[n++] = {uint32_t(seen >> 32), uint32_t(seen)};
	}
	return n;
}

extern "C" void tgm_coherency_take_stats(uint64_t out[6])
{
	bzero(out, 6 * sizeof(uint64_t));
#if TGM_STATS
	if (!gRegistryLock)
		return;
	IOLockLock(gRegistryLock);
	out[0] = gStats.invalN;
	out[1] = gStats.invalBytes;
	out[2] = gStats.cleanN;
	out[3] = gStats.cleanBytes;
	out[4] = gStats.waitedN;
	out[5] = gStats.cannotLockN;
	bzero(&gStats, sizeof(gStats));
	IOLockUnlock(gRegistryLock);
#endif
}

extern "C" void tegra_gm20b_iosurface_trap(uint32_t index, uint32_t surface_id, uint32_t options)
{
	if (!surface_id || !__atomic_load_n(&gWithId, __ATOMIC_RELAXED))
		return;
	if (index == kTrapLock) {
		surfaceTrapped(surface_id, false, (options & kLockReadOnly) != 0u);
	} else if (index == kTrapUnlock) {
		surfaceTrapped(surface_id, true, (options & kLockReadOnly) != 0u);
	}
}

extern "C" void tgm_coherency_import(struct tgm_coh *c, struct tgm_record *record, int fresh)
{
	if (!c)
		return;
	IOLockLock(c->lock);
	const bool held = c->surface != nullptr;
	IOLockLock(gRegistryLock);
	if (held && !c->record)
		c->record = record;
	if (!c->noted) {
		c->noted = true;
		c->fresh = fresh && c->refs == 1 && !__atomic_load_n(&c->tiled, __ATOMIC_ACQUIRE);
	}
	IOLockUnlock(gRegistryLock);
	IOLockUnlock(c->lock);
}

extern "C" int tgm_coh_fresh(const struct tgm_coh *c)
{
	return static_cast<int>(c->fresh && c->surface && tgm_coh_written(c) == 0 &&
				tgm_surface_unwritten(c->surface));
}

extern "C" int tgm_coh_deferred_for(struct tgm_coh *c, uint32_t id)
{
	IOLockLock(gRegistryLock);
	const tgm_record *record = c->deferred ? c->record : nullptr;
	IOLockUnlock(gRegistryLock);
	if (!record)
		return 0;
	return tgm_record_is_surface(record, id) ? 1 : -1;
}

extern "C" void tgm_coh_undefer(struct tgm_coh *c)
{
	IOLockLock(gRegistryLock);
	const bool deferred = c->deferred;
	if (deferred) {
		c->deferred = false;
		__atomic_sub_fetch(&gDeferred, 1, __ATOMIC_RELAXED);
		tgm_record_take(c->record);
	}
	IOLockUnlock(gRegistryLock);
	if (deferred)
		drop(&c, 1);
}

namespace
{
// The caller's reference on a deferred entry: its texels back to rows unless another holder reads them block-linear
void rowsFor(tgm_coh *c)
{
	if (!tgm_tiled_untile_deferred(c))
		tgm_coh_undefer(c);
	tgm_coherency_put(c);
}
}  // namespace

extern "C" int tgm_coh_any_deferred(void)
{
	return static_cast<int>(__atomic_load_n(&gDeferred, __ATOMIC_RELAXED) != 0);
}

extern "C" void tgm_coherency_rows_at(uint64_t pa)
{
	if (!__atomic_load_n(&gDeferred, __ATOMIC_RELAXED) || !gRegistryLock)
		return;
	IOLockLock(gRegistryLock);
	tgm_coh *c = *pageSlot(pa);
	if (c && c->deferred)
		c->refs++;
	else
		c = nullptr;
	IOLockUnlock(gRegistryLock);
	if (c)
		rowsFor(c);
}

extern "C" void tgm_coherency_engine_rows(struct tgm_record *record)
{
	if (!__atomic_load_n(&gDeferred, __ATOMIC_RELAXED))
		return;
	IOLockLock(gRegistryLock);
	tgm_coh *c = tgm_record_take(record);
	tgm_record_park(record, c);
	if (c)
		c->refs++;
	IOLockUnlock(gRegistryLock);
	if (c)
		rowsFor(c);
}

extern "C" void tgm_coherency_record_gone(struct tgm_record *record)
{
	if (!gRegistryLock)
		return;
	IOLockLock(gRegistryLock);
	tgm_coh *c = tgm_record_take(record);
	bool dead = false;
	if (c && c->deferred) {
		c->deferred = false;
		__atomic_sub_fetch(&gDeferred, 1, __ATOMIC_RELAXED);
		c->record = nullptr;
		dead = release(c);
	}
	IOLockUnlock(gRegistryLock);
	if (dead) {
		tgm_tiled_count(TGM_RELAYOUT_NEVER_UNTILED);
		entryFree(c);
	}
}
