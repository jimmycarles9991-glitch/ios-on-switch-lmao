// SPDX-License-Identifier: MIT
// A large new IOSurface's pages faulted in on a thread call, so the GPU import that follows (BoImport's wiring, on the
// render thread) finds them resident instead of having the VM zero-fill each one there. The VM still does the zeroing
// (IOMemoryDescriptor::prepare of the surface's own descriptor, then complete), so no one sees stale data; the import
// takes no lock of this file, and racing the fault it only waits on the VM's busy pages, for work it would do itself.
// kern/switch's IOSurfaceRootUserClient hook (is_io_connect_method) hands every method's structure output here, with the
// calling task; the surface is looked up and measured with IOSurface's own functions (kits/SwitchABI IOSurface, bound by
// address in TegraVIC's original-bindings.S).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <kern/task.h>
#include <kern/thread_call.h>
#include "OriginalIOSurfaceRootUserClient.h"
#include "TegraGM20BSurfacePrefault.h"
#include "TegraGM20BViews.h"

extern "C" {
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
	thread_call_t call;
	Created queue[kQueued];
	uint32_t head, tail;  // lock
};

Prefault *gPrefault;

void prefaultWork(thread_call_param_t param, thread_call_param_t);

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
	if (!fresh->call || !__atomic_compare_exchange_n(&gPrefault, &p, fresh, false,
							 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (fresh->call)
			thread_call_free(fresh->call);
		if (fresh->lock)
			IOLockFree(fresh->lock);
		IODelete(fresh, Prefault, 1);
		return p;
	}
	return fresh;
}

void faultIn(const Prefault *p, const Created &c)
{
	OSObject *surface = tgm_surface_lookup(c.id, c.task);
	if (!surface)
		return;
	if (tegra_gm20b_prefault_wanted(
		    tegra_gm20b_ios_alloc_size(surface),
		    __atomic_load_n(&memorystatus_vm_pressure_level, __ATOMIC_RELAXED))) {
		IOMemoryDescriptor *memory = tegra_gm20b_ios_memory(surface);
		if (memory && memory->prepare(kIODirectionInOut) == kIOReturnSuccess)
			memory->complete(kIODirectionInOut);
	}
	// NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): tgm_surface_lookup returns it retained
	surface->release();
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
