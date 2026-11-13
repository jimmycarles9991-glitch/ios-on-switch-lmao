// SPDX-License-Identifier: MIT
// BO allocation and import, and the allocation policy.
#include "TegraGM20BUserClientInternal.h"
#include "SwitchGpuAllocatedMemory.h"
#include "TegraGM20BRecord.h"

namespace
{
// A compressible BO's granule: one comptag line, one big page (TegraGM20BComp.h TGM_COMP_BYTES)
constexpr uint64_t kChunkBytes = 128ULL << 10;
constexpr uint32_t kBoFlags = static_cast<uint32_t>(TegraGM20BBoCoherent) |
			      static_cast<uint32_t>(TegraGM20BBoContiguous) |
			      static_cast<uint32_t>(TegraGM20BBoCompressible) |
			      static_cast<uint32_t>(TegraGM20BBoControl);

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

const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)
{
	const uint64_t avail = tgm_pool_avail();
	return tgm_pool::refusal(
		bytes, held, avail,
		{kPoolSmallBytes, kPoolFloorBytes, poolReserveBytes(), clientPoolBytes()},
		compositor);
}

const char *compressibleRefusal(uint64_t bytes, uint64_t held, bool compositor)
{
	const tgm_pool::Limits limits = {kPoolSmallBytes, kPoolFloorBytes, poolReserveBytes(),
					 clientPoolBytes()};
	const uint64_t avail = tgm_pool_avail();
	return tgm_pool::compressibleRefusal(bytes, held, avail, limits, compositor);
}

IOReturn takeChunks(uint64_t bytes, uint64_t *pas)
{
	return !tgm_pool_alloc_chunks(bytes, pas) ? kIOReturnSuccess : kIOReturnNoMemory;
}

// One BoAlloc of `owner`'s for the window's "mem3" line; `made`: a cached BO made on the caller's thread.
#if TGM_STATS
void ownerNote(const char *owner, uint64_t bytes, uint64_t ns, bool made)
{
	SparePool *p = spares();
	if (!p)
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
#endif
#if TGM_STATS
#else
void ownerNote(const char *, uint64_t, uint64_t, bool)
{
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
	const bool compressible = (in->flags & TegraGM20BBoCompressible) != 0u;
	if (!in->bytes || (in->bytes & 0xfff) || in->bytes > kMaxBoBytes || in->align_log2 < 12 ||
	    in->align_log2 > 21 || (in->flags & ~kBoFlags) ||
	    ((in->flags & TegraGM20BBoControl) && !(in->flags & TegraGM20BBoCoherent)) ||
	    (compressible &&
	     (in->flags != TegraGM20BBoCompressible || (in->bytes & (kChunkBytes - 1)))))
		return kIOReturnBadArgument;
	if (!boIndex_.reserve())
		return kIOReturnNoMemory;
	if (__atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED))
		scheduleOrphanRelease();  // not here: releasing a view can wait for another connection (releaseOrphans)
	const uint64_t start = statsNowNs();
	uint64_t phase[TGM_PHASES] = {};
	uint64_t mark = start;  // the stats: kernel allocation and wiring, clean, pages, nvkm
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
	bo->flags = in->flags;
	bo->npages = static_cast<uint32_t>(in->bytes >> 12);
	bo->backing_pages = bo->npages;
	bo->pas = IONew(uint64_t, bo->npages);
	IOReturn ret = bo->pas ? kIOReturnSuccess : kIOReturnNoMemory;
	int kind = TGM_BO_CACHED;
	Spare *spare = nullptr;
	const char *refused = nullptr;
	if (!ret && compressible) {
		refused = compressibleRefusal(in->bytes, pool_bytes_, critical_);
		ret = refused ? kIOReturnNoMemory : takeChunks(in->bytes, bo->pas);
		bo->chunked = ret == kIOReturnSuccess;
		if (ret && !compressibleLogged_) {
			IOLog("tegra-gm20b: %s: compressible BoAlloc of %llu KiB refused: %s "
			      "(client holds %llu MiB of the pool, %llu MiB free); "
			      "further ones not logged\n",
			      owner_, in->bytes >> 10, refused ? refused : "no free 128 KiB chunks",
			      pool_bytes_ >> 20, tgm_pool_avail() >> 20);
			compressibleLogged_ = true;
		}
	}
	// A coherent BO the pool does not serve (its tiers keep it out, or the pool has no room) is not refused: it is a
	// write-combined buffer of ordinary pages made below (tgm_pool::FromSystem). A refusal would abort clients such as
	// VectorKit (on Maps' first 4 MiB buffer).
	bool system = false;
	if (!ret && (in->flags & TegraGM20BBoCoherent)) {
		const bool control = tgm_pool::controlReserve(
			(in->flags & TegraGM20BBoControl) != 0, control_bytes_,
			(in->bytes + PAGE_SIZE - 1) & ~(uint64_t(PAGE_SIZE) - 1));
		const uint64_t readyReserve =
			control ? tgm_pool::KernelReadyBytes : tgm_pool::ControlReadyBytes;
		refused = control ? nullptr : poolRefusal(in->bytes, pool_bytes_, critical_);
		if (!refused) {
			uint64_t align =
				(in->flags & TegraGM20BBoContiguous) ? 1ULL << in->align_log2 : 0;
			if (in->bytes >= kChunkBytes && align < kChunkBytes)
				align = kChunkBytes;
			bo->pool_va =
				tgm_pool_alloc_ready(in->bytes, align, readyReserve, &bo->pool_pa);
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
	if (!ret && !bo->pool_va && !compressible && !system &&
	    !(in->flags & TegraGM20BBoContiguous))
		spare = spareTake(in->bytes);
	else if (TGM_STATS && !ret && !bo->pool_va && !compressible)
		spareIneligible();
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
		bo->cachedMapping = spare->mapping;
		IODelete(spare, Spare, 1);
		kind = TGM_BO_SPARE;
	} else if (!ret && !compressible) {
		IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
				       static_cast<IOOptionBits>(kIOMemoryMapperNone);
		// Made here only when no spare was ready (the class is kept warm from now on), at its class's size so that it
		// serves the class again once freed; the BO is its first pages.
		// A system BO is write-combined by its own buffer's VM object, which keeps the pages until its last mapping
		// goes. Not by a physical view: a device pager's private pages set a managed page's cache attribute, and reset
		// it to cacheable when they are freed, whenever that is (a dying task's mapping outlives the BO), under the
		// next owner's write-combined mapping, whose CPU writes then stay in the cache while the GPU reads DRAM (the
		// GPU fetched zeros from a raw push ring: PBENTRY). Nor from the spares, which are cacheable.
		// (kIOMemoryMapperNone shares a bit with kIOMapCacheMask; IOBMD keeps the cache mode's low three bits, so this
		// stays write-combined and the cached BOs stay default)
		if (system)
			options |= kIOMapWriteCombineCache;
		const uint32_t c = (in->flags & TegraGM20BBoContiguous) || system
					   ? kClasses
					   : classUp(in->bytes);
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
			cleanZeros(va, backing);
			lap(1);
			ret = collectKernelPages(va, backing, bo->pas);
			lap(2);
		}
	}
	// A cached or system BO's view, which its task maps: the descriptor itself, or its first in->bytes when it is a
	// spare's or a class's larger buffer.
	if (!ret && bo->bmd) {
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
	if (!ret) {
		if (bo->chunked)
			bo->backing = switch_gpu_pin_chunks(bo->pas, bo->npages);
		else if (bo->bmd && !system && !(in->flags & TegraGM20BBoContiguous)) {
			bo->backing = switch_gpu_pin_cached(bo->pas, bo->backing_pages, bo->npages,
							    bo->bmd, bo->cachedMapping);
			if (bo->backing)
				bo->cachedMapping = nullptr;
		} else
			bo->backing = switch_gpu_pin_memory(bo->pas, bo->npages, 0,
							    bo->bmd ? bo->bmd : bo->view,
							    bo->bmd != nullptr);
		if (!bo->backing)
			ret = kIOReturnNoMemory;
	}
	if (!ret && !bo->chunked && !(in->flags & TegraGM20BBoCoherent))
		ret = ioreturn(switch_gpu_backing_cpu_track(bo->backing));
	if (!ret) {
		ret = ioreturn(allocatedMemory(bo->backing, bo->npages, &bo->mem));
		bo->backingMemory = ret == kIOReturnSuccess;
	}
	lap(3);
	if (ret) {
		if (!quotaLogged_ && (bo->chunked || !compressible)) {
			IOLog("tegra-gm20b: BoAlloc %llu bytes flags %#x align 2^%u refused: %#x\n",
			      in->bytes, in->flags, in->align_log2, ret);
		}
		releaseBo(bo);
		return ret;
	}
	if (bo->chunked) {
		pool_bytes_ += bo->bytes;
		kind = TGM_BO_POOL;
		__atomic_add_fetch(&gCompressibleBytes, bo->bytes, __ATOMIC_RELAXED);
		__atomic_add_fetch(&gCompressibles, 1, __ATOMIC_RELAXED);
	}
	if (bo->pool_va) {
		pool_bytes_ += bo->bytes;
		if (bo->flags & TegraGM20BBoControl) {
			bo->controlCharge =
				(bo->bytes + PAGE_SIZE - 1) & ~(uint64_t(PAGE_SIZE) - 1);
			control_bytes_ += bo->controlCharge;
			switch_gpu_path_taken(SWITCH_GPU_PATH_CONTROL_BO, 1);
		}
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
	boIndex_.insert(bo->handle, bo);
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
	if (!surfaces_.reserve() || !boIndex_.reserve())
		return kIOReturnNoMemory;
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
	if (bo->surface) {
		tgm_record *record = tgm_record_of(bo->surface);
		const int fresh = record && tgm_record_first_import(record) &&
				  tgm_surface_unwritten(bo->surface);
		tgm_coherency_import(bo->coh, record, fresh);
	}
	const uint64_t added = TGM_STATS ? statsNowNs() : 0;
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
	boIndex_.insert(bo->handle, bo);
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

#include "SwitchGpuNativeImport.h"
