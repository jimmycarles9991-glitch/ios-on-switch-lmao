// SPDX-License-Identifier: MIT
// What TegraGM20BUserClient.cpp's units share: the includes, the class, and the helpers more than one unit uses,
// in tgm_uc. Hidden, so the units reach each other's variables directly, not through the GOT.
#pragma once
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <mach/vm_region.h>
#include "SwitchGpuSpares.h"
#include <stddef.h>
#include <string.h>
#include <sys/systm.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BJobABI.h"
#include "TegraGM20BBindHistory.h"
#include "TegraGM20BExecSurfaces.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BLayout.h"
#include "TegraGM20BLayoutFit.h"
#include "TegraGM20BPathsABI.h"
#include "TegraGM20BPoolPolicy.h"
#include "TegraGM20BViews.h"
#include "TegraGM20BPushScan.h"
#include "TegraGM20BReadback.h"
#include "TegraGM20BRegDumpABI.h"
#include "TegraGM20BCopyResetABI.h"
#include "TegraGM20BSchedPolicy.h"
#include "TegraGM20BSharedWiring.h"
#include "SwitchGpuPin.h"
#include "SwitchGpuBindings.h"
#include "SwitchGpuWriterPromise.h"
#include "SwitchGpuBoIndex.h"
#include "SwitchGpuLegacyUses.h"
#include "SwitchGpuSubmitGate.h"
#include "SwitchGpuNativeSurface.h"
#include "SwitchGpuNativeUse.h"
#include "SwitchGpuScanoutSnapshot.h"
#include "SwitchGpuSurfaceRefresh.h"
#include "SwitchGpuCpuSurface.h"
#include "SwitchGpuCpuComplete.h"
#include "SwitchGpuChannelPolicy.h"
#include "SwitchGpuJobFences.h"
#include "SwitchGpuJobVisibility.h"
#include "SwitchGpuPathStats.h"
#include "SwitchGpuTargetPeaks.h"
#include "SwitchGpuEventClient.h"
#include "SwitchGpuAsyncReference.h"
#include "SwitchGpuAdmission.h"
#include "SwitchGpuResourceClient.h"
#include "SwitchGpuResourceSet.h"
#include "SwitchGpuFenceRecord.h"
#include "SwitchGpuJobStats.h"

#include "SwitchGpuSurfaceGeometry.h"
#include "shim/include/switch_tiled_cache.h"
using tgm_scan::PushCounts;
using tgm_scan::PushParse;
using tgm_scan::scanPush;
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

#pragma GCC visibility push(hidden)

namespace tgm_uc
{
constexpr uint32_t kMaxChannels = 8;  // fence slots: 16 bytes each in the connection's fence page
constexpr uint32_t kJobBytes =
	(sizeof(TegraGM20BAbiJob) > sizeof(TegraGM20BAbiSubmit) ? sizeof(TegraGM20BAbiJob)
								: sizeof(TegraGM20BAbiSubmit)) +
	TegraGM20BMaxJobPushes * sizeof(TegraGM20BAbiPush) +
	TegraGM20BMaxJobUses * sizeof(TegraGM20BAbiSurfaceUse) + sizeof(TegraGM20BAbiSubmitEvents) +
	TegraGM20BMaxJobEvents *
		(sizeof(TegraGM20BAbiEventUse) + sizeof(TegraGM20BAbiSignalRelease)) +
	sizeof(TegraGM20BAbiSubmitAdmission) + sizeof(TegraGM20BAbiJobPromises);
constexpr uint64_t kMaxBoBytes = 256ULL << 20;
constexpr uint32_t kFenceToken = 0xfe000001u;
// GPU pool pages are outside the VM (arm_vm_init.c switch_ram_init), so the kernel reports them as device
// memory, and for a physical descriptor IOKit fixes the user mapping's memory type from that default when the descriptor
// is made: kIOMapWriteCombineCache at map time does not change it. Without this the client gets Device-nGnRE and its
// first unaligned memmove into a BO is an alignment fault (backboardd SIGBUS in Texture::replace_region).
constexpr IOOptionBits kPoolCacheMode = static_cast<IOOptionBits>(kIOWriteCombineCache)
					<< kIOMemoryBufferCacheShift;

extern "C" void proc_selfname(char *buf, int size);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" vm_offset_t ml_vtophys(vm_offset_t vaddr);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" task_t current_task(void);

// Freed pool BOs (see releaseBo): every one's view is released and its pages given back here, on a thread call, never on
// the freeing thread. A view a task still maps keeps its pages until the last such mapping is gone, so the task can never
// write pages the pool has handed out again (GPU page tables among them); IOKit keeps a task's mappings past its
// connection's close, until the task unmaps them or dies, so the list is global and any connection's BoAlloc and close
// kick the thread call to look at it again.
struct PoolOrphan {
	PoolOrphan *next;
	IOMemoryDescriptor *view;
	void *va;
	uint64_t pa, bytes;
	bool zero;  // a BO a client had: its pages are zeroed before they go back (poolZeroLater)
};

extern PoolOrphan *gPoolOrphans;
extern volatile uint64_t gSystemCoherent, gSystemCoherentBytes;
extern volatile uint64_t gClientPoolBytes, gOrphanBytes, gOrphans;
extern volatile uint64_t gCompressibleBytes, gCompressibles;
void pushOrphan(PoolOrphan *orphan);

bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes);
void scheduleOrphanRelease();
#if TGM_STATS
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" vm_map_t get_task_map(task_t task);
extern "C" kern_return_t vm_map_region_recurse_64(vm_map_t map, uint64_t *address, uint64_t *size,
						  natural_t *depth, vm_region_submap_info_64_t info,
						  mach_msg_type_number_t *count);

// SwitchGpuTargetPeaks.h's line of every process's largest target image since boot
void tgm_target_peaks_line(char *line, size_t size);
#endif
static inline uint64_t nowNs()
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static inline uint64_t statsNowNs()
{
#if TGM_STATS
	return nowNs();
#else
	return 0;
#endif
}

// Retired cached buffers keep their CE mapping through spare reuse. Only the final backing owner can recycle them;
// client mappings or imported aliases keep the descriptor retained and exclude it from this pool.
constexpr uint32_t kClasses = 53;  // 4 KiB .. 256 MiB (kMaxBoBytes)
#if TGM_STATS
constexpr uint32_t kOwners = 16;
#endif
uint64_t classBytes(uint32_t c);
uint32_t classUp(uint64_t bytes);

struct Spare {
	Spare *next;
	IOBufferMemoryDescriptor *bmd;  // prepared; ours alone (no view, no task mapping)
	uint64_t *pas;
	uint64_t bytes;
	switch_gpu_cached_mapping *mapping;
};

struct PoolZero {
	PoolZero *next;
	void *va;
	uint64_t pa, bytes;
};
#if TGM_STATS
struct OwnerStat {
	char name[17];
	uint64_t n, bytes, ns, max_ns,
		made;  // made: cached BOs made on the caller's thread (a miss)
};

#endif
struct SparePool {
	IOLock *lock;
	thread_call_t call;
	Spare *dirty;         // freed, not yet zeroed
	PoolZero *poolDirty;  // freed pool BOs, not yet zeroed
	Spare *ready
		[kClasses];  // zeroed and clean, by class (classUp(bytes) <= class of a spare's bytes)
	uint16_t count[kClasses], target[kClasses], zeroing[kClasses];
	uint64_t clean, dirtyBytes, cleaningBytes,
		poolZeroing;  // bytes ready, freed and waiting, pool bytes waiting
#if TGM_STATS
	OwnerStat owners[kOwners];
	uint32_t misses[SWITCH_GPU_SPARE_MISSES], zeroBusy, zeroFailed, refillRefused;
#endif
};

void cleanZeros(void *va, uint64_t bytes);
SparePool *spares();
Spare *spareTake(uint64_t bytes);
void spareIneligible();
bool spareKeep(IOBufferMemoryDescriptor *bmd, uint64_t *pas, uint64_t bytes,
	       switch_gpu_cached_mapping *mapping = nullptr);

// With the stats (also gpu_stats_tick.c's stats lines): the method stream of every kScanEvery-th submission of each
// connection is read back and what it asks of the GPU is counted, one line per process every 5 s ("tegra-gm20b: push
// <process>", the window's first kPushOwners - 1 processes by name, the rest as "(others)"), per submission: render passes
// (and of those, the ones with a depth target and the multisampled ones), bound color targets (RTs) and the pitch-linear
// ones, draws, clears; every 3D wait-for-idle (WFI), shader cache invalidate that waits for idle and flushes (INVALIDATE_SHADER_CACHES,
// NVK's FLUSH_SHADER_DATA barrier) and the ones that do not (_NO_WFI), texture data cache invalidate, host SET_REFERENCE
// (a host wait for idle: NVK's HOST_WFI_FLUSH_SYSMEM and MME-data barriers), MEM_OP_D L2 system-memory invalidate and
// host WFI; copy-engine copies with their bytes and the zero-length LAUNCH_DMAs NVK uses as a copy-engine WFI, 2D blits,
// compute dispatches and semaphore acquires; "bad" counts streams with an opcode NVK does not emit. Pushes in host
// imports (no kernel mapping) are not read; the line says what share of the push bytes was. The reader:
// TegraGM20BPushScan.h. Every kProfileEvery-th submission read is also profiled (gpu_fenced.h "push profile").
constexpr uint32_t kScanEvery = 4, kPushOwners = 4,
		   kProfileEvery = 16;  // 1 submission in 64 profiled (its pushes WFI-stamped)

static inline IOReturn ioreturn(int err)
{
	switch (err) {
	case 0:
		return kIOReturnSuccess;
	case -12:
		return kIOReturnNoMemory;  // ENOMEM
	case -22:
		return kIOReturnBadArgument;  // EINVAL
	case -16:
		return kIOReturnBusy;  // EBUSY
	case SWITCH_GPU_BACKING_DEPENDENCY_FAILED:
		return kIOReturnAborted;
	case -5:
		return kIOReturnNotResponding;  // EIO
	case -110:
		return kIOReturnTimeout;  // ETIMEDOUT
	case SWITCH_GPU_BACKING_STALE:
	case -19:
		return kIOReturnNotReady;  // ENODEV
	case -28:
		return kIOReturnNoResources;  // ENOSPC
	default:
		return kIOReturnError;
	}
}

struct KernelMemory {
	template <typename T> static T *allocate(uint32_t count)
	{
		return IONew(T, count);
	}

	template <typename T> static void release(T *items, uint32_t count)
	{
		if (items)
			IODelete(items, T, count);
	}
};

IOReturn collectPages(IOMemoryDescriptor *md, uint64_t bytes, uint64_t *pas);
}  // namespace tgm_uc

#pragma GCC visibility pop

using namespace tgm_uc;

struct SwitchGpuJobRecord;

extern "C" int switch_native_surface_gpu_acquire(void *, switch_gpu_native_surface_view *)
	__attribute__((weak));
extern "C" void switch_native_surface_gpu_release(switch_gpu_native_surface_view *)
	__attribute__((weak));

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AGXDeviceUserClient : public IOUserClient
{
	// NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
	OSDeclareDefaultStructors(AGXDeviceUserClient)

		struct Bo {
		Bo *next;
		uint64_t handle, bytes, controlCharge;
		uint32_t token, flags, npages;
		uint32_t
			backing_pages;  // pas' length: npages, or a spare's pages when the BO is a view of its first ones
		bool readOnly;  // imports of pages the task maps read-only: the GPU maps them read-only
		bool unlockedReads;  // an IOSurface over its creator's memory, which the CPU reads without IOSurfaceLock
		uint64_t *pas;  // one per 4 KiB page; tgm_mem keeps a pointer to it
		void *pool_va;  // Coherent BOs: pool memory (write-combined, contiguous)
		uint64_t pool_pa;
		bool chunked;  // Compressible BOs: pas are pool chunks (tgm_pool_alloc_chunks); no view, never CPU-mapped
		switch_gpu_cached_mapping *cachedMapping;
		IOBufferMemoryDescriptor *bmd;  // cached BOs
		IOMemoryDescriptor *wired;      // imports
		OSObject
			*surface;  // Keeps its global ID alive through the import's GPU retirement.
		const volatile uint64_t *representation;
		uint64_t representationValue;
		IOMemoryDescriptor *view;  // cached clientMemoryForType descriptor
		bool system;  // a coherent BO from ordinary pages (tgm_pool::FromSystem): a write-combined bmd
		bool compositorPool;  // a pool BO counted as the compositor's (critical_ when it was made)
		tgm_mem *mem;
		bool backingMemory;
		tgm_coh *coh;  // imports: the surface's coherency entry, held (TegraGM20BCoherency)
		switch_gpu_backing *backing;
		switch_gpu_native_surface_view *nativeView;
		SharedWiring *
			wiring;  // a surface import's pages, mem and wiring, shared with the task's other imports
		uint32_t bindings;
		Bo **zombieLink;
	};

	struct RetiredBinding {
		RetiredBinding *next;
		uint64_t va, range;
		Bo *bo;
		switch_gpu_mapping *mapping;
		switch_gpu_backing_token snapshots[kMaxChannels];
		uint8_t kind, vol;
		uint32_t
			epoch;  // a pending binding's: the bind epoch released once its PTEs are in
		bool unmapped;  // its install failed: logged once, tried again at each retirement
	};

	struct Binding {
		uint64_t va, range;
		Bo *bo;
		uint64_t bo_offset;
		RetiredBinding *retirement;
	};

	// A mapping method 12 (Map) made into the client's task: what IOKit's io_connect_map_memory_into_task keeps in the
	// client's mapping set, kept here (that set is IOKit's own). Unmap and close release it, and its release unmaps it.
	struct Mapping {
		Mapping *next;
		IOMemoryMap *map;
		uint32_t token;
		uint64_t address;
	};

	struct CpuLease {
		CpuLease *next;
		switch_gpu_backing *backing;
		TegraGM20BAbiCpuAccess access;
		SwitchGpuCpuSurface reservation;
	};

	// Debug kernels: a SubmitAddress's time by phase, for the "exec by process" line (tgm_stats_submit).
	struct SubmitPhases {
#if TGM_STATS
		uint64_t at = 0, ns[TGM_SUBMIT_PHASES] = {};

		void start(uint64_t now)
		{
			at = now;
			for (auto &phase : ns)
				phase = 0;
		}

		void end(unsigned phase)
		{
			const uint64_t now = statsNowNs();
			ns[phase] += now - at;
			at = now;
		}
#else
		void start(uint64_t)
		{
		}

		void end(unsigned)
		{
		}
#endif
	};

	struct JobScratch {
		uint64_t *bytes;
		switch_gpu_backing_token *dependencies;
		switch_gpu_backing_use *uses;
		switch_gpu_backing_write_range *writes;
		switch_gpu_backing_guard *guards;
		SwitchGpuSurfaceRefresh *surfaces;
		switch_gpu_cpu_lease **leases;
		tgm_coh **
			written;  // the listed uses' imports a job writes, as its uses resolved them
		uint32_t capacity;
		SwitchGpuEventJob *events;
		SubmitPhases phases;
	};

	SwitchGpuEventClient events_;
	SwitchGpuResourceClient resources_;
	SwitchGpuAdmission admissions_[kMaxChannels];
	uint64_t admissionCookies_[kMaxChannels] = {}, lastAdmissionCookies_[kMaxChannels] = {};
	SwitchGpuEventChannel eventChannels_[kMaxChannels];

	struct EventNotification {
		AGXDeviceUserClient *owner;
		OSAsyncReference64 reference;
	};

	static void eventNotified(void *context, uint64_t cookie, int result);
	IOReturn eventObserve(const TegraGM20BAbiEventObserve *input,
			      IOExternalMethodArguments *arguments,
			      SwitchGpuAsyncReference &reference);
	IOReturn resourceObserve(const TegraGM20BAbiResourceObserve *, IOExternalMethodArguments *,
				 SwitchGpuAsyncReference &);
	IOReturn externalCall(uint32_t selector, IOExternalMethodArguments *arguments,
			      SwitchGpuAsyncReference &asyncReference);
	JobScratch job_ = {};
	JobScratch channelJobs_[kMaxChannels] = {};
	IOLock *submitLocks_[kMaxChannels] = {};
	SwitchGpuSubmitGate submitGate_;
	CpuLease *cpuLeases_ = nullptr;

	IOLock *lock_ = nullptr;
	task_t task_ = nullptr;
	tgm_vm *vm_ = nullptr;
	volatile uint32_t *fence_va_ =
		nullptr;  // tgm_fence_page_get: the GPU present maps every connection's
	uint64_t fence_pa_ = 0, fence_gpu_ = 0;
	tgm_mem *fence_mem_ = nullptr;
	IOMemoryDescriptor *fence_view_ = nullptr;
	tgm_chan *chans_[kMaxChannels] = {};
	uint32_t channelLevels_[kMaxChannels] = {};
	bool leanChannels_[kMaxChannels] = {};
	Bo *bos_ = nullptr, *zombies_ = nullptr, *readyBos_ = nullptr;
	SwitchGpuBoIndex<Bo, KernelMemory> boIndex_;
	RetiredBinding *retired_ = nullptr, **retiredTail_ = &retired_, *unmapping_ = nullptr;
	// a retired binding's PTEs go under it (retireBatch, takeRetiredOverlaps)
	IOLock *unmapLock_ = nullptr;
	bool faultLogged_[kMaxChannels] = {};
	// the bind epoch (issueBind): the last issued and the last up to which every pending binding's PTEs are in place,
	// its word in the fence page
	static constexpr uint32_t kBindEpochWord = 256;
	uint32_t bindIssued_ = 0, bindDone_ = 0;
#if TGM_STATS
	// NVK's own heap is the client window's lower half (its replay heap and the raw path's quarter lie above): its maps
	// and unmaps are kept apart from the raw path's
	tgm_bind_history *bindHistory_[2] = {};
#endif
	void noteBind(uint8_t op, uint64_t va, uint64_t range, const Bo *bo, uint64_t boOffset,
		      uint8_t kind);
	void describeFault(uint32_t slot, uint64_t va);
	thread_call_t retireCall_ = nullptr;
	// quiet job ends (SwitchGpuQuiet.h): the channel slots a waiter asked a wake entry for, pushed by wakeCall_
	thread_call_t wakeCall_ = nullptr;
	uint32_t wakeSlots_ = 0;
	// writer promises (SwitchGpuWriterPromise.h, method 34), under lock_: each one's state and the surfaces that keep a
	// ref to it until its job is submitted; promisesKept_ (atomic): kept ones a kill or destruction of their channel
	// may leave unreleased
	switch_gpu_promise_slot promises_[TegraGM20BMaxPromises] = {};
	switch_gpu_backing *promiseSurfaces_[TegraGM20BMaxPromises][TegraGM20BMaxPromiseUses] = {};
	uint8_t promiseSurfaceCount_[TegraGM20BMaxPromises] = {};
	uint32_t promisesKept_ = 0;
	uint32_t retiredCount_ = 0, retireRemaining_ = 0, retirementPending_ = 0,
		 retirementStopped_ = 0;
	uint64_t retirementEpoch_ = 0, retireScanEpoch_ = 0;
	tgm_fences::ExecSurfaces
		surfaces_;  // the imports of bos_ with a coherency entry, and method 17's lists
	Mapping *maps_ = nullptr;  // under lock_
	Binding *binds_ = nullptr;
	uint32_t nbinds_ = 0, cap_binds_ = 0;
	uint64_t next_handle_ = 1;
	uint32_t next_token_ = 1;
	bool counted_ = false;
	char owner_[17] = {};  // the opening process
	uint32_t pushSubmits_ =
		0;  // the stats: its submissions, every kScanEvery-th read (scanPushes)
	uint32_t pushScans_ =
		0;  // the ones read, every kProfileEvery-th also profiled (gpu_fenced.h "push profile")
	// the compositor's: its queues asked for the top GPU priority (method 18); its channels are high, it has no pool quota
	bool critical_ = false;
	// what the connection's queues asked for (method 18), whether runningboardd denies it the GPU (then the background
	// priority counts) and the runlist level they give its channels
	uint32_t priority_ = 2, backgroundPriority_ = 2;
	bool backgroundRole_ = false;
	uint32_t level_ = TGM_LEVEL_FOREGROUND;
	bool importsWritten_ =
		false;  // TegraGM20BPushImportsWritten: every submission marks every writable import
	bool quotaLogged_ = false;
	bool compressibleLogged_ = false;  // a compressible BoAlloc refused, logged once
	bool systemLogged_ =
		false;  // a coherent BoAlloc served from system memory (tgm_pool::FromSystem), logged once
	uint64_t pool_bytes_ = 0;  // pool BOs held
	uint64_t control_bytes_ = 0;
	// the thread about to take a hang's line while it holds lock_ (tgm_chan_hung, tgm_chan_del), so hungInterlocks
	// reads the binds without taking it again
	thread_t hangHolder_ = nullptr;
	switch_gpu_resource_set *sets_[TegraGM20BMaxResourceSets] = {};

	Bo *findBo(uint64_t handle);
	Bo *findAddress(uint64_t address, uint64_t bytes = 1);
	Bo *findToken(uint32_t token);
	void releaseBo(Bo *bo);
	void reap(bool force);
	void queueZombie(Bo *bo);
	void readyBo(Bo *bo);
	void accountRelease(Bo *bo);
	void drainResources();
	void retireBatch();
	void scheduleRetirement();
	bool bindingIdle(const RetiredBinding *binding) const;
	void takeRetiredOverlaps(uint64_t va, uint64_t bytes);
	void retire(RetiredBinding *record);
	IOReturn cutOverlaps(uint64_t va, uint64_t range, bool now, bool replace,
			     RetiredBinding **spare);
	bool workQueued(uint64_t va, uint64_t range) const;
	IOReturn mapRange(const TegraGM20BAbiBindOp &op, Bo *bo, RetiredBinding *record,
			  bool queued);
	uint32_t issueBind();
	void installPendingBinds();
	void logFaultBindings();
	static void retirementWork(thread_call_param_t, thread_call_param_t);
	static void retirementFence(void *owner);
	static void wakeAsked(void *owner, uint32_t slot);
	static void wakeWork(thread_call_param_t owner, thread_call_param_t);
	void pushWakes();
	void unbindOverlaps(uint64_t va, uint64_t range, Bo *only);
	void scanPushes(const tgm_push *pushes, uint32_t count, tgm_prof *prof, bool rawMacros);
	const tgm_prof *scanSubmission(const tgm_push *pushes, uint32_t count, tgm_prof &prof,
				       bool rawMacros);
	const uint32_t *pushView(uint64_t va, uint64_t bytes, const Binding *&hit) const;
	IOReturn run(uint32_t method, const void *in, uint32_t inSize, void *out,
		     const uint32_t *outSize);

	static IOReturn getInfo(TegraGM20BAbiInfo *out);
	IOReturn boAlloc(const TegraGM20BAbiBoAlloc *in, TegraGM20BAbiBoAllocReply *out);
	IOReturn boImport(const TegraGM20BAbiBoImport *in, TegraGM20BAbiBoAllocReply *out);
	static void adoptWiring(Bo *bo, SharedWiring *w);
	void publishWiring(Bo *bo, const TegraGM20BAbiBoImport *in);
	IOReturn boFree(const TegraGM20BAbiBoFree *in);
	IOReturn cpuComplete(const TegraGM20BAbiCpuComplete *, TegraGM20BAbiCpuCompleteReply *);
	IOReturn cpuAccess(const TegraGM20BAbiCpuAccess *in, bool release);
	void releaseCpuLeases();
	static bool reserveJob(JobScratch &job, uint32_t uses);
	static void freeJob(JobScratch &job);
	IOReturn submitAddress(const TegraGM20BAbiSubmitAddress *in, TegraGM20BAbiExecReply *out);
	IOReturn vmBind(const TegraGM20BAbiVmBind *in, uint32_t count);
	IOReturn channelCreate(const TegraGM20BAbiChannelCreate *in,
			       TegraGM20BAbiChannelCreateReply *out);
	IOReturn channelDestroy(const TegraGM20BAbiChannel *in);
	IOReturn exec(uint32_t channel, TegraGM20BAbiPush *input, uint32_t count,
		      TegraGM20BAbiExecReply *out, const TegraGM20BAbiSurfaceUse *uses = nullptr,
		      uint32_t useCount = 0, bool native = false, JobScratch *scratch = nullptr,
		      uint32_t visibility = 0, SwitchGpuEventJob *events = nullptr,
		      uint64_t admissionCookie = 0);
	IOReturn waitFence(const TegraGM20BAbiWaitFence *in);
	IOReturn getStatus(const TegraGM20BAbiChannel *in, TegraGM20BAbiStatus *out);
	IOReturn mapToken(const TegraGM20BAbiMap *in, TegraGM20BAbiMapReply *out);
	IOReturn order(const TegraGM20BAbiOrder *in, TegraGM20BAbiOrderReply *out);
	IOReturn nativeSurfaceView(const TegraGM20BAbiNativeSurfaceView *in,
				   TegraGM20BAbiNativeSurfaceViewReply *out);
	IOReturn surfaceLayout(const TegraGM20BAbiSurfaceLayout *in,
			       TegraGM20BAbiSurfaceLayoutReply *out);
	IOReturn surfaceLayoutInfo(const TegraGM20BAbiSurfaceLayout *in,
				   TegraGM20BAbiSurfaceLayoutInfo *out, bool canonical);
	void applyLevel(bool allChannels = true);
	IOReturn execSurfaces(const TegraGM20BAbiExecSurfaces *in);
	IOReturn resourceSetCreate(const TegraGM20BAbiResourceSetCreate *in,
				   TegraGM20BAbiResourceSetCreateReply *out);
	IOReturn attachSet(const TegraGM20BAbiResourceSetCreate *in, TegraGM20BAbiSurfaceUse *uses,
			   switch_gpu_set_member *members, TegraGM20BAbiResourceSetCreateReply *out,
			   uint32_t &refusedAt);
	IOReturn resourceSetRelease(const TegraGM20BAbiResourceSetRelease *in);
	IOReturn submitJob(const TegraGM20BAbiSubmitAddress *in, TegraGM20BAbiExecReply *out);
	IOReturn writerPromise(const TegraGM20BAbiWriterPromise *in,
			       TegraGM20BAbiWriterPromiseReply *out);

	volatile uint32_t *promiseWord(uint32_t promise) const
	{
		return fence_va_ + TegraGM20BPromiseWord + promise;
	}

	void dropPromiseSurfaces(uint32_t promise);
	void releasePromise(uint32_t promise, bool abandoned);
	void releasePromises(bool all);
	IOReturn keepPromises(const TegraGM20BAbiJobPromises *promises,
			      switch_gpu_release *releases);
	void keptPromises(const TegraGM20BAbiJobPromises *promises, uint32_t slot,
			  const switch_gpu_backing_token &job);
	IOReturn runJob(const SwitchGpuJobRecord &record, JobScratch &job,
			TegraGM20BAbiExecReply *out);
	IOReturn runJobUses(const SwitchGpuJobRecord &record, JobScratch &job,
			    TegraGM20BAbiExecReply *out, switch_gpu_resource_set *set,
			    const switch_gpu_sibling_wait *siblings, uint32_t siblingCount);
	void destroySets();
	IOReturn unmapToken(const TegraGM20BAbiUnmap *in);
	void releaseMappings();

public:
	bool setUp(task_t task);
	int hungInterlocks(const uint64_t *va, const uint32_t *dwords, uint32_t n,
			   uint64_t *interlocks, uint32_t *lastEngine);

	void countConnection()
	{
		counted_ = true;
	}

	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
				     IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
				IOExternalMethodDispatch *dispatch, OSObject *target,
				void *reference) APPLE_KEXT_OVERRIDE;
};

inline AGXDeviceUserClient::Bo *AGXDeviceUserClient::findBo(uint64_t handle)
{
	return boIndex_.find(handle);
}
