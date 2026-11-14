// SPDX-License-Identifier: MIT
// A large new IOSurface's pages faulted in on a thread call, so the GPU import that follows (BoImport's wiring, on the
// render thread) finds them resident instead of having the VM zero-fill each one there. The VM still does the zeroing
// (IOMemoryDescriptor::prepare of the surface's own descriptor), so no one sees stale data; racing the fault the import
// only waits on the VM's busy pages, for work it would do itself. The zeros went through the cacheable physical
// aperture: the thread call cleans them out to DRAM and keeps the pages wired (TegraGM20BPrefaultHeld) until the
// surface's first import takes the wiring, which then needs no clean of its own (tegra_gm20b_import_cleans).
// kern/switch's IOSurfaceRootUserClient hook (is_io_connect_method) hands every method's structure output here, with the
// calling task; the surface is looked up and measured with IOSurface's own functions (kits/SwitchABI IOSurface, bound by
// address in TegraVIC's original-bindings.S). Every reply that gives the calling task a CPU mapping of a surface is noted on
// the surface's record (TegraGM20BRecord.h tgm_record_mappers), which its layout decision reads.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread_call.h>
#include "OriginalIOSurfaceRootUserClient.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BRecord.h"
#include "TegraGM20BSurfacePrefault.h"
#include "TegraGM20BViews.h"

extern "C" {
// XNU's VM modified bit of a physical page (osfmk/vm/pmap.h): clearing it write-protects the page's every mapping
void pmap_clear_modify(ppnum_t pn);
boolean_t pmap_is_modified(ppnum_t pn);
IOMemoryDescriptor *tegra_gm20b_ios_memory(OSObject *surface);
uint64_t tegra_gm20b_ios_alloc_size(OSObject *surface);
// XNU's vm_pressure_level_t: 0 normal
extern int memorystatus_vm_pressure_level;
}

namespace
{
using RootClient = OriginalIOSurfaceRootUserClient;
constexpr TegraGM20BSurfaceCreateAbi kCreateAbi = {
	{static_cast<uint32_t>(RootClient::createSurfaceSelector),
	 static_cast<uint32_t>(RootClient::createSurfaceFastPathSelector),
	 static_cast<uint32_t>(RootClient::createSurfaceClientMemorySelector), ~0u},
	static_cast<uint32_t>(RootClient::createOutputSurfaceID)};
constexpr uint32_t kQueued =
	16;  // creates waiting for the thread call; past it a create is left to fault lazily

struct Created {
	uint32_t id;
	task_t task;  // referenced while queued
};

struct Prefault {
	IOLock *lock;
	thread_call_t call, expiry;
	Created queue[kQueued];
	uint32_t head, tail;                              // lock
	TegraGM20BPrefaultHeld<IOMemoryDescriptor> held;  // lock: prepared and retained
	uint64_t armed;  // lock: the expiry's deadline, 0 when not armed
};

Prefault *gPrefault;

void prefaultWork(thread_call_param_t param, thread_call_param_t);
void expiryWork(thread_call_param_t param, thread_call_param_t);

Prefault *prefault()
{
	Prefault *p = __atomic_load_n(&gPrefault, __ATOMIC_ACQUIRE);
	if (p)
		return p;
	Prefault *fresh = IONew(Prefault, 1);
	if (!fresh)
		return nullptr;
	bzero(fresh, sizeof(*fresh));
	fresh->lock = IOLockAlloc();
	fresh->call = fresh->lock ? thread_call_allocate_with_priority(prefaultWork, fresh,
								       THREAD_CALL_PRIORITY_USER)
				  : nullptr;
	fresh->expiry = fresh->call ? thread_call_allocate_with_options(expiryWork, fresh,
									THREAD_CALL_PRIORITY_USER,
									THREAD_CALL_OPTIONS_ONCE)
				    : nullptr;
	if (!fresh->expiry || !__atomic_compare_exchange_n(&gPrefault, &p, fresh, false,
							   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (fresh->expiry)
			thread_call_free(fresh->expiry);
		if (fresh->call)
			thread_call_free(fresh->call);
		if (fresh->lock)
			IOLockFree(fresh->lock);
		IODelete(fresh, Prefault, 1);
		return p;
	}
	return fresh;
}

bool pressure()
{
	return __atomic_load_n(&memorystatus_vm_pressure_level, __ATOMIC_RELAXED) != 0;
}

void giveBack(IOMemoryDescriptor *memory)
{
	memory->complete(kIODirectionInOut);
	memory->release();
}

// `visit(page)` for each VM page of the prepared `memory`; false when a segment is not whole pages
template <typename Visit> bool eachPage(IOMemoryDescriptor *memory, Visit &&visit)
{
	const uint64_t bytes = memory->getLength();
	for (uint64_t off = 0; off < bytes;) {
		IOByteCount len = 0;
		const addr64_t pa = memory->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		if (!pa || (pa & PAGE_MASK) || len < PAGE_SIZE)
			return false;
		for (uint64_t at = 0; at + PAGE_SIZE <= len && off + at < bytes; at += PAGE_SIZE)
			visit(ppnum_t(atop_64(pa + at)));
		off += len & ~uint64_t(PAGE_MASK);
	}
	return true;
}

// Cleans and invalidates every page of the prepared `memory` (64 at a time from its segments).
bool clean(IOMemoryDescriptor *memory)
{
	uint64_t pages[64];
	uint32_t n = 0;
	const uint64_t bytes = memory->getLength();
	for (uint64_t off = 0; off < bytes;) {
		IOByteCount len = 0;
		const addr64_t pa = memory->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		if (!pa || (pa & 0xfff) || len < 4096)
			return false;
		for (uint64_t at = 0; at + 4096 <= len && off + at < bytes; at += 4096) {
			pages[n++] = pa + at;
			if (n == 64) {
				tgm_cpu_clean_pages(pages, n);
				n = 0;
			}
		}
		off += len & ~0xfffULL;
	}
	if (n)
		tgm_cpu_clean_pages(pages, n);
	return true;
}

void arm(Prefault *p)
{
	const uint64_t next = p->held.next();
	if (next == p->armed)
		return;
	p->armed = next;
	if (next)
		thread_call_enter_delayed(p->expiry, next);
	else
		thread_call_cancel(p->expiry);
}

// `memory`, prepared and cleaned, held with that prepare for the first import of `record`'s surface; given back at once
// when that import already came (it cleaned for itself): it marked the record before it looked here
// (tgm_record_first_import), and this looks at the mark after holding.
void hold(Prefault *p, IOMemoryDescriptor *memory, tgm_record *record)
{
	uint64_t deadline = 0;
	clock_interval_to_deadline(uint32_t(kTegraGM20BPrefaultHeldNs / 1000), 1000, &deadline);
	memory->retain();
	IOLockLock(p->lock);
	IOMemoryDescriptor *out = p->held.hold(memory, deadline);
	arm(p);
	IOLockUnlock(p->lock);
	if (out)
		giveBack(out);
	if (!tgm_record_imported(record))
		return;
	IOLockLock(p->lock);
	IOMemoryDescriptor *mine = p->held.take(memory);
	arm(p);
	IOLockUnlock(p->lock);
	if (mine)
		giveBack(mine);
}

void faultIn(Prefault *p, const Created &c)
{
	OSObject *surface = tgm_surface_lookup(c.id, c.task);
	if (!surface)
		return;
	if (tegra_gm20b_prefault_wanted(
		    tegra_gm20b_ios_alloc_size(surface),
		    __atomic_load_n(&memorystatus_vm_pressure_level, __ATOMIC_RELAXED))) {
		IOMemoryDescriptor *memory = tegra_gm20b_ios_memory(surface);
		tgm_record *record = tgm_record_of(surface);
		if (memory && memory->prepare(kIODirectionInOut) == kIOReturnSuccess) {
			// the modified bits go first: a store between them and the clean is cleaned, one after sets its bit
			const bool watched = eachPage(memory, [](ppnum_t page) {
				pmap_clear_modify(page);
			});
			if (record && watched && !pressure() && clean(memory))
				hold(p, memory, record);
			else
				memory->complete(kIODirectionInOut);
		}
	}
	// NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): tgm_surface_lookup returns it retained
	surface->release();
}

void expiryWork(thread_call_param_t param, thread_call_param_t)
{
	Prefault *p = static_cast<Prefault *>(param);
	const uint64_t now = mach_absolute_time();
	const bool squeezed = pressure();
	for (;;) {
		IOLockLock(p->lock);
		IOMemoryDescriptor *out = p->held.expire(now, squeezed);
		if (!out) {
			p->armed = 0;
			arm(p);
		}
		IOLockUnlock(p->lock);
		if (!out)
			return;
		giveBack(out);
	}
}

void prefaultWork(thread_call_param_t param, thread_call_param_t)
{
	Prefault *p = static_cast<Prefault *>(param);
	for (;;) {
		IOLockLock(p->lock);
		if (p->head == p->tail) {
			IOLockUnlock(p->lock);
			return;
		}
		const Created c = p->queue[p->tail++ % kQueued];
		IOLockUnlock(p->lock);
		faultIn(p, c);
		task_deallocate(c.task);
	}
}
}  // namespace

extern "C" void tegra_gm20b_iosurface_method_done(uint32_t selector, const void *output,
						  uint32_t outputSize, task_t task)
{
	if (const uint32_t mapped = tegra_gm20b_mapped_surface(
		    static_cast<uint32_t>(RootClient::createOutputSize),
		    static_cast<uint32_t>(RootClient::createOutputSurfaceID), output, outputSize))
		tgm_surface_mapped(mapped, task);
	const uint32_t id = tegra_gm20b_created_surface(kCreateAbi, selector, output, outputSize);
	if (!id || __atomic_load_n(&memorystatus_vm_pressure_level, __ATOMIC_RELAXED))
		return;
	Prefault *p = prefault();
	if (!p)
		return;
	bool queued = false;
	IOLockLock(p->lock);
	if (p->head - p->tail < kQueued) {
		task_reference(task);
		p->queue[p->head++ % kQueued] = {id, task};
		queued = true;
	}
	IOLockUnlock(p->lock);
	if (queued)
		thread_call_enter(p->call);
}

// The prefault's wiring of `surface`'s memory, cleaned after its zero-fill and wired since, if it holds one and no CPU
// wrote a page of it since: the caller completes and releases it once its own wiring holds the pages. Taken before the
// caller wires the pages itself, which writes their modified bits.
extern "C" IOMemoryDescriptor *tegra_gm20b_prefault_take(OSObject *surface)
{
	Prefault *p = __atomic_load_n(&gPrefault, __ATOMIC_ACQUIRE);
	IOMemoryDescriptor *memory = p && surface ? tegra_gm20b_ios_memory(surface) : nullptr;
	if (!memory)
		return nullptr;
	IOLockLock(p->lock);
	IOMemoryDescriptor *held = p->held.take(memory);
	arm(p);
	IOLockUnlock(p->lock);
	if (!held)
		return nullptr;
	bool written = false;
	eachPage(held, [&](ppnum_t page) {
		written = written || pmap_is_modified(page);
	});
	if (!written)
		return held;
	giveBack(held);
	return nullptr;
}
