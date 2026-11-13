// SPDX-License-Identifier: MIT
// The display's present: scanout copies and fences on the GPU.
#include "TegraGM20BUserClientInternal.h"

// GPU frame copy for the display (TegraScanoutCopy.h). The display offers each swapped surface; gpu_present.c's present channel
// copies it, after the compositor's GPU work, into the block-linear target, and calls native_legacy_gpu_complete from the
// GPU interrupt pass. Offers come one at a time under the display's gate: nothing here waits for the GPU.
//   targets   the display's scanout buffers (three: latest-frame-wins flips), physically contiguous, mapped once
//             each at kPresentTarget + i * 4 MiB, kind 0xfe
//   surfaces  the last four swapped surfaces, like the display's own surface maps: each keeps a retained and prepared
//             (wired) descriptor and its GPU mapping at kPresentSurface + i * 16 MiB; the least recently offered one goes
//             when a fifth surface comes. Its CPU caches are cleaned once, when it is imported (the GPU, which wrote it,
//             does not snoop them, and a dirty line written back later would land on its pixels).
//   read-back debug kernels: two buffers at 0x50000000 + i * 8 MiB (TegraGM20BReadback.cpp)
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
	switch_gpu_backing *backing;
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
SwitchGpuScanoutSnapshot gPresentSnapshot;

void releasePresentBacking(void *owner)
{
	auto *memory = static_cast<IOMemoryDescriptor *>(owner);
	memory->complete();
	memory->release();
}

void presentDone(void *display, int status)
{
	native_legacy_gpu_complete(display, status);
}

void dropSurface(PresentSurface &s, unsigned slot)
{
	if (!s.memory)
		return;
	tgm_present_unmap(kPresentSurface + slot * kPresentSurfaceSpan, s.bytes);
	tgm_mem_del(&s.mem);
	switch_gpu_backing_release(s.backing);
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
uint64_t presentSurface(IOMemoryDescriptor *memory, uint64_t bytes, bool blockLinear, uint64_t *pa,
			switch_gpu_backing **backing)
{
	bytes = (bytes + 0xfff) & ~0xfffULL;
	const uint32_t kind = blockLinear ? TGM_LAYOUT_KIND : 0;
	unsigned slot = 0;
	for (unsigned i = 0; i < 4; i++) {
		if (gSurfaces[i].memory == memory && gSurfaces[i].bytes >= bytes &&
		    gSurfaces[i].kind == kind) {
			gSurfaces[i].used = ++gSurfaceUses;
			*pa = gSurfaces[i].pas[0];
			*backing = gSurfaces[i].backing;
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
	memory->retain();
	if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
		memory->release();
		dropSurface(s, slot);
		return 0;
	}
	s.backing = switch_gpu_backing_register(s.pas, s.npages, 0, memory, releasePresentBacking);
	if (!s.backing)
		releasePresentBacking(memory);
	if (!s.backing || switch_gpu_surface_adopt(memory, s.backing)) {
		dropSurface(s, slot);
		return 0;
	}
	s.used = ++gSurfaceUses;
	*pa = s.pas[0];
	*backing = s.backing;
	return kPresentSurface + slot * kPresentSurfaceSpan;
}

// The display's direct frame with its producer still running: the producer's interrupt pass completes it, no job.
int presentWatched()
{
	return tgm_present_watch_dependencies(SwitchGpuScanoutSnapshot::complete, &gPresentSnapshot,
					      gPresentSnapshot.tokens, gPresentSnapshot.count);
}

IOLock *presentLock()
{
	if (!gPresentLock) {
		IOLock *fresh = IOLockAlloc();
		// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap writes the current value back into it
		IOLock *expected = nullptr;
		if (!__atomic_compare_exchange_n(&gPresentLock, &expected, fresh, false,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			IOLockFree(fresh);
	}
	return gPresentLock;
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
	const bool watch = fenceOnly && (copy->flags & TEGRA_SCANOUT_COPY_PRODUCER_WATCH) != 0u;
	const bool sourceBlock = (copy->flags & TEGRA_SCANOUT_COPY_SOURCE_BLOCK) != 0u;
	if (sourceBlock && copy->sourceBlockHeightLog2 > 5)
		return kIOReturnBadArgument;
	const int32_t blockLog2 =
		sourceBlock ? static_cast<int32_t>(copy->sourceBlockHeightLog2) : -1;
	const uint64_t sourceLines =
		sourceBlock ? (copy->height + (8ULL << blockLog2) - 1) & ~((8ULL << blockLog2) - 1)
			    : copy->height;
	if (copy->dependencyCount > SWITCH_SCANOUT_MAX_DEPENDENCIES ||
	    (copy->dependencyCount && !copy->dependencies) || !memory || !copy->width ||
	    !copy->height || copy->width > 16384 ||
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
	if (!presentLock())
		return kIOReturnNoMemory;
	IOLockLock(gPresentLock);
	if (tgm_present_outstanding(copy->display) ||
	    __atomic_load_n(&gPresentSnapshot.active, __ATOMIC_ACQUIRE)) {
		IOLockUnlock(gPresentLock);
		return kIOReturnBusy;
	}
	int ret = gPresentSnapshot.begin(copy->dependencies, copy->dependencyCount);
	if (ret) {
		IOLockUnlock(gPresentLock);
		return ioreturn(ret);
	}
	switch_gpu_backing *sourceBacking = nullptr;
	uint64_t sourcePa = 0;
	// fence only: no target; the source is still imported (its first page names the channels that write it)
	const uint64_t target = fenceOnly ? 0 : presentTarget(copy->target, copy->targetBytes);
	const uint64_t source = target || fenceOnly
					? presentSurface(memory, copy->sourceBytes, sourceBlock,
							 &sourcePa, &sourceBacking)
					: 0;
	ret = (target || fenceOnly) && source ? gPresentSnapshot.add(sourceBacking) : -12;
	if (!ret && copy->alsoWritten) {
		auto *also = switch_gpu_backing_find_pa(copy->alsoWritten & ~0xfffULL);
		ret = also ? gPresentSnapshot.add(also) : -19;
		switch_gpu_backing_release(also);
	}
	const bool ownershipBusy = ret == -16;
	if (!ret) {
		gPresentSnapshot.display = copy->display;
#if TGM_STATS
		tgm_present_read reads[TGM_PRESENT_READS] = {};
		const uint32_t nreads = tgm_readback_take(copy, source, blockLog2, target, reads);
		gPresentSnapshot.done = nreads ? tgm_readback_done : presentDone;
		ret = watch && !nreads ? presentWatched() : 1;
		if (ret == 1)
			ret = tgm_present_copy_read_dependencies(
				source, copy->sourceStride, blockLog2, copy->width, copy->height,
				target, copy->targetPitch, copy->blockHeightLog2,
				SwitchGpuScanoutSnapshot::complete, &gPresentSnapshot, reads,
				nreads, gPresentSnapshot.tokens, gPresentSnapshot.count);
		if (ret && nreads)
			tgm_readback_refused();
#else
		gPresentSnapshot.done = presentDone;
		ret = watch ? presentWatched() : 1;
		if (ret == 1)
			ret = tgm_present_copy_dependencies(
				source, copy->sourceStride, blockLog2, copy->width, copy->height,
				target, copy->targetPitch, copy->blockHeightLog2,
				SwitchGpuScanoutSnapshot::complete, &gPresentSnapshot,
				gPresentSnapshot.tokens, gPresentSnapshot.count);
#endif
	}
	if (ret)
		gPresentSnapshot.cancel();
	IOLockUnlock(gPresentLock);
	if (ownershipBusy)
		return kIOReturnCannotLock;
	return (ret == SWITCH_GPU_BACKING_DEPENDENCY_FAILED || ret == -5)
		       ? TEGRA_SCANOUT_SOURCE_LOST
	       : ret ? ioreturn(ret)
		     : 0;
}

extern "C" int tegra_gm20b_scanout_fence(void *display, uint64_t pa)
{
	if (!display || !pa || tgm_stage() < 3)
		return kIOReturnNotReady;
	tgm_vm *vm = nullptr;
	if (tgm_present_open(&vm))
		return kIOReturnNotReady;
	if (!presentLock())
		return kIOReturnNoMemory;
	auto *backing = switch_gpu_backing_find_pa(pa & ~0xfffULL);
	if (!backing)
		return kIOReturnNotFound;
	IOLockLock(gPresentLock);
	int ret = tgm_present_outstanding(display) ||
				  __atomic_load_n(&gPresentSnapshot.active, __ATOMIC_ACQUIRE)
			  ? -16
			  : gPresentSnapshot.begin(nullptr, 0);
	const bool presentBusy = ret == -16;
	if (!ret)
		ret = gPresentSnapshot.add(backing);
	const bool ownershipBusy = !presentBusy && ret == -16;
	if (!ret) {
		gPresentSnapshot.display = display;
		gPresentSnapshot.done = presentDone;
		// No copy (dst_va 0): only the acquires on the surface's producer and the fence; nothing of it is mapped.
		ret = tgm_present_copy_dependencies(
			0, 0, -1, 1, 1, 0, 0, 0, SwitchGpuScanoutSnapshot::complete,
			&gPresentSnapshot, gPresentSnapshot.tokens, gPresentSnapshot.count);
	}
	if (ret && !presentBusy)
		gPresentSnapshot.cancel();
	IOLockUnlock(gPresentLock);
	switch_gpu_backing_release(backing);
	if (presentBusy)
		return kIOReturnBusy;
	if (ownershipBusy)
		return kIOReturnCannotLock;
	return (ret == SWITCH_GPU_BACKING_DEPENDENCY_FAILED || ret == -5)
		       ? TEGRA_SCANOUT_SOURCE_LOST
	       : ret ? ioreturn(ret)
		     : 0;
}

extern "C" int tegra_gm20b_scanout_outstanding(const void *display)
{
	return (__atomic_load_n(&gPresentSnapshot.active, __ATOMIC_ACQUIRE) &&
		gPresentSnapshot.display == display) ||
	       tgm_present_outstanding(display);
}

extern "C" bool switch_native_surface_gpu_ready(void)
{
	return true;
}
