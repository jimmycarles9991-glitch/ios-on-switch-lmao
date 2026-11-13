// SPDX-License-Identifier: MIT
// AGXDeviceUserClient: one connection = one GPU address space, implementing scratchpad/gpu/CONTRACT.md v1 over the wire
// records of TegraGM20BABI.h. Transport modelled on drivers/NativeAdreno/NativeAdrenoPublish.cpp: Apple user-client
// class name (sandbox profiles admit it), one open type, struct-in/struct-out methods with exact sizes, the same
// methods tunnelled through any admitted selector, and clientMemoryForType views cached per token so
// IOConnectUnmapMemory64 finds the same descriptor. GPU work goes through gpu_*.c (TegraGM20BGpu.h). Fence waits sleep
// until the GPU's nonstall interrupt wakes them; a fence found complete without that wake panics (no polled mode), and
// so does one that never completes.
#include "TegraGM20BUserClientInternal.h"
#include "SwitchGpuShaderCompiler.h"
#include "TegraGM20BCompilerABI.h"

namespace
{
// Connections system-wide: 16, or 64 with every process on the GPU (gpu_internal.h TGM_FENCE_USER_PAGES: one fence page each).
constexpr uint32_t kMaxConnections = 16, kMaxConnectionsAll = 64;
volatile uint32_t gConnections;

// A cached BO's view up to this size is mapped into its task with every page entered at once (kIOMapPrefault: one
// vm_map_enter_mem_object_prefault over the wired page list), not one page fault per 4 KiB page as the task first touches
// it. NVK fills such BOs (64 KiB command, upload and stream memory; small descriptor and query pools); larger ones (the
// runtime's pooled blocks, mapped whole and touched only where it places CPU-visible resources) keep lazy faults.
constexpr uint64_t kPrefaultMaxBytes = 256ULL << 10;
}  // namespace

OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)

#include "SwitchGpuClientRetirement.h"

	bool AGXDeviceUserClient::setUp(task_t task)
{
	task_ = task;
	proc_selfname(owner_, sizeof(owner_));

	lock_ = IOLockAlloc();
	unmapLock_ = IOLockAlloc();
	if (!lock_ || !unmapLock_ || !events_.initialize() || !resources_.initialize())
		return false;
	for (auto &submitLock : submitLocks_)
		if (!(submitLock = IOLockAlloc()))
			return false;
	for (auto &channel : eventChannels_)
		if (!channel.initialize())
			return false;
	retireCall_ = thread_call_allocate_with_options(
		retirementWork, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	wakeCall_ = thread_call_allocate_with_options(wakeWork, this, THREAD_CALL_PRIORITY_KERNEL,
						      THREAD_CALL_OPTIONS_ONCE);
	if (!retireCall_ || !wakeCall_)
		return false;
	if (tgm_vm_new(&vm_))
		return false;
	tgm_vm_claim_connection(vm_, this);
	// fence page: pool memory (write-combined), GPU-mapped in the kernel range, read-only to the client
	if (tgm_fence_page_get(&fence_va_, &fence_pa_) ||
	    tgm_mem_new(vm_, &fence_pa_, 1, &fence_mem_) ||
	    tgm_vm_kmap(vm_, fence_mem_, 4096, &fence_gpu_))
		return false;
	events_.attachWords(fence_va_, fence_gpu_, tgm_fence_offset(fence_va_),
			    switch_gpu_fence_hold, switch_gpu_fence_put);
	// the whole fence slot (gpu_internal.h TGM_FENCE_SLOT): one VM page
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
	switch_gpu_compiler_close(this);
	drainResources();
	terminate();
	return kIOReturnSuccess;
}

void AGXDeviceUserClient::free()
{
	if (lock_) {
		drainResources();
		if (retireCall_)
			thread_call_free(retireCall_);
		if (wakeCall_)
			thread_call_free(wakeCall_);
		if (binds_)
			IODelete(binds_, Binding, cap_binds_);
		freeJob(job_);
		for (auto &job : channelJobs_)
			freeJob(job);
		boIndex_.destroy();
		for (auto &submitLock : submitLocks_)
			if (submitLock)
				IOLockFree(submitLock);
		if (fence_view_)
			fence_view_->release();
		tgm_mem_del(&fence_mem_);
		tgm_vm_del(&vm_);
		tgm_fence_page_put(fence_va_);
		for (auto &channel : eventChannels_)
			channel.destroy();
		events_.destroy();
		resources_.destroy();
		IOLockFree(lock_);
		lock_ = nullptr;
	}
	if (unmapLock_) {
		IOLockFree(unmapLock_);
		unmapLock_ = nullptr;
	}
#if TGM_STATS
	for (tgm_bind_history *&history : bindHistory_)
		if (history)
			IOFree(history, sizeof(*history));
#endif
	if (counted_)
		__atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
	IOUserClient::free();
}

AGXDeviceUserClient::Bo *AGXDeviceUserClient::findAddress(uint64_t address, uint64_t bytes)
{
	const auto *binding = switch_gpu_binding_find(binds_, nbinds_, address, bytes);
	return binding ? binding->bo : nullptr;
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
	if (bo->wiring || bo->backingMemory)
		bo->mem = nullptr;
	tgm_mem_del(&bo->mem);
	if (bo->pool_va && bo->handle) {
		if (bo->compositorPool) {
			tgm_stats_compositor_pool(-static_cast<int64_t>(bo->bytes));
		} else {
			__atomic_sub_fetch(&gClientPoolBytes, bo->bytes, __ATOMIC_RELAXED);
		}
	}
	if (bo->chunked) {
		if (bo->handle) {
			__atomic_sub_fetch(&gCompressibleBytes, bo->bytes, __ATOMIC_RELAXED);
			__atomic_sub_fetch(&gCompressibles, 1, __ATOMIC_RELAXED);
		}
		if (!bo->backing)
			tgm_pool_free_chunks(bo->pas, bo->npages);
	}
	// A pool BO's view and pages go to the thread call (releaseOrphans: releasing the view can wait for an import on
	// another connection), which keeps the pages while a task still maps the view. Cached and system BOs need no such
	// care: their mappings hold the buffer itself, and releasing one waits for nothing.
	PoolOrphan *orphan = nullptr;
	if (bo->system && bo->handle) {
		__atomic_sub_fetch(&gSystemCoherent, 1, __ATOMIC_RELAXED);
		__atomic_sub_fetch(&gSystemCoherentBytes, bo->bytes, __ATOMIC_RELAXED);
	}
	if (bo->pool_va && bo->view)
		orphan = IONew(PoolOrphan, 1);
	if (orphan) {
		*orphan = {nullptr, bo->view, bo->pool_va, bo->pool_pa, bo->bytes, bo->handle != 0};
		__atomic_add_fetch(&gOrphanBytes, bo->bytes, __ATOMIC_RELAXED);
		__atomic_add_fetch(&gOrphans, 1, __ATOMIC_RELAXED);
		pushOrphan(orphan);
		scheduleOrphanRelease();
	} else {
		if (bo->view)
			bo->view->release();
		// a pool BO's pages are zeroed off this thread and go back clean; a cached BO no task maps any more goes to
		// the spares as it is (CE-zeroed before reuse, or given back to the VM)
		if (bo->pool_va &&
		    !(bo->handle && poolZeroLater(bo->pool_va, bo->pool_pa, bo->bytes)))
			tgm_pool_free(bo->pool_va, bo->bytes, bo->pool_pa);
	}
	if (bo->bmd && bo->handle && !bo->system && !(bo->flags & TegraGM20BBoContiguous) &&
	    spareKeep(bo->bmd, bo->pas, static_cast<uint64_t>(bo->backing_pages) << 12,
		      bo->cachedMapping)) {
		bo->bmd = nullptr;
		bo->pas = nullptr;
		bo->cachedMapping = nullptr;
	}
	switch_gpu_cached_mapping_drop(&bo->cachedMapping);
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
	if (bo->nativeView) {
		switch_native_surface_gpu_release(bo->nativeView);
		IODelete(bo->nativeView, switch_gpu_native_surface_view, 1);
		bo->pas = nullptr;
	}
	if (bo->pas)
		IODelete(bo->pas, uint64_t, bo->backing_pages);
	switch_gpu_backing_release(bo->backing);
	OSSafeReleaseNULL(bo->surface);
	IODelete(bo, Bo, 1);
}

// Sleeps without the connection lock (a waiter must not block submitters), so the channel can be destroyed meanwhile:
// it is looked up again, under the lock, before every look at it but one: a wake is first checked against the fence
// page alone (SwitchGpuFenceRecord.h), as the channel the wait began on.
IOReturn AGXDeviceUserClient::waitFence(const TegraGM20BAbiWaitFence *in)
{
	if (!in->channel || in->channel > kMaxChannels)
		return kIOReturnBadArgument;
	const uint64_t limit_ns = in->timeout_ns == UINT64_MAX ? 10000000000ULL : in->timeout_ns;
	uint64_t start = nowNs();
	uint64_t deadline = limit_ns > UINT64_MAX - start ? UINT64_MAX : start + limit_ns;
	const volatile uint32_t *record = fence_va_ + static_cast<size_t>(in->channel - 1) * 16;
	uint64_t waitedOn = 0;
	uint32_t waiter = 0;
	uint32_t seq = 0;
	bool woken = false;
	for (bool slept = false;; slept = true) {
		const uint32_t sleptOn = waiter;
		IOReturn ret = kIOReturnNotReady;
		uint64_t eventGeneration = 0;
		if (woken && waitedOn && switch_gpu_fence_passed(record, in->seqno, waitedOn)) {
			eventGeneration = eventChannels_[in->channel - 1].generation();
			if (switch_gpu_fence_identity(record) == waitedOn)
				ret = kIOReturnSuccess;
		}
		if (ret != kIOReturnSuccess) {
			IOLockLock(lock_);
			tgm_chan *chan = chans_[in->channel - 1];
			if (chan) {
				waiter = tgm_chan_waiter(chan);
				seq = tgm_waiter_seq(waiter);
				tgm_chan_want(chan, in->seqno);
				waitedOn = switch_gpu_chan_identity(chan);
			}
			const uint64_t now = nowNs();
			if (!chan) {
				ret = slept ? kIOReturnNotResponding : kIOReturnBadArgument;
			} else if (tgm_chan_error(chan)) {
				ret = kIOReturnNotResponding;
			} else if (static_cast<int32_t>(tgm_chan_completed(chan) - in->seqno) >=
				   0) {
				ret = kIOReturnSuccess;
				eventGeneration = eventChannels_[in->channel - 1].generation();
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
		}
		if (slept) {
			tgm_stats_wait(sleptOn, static_cast<int>(woken),
				       static_cast<int>(ret == kIOReturnSuccess));
		}
		if (ret == kIOReturnSuccess) {
			const int semantic = eventChannels_[in->channel - 1].wait(
				in->seqno, deadline, eventGeneration);
			return semantic > 0       ? kIOReturnSuccess
			       : semantic == -110 ? kIOReturnTimeout
			       : semantic == -19  ? kIOReturnNotResponding
						  : kIOReturnAborted;
		}
		if (ret != kIOReturnNotReady)
			return ret;
		woken = (tgm_waiter_idle(waiter, seq, deadline) != 0);
		if (!woken) {
			IOLockLock(lock_);
			tgm_chan *chan = chans_[in->channel - 1];
			const bool unwoken = (chan != nullptr) && tgm_chan_waiter(chan) == waiter &&
					     tgm_chan_announced(chan, in->seqno);
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
	if (submitGate_.closed()) {
		IOLockUnlock(lock_);
		map->unmap();
		map->release();
		IODelete(mapping, Mapping, 1);
		return kIOReturnNotOpen;
	}
	*mapping = {maps_, map, in->token, out->address};
	maps_ = mapping;
	IOLockUnlock(lock_);
	return kIOReturnSuccess;
}

// IOKit hands a second Map of the same token (no kIOMapUnique) the first one's IOMemoryMap, retained: every channel's Map of
// the connection's one fence page, which NVK and the raw path share, is the same map. Unmap drops this caller's reference and
// tears the map down only when no other Map holds it, or one channel's teardown unmaps every other channel's fence word.
IOReturn AGXDeviceUserClient::unmapToken(const TegraGM20BAbiUnmap *in)
{
	Mapping *found = nullptr;
	bool shared = false;
	IOLockLock(lock_);
	for (Mapping **p = &maps_; *p; p = &(*p)->next) {
		if ((*p)->token == in->token && (*p)->address == in->address) {
			found = *p;
			*p = found->next;
			break;
		}
	}
	for (const Mapping *m = maps_; found && m && !shared; m = m->next)
		shared = m->map == found->map;
	IOLockUnlock(lock_);
	if (!found)
		return kIOReturnBadArgument;
	if (!shared)
		found->map->unmap();
	found->map->release();
	IODelete(found, Mapping, 1);
	// A freed pool BO's pages wait for its last mapping (a client unmaps after its BoFree): release them now.
	if (__atomic_load_n(&gPoolOrphans, __ATOMIC_ACQUIRE))
		scheduleOrphanRelease();
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
	if (__atomic_load_n(&gPoolOrphans, __ATOMIC_ACQUIRE))
		scheduleOrphanRelease();
}

extern "C" uint32_t switch_gpu_info_flags(void)
{
	tgm_info info;
	tgm_get_info(&info);
	uint32_t flags = static_cast<uint32_t>(TegraGM20BInfoHostImport) | TegraGM20BInfoKernelMap |
			 TegraGM20BInfoOrder | TegraGM20BInfoSurfaceWrites |
			 TegraGM20BInfoSurfaceLayout | TegraGM20BInfoExecSurfaces |
			 TegraGM20BInfoPriority | TegraGM20BInfoKillRecord | TegraGM20BInfoSubmit |
			 TegraGM20BInfoCpuAccess | TegraGM20BInfoSubmitAddress |
			 TegraGM20BInfoCpuPreserveDirty | TegraGM20BInfoCanonicalSurfaceLayout |
			 TegraGM20BInfoPushFences;
	if (info.comptag_lines && switch_gpu_compression_on())
		flags |= TegraGM20BInfoCompression;
	if (switch_tiled_cache_on())
		flags |= TegraGM20BInfoTiledCache;
	return flags;
}

IOReturn AGXDeviceUserClient::getInfo(TegraGM20BAbiInfo *out)
{
	tgm_info info;
	tgm_get_info(&info);
	bzero(out, sizeof(*out));
	out->abi_version = TegraGM20BAbiVersion;
	out->flags = switch_gpu_info_flags();
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

#include "SwitchGpuUserEvents.h"
#include "SwitchGpuUserResources.h"
#include "SwitchGpuUserCpuComplete.h"

IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector,
					     IOExternalMethodArguments *arguments,
					     IOExternalMethodDispatch *dispatch, OSObject *target,
					     void *reference)
{
	(void)dispatch;
	(void)target;
	(void)reference;
	SwitchGpuAsyncReference asyncReference(arguments);
	return asyncReference.settle(externalCall(selector, arguments, asyncReference));
}

IOReturn AGXDeviceUserClient::externalCall(uint32_t selector, IOExternalMethodArguments *arguments,
					   SwitchGpuAsyncReference &asyncReference)
{
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
	IOReturn ret;
	if (switch_gpu_compiler_method(method)) {
		ret = switch_gpu_compiler_call(
			this, current_task(), method, inSize ? copy : nullptr, inSize,
			arguments->structureOutput, outSize, arguments, asyncReference);
	} else if (method == TegraGM20BMethodEventObserve &&
		   inSize == sizeof(TegraGM20BAbiEventObserve) && !outSize) {
		ret = eventObserve(static_cast<const TegraGM20BAbiEventObserve *>(copy), arguments,
				   asyncReference);
	} else if (method == TegraGM20BMethodResourceObserve &&
		   inSize == sizeof(TegraGM20BAbiResourceObserve) && !outSize) {
		ret = resourceObserve(static_cast<const TegraGM20BAbiResourceObserve *>(copy),
				      arguments, asyncReference);
	} else {
		ret = arguments->asyncReferenceCount ? kIOReturnBadArgument
						     : run(method, inSize ? copy : nullptr, inSize,
							   arguments->structureOutput, &outSize);
	}
	if (copy != small)
		IOFreeData(copy, inSize);
	arguments->structureOutputSize = ret ? 0 : outSize;
	return ret;
}

IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
						  IOMemoryDescriptor **memory)
{
#if TGM_STATS
	if (IOMemoryDescriptor *readback = tgm_readback_memory(this, type)) {
		*memory = readback;
		*options = kIOMapAnywhere | kIOMapDefaultCache;
		return kIOReturnSuccess;
	}
#endif
	if (type == TegraGM20BCompilerStoreToken)
		return switch_gpu_compiler_store(options, memory);
	IOLockLock(lock_);
	if (submitGate_.closed()) {
		IOLockUnlock(lock_);
		return kIOReturnNotOpen;
	}
	IOReturn ret = kIOReturnBadArgument;
	if (type == kFenceToken && fence_view_) {
		fence_view_->retain();
		*memory = fence_view_;
		*options = kIOMapAnywhere | kIOMapReadOnly | kIOMapWriteCombineCache;
		ret = kIOReturnSuccess;
	} else if (const Bo *bo = findToken(type); bo && bo->view) {
		bo->view->retain();
		*memory = bo->view;
		*options = kIOMapAnywhere |
			   ((bo->flags & TegraGM20BBoCoherent) ? kIOMapWriteCombineCache
							       : kIOMapDefaultCache);
		// Pool views are physical: IOKit puts their pages in the mapping's device pager at map time
		// (populateDevicePager), but enters them in the pmap only for kernel_map, so a task's first CPU touch of each
		// 4 KiB still faults (the "device" class on the kprof faults line), and IOKit drops kIOMapPrefault for device
		// pager memory. A cached BO's buffer is prepared (BoAlloc), which kIOMapPrefault requires.
		if (bo->bmd && bo->view == bo->bmd && !bo->system &&
		    bo->bytes <= kPrefaultMaxBytes) {
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
