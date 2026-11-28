// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/resource.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include "SwitchGpuMapping.h"
#include "SwitchGpuPackImage.h"
#include "SwitchGpuPackTransfer.h"
#include "TegraGM20BGpu.h"

#ifndef O_NOFOLLOW_ANY
#define O_NOFOLLOW_ANY 0x20000000
#endif

// The boot replay's class in the SD driver (big/boot: kernel/switch/vm/prefetch_vm.c switch_vm_prefetch_is_replay_thread,
// SwitchSD's kSwitchSdReplayClass, one class below tier-0 demand reads): `thread`'s reads join it while `on`. The kernel's
// static link resolves no undefined weak reference, so this default stands where big/boot's definition is absent, and the
// reads keep their own class.
extern "C" __attribute__((weak)) void switch_vm_prefetch_replay_thread(void *, int)
{
}

extern "C" {
// NOLINTNEXTLINE(readability-redundant-declaration): not declared in every kernel config's headers
kern_return_t thread_terminate(thread_t thread);
// NOLINTNEXTLINE(readability-redundant-declaration)
vfs_context_t vfs_context_kernel(void);
}

namespace
{
struct PackImageStore {
	IOLock *lock;
	SwitchGpuPackState state;
	bool buildActive;
	uint8_t bootConditions;  // kVolumesMounted | kGpuUsable, as each has happened
#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
	uint32_t refusedCount;
#endif
	SwitchGpuPackHeader header;
	uint64_t fileSize;
	uint64_t *pas;
	uint32_t npages;
	tgm_mem *mem;
	IOMemoryDescriptor *cpuMemory;
	// The stream: its phase (kPhase*, read without the lock at each job's submission), the word it raises to the chunks
	// landed (in the kernel's fence page, every address space's fence view), that word's offset in the fence pool, and
	// the chunks it ends at
	uint32_t phase;
	volatile uint32_t *frontier;
	uint32_t frontierPool, chunks;
#if TGM_STATS
	char firstProc[17];
#endif
};

PackImageStore gPack = {};
volatile uint32_t gShutdown = 0;

constexpr uint8_t kVolumesMounted = 1, kGpuUsable = 2;
constexpr uint32_t kPhaseNone = 0, kPhaseStreaming = 1, kPhaseComplete = 2, kPhaseFailed = 3;
#if TGM_STATS
uint64_t gGatedFirstNs = 0;
uint32_t gGatedJobs = 0;
#endif

IOLock *packLock()
{
	if (IOLock *lock = __atomic_load_n(&gPack.lock, __ATOMIC_SEQ_CST))
		return lock;
	IOLock *made = IOLockAlloc();
	if (!made)
		return nullptr;
	IOLock *expected = nullptr;
	if (!__atomic_compare_exchange_n(&gPack.lock, &expected, made, false, __ATOMIC_SEQ_CST,
					 __ATOMIC_SEQ_CST)) {
		IOLockFree(made);
		return expected;
	}
	return made;
}

void finishRefused(IOLock *lock, const char *why, int err, bool silent)
{
	if (!silent)
		IOLog("tegra-gm20b: pack image: refused (%s, err %d)\n", why, err);
	IOLockLock(lock);
	gPack.state = SwitchGpuPackState::Refused;
	gPack.buildActive = false;
#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
	if (!silent)
		++gPack.refusedCount;
#endif
	IOLockWakeup(lock, &gPack.state, false);
	IOLockWakeup(lock, &gPack.buildActive, false);
	IOLockUnlock(lock);
}

void buildThread(void *, wait_result_t)
{
	IOLock *lock = packLock();
#if TGM_STATS
	const uint64_t startedNs = switch_gpu_pack_now_ns();
#endif
	SwitchGpuPackStreamStats stats = {};
	if (switch_gpu_pack_shutdown_requested()) {
		finishRefused(lock, "shutdown", ECANCELED, true);
		thread_terminate(current_thread());
		return;
	}
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = nullptr;
	int err = vnode_open(TGM_PACK_PATH, FREAD | O_NOFOLLOW_ANY, 0, 0, &vp, ctx);
	if (err != 0 || !vp) {
		finishRefused(lock, "open failed", err, false);
		thread_terminate(current_thread());
		return;
	}
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	SwitchGpuPackHeader hdr = {};
	const char *why = nullptr;
	if (!vnode_isreg(vp)) {
		why = "not a regular file";
		err = EINVAL;
	} else if ((err = vnode_getattr(vp, &va, ctx)) != 0 ||
		   !VATTR_IS_SUPPORTED(&va, va_data_size)) {
		why = "getattr failed";
	} else if (va.va_data_size < sizeof(hdr)) {
		why = "file shorter than header";
		err = EINVAL;
	} else if ((err = switch_gpu_pack_read_exact(vp, &hdr, sizeof(hdr), 0, ctx)) != 0) {
		why = "header read failed";
	} else if (!switch_gpu_pack_validate_header(&hdr, va.va_data_size, &why)) {
		err = EINVAL;
	}
	if (why) {
		vnode_close(vp, FREAD, ctx);
		finishRefused(lock, why, err, false);
		thread_terminate(current_thread());
		return;
	}
	const uint64_t tier0Bytes = hdr.tier0[kSwitchGpuPackHeap];
	const uint64_t heapOff = hdr.section[kSwitchGpuPackHeap].offset;
	const uint32_t totalGpuPages = uint32_t(tier0Bytes >> 12);
	IOMemoryDescriptor *multi = nullptr;
	void **chunkCpu = nullptr;
	uint64_t *pas = nullptr;
	uint32_t chunkCount = 0;
	if (!switch_gpu_pack_alloc_tier0(tier0Bytes, &multi, &chunkCpu, &pas, &chunkCount)) {
		vnode_close(vp, FREAD, ctx);
		const bool shuttingDown = switch_gpu_pack_shutdown_requested();
		finishRefused(lock, shuttingDown ? "shutdown" : "tier-0 allocation failed",
			      shuttingDown ? ECANCELED : ENOMEM, shuttingDown);
		thread_terminate(current_thread());
		return;
	}
	tgm_mem *mem = nullptr;
	volatile uint32_t *frontier = nullptr;
	uint64_t frontierPage = 0;
	uint32_t frontierInPage = 0;
	err = tgm_mem_new(nullptr, pas, totalGpuPages, &mem);
	if (err == 0 && mem)
		err = tgm_fence_kernel(TGM_FENCE_KERNEL_PACK, &frontier, &frontierPage,
				       &frontierInPage);
	if (err != 0 || !mem) {
		const char *refused = mem ? "no frontier word" : "tgm_mem_new failed";
		vnode_close(vp, FREAD, ctx);
		IODelete(chunkCpu, void *, chunkCount);
		if (mem)
			tgm_mem_del(&mem);
		multi->complete(kIODirectionInOut);
		multi->release();
		IODelete(pas, uint64_t, totalGpuPages);
		finishRefused(lock, refused, err, false);
		thread_terminate(current_thread());
		return;
	}
	switch_gpu_pack_raise(frontier, 0);
	// ahead of use, before launchd: below launchd's and its daemons' demand page-ins, as the boot replay's reads are
	switch_vm_prefetch_replay_thread(current_thread(), 1);
	const bool tablesRead = switch_gpu_pack_prefetch_tables(vp, hdr, ctx);
	IOLockLock(lock);
	if (switch_gpu_pack_shutdown_requested()) {
		IOLockUnlock(lock);
		switch_vm_prefetch_replay_thread(current_thread(), 0);
		vnode_close(vp, FREAD, ctx);
		IODelete(chunkCpu, void *, chunkCount);
		tgm_mem_del(&mem);
		multi->complete(kIODirectionInOut);
		multi->release();
		IODelete(pas, uint64_t, totalGpuPages);
		finishRefused(lock, "shutdown", ECANCELED, true);
		thread_terminate(current_thread());
		return;
	}
	gPack.header = hdr;
	gPack.fileSize = va.va_data_size;
	gPack.pas = pas;
	gPack.npages = totalGpuPages;
	gPack.mem = mem;
	gPack.cpuMemory = multi;
	gPack.frontier = frontier;
	gPack.frontierPool = tgm_fence_offset(frontier);
	gPack.chunks = uint32_t((tier0Bytes + kSwitchGpuPackTransferBytes - 1) /
				kSwitchGpuPackTransferBytes);
	__atomic_store_n(&gPack.phase, kPhaseStreaming, __ATOMIC_RELEASE);
	gPack.state = SwitchGpuPackState::Ready;
#if TGM_STATS
	const uint64_t publishedNs = switch_gpu_pack_now_ns();
	char firstProc[sizeof(gPack.firstProc)] = {};
	memcpy(firstProc, gPack.firstProc, sizeof(firstProc));
#endif
	IOLockWakeup(lock, &gPack.state, false);
	IOLockUnlock(lock);

	const bool streamed = switch_gpu_pack_stream_tier0(vp, heapOff, tier0Bytes, pas, chunkCpu,
							   &why, &err, &stats, frontier);
	switch_vm_prefetch_replay_thread(current_thread(), 0);
	vnode_close(vp, FREAD, ctx);
	IODelete(chunkCpu, void *, chunkCount);
	const bool shuttingDown = switch_gpu_pack_shutdown_requested();
	IOLockLock(lock);
	__atomic_store_n(&gPack.phase, streamed ? kPhaseComplete : kPhaseFailed, __ATOMIC_RELEASE);
	if (!streamed) {
		gPack.state = SwitchGpuPackState::Refused;
#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
		if (!shuttingDown)
			++gPack.refusedCount;
#endif
	}
	IOLockWakeup(lock, &gPack.state, false);
	IOLockUnlock(lock);
	if (!streamed) {
		if (!shuttingDown)
			IOLog("tegra-gm20b: pack image: its stream stopped after %u of %u chunks "
			      "(%s, err %d): the work "
			      "waiting on it fails\n",
			      *frontier, gPack.chunks, why ? why : "stream failed", err);
		// While buildActive holds shutdown's wait, so no channel is recovered after the GPU's power-off began
		tgm_acquire_recover_failed("the pack image's stream stopped");
	}
	IOLockLock(lock);
	gPack.buildActive = false;
	IOLockWakeup(lock, &gPack.buildActive, false);
	IOLockUnlock(lock);
#if TGM_STATS
	const uint64_t endNs = switch_gpu_pack_now_ns();
	const uint64_t gatedNs = __atomic_load_n(&gGatedFirstNs, __ATOMIC_ACQUIRE);
	IOLog("tegra-gm20b: pack image: tier 0 %llu MiB for %s, open %llu ms after the build began "
	      "(tables %s), "
	      "streamed %llu ms after that (pagein requests %u, copy %llu ms, big-page bytes "
	      "%llu); %u jobs "
	      "waited on it on the GPU, the first submitted %llu ms before its end\n",
	      (unsigned long long)(tier0Bytes >> 20), firstProc,
	      (unsigned long long)((publishedNs - startedNs) / 1000000ULL),
	      tablesRead ? "read" : "unread",
	      (unsigned long long)((endNs - publishedNs) / 1000000ULL), stats.pageinReqs,
	      (unsigned long long)(stats.copyNs / 1000000ULL),
	      (unsigned long long)switch_gpu_mapping_big_bytes(pas, totalGpuPages,
							       TGM_PACK_WINDOW_VA, 0, tier0Bytes),
	      __atomic_load_n(&gGatedJobs, __ATOMIC_RELAXED),
	      (unsigned long long)(gatedNs && gatedNs < endNs ? (endNs - gatedNs) / 1000000ULL
							      : 0));
#else
	(void)tablesRead;
#endif
	thread_terminate(current_thread());
}

// Under the lock: a store no one has asked for becomes Building, its build named for `who`; whether the caller starts it
bool claimBuild(const char *who)
{
	if (gPack.state != SwitchGpuPackState::None)
		return false;
	gPack.state = SwitchGpuPackState::Building;
	gPack.buildActive = true;
#if TGM_STATS
	if (who)
		strlcpy(gPack.firstProc, who, sizeof(gPack.firstProc));
#else
	(void)who;
#endif
	return true;
}

// For a store the caller claimed: its build's thread, or the store refused
void startBuild(IOLock *lock)
{
	thread_t worker = nullptr;
	const kern_return_t kr = kernel_thread_start(buildThread, nullptr, &worker);
	if (kr != KERN_SUCCESS) {
		finishRefused(lock, "kernel_thread_start failed", int(kr), false);
		return;
	}
	thread_deallocate(worker);
}

// The build needs pack.bin reachable and the copy engine: the second of the two to happen starts it, before any client
// opens a device, so the render server's first acquire finds it under way or Ready instead of starting it
void bootCondition(uint8_t condition)
{
	IOLock *lock = packLock();
	if (!lock)
		return;
	IOLockLock(lock);
	gPack.bootConditions |= condition;
	const bool start = gPack.bootConditions == (kVolumesMounted | kGpuUsable) &&
			   !switch_gpu_pack_shutdown_requested() && claimBuild("boot");
	IOLockUnlock(lock);
	if (start)
		startBuild(lock);
}

}  // namespace

extern "C" void switch_gpu_pack_volumes_mounted(void)
{
	bootCondition(kVolumesMounted);
}

void switch_gpu_pack_gpu_usable(void)
{
	bootCondition(kGpuUsable);
}

extern "C" int switch_gpu_pack_job_wait(uint32_t *pool, uint32_t *payload)
{
	switch (__atomic_load_n(&gPack.phase, __ATOMIC_ACQUIRE)) {
	case kPhaseStreaming:
		*pool = gPack.frontierPool;
		*payload = gPack.chunks;
#if TGM_STATS
		{
			uint64_t none = 0;
			__atomic_compare_exchange_n(&gGatedFirstNs, &none, switch_gpu_pack_now_ns(),
						    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
			__atomic_fetch_add(&gGatedJobs, 1u, __ATOMIC_RELAXED);
		}
#endif
		return 1;
	case kPhaseFailed:
		return -1;
	case kPhaseNone:
	case kPhaseComplete:
	default:
		return 0;
	}
}

extern "C" int switch_gpu_pack_failed_word(uint32_t pool)
{
	return __atomic_load_n(&gPack.phase, __ATOMIC_ACQUIRE) == kPhaseFailed &&
	       pool == gPack.frontierPool;
}

bool switch_gpu_pack_shutdown_requested(void)
{
	return __atomic_load_n(&gShutdown, __ATOMIC_SEQ_CST) != 0u;
}

IOReturn switch_gpu_pack_image_acquire(const char *procName, SwitchGpuPackImageView *out,
				       uint64_t *waitedNs)
{
	if (!out)
		return kIOReturnBadArgument;
	*out = {};
	if (waitedNs)
		*waitedNs = 0;
	IOLock *lock = packLock();
	if (!lock)
		return kIOReturnNoMemory;
#if TGM_STATS
	uint64_t waitStart = 0;
#endif
	IOLockLock(lock);
	if (switch_gpu_pack_shutdown_requested()) {
		if (gPack.state == SwitchGpuPackState::None)
			gPack.state = SwitchGpuPackState::Refused;
		IOLockUnlock(lock);
		return kIOReturnSuccess;
	}
	if (claimBuild(procName)) {
		IOLockUnlock(lock);
		startBuild(lock);
		IOLockLock(lock);
	}
	if (gPack.state == SwitchGpuPackState::Building) {
#if TGM_STATS
		waitStart = switch_gpu_pack_now_ns();
#endif
		while (gPack.state == SwitchGpuPackState::Building) {
			if (IOLockSleep(lock, &gPack.state, THREAD_ABORTSAFE) ==
			    THREAD_INTERRUPTED) {
				IOLockUnlock(lock);
				return kIOReturnAborted;
			}
		}
#if TGM_STATS
		if (waitedNs && waitStart)
			*waitedNs = switch_gpu_pack_now_ns() - waitStart;
#endif
	}
	if (gPack.state == SwitchGpuPackState::Ready) {
		out->ready = true;
		out->mem = gPack.mem;
		out->cpuMemory = gPack.cpuMemory;
		out->tier0HeapBytes = gPack.header.tier0[kSwitchGpuPackHeap];
		out->heapBytes = gPack.header.section[kSwitchGpuPackHeap].bytes;
		out->stack = gPack.header.stack;
	}
	IOLockUnlock(lock);
	return kIOReturnSuccess;
}

namespace
{
uint64_t gPackCpuWiredBytes = 0;

bool reserveCpuWireBytes(uint64_t bytes)
{
	uint64_t cur = __atomic_load_n(&gPackCpuWiredBytes, __ATOMIC_RELAXED);
	for (;;) {
		if (bytes > kSwitchGpuPackMaxCpuWireBytes ||
		    cur > kSwitchGpuPackMaxCpuWireBytes - bytes)
			return false;
		if (__atomic_compare_exchange_n(&gPackCpuWiredBytes, &cur, cur + bytes, false,
						__ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
			return true;
	}
}

void releaseCpuWireBytes(uint64_t bytes)
{
	if (bytes)
		__atomic_fetch_sub(&gPackCpuWiredBytes, bytes, __ATOMIC_RELEASE);
}
}  // namespace

IOReturn switch_gpu_pack_client_wire_cpu(task_t task, const TegraGM20BAbiPackCpuRange *ranges,
					 uint32_t count, IOMemoryDescriptor **outDescriptors,
					 uint64_t *outBytes)
{
	uint64_t total = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (ranges[i].bytes > kSwitchGpuPackMaxCpuWireBytes - total)
			return kIOReturnNoResources;
		total += ranges[i].bytes;
	}
	if (!reserveCpuWireBytes(total))
		return kIOReturnNoResources;
	for (uint32_t i = 0; i < count; ++i) {
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
			ranges[i].address, ranges[i].bytes, kIODirectionOut, task);
		if (!md || md->prepare(kIODirectionOut) != kIOReturnSuccess) {
			if (md)
				md->release();
			for (uint32_t j = 0; j < i; ++j) {
				outDescriptors[j]->complete(kIODirectionOut);
				outDescriptors[j]->release();
				outDescriptors[j] = nullptr;
			}
			releaseCpuWireBytes(total);
			return kIOReturnBadArgument;
		}
		outDescriptors[i] = md;
	}
	if (outBytes)
		*outBytes = total;
	return kIOReturnSuccess;
}

void switch_gpu_pack_client_release_cpu(SwitchGpuPackClient *client)
{
	if (!client)
		return;
	for (uint32_t i = 0; i < client->cpuWiredCount; ++i) {
		if (client->cpuWired[i]) {
			client->cpuWired[i]->complete(kIODirectionOut);
			client->cpuWired[i]->release();
			client->cpuWired[i] = nullptr;
		}
	}
	client->cpuWiredCount = 0;
	releaseCpuWireBytes(client->cpuWiredBytes);
	client->cpuWiredBytes = 0;
}

void switch_gpu_pack_client_unmap_gpu(tgm_vm *vm, SwitchGpuPackClient *client)
{
	if (!client || !client->gpuMapping)
		return;
	switch_gpu_vm_unmap(vm, &client->gpuMapping);
	client->gpuBytes = 0;
}

extern "C" void switch_gpu_pack_shutdown_request(void)
{
	// Dekker seq_cst pair with packLock(): either shutdown observes the initialized lock and
	// drains active workers, or any late caller observes gShutdown before launching a worker.
	__atomic_store_n(&gShutdown, 1u, __ATOMIC_SEQ_CST);
	IOLock *lock = __atomic_load_n(&gPack.lock, __ATOMIC_SEQ_CST);
	if (!lock)
		return;
	IOLockLock(lock);
	if (gPack.state == SwitchGpuPackState::None)
		gPack.state = SwitchGpuPackState::Refused;
	IOLockWakeup(lock, &gPack.state, false);
	IOLockUnlock(lock);
}

// The build thread checks gShutdown before each 1 MiB read, each copy and each table read, so a stop waits on at most
// one of those. A thread still running this long into shutdown is on one that never completed, and release kernels have
// no other shutdown watchdog.
constexpr uint32_t kShutdownWaitSeconds = 20;

extern "C" void switch_gpu_pack_shutdown(void)
{
	switch_gpu_pack_shutdown_request();
	IOLock *lock = __atomic_load_n(&gPack.lock, __ATOMIC_SEQ_CST);
	if (!lock)
		return;
	uint64_t deadline = 0;
	clock_interval_to_deadline(kShutdownWaitSeconds, NSEC_PER_SEC, &deadline);
	IOLockLock(lock);
	while (gPack.buildActive) {
		if (IOLockSleepDeadline(lock, &gPack.buildActive, deadline, THREAD_UNINT) ==
			    THREAD_TIMED_OUT &&
		    gPack.buildActive)
			panic("tegra-gm20b: pack image: the build thread is still running %u s "
			      "into shutdown: a read or a "
			      "copy never completed",
			      kShutdownWaitSeconds);
	}
	IOLockUnlock(lock);
}

#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
void switch_gpu_pack_image_reset_for_test(void)
{
	switch_gpu_pack_shutdown();
	if (gPack.mem)
		tgm_mem_del(&gPack.mem);
	if (gPack.cpuMemory) {
		gPack.cpuMemory->complete(kIODirectionInOut);
		gPack.cpuMemory->release();
		gPack.cpuMemory = nullptr;
	}
	if (gPack.pas) {
		IODelete(gPack.pas, uint64_t, gPack.npages);
		gPack.pas = nullptr;
	}
	if (gPack.lock) {
		IOLockFree(gPack.lock);
		gPack.lock = nullptr;
	}
	gPack = {};
	__atomic_store_n(&gPackCpuWiredBytes, 0u, __ATOMIC_SEQ_CST);
	__atomic_store_n(&gShutdown, 0u, __ATOMIC_SEQ_CST);
}

uint32_t switch_gpu_pack_image_refused_count_for_test(void)
{
	return gPack.refusedCount;
}
#endif
