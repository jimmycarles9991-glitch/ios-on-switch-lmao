// SPDX-License-Identifier: MIT
// The display's present: scanout copies and fences on the GPU.
#include "TegraGM20BUserClientInternal.h"
#include "SwitchGpuSurfaceComp.h"
#include "TegraGM20BComp.h"

// GPU frame copy for the display (TegraScanoutCopy.h). The display offers each swapped surface; gpu_present.c's present channel
// copies it, after the compositor's GPU work, into the block-linear target, and calls native_legacy_gpu_complete from the
// GPU interrupt pass. Offers come one at a time under the display's gate: nothing here waits for the GPU.
//   targets   the display's scanout buffers (three: latest-frame-wins flips), physically contiguous, mapped once
//             each at kPresentTarget + i * 4 MiB, kind 0xfe
//   surfaces  recently swapped surfaces, like the display's own surface maps: each keeps a retained and prepared
//             (wired) descriptor and its GPU mapping at kPresentSurface + i * 16 MiB; the least recently offered one goes
//             when the table fills. Its CPU caches are cleaned once, when it is imported (the GPU, which wrote it,
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
	tgm_vm *vm;
	tgm_mem *mem;
	switch_gpu_mapping *mapping;
	bool ownsMemory;
#if DEVELOPMENT || DEBUG
	// the content generation its last accepted present showed, and that present
	uint64_t shownContent, shownAt;
#endif
};

// TegraScanoutCopy.h: the boot framebuffer and the display's two other buffers
constexpr unsigned kPresentTargets = 3, kPresentSurfaces = 8;
PresentTarget gTargets[kPresentTargets];
PresentSurface gSurfaces[kPresentSurfaces];
uint64_t gSurfaceUses;
IOLock *gPresentLock;
SwitchGpuScanoutSnapshot gPresentSnapshot;

void releasePresentBacking(void *owner)
{
	auto *memory = static_cast<IOMemoryDescriptor *>(owner);
	memory->complete();
	memory->release();
}

#if DEVELOPMENT || DEBUG
void logUnshown(int status);
#endif

void presentDone(void *display, int status)
{
#if DEVELOPMENT || DEBUG
	logUnshown(status);
#endif
	native_legacy_gpu_complete(display, status);
}

void dropSurface(PresentSurface &s)
{
	if (!s.memory)
		return;
	if (s.vm && s.mapping)
		switch_gpu_vm_unmap(s.vm, &s.mapping);
	if (s.ownsMemory)
		tgm_mem_del(&s.mem);
	else
		s.mem = nullptr;
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

uint32_t presentCompressedKind(IOMemoryDescriptor *memory)
{
	if (!memory || memory->prepare(kIODirectionOut) != kIOReturnSuccess)
		return 0;
	IOByteCount len = 0;
	const addr64_t first = memory->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
	memory->complete(kIODirectionOut);
	if (!first)
		return 0;
	const uint32_t kind = tegra_gm20b_surface_kind(first & ~0xfffULL);
	return switch_gpu_surface_kind_compressed(kind) ? kind : 0;
}

// The GPU address of the surface, imported on first use (or again after it fell out of the four, or became block-linear);
// *pa its first page. A block-linear surface is mapped with its decided kind (compressed kinds non-volatile at 64 KiB
// via switch_gpu_vm_map so L2/CBC decompresses and unmap tears down LPT PTEs and flushes LTC).
uint64_t presentSurface(IOMemoryDescriptor *memory, uint64_t bytes, bool blockLinear, uint64_t *pa,
			switch_gpu_backing **backing)
{
	bytes = (bytes + 0xfff) & ~0xfffULL;
	const uint32_t compKind = blockLinear ? presentCompressedKind(memory) : 0u;
	bool compressed = compKind != 0u;
	uint32_t kind = compressed ? compKind : (blockLinear ? TGM_LAYOUT_KIND : 0);
	unsigned slot = 0;
	bool replaceSlot = false;
	for (unsigned i = 0; i < kPresentSurfaces; i++) {
		if (gSurfaces[i].memory == memory) {
			bool staleAlias = false;
			if (gSurfaces[i].ownsMemory && gSurfaces[i].backing) {
				tgm_mem *canonical = nullptr;
				const int found = switch_gpu_backing_tgm_mem_existing(
					gSurfaces[i].backing, gSurfaces[i].npages, &canonical);
				staleAlias = found == 0 && canonical != gSurfaces[i].mem;
			}
			if (gSurfaces[i].bytes >= bytes && gSurfaces[i].kind == kind &&
			    !staleAlias) {
				gSurfaces[i].used = ++gSurfaceUses;
				*pa = gSurfaces[i].pas[0];
				*backing = gSurfaces[i].backing;
				return kPresentSurface + i * kPresentSurfaceSpan;
			}
			dropSurface(gSurfaces[i]);
			slot = i;
			replaceSlot = true;
			continue;
		}
		if (!replaceSlot && gSurfaces[i].used < gSurfaces[slot].used)
			slot = i;
	}
	const uint64_t fullBytes = (memory->getLength() + 0xfff) & ~0xfffULL;
	if (bytes > kPresentSurfaceSpan || bytes > fullBytes)
		return 0;
	PresentSurface &s = gSurfaces[slot];
	dropSurface(s);
	if (memory->prepare(kIODirectionOut) != kIOReturnSuccess)
		return 0;
	memory->retain();
	if (blockLinear && !compressed) {
		const uint32_t preparedKind = presentCompressedKind(memory);
		if (preparedKind) {
			compressed = true;
			kind = preparedKind;
		}
	}
	uint64_t mapBytes = bytes;
	uint32_t mapPages = static_cast<uint32_t>(bytes >> 12);
	if (compressed) {
		IOByteCount firstLen = 0;
		const addr64_t firstPa =
			memory->getPhysicalSegment(0, &firstLen, kIOMemoryMapperNone);
		s.backing = firstPa ? switch_gpu_backing_find_pa(firstPa & ~0xfffULL) : nullptr;
		switch_gpu_backing_info info = {};
		if (s.backing)
			switch_gpu_backing_describe(s.backing, &info);
		mapPages = info.page_count;
		mapBytes = static_cast<uint64_t>(mapPages) << 12;
		if (!s.backing || !info.pages || !mapPages || mapBytes < bytes ||
		    mapBytes > kPresentSurfaceSpan || mapBytes > fullBytes ||
		    (mapPages % TGM_COMP_PAGES) != 0u) {
			switch_gpu_backing_release(s.backing);
			memory->complete();
			memory->release();
			s = {};
			return 0;
		}
	}
	s.pas = IONew(uint64_t, mapPages);
	if (!s.pas) {
		switch_gpu_backing_release(s.backing);
		memory->complete();
		memory->release();
		s = {};
		return 0;
	}
	s.memory = memory;
	s.bytes = mapBytes;
	s.npages = mapPages;
	s.kind = kind;
	if (collectPages(memory, mapBytes, s.pas)) {
		dropSurface(s);
		return 0;
	}
	if (compressed) {
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(s.backing, &info);
		if (info.page_count != mapPages || !info.pages ||
		    memcmp(s.pas, info.pages, static_cast<size_t>(mapPages) * sizeof(uint64_t)) !=
			    0 ||
		    switch_gpu_surface_adopt(memory, s.backing)) {
			dropSurface(s);
			return 0;
		}
	} else {
		memory->retain();
		if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
			memory->release();
			dropSurface(s);
			return 0;
		}
		s.backing = switch_gpu_backing_register(s.pas, s.npages, 0, memory,
							releasePresentBacking);
		if (!s.backing) {
			releasePresentBacking(memory);
			dropSurface(s);
			return 0;
		}
		if (switch_gpu_surface_adopt(memory, s.backing)) {
			dropSurface(s);
			return 0;
		}
	}
	tgm_vm *vm = nullptr;
	if (tgm_present_open(&vm)) {
		dropSurface(s);
		return 0;
	}
	s.vm = vm;
	const uint64_t va = kPresentSurface + slot * kPresentSurfaceSpan;
	int mapped = 0;
	if (compressed) {
		s.ownsMemory = false;
		int memRet = switch_gpu_backing_tgm_mem_existing(s.backing, s.npages, &s.mem);
		if (memRet)
			memRet = switch_gpu_backing_tgm_mem(s.backing, s.npages, &s.mem);
		if (memRet || !tgm_mem_can_compress(s.mem)) {
			s.mem = nullptr;
			dropSurface(s);
			return 0;
		}
		mapped = switch_gpu_vm_map(vm, va, mapBytes, s.mem, 0, static_cast<uint8_t>(kind),
					   0, &s.mapping);
	} else {
		if (switch_gpu_backing_tgm_mem_existing_or_direct(s.backing, s.npages, &s.mem) ==
		    0) {
			s.ownsMemory = false;
		} else {
			s.ownsMemory = true;
			if (tgm_mem_new(vm, s.pas, s.npages, &s.mem)) {
				dropSurface(s);
				return 0;
			}
		}
		mapped = switch_gpu_vm_map_volatile(vm, va, mapBytes, s.mem, 0,
						    static_cast<uint8_t>(kind), 0, 1, &s.mapping);
	}
	if (mapped) {
		dropSurface(s);
		return 0;
	}
	s.used = ++gSurfaceUses;
	*pa = s.pas[0];
	*backing = s.backing;
	return va;
}

switch_gpu_backing *presentFenceBacking(IOMemoryDescriptor *memory, uint64_t bytes)
{
	bytes = (bytes + 0xfff) & ~0xfffULL;
	const uint64_t fullBytes = (memory->getLength() + 0xfff) & ~0xfffULL;
	if (!bytes || bytes > fullBytes)
		return nullptr;
	tgm_vm *vm = nullptr;
	if (tgm_present_open(&vm))
		return nullptr;
	if (memory->prepare(kIODirectionOut) != kIOReturnSuccess)
		return nullptr;
	IOByteCount firstLen = 0;
	const addr64_t firstPa = memory->getPhysicalSegment(0, &firstLen, kIOMemoryMapperNone);
	memory->complete();
	return firstPa ? switch_gpu_backing_find_pa(firstPa & ~0xfffULL) : nullptr;
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

static_assert(SWITCH_GPU_BACKING_WRITER_GONE == TEGRA_SCANOUT_SOURCE_GONE &&
		      SWITCH_GPU_BACKING_WRITER_FAILED == TEGRA_SCANOUT_SOURCE_LOST &&
		      SWITCH_GPU_BACKING_WRITE_REFUSED == TEGRA_SCANOUT_SOURCE_REFUSED,
	      "a present's completion passes why its frame is missing to the display as is");

// Why a frame its writer did not leave is missing (SwitchGpuBacking.h)
bool unshown(int status)
{
	return status == SWITCH_GPU_BACKING_WRITER_GONE ||
	       status == SWITCH_GPU_BACKING_WRITER_FAILED ||
	       status == SWITCH_GPU_BACKING_WRITE_REFUSED;
}

// An offer refused for such a frame answers why (TegraScanoutCopy.h); any other refusal is a decline.
int offerStatus(int ret)
{
	return unshown(ret) ? ret : ioreturn(ret);
}

#if DEVELOPMENT || DEBUG
// An accepted present's surfaces as its snapshot found them. Read from the snapshot's leases before the offer, whose
// completion may release them on another CPU; counted under gPresentLock; logged after it is let go.
struct PresentShown {
	switch_gpu_scanout_look looks[2];
	uint32_t looked, dependencies;
	bool alsoWritten;
	uint64_t present, lastShown;
	uint32_t againNth;  // from 1, the line's place among its kind; 0: nothing shown again
	// an offer refused for a frame its writer did not leave: why, the surface offered and the line's place
	int refused;
	uint32_t refusedSurface, refusedNth;
	uint64_t refusedAllocation;
};

uint64_t gPresents;
uint32_t gShownAgain, gRefused;

const char *whyUnshown(int status)
{
	return status == SWITCH_GPU_BACKING_WRITER_GONE     ? "its writer's client is gone"
	       : status == SWITCH_GPU_BACKING_WRITE_REFUSED ? "its write was never accepted"
							    : "its writer failed on the GPU";
}

// An accepted present that ends without its frame (its completion's status): one line, as a refused offer's.
void logUnshown(int status)
{
	if (!unshown(status))
		return;
	const uint32_t nth = __atomic_add_fetch(&gRefused, 1u, __ATOMIC_RELAXED);
	if (switch_gpu_refusal::logged(nth - 1))
		IOLog("SwitchGpu: present %llu ended unshown: %s (%d) (%u so far)\n",
		      __atomic_load_n(&gPresents, __ATOMIC_RELAXED), whyUnshown(status), status,
		      nth);
}

// ret: the snapshot's; nothing to read unless it was taken
PresentShown presentLooks(int ret, uint32_t dependencies, bool alsoWritten)
{
	PresentShown shown = {};
	if (ret)
		return shown;
	for (; shown.looked < gPresentSnapshot.leaseCount; ++shown.looked)
		switch_gpu_cpu_lease_look(gPresentSnapshot.leases[shown.looked],
					  &shown.looks[shown.looked]);
	shown.dependencies = dependencies;
	shown.alsoWritten = alsoWritten;
	return shown;
}

// gPresentLock held; ret: the offer's, counted only when accepted. source: the GPU address presentSurface gave the
// source (0: none, a plane's fence). A pending writer promise is a write still to come, so its surface is not shown
// again whatever its generation. offered: the surface the offer named, described for a refusal's line.
void presentShown(PresentShown &shown, int ret, uint64_t source, switch_gpu_backing *offered)
{
	if (unshown(ret)) {
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(offered, &info);
		shown.refused = ret;
		shown.refusedSurface = info.surface_id;
		shown.refusedAllocation = info.allocation_id;
		shown.refusedNth = __atomic_add_fetch(&gRefused, 1u, __ATOMIC_RELAXED);
	}
	if (ret)
		return;
	shown.present = __atomic_add_fetch(&gPresents, 1u, __ATOMIC_RELAXED);
	if (!source)
		return;
	PresentSurface &s = gSurfaces[(source - kPresentSurface) / kPresentSurfaceSpan];
	const switch_gpu_scanout_look &look = shown.looks[0];
	if (s.shownAt && s.shownContent == look.content && !look.promised) {
		shown.lastShown = s.shownAt;
		shown.againNth = ++gShownAgain;
	}
	s.shownContent = look.content;
	s.shownAt = shown.present;
}

void logPresentShown(const PresentShown &shown)
{
	char process[17];
	if (shown.refused && switch_gpu_refusal::logged(shown.refusedNth - 1))
		IOLog("SwitchGpu: present of surface %u (allocation %llu) refused: %s (%d) (%u so "
		      "far)\n",
		      shown.refusedSurface, shown.refusedAllocation, whyUnshown(shown.refused),
		      shown.refused, shown.refusedNth);
	if (!shown.againNth || !switch_gpu_refusal::logged(shown.againNth - 1))
		return;
	const switch_gpu_scanout_look &look = shown.looks[0];
	switch_gpu_writer_process(&look.writer, process, sizeof(process));
	IOLog("SwitchGpu: present %llu shows surface %u (allocation %llu) again with nothing "
	      "written since present %llu, %llu presents ago (generation %llu, writer engine %u "
	      "queue %u (%s) job %llu); %s (%u so far)\n",
	      shown.present, look.surface, look.allocation, shown.lastShown,
	      shown.present - shown.lastShown, look.content, look.writer.engine, look.writer.queue,
	      process, look.writer.value,
	      shown.dependencies || shown.alsoWritten ? "a window B change goes with it"
						      : "nothing else changes with it",
	      shown.againNth);
}
#endif

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
	    (!fenceOnly && sourceBlock && (copy->width * 4) % 64) ||
	    (fenceOnly && sourceBlock && (copy->sourceStride % 64)) ||
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
	if (tgm_present_outstanding(nullptr) ||
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
	switch_gpu_backing *ownedSourceBacking = nullptr;
	uint64_t sourcePa = 0;
	// fence only: no target; the source is still imported (its first page names the channels that write it)
	const uint64_t target = fenceOnly ? 0 : presentTarget(copy->target, copy->targetBytes);
	const uint64_t source = target || fenceOnly
					? presentSurface(memory, copy->sourceBytes, sourceBlock,
							 &sourcePa, &sourceBacking)
					: 0;
	if (fenceOnly && !source) {
		ownedSourceBacking = presentFenceBacking(memory, copy->sourceBytes);
		sourceBacking = ownedSourceBacking;
	}
	const bool haveSource = fenceOnly ? sourceBacking != nullptr : (target != 0 && source != 0);
	ret = haveSource ? gPresentSnapshot.add(sourceBacking) : -12;
	if (!ret && copy->alsoWritten) {
		auto *also = switch_gpu_backing_find_pa(copy->alsoWritten & ~0xfffULL);
		ret = also ? gPresentSnapshot.add(also) : -19;
		switch_gpu_backing_release(also);
	}
	const bool ownershipBusy = ret == -16;
#if DEVELOPMENT || DEBUG
	PresentShown shown = presentLooks(ret, copy->dependencyCount, copy->alsoWritten != 0);
#endif
	if (!ret) {
		gPresentSnapshot.display = copy->display;
		tgm_present_set_owner(copy->display);
		tgm_present_set_filter_display(copy->display);
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
#if DEVELOPMENT || DEBUG
	presentShown(shown, ret, source, sourceBacking);
#endif
	IOLockUnlock(gPresentLock);
#if DEVELOPMENT || DEBUG
	logPresentShown(shown);
#endif
	switch_gpu_backing_release(ownedSourceBacking);
	if (ownershipBusy)
		return kIOReturnCannotLock;
	return offerStatus(ret);
}

#if TGM_STATS
extern "C" void tegra_gm20b_scanout_bypassed(void)
{
	if (!presentLock())
		return;
	IOLockLock(gPresentLock);
	tgm_readback_bypassed();
	IOLockUnlock(gPresentLock);
}
#endif

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
	int ret = tgm_present_outstanding(nullptr) ||
				  __atomic_load_n(&gPresentSnapshot.active, __ATOMIC_ACQUIRE)
			  ? -16
			  : gPresentSnapshot.begin(nullptr, 0);
	const bool presentBusy = ret == -16;
	if (!ret)
		ret = gPresentSnapshot.add(backing);
	const bool ownershipBusy = !presentBusy && ret == -16;
#if DEVELOPMENT || DEBUG
	PresentShown shown = presentLooks(ret, 0, false);
#endif
	if (!ret) {
		gPresentSnapshot.display = display;
		tgm_present_set_owner(display);
		tgm_present_set_filter_display(nullptr);
		gPresentSnapshot.done = presentDone;
		// No copy (dst_va 0): only the acquires on the surface's producer and the fence; nothing of it is mapped.
		ret = tgm_present_copy_dependencies(
			0, 0, -1, 1, 1, 0, 0, 0, SwitchGpuScanoutSnapshot::complete,
			&gPresentSnapshot, gPresentSnapshot.tokens, gPresentSnapshot.count);
	}
	if (ret && !presentBusy)
		gPresentSnapshot.cancel();
#if DEVELOPMENT || DEBUG
	presentShown(shown, ret, 0, backing);
#endif
	IOLockUnlock(gPresentLock);
#if DEVELOPMENT || DEBUG
	logPresentShown(shown);
#endif
	switch_gpu_backing_release(backing);
	if (presentBusy)
		return kIOReturnBusy;
	if (ownershipBusy)
		return kIOReturnCannotLock;
	return offerStatus(ret);
}

extern "C" int tegra_gm20b_scanout_outstanding(const void *display)
{
	const bool active = __atomic_load_n(&gPresentSnapshot.active, __ATOMIC_ACQUIRE) != 0;
	return (active && (!display || gPresentSnapshot.display == display)) ||
	       tgm_present_outstanding(display);
}

extern "C" bool switch_native_surface_gpu_ready(void)
{
	return true;
}
