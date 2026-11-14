// SPDX-License-Identifier: MIT
// BO allocation and its pool policy, and a BO's publication.
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

// The pool tiers' answer for a BoAlloc and what it charged the connection, under lock_: BoAllocs of one connection made
// at once each see the others' charge, while the BO itself is made without lock_.
AGXDeviceUserClient::BoCharge AGXDeviceUserClient::chargeBo(const TegraGM20BAbiBoAlloc *in)
{
	BoCharge c = {nullptr, 0, 0, 0, critical_};
	if (in->flags & TegraGM20BBoCompressible) {
		c.refused = compressibleRefusal(in->bytes, pool_bytes_, critical_);
	} else if (in->flags & TegraGM20BBoCoherent) {
		const uint64_t pages = (in->bytes + PAGE_SIZE - 1) & ~(uint64_t(PAGE_SIZE) - 1);
		const bool control = tgm_pool::controlReserve(
			(in->flags & TegraGM20BBoControl) != 0, control_bytes_, pages);
		c.readyReserve = control ? tgm_pool::KernelReadyBytes : tgm_pool::ControlReadyBytes;
		c.refused = control ? nullptr : poolRefusal(in->bytes, pool_bytes_, critical_);
		c.control = !c.refused && (in->flags & TegraGM20BBoControl) ? pages : 0;
	}
	if (!c.refused && (in->flags & (TegraGM20BBoCompressible | TegraGM20BBoCoherent)))
		c.pool = in->bytes;
	pool_bytes_ += c.pool;
	control_bytes_ += c.control;
	return c;
}

// lock_ held: the BoAlloc's lines, its charge kept for what the BO holds of the pool and given back for the rest, and
// the BO published; anything but success leaves the caller to release it.
IOReturn AGXDeviceUserClient::settleBo(const TegraGM20BAbiBoAlloc *in, const BoCharge &charge,
				       Bo *bo, IOReturn ret, bool chunksRefused, bool system)
{
	if (chunksRefused && !compressibleLogged_) {
		IOLog("tegra-gm20b: %s: compressible BoAlloc of %llu KiB refused: %s "
		      "(client holds %llu MiB of the pool, %llu MiB free); "
		      "further ones not logged\n",
		      owner_, in->bytes >> 10,
		      charge.refused ? charge.refused : "no free 128 KiB chunks",
		      (pool_bytes_ - charge.pool) >> 20, tgm_pool_avail() >> 20);
		compressibleLogged_ = true;
	}
	if (system && !systemLogged_) {
		IOLog("tegra-gm20b: %s: coherent BoAlloc of %llu KiB from system memory, "
		      "write-combined: %s "
		      "(client holds %llu MiB of the pool, %llu MiB free); further ones "
		      "not logged\n",
		      owner_, in->bytes >> 10,
		      charge.refused ? charge.refused : "the pool has no room",
		      (pool_bytes_ - charge.pool) >> 20, tgm_pool_avail() >> 20);
	}
	systemLogged_ = systemLogged_ || system;
	if (!ret) {
		bo->system = system;
		bo->compositorPool = bo->pool_va && charge.critical;
		bo->controlCharge = bo->pool_va ? charge.control : 0;
		bo->ownerKind = bo->pool_va || bo->chunked ? SWITCH_GPU_OWNER_POOL
							   : SWITCH_GPU_OWNER_ALLOCATED;
		ret = publishBo(bo, true);
	}
	const bool pooled = !ret && (bo->chunked || bo->pool_va);
	if (!pooled)
		pool_bytes_ -= charge.pool;
	if (ret || !bo->controlCharge)
		control_bytes_ -= charge.control;
	if (ret && !quotaLogged_ && bo && (bo->chunked || !(in->flags & TegraGM20BBoCompressible)))
		IOLog("tegra-gm20b: BoAlloc %llu bytes flags %#x align 2^%u refused: %#x\n",
		      in->bytes, in->flags, in->align_log2, ret);
	if (ret)
		return ret;
	if (bo->chunked) {
		__atomic_add_fetch(&gCompressibleBytes, bo->bytes, __ATOMIC_RELAXED);
		__atomic_add_fetch(&gCompressibles, 1, __ATOMIC_RELAXED);
	}
	if (bo->controlCharge)
		switch_gpu_path_taken(SWITCH_GPU_PATH_CONTROL_BO, 1);
	if (bo->compositorPool)
		tgm_stats_compositor_pool(static_cast<int64_t>(bo->bytes));
	else if (bo->pool_va)
		__atomic_add_fetch(&gClientPoolBytes, bo->bytes, __ATOMIC_RELAXED);
	if (system) {
		__atomic_add_fetch(&gSystemCoherent, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&gSystemCoherentBytes, bo->bytes, __ATOMIC_RELAXED);
	}
	return kIOReturnSuccess;
}

// lock_ held: `bo`, made without it, becomes the connection's: a handle (and a map token), the BO list, the index
// SubmitJob reads with no lock (SwitchGpuBoIndex.h) and the imports' list. The submit gate stays open: nothing a job in
// flight reads changes.
IOReturn AGXDeviceUserClient::publishBo(Bo *bo, bool mapped)
{
	if (submitGate_.closed())
		return kIOReturnNotOpen;
	if (!boIndex_.reserve() || (bo->coh && !surfaces_.reserve()))
		return kIOReturnNoMemory;
	bo->handle = next_handle_++;
	bo->token = mapped ? next_token_++ : 0;
	bo->next = bos_;
	bos_ = bo;
	boIndex_.insert(bo->handle, bo);
	if (bo->coh)
		surfaces_.add(bo->handle, bo->coh, !bo->readOnly);
	if (SWITCH_GPU_OWNER_COUNTS)
		ownerMemory_.change(bo->ownerKind, bo->bytes, true);
	return kIOReturnSuccess;
}

// No lock_ but chargeBo's and settleBo's: the pool pages, a spare, or a buffer of fresh pages and its clean are this
// BO's alone until settleBo publishes it.
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
	if (__atomic_load_n(&gPoolOrphans, __ATOMIC_RELAXED))
		scheduleOrphanRelease();  // not here: releasing a view can wait for another connection (releaseOrphans)
	IOLockLock(lock_);
	const BoCharge charge = chargeBo(in);
	IOLockUnlock(lock_);
	const uint64_t start = statsNowNs();
	uint64_t phase[TGM_PHASES] = {};
	uint64_t mark = start;  // the stats: kernel allocation and wiring, clean, pages, nvkm
	const auto lap = [&](int i) {
		const uint64_t now = statsNowNs();
		phase[i] += now - mark;
		mark = now;
	};
	Bo *bo = IONew(Bo, 1);
	IOReturn ret = bo ? kIOReturnSuccess : kIOReturnNoMemory;
	if (bo) {
		bzero(bo, sizeof(*bo));
		bo->bytes = in->bytes;
		bo->flags = in->flags;
		bo->npages = static_cast<uint32_t>(in->bytes >> 12);
		bo->backing_pages = bo->npages;
		bo->pas = IONew(uint64_t, bo->npages);
		ret = bo->pas ? kIOReturnSuccess : kIOReturnNoMemory;
	}
	int kind = TGM_BO_CACHED;
	Spare *spare = nullptr;
	bool chunksRefused = false;
	if (!ret && compressible) {
		ret = charge.refused ? kIOReturnNoMemory : takeChunks(in->bytes, bo->pas);
		bo->chunked = ret == kIOReturnSuccess;
		chunksRefused = !bo->chunked;
	}
	// A coherent BO the pool does not serve (its tiers keep it out, or the pool has no room) is not refused: it is a
	// write-combined buffer of ordinary pages made below (tgm_pool::FromSystem). A refusal would abort clients such as
	// VectorKit (on Maps' first 4 MiB buffer).
	bool system = false;
	if (!ret && (in->flags & TegraGM20BBoCoherent)) {
		if (!charge.refused) {
			uint64_t align =
				(in->flags & TegraGM20BBoContiguous) ? 1ULL << in->align_log2 : 0;
			if (in->bytes >= kChunkBytes && align < kChunkBytes)
				align = kChunkBytes;
			bo->pool_va = tgm_pool_alloc_ready(in->bytes, align, charge.readyReserve,
							   &bo->pool_pa);
		}
		system = tgm_pool::coherentSource(charge.refused, bo->pool_va != nullptr) ==
			 tgm_pool::FromSystem;
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
		bo->buffer = spare->buffer;
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
		IOBufferMemoryDescriptor *bmd =
			bo->pas ? IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
					  kernel_task, options, backing, mask)
				: nullptr;
		if (!bmd || bmd->prepare() != kIOReturnSuccess) {
			if (bmd)
				bmd->release();
			ret = kIOReturnNoMemory;
		} else {
			bo->buffer = switch_gpu_cached_buffer_of(bmd);
			lap(0);
			// The pages come zeroed: the mask makes IOKit allocate with KMA_ZERO (IOKernelAllocateWithPhysicalRestrict),
			// and vm_page_alloc_list zero-fills each page through the cacheable physical aperture. Clean those zeros to
			// DRAM and drop the lines, so the GPU and a fresh client view see zeros and no dirty line is written back
			// later over what the GPU wrote. (A bzero here would be a second pass over the whole buffer.)
			void *va = bo->buffer.cpu;
			cleanZeros(va, backing);
			lap(1);
			ret = collectKernelPages(va, backing, bo->pas);
			lap(2);
		}
	}
	// A cached or system BO's view, which its task maps: the descriptor itself, or its first in->bytes when it is a
	// spare's or a class's larger buffer.
	if (!ret && bo->buffer.memory) {
		bo->view = bo->backing_pages == bo->npages
				   ? bo->buffer.memory
				   : IOSubMemoryDescriptor::withSubRange(
					     bo->buffer.memory, 0, in->bytes, kIODirectionInOut);
		if (bo->view == bo->buffer.memory) {
			bo->buffer.memory->retain();
		} else if (!bo->view) {
			ret = kIOReturnNoMemory;
		}
	}
	if (!ret) {
		if (bo->chunked)
			bo->backing = switch_gpu_pin_chunks(bo->pas, bo->npages);
		else if (bo->buffer.memory && !system && !(in->flags & TegraGM20BBoContiguous)) {
			bo->backing = switch_gpu_pin_cached(bo->pas, bo->backing_pages, bo->npages,
							    bo->buffer, bo->cachedMapping);
			if (bo->backing) {
				bo->cachedMapping = nullptr;
				bo->buffer.map = nullptr;
			}
		} else
			bo->backing = switch_gpu_pin_memory(bo->pas, bo->npages, 0,
							    bo->buffer.memory ? bo->buffer.memory
									      : bo->view,
							    bo->buffer.memory != nullptr);
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
	IOLockLock(lock_);
	ret = settleBo(in, charge, bo, ret, chunksRefused, system);
	const uint64_t handle = ret ? 0 : bo->handle;
	const uint32_t token = ret ? 0 : bo->token;
	if (!ret && (bo->chunked || bo->pool_va))
		kind = TGM_BO_POOL;
	IOLockUnlock(lock_);
	if (ret) {
		if (bo)
			releaseBo(bo);
		return ret;
	}
	tgm_stats_bo_alloc(in->bytes, kind, mark - start, phase, owner_);
	if (charge.critical)
		switch_gpu_compositor_alloc(mark - start, kind == TGM_BO_CACHED);
	ownerNote(owner_, in->bytes, mark - start, kind == TGM_BO_CACHED);
	out->handle = handle;
	out->map_token = token;
	out->reserved = 0;
	return kIOReturnSuccess;
}

#include "SwitchGpuNativeImport.h"
