// SPDX-License-Identifier: MIT
// BoImport: a task's pages wired for the GPU.
#include "TegraGM20BUserClientInternal.h"
#include "TegraGM20BRecord.h"
#include "TegraGM20BSurfacePrefault.h"

extern "C" IOMemoryDescriptor *tegra_gm20b_prefault_take(OSObject *surface);

namespace
{
#if TGM_STATS
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
}  // namespace

// The page list of a prepared descriptor: one getPhysicalSegment per physically contiguous run (each call is a walk of the
// descriptor's DMA state; per 4 KiB page that would be 900 walks for a 1280x720 surface).
namespace tgm_uc
{
IOReturn collectPages(IOMemoryDescriptor *md, uint64_t bytes, uint64_t *pas)
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
}  // namespace tgm_uc

namespace
{
// The prefault's held wiring of an imported surface's memory (tegra_gm20b_prefault_take), kept until the import is done
class CreationWiring
{
public:
	CreationWiring(uint32_t surfaceId, task_t task)
	{
		OSObject *surface = surfaceId ? tgm_surface_lookup(surfaceId, task) : nullptr;
		memory_ = surface ? tegra_gm20b_prefault_take(surface) : nullptr;
		if (surface)
			surface->release();
	}

	~CreationWiring()
	{
		if (memory_) {
			memory_->complete(kIODirectionInOut);
			memory_->release();
		}
	}

	CreationWiring(const CreationWiring &) = delete;
	CreationWiring &operator=(const CreationWiring &) = delete;

	bool held() const
	{
		return memory_ != nullptr;
	}

private:
	IOMemoryDescriptor *memory_ = nullptr;
};
}  // namespace

// No lock_ until publishBo: the wiring (the VM's zero-fill of pages never touched), page list, memory object, coherency
// entry and clean are this BO's alone until then, so neither the connection's other calls nor its submissions wait.
IOReturn AGXDeviceUserClient::boImport(const TegraGM20BAbiBoImport *in,
				       TegraGM20BAbiBoAllocReply *out)
{
	if (!in->bytes || ((in->address | in->bytes) & 0xfff) || in->bytes > kMaxBoBytes)
		return kIOReturnBadArgument;
	// before this import's own wiring, which sets the pages' modified bits
	const CreationWiring creation(in->surface_id, task_);
	const uint64_t start = statsNowNs();
	uint64_t phase[TGM_PHASES] = {};
	uint64_t mark = start;  // the stats: wiring, pages, nvkm, clean
	const auto lap = [&](int i) {
		const uint64_t now = statsNowNs();
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
	if (!ret && in->surface_id) {
		bo->surface = switch_gpu_surface_hold(in->surface_id, task_, bo->pas, bo->npages);
		if (!bo->surface) {
			ret = kIOReturnBadArgument;
		} else {
			bo->unlockedReads = switch_gpu_surface_client_memory(bo->surface) != 0;
			bo->representation = switch_gpu_surface_generation(bo->surface);
			if (!bo->representation)
				ret = kIOReturnNoMemory;
			// The original imported pages need a checked refresh after representation replacement.
			bo->representationValue = 1;
		}
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
	tgm_coherency_rows_at(bo->pas[0]);
	const int gpuWrites =
		static_cast<int>(!bo->readOnly && (in->flags & TegraGM20BImportGpuReads) == 0u);
	bo->coh = tgm_coherency_add(bo->pas[0], in->surface_id, bo->pas, bo->npages,
				    static_cast<int>(!bo->readOnly), gpuWrites, &covered);
	bo->backing = switch_gpu_pin_memory(bo->pas, bo->npages, in->surface_id, bo->wired, true);
	if (bo->surface)
		bo->surface->retain();
	const int pinned = bo->backing
				   ? tgm_coherency_pin(bo->coh, bo->backing, bo->surface,
						       bo->surface ? tgm_memory_release : nullptr)
				   : -12;
	if (pinned && bo->surface)
		bo->surface->release();
	const int adopted = bo->backing && bo->surface
				    ? switch_gpu_surface_imported(bo->surface, bo->backing)
				    : 0;
	const int tracked = !pinned && !adopted ? switch_gpu_backing_cpu_track(bo->backing) : 0;
	if (pinned || adopted || tracked) {
		releaseBo(bo);
		return ioreturn(pinned ? pinned : adopted ? adopted : tracked);
	}
	int fresh = 0;
	if (bo->surface) {
		tgm_record *record = tgm_record_of(bo->surface);
		fresh = record && tgm_record_first_import(record) &&
			tgm_surface_unwritten(bo->surface);
		tgm_coherency_import(bo->coh, record, fresh);
	}
	const uint64_t added = TGM_STATS ? statsNowNs() : 0;
	// The GPU does not snoop the CPU caches: what the task wrote must be in DRAM before the GPU reads it, and no dirty
	// line (fresh pages are zeroed through the cache) may be written back later over what the GPU wrote. The prefault's
	// clean of a fresh surface's pages, wired and unwritten since, already did that (TegraGM20BSurfacePrefault.cpp).
	const bool cleans =
		tegra_gm20b_import_cleans(covered, creation.held(), fresh != 0, gpuWrites != 0);
	if (cleans)
		tgm_cpu_clean_pages(bo->pas, bo->npages);
	if (!covered)
		tgm_coherency_cleaned(bo->coh);
	lap(3);
#if TGM_STATS
	tgm_stats_unlocked(TGM_UNLOCKED_IMPORT, mark - start);
#endif
	const int readOnly = static_cast<int>(bo->readOnly);
	const uint64_t first = bo->pas[0];
	bo->ownerKind = SWITCH_GPU_OWNER_IMPORTED;
	IOLockLock(lock_);
	const IOReturn published = publishBo(bo, false);
	const uint64_t handle = bo->handle;
	const bool critical = critical_;
	if (!published) {
		tgm_stats_import(static_cast<int64_t>(in->bytes), first);
		// this connection may draw into what it imports (the GPU present asks)
		tgm_vm_import_note(vm_, first, 1);
	}
	IOLockUnlock(lock_);
	if (published) {
		releaseBo(bo);
		return published;
	}
	tgm_stats_bo_import(in->bytes, readOnly, !cleans, mark - start, phase);
	if (TGM_STATS) {
		cost.bytes = in->bytes;
		cost.total_ns = mark - start;
		cost.wire_ns = phase[0];
		cost.pages_ns = phase[1];
		cost.mem_ns = phase[2];
		cost.coh_ns = added - (mark - phase[3]);
		cost.clean_ns = mark - added;
		cost.surface_id = in->surface_id;
		cost.read_only = readOnly;
		cost.owner = owner_;
		tgm_stats_import_cost(&cost);
		if (critical)
			switch_gpu_compositor_import(cost.total_ns);
	}
	out->handle = handle;
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
