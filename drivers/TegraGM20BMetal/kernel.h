// SPDX-License-Identifier: MIT
// The TegraGM20B user client (drivers/TegraGM20B/TegraGM20BABI.h) as the driver calls it: the service, open type, call
// forms (direct, or tunnelled through the one selector a sandbox admits) and records of Mesa NVK's nvkmd_darwin. The
// connection's GPU VM is the process's own: VAs come from the whole client range GetInfo names, through a first-fit free
// list (VA is given back when a BO is freed). VM allocation state is serialized independently of channel submission.
#pragma once
#include "lock.h"
#include <atomic>
#include <mutex>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "TegraGM20BABI.h"
#include "TegraGM20BLayout.h"
#include "surfaces.h"
#include "shared_events.h"
#include "cpu_completion.h"
#include "control_memory.h"

struct TegraGM20BAbiZcullInfo;
struct TegraGM20BAbiClockAnchors;

namespace gm20b {

struct Bo {
    uint64_t handle = 0;  // kernel BO
    uint64_t va = 0;      // where it is bound in the connection's GPU VM (an import: the first byte asked for)
    uint64_t bytes = 0;   // what is bound (an import: whole pages)
    uint8_t *cpu =
        nullptr;  // CPU address of va (allocations: IOConnectMapMemory64 of map_token; imports: the caller's pages)
    uint32_t token = 0;   // allocations: the map token (unmapped at free)
    uint64_t vaBase = 0;  // the bound range's start (an import's va is inside its first page)
    uint64_t mapped = 0;  // CPU mapping bytes (allocations)
    bool gpuVolatile = false;
    bool vaReserved =
        false;  // bound inside a reservation (bindImportAt): the VA is the reservation's, not freed with it
    bool allocated = false;  // a BoAlloc's (Kernel::allocatedBytes counts it), not an import
    bool control = false;    // from the control reserve (Kernel::controlMappedBytes counts its mapping)
    bool parked = false;
};

// The offset into `bo` of the CPU bytes [cpu, cpu + bytes): bo.cpu is the CPU address of bo.va, which lies bo.va - bo.vaBase
// into the BO. False when they are not all inside it.
inline bool boCpuOffset(const Bo &bo, const void *cpu, uint64_t bytes, uint64_t &offset)
{
    if (!bo.cpu || !cpu || bo.va < bo.vaBase || bo.va - bo.vaBase > uintptr_t(bo.cpu))
        return false;
    const uintptr_t base = uintptr_t(bo.cpu) - (bo.va - bo.vaBase), address = uintptr_t(cpu);
    if (address < base || address - base > bo.bytes || bytes > bo.bytes - (address - base))
        return false;
    offset = address - base;
    return true;
}

// One SubmitJob (drivers/TegraGM20B/TegraGM20BJobABI.h, method 33): its pushes, the uses no resource set holds, and the
// set by id and generation (set 0: none)
struct KernelJob {
    uint32_t channel = 0;
    const TegraGM20BAbiPush *pushes = nullptr;
    uint32_t pushCount = 0;
    const TegraGM20BAbiSurfaceUse *uses = nullptr;
    uint32_t useCount = 0;
    uint32_t set = 0, generation = 0;
    JobVisibility visibility = JobVisibility::External;
    JobEvents events;
    bool hostWritten = false;  // the CPU wrote a member the job reads through the GPU L2 since the last job
    // A command buffer of several jobs: its cookie (Kernel::nextAdmission) and TegraGM20BJobAdmission* flags
    uint64_t admission = 0;
    uint32_t admissionFlags = 0;
    bool quiet = false;  // its end raises no interrupt unless a wait asks for one (TegraGM20BJobQuiet)
    // writer promises (method 34) the job keeps: its end releases them
    const uint32_t *promises = nullptr;
    uint32_t promiseCount = 0;
    bool sameUses = false;
};

class Kernel;

// What names the connection's BOs by handle beyond their owners (the device's resource set, resource_set.h). Every
// BoFree goes through Kernel::free, which tells it before the handle dies (`leaving`: true when it named it) and after
// (`left`), so no list it sends the kernel names a BO the kernel no longer holds.
class FreeObserver {
public:
    virtual bool leaving(uint64_t handle) = 0;
    virtual void left(Kernel &k) = 0;

protected:
    ~FreeObserver() = default;
};

class Kernel {
public:
    ~Kernel();
    bool open(std::string &err);

    // The connection and the form its calls take (kernel_transport.h)
    uint32_t connection() const
    {
        return conn_;
    }

    bool tunnelled() const
    {
        return tunnel_;
    }

    uint32_t tunnelSelector() const
    {
        return selector_;
    }

    const TegraGM20BAbiInfo &info() const
    {
        return info_;
    }

    // `control`: from the kernel's control reserve (TegraGM20BBoControl, coherent kind-0 memory only). `cpu`: mapped for the
    // CPU at bo.cpu; an IOKit mapping counts whole in the task's phys_footprint (iokit_mapped), touched or not. `at`: bound
    // at that VA of a reservation (reserveVa), which freeing the BO leaves to the reservation; 0: a VA of its own.
    bool alloc(uint64_t bytes, bool coherent, Bo &bo, uint32_t kind = 0, uint64_t align = 0x10000,
               bool gpuVolatile = false, bool control = false, bool cpu = true, uint64_t at = 0,
               bool systemOnly = false);

    // Memory only the GPU reads and writes (shader local memory): no CPU mapping
    bool allocGpu(uint64_t bytes, Bo &bo)
    {
        return alloc(bytes, false, bo, 0, 0x10000, false, false, false);
    }

    bool allocControl(uint64_t bytes, Bo &bo, ControlUse use)
    {
        return alloc(bytes, true, bo, 0, 0x10000, controlMapping(use) == ControlMapping::Volatile, true);
    }

    // Wires [addr, addr + bytes) of this task (whole pages) and binds them; bo.va/bo.cpu name addr itself.
    bool import(void *addr, uint64_t bytes, Bo &bo, uint32_t kind = 0, uint32_t surfaceId = 0);
    // The same in two steps, for a caller that serialises the VA list itself: the kernel's BoImport (the wiring: the slow part,
    // safe beside the other calls) and then the VA and its bind (bindImport, under the caller's lock).
    // `gpuReads`: the GPU never writes it (TegraGM20BImportGpuReads), so no submission marks it GPU-dirty
    bool importPages(void *addr, uint64_t bytes, Bo &bo, uint32_t surfaceId = 0, bool gpuReads = false);
    // The GPU may write the IOSurface from now on (BoImport took it as only sampled): before its first render or store.
    bool surfaceGpuWrites(uint32_t surfaceId);
    // Priority (method 18): every channel of the connection runs at the most urgent of the process's queues' Metal
    // priorities (TegraGM20BABI.h); `*critical` reports whether the kernel treats this connection as UI-critical
    // (TegraGM20BSchedABI.h). False without the method.
    bool priority(uint32_t priority, uint32_t background, bool *critical = nullptr);
    // SurfaceLayout (method 16) for the import `bo` of a surface: its layout, decided by the kernel once for every process
    // at the first asking import (TegraGM20BLayout.h: block-linear when that importer writes it alone). `writes`: this
    // texture renders into or stores to it. TegraGM20BLayoutTiled, TegraGM20BLayoutOtherShape, or 0 (pitch; also without
    // the method).
    uint32_t surfaceLayout(const Bo &bo, uint32_t width, uint32_t height, uint32_t bpp, uint32_t bytesPerRow,
                           bool writes, tgm_layout *canonical = nullptr, uint32_t *kind = nullptr);
    int nativeSurfaceView(const Bo &source, Bo &view, TegraGM20BAbiNativeSurfaceViewReply &layout,
                          bool writable = false);
    bool bindImport(void *addr, Bo &bo, uint32_t kind);
    // BoAlloc Compressible (comp.h), never CPU-mapped; safe beside the other calls (it may zero pages a free left
    // dirty); the kern_return_t.
    int allocCompressible(uint64_t bytes, Bo &bo);
    // Binds it whole at a VA aligned as its memory with compressible `kind`: the VmBind's kern_return_t (kIOReturnNoResources:
    // the kernel had no comptags left for it), or kIOReturnNoSpace with no VA left; refused, the BO is left unbound. Safe
    // without Device::lock_ (the VA allocator holds vaLock_).
    int bindCompressed(Bo &bo, uint32_t kind);
    // Method 19: adds a zero-bandwidth-clear entry to the GPU's table (zbc.h); the kern_return_t (kIOReturnNoResources:
    // the table is full)
    int zbcAdd(const TegraGM20BAbiZbc &entry);
    // Method 'ZC': zcull's layout (zcull.h); kIOReturnUnsupported without zcull
    int zcullInfo(TegraGM20BAbiZcullInfo &out);
    // GPU VA held for binds made into it later (bindImportAt), whole granules; 0: none left. releaseVa gives it back.
    uint64_t reserveVa(uint64_t bytes);
    void releaseVa(uint64_t va, uint64_t bytes);
    // bindImport at `va`, a granule boundary inside a reservation: freeing the BO leaves the VA to the reservation
    bool bindImportAt(void *addr, Bo &bo, uint64_t va, uint32_t kind);
    bool bindAlias(const Bo &bo, uint64_t va, uint32_t kind);

    // MapSparse (TegraGM20BInfoSparse, read from the info word GetInfo filled at open): a range that reads 0 and drops writes
    // until an Unmap makes it fault again or a bind over it replaces it
    bool sparse() const
    {
        return (info_.flags & TegraGM20BInfoSparse) != 0;
    }

    bool supportsPackImage() const
    {
        return (info_.flags & TegraGM20BInfoPackImage) != 0;
    }

    bool supportsBoPark() const
    {
        return (info_.flags & TegraGM20BInfoBoPark) != 0;
    }

    bool supportsSameUses() const
    {
        return sameUses_;
    }

    int park(uint64_t handle, bool guardNone);
    int unpark(uint64_t handle, uint64_t &address, uint64_t &bytes, uint32_t &status);
    // Memory pressure or the app gone to the background: the kernel's zeroed spares go back if the VM is short (method 43;
    // a kernel without it ignores the call)
    void sparesGiveBack();
    bool mapAt(uint32_t token, uint64_t bytes, uint64_t fixedVa, bool readOnly = false);
    void unmap(uint32_t token, uint64_t address);

    int packImageMap(const TegraGM20BAbiPackCpuRange *cpuRanges, uint32_t cpuRangeCount,
                     TegraGM20BAbiPackImageMapReply &reply);

    // One VmBind: `bo` whole at `va`, a reservation's granule, and [va + bo.bytes, va + bytes) sparse (whole pages). BoFree
    // unbinds only the BO's own binding: unbind [va, va + bytes) before releasing the reservation
    bool bindWindow(const Bo &bo, uint64_t va, uint32_t kind, uint64_t bytes);
    // One VmBind: [va, va + bytes) of a reservation sparse (whole pages)
    bool bindSparse(uint64_t va, uint64_t bytes);
    void unbind(uint64_t va, uint64_t range);
    // BoFree (the kernel unbinds it at once and releases the pages after the channels complete what they had); the VA goes
    // back to the free list at once: call it only once no submission that uses the BO is still running (gm20b::Device does).
    // The observer's list loses the handle first.
    void free(Bo &bo);

    // Set once, before the first free
    void observeFrees(FreeObserver &observer)
    {
        freeObserver_ = &observer;
    }

    bool channel(uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence);
    bool channelProgress(uint32_t channel, uint64_t &gpuVa);
    void closeChannel(uint32_t channel);
    // The GPU's channel epoch (TegraGM20BChannelEpochWord) in the connection's fence page, mapped at the first call; null:
    // the page could not be mapped
    const volatile uint32_t *channelEpoch();

    // A SubmitJob or WaitFence found `channel` dead (the kernel killed it for a GPU fault or hang; its fence was released, and
    // the connection's other channels, BOs and binds go on)
    bool lost(uint32_t channel) const
    {
        return lostChannel_.load(std::memory_order_relaxed) == channel;
    }

    // Why the last channel found dead was killed (TegraGM20BAbiStatus: 1 MMU fault at lostFaultVa(), 2 GR exception, 3 host
    // error, 4 hang); 0 when the kernel did not say
    uint32_t lostError() const
    {
        return lostError_.load(std::memory_order_relaxed);
    }

    uint64_t lostFaultVa() const
    {
        return lostFaultVa_.load(std::memory_order_relaxed);
    }

    // Channels made so far: with a channel's id, the count when it was the newest names that channel alone (the kernel reuses
    // ids, and a destroyed channel's waiters may answer after its id is another's)
    uint32_t opened() const
    {
        return opened_.load(std::memory_order_acquire);
    }

    // `channel`, as it was when `opened` channels had been made, is dead (a SubmitJob or WaitFence answered so, or its kill record
    // says so): lost() and the cause from GetStatus. Nothing when a channel was made since: that one is not it.
    void markLost(uint32_t channel, uint32_t opened);
    // GetStatus: false when the kernel has no such channel (destroyed) or refused the call
    bool status(uint32_t channel, TegraGM20BAbiStatus &st);
    // ClockAnchors ('CA', TegraGM20BClockABI.h): the GPU clock's anchors in the connection's fence page, read where the
    // CPU maps it and at `gpuVa` in the connection's GPU address space; false without the method
    bool clockAnchors(const volatile TegraGM20BAbiClockAnchors *&table, uint64_t &gpuVa);

    // Method 33. kIOReturnNotReady (in `result`) is the set's ended generation or a stale native view: no error is kept.
    bool submitJob(const KernelJob &job, uint32_t &seqno, int *result = nullptr);
    // Method 31: `set` (0: a new one) made with `members`, but each the kernel refused, which comes back with access 0; its
    // id and generation; `mirror`, the generation's word in the fence page. The kern_return_t; with
    // kIOReturnBadArgument the member the kernel refused the whole call for has its reason in `reserved`
    // (TegraGM20BJobABI.h).
    int resourceSet(std::vector<TegraGM20BAbiSurfaceUse> &members, uint32_t &set, uint32_t &generation,
                    const volatile uint32_t *&mirror);
    // Method 32
    int releaseSet(uint32_t set);
    // Method 34: a writer promise of `channel`'s job for the IOSurface imports `handles` (Make), or its release (Cancel;
    // withdrawn: its command buffer failed, Withdraw). withdrawWrites: Withdraw of writes of `handles` no promise names.
    // The kern_return_t.
    int promise(uint32_t channel, const std::vector<uint64_t> &handles, uint32_t &promise);
    int cancelPromise(uint32_t promise, bool withdrawn = false);
    // Arm: the promises of `mask` given up by the kernel `ns` from now unless kept first (`fails`: their command buffer
    // fails there, and a later keep is refused)
    int armPromises(uint32_t mask, uint64_t ns, bool fails);
    int withdrawWrites(const std::vector<uint64_t> &handles);

    std::shared_ptr<SharedEvents> sharedEvents();
    // TegraGM20BJobABI.h method 40: where `channel` releases the event of `handle` mid-job, and the payloads it may release.
    // The kern_return_t.
    int eventWord(uint64_t handle, uint32_t channel, uint64_t &address, uint64_t &base, uint32_t &floor);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // hw_test_app's gpu.gm20b.event_release: the jobs this connection submitted, and the signals they released mid-job
    uint64_t jobsSubmitted() const
    {
        return jobs_.load(std::memory_order_relaxed);
    }

    uint64_t signalsReleased() const
    {
        return released_.load(std::memory_order_relaxed);
    }
#endif

    uint64_t nextAdmission()
    {
        return admissionCookie_.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t maxPushes() const
    {
        return (info_.flags & TegraGM20BInfoSubmitAddress) ? TegraGM20BMaxJobPushes : TegraGM20BMaxPushes;
    }

    bool exactJobs() const
    {
        return (info_.flags & TegraGM20BInfoSubmitAddress) != 0;
    }

    bool volatileWrites() const
    {
        return volatileWrites_;
    }

    // Internal fences a job may carry (TegraGM20BPushFence); 0: each fence is a submission of its own.
    uint32_t maxFences() const
    {
        return exactJobs() && (info_.flags & TegraGM20BInfoPushFences) ? TegraGM20BMaxJobFences : 0;
    }

    bool cpuAccess(const Bo &bo, uint64_t offset, uint64_t bytes, uint32_t access, bool acquire);
    CpuCompletionResult completeCpu(const CpuCompletion &completion);
    // Blocks in the kernel until the channel's completed seqno passes `seqno` (the kext sleeps on the fence interrupt).
    // `result` (null: not wanted): WaitFence's kern_return_t; kIOReturnAborted is that job's own event wait failing.
    bool wait(uint32_t channel, uint32_t seqno, uint64_t timeoutNs = ~0ull, int *result = nullptr);

    // A channel's fence word is a TegraGM20BAbiFence: a kill names the seqnos it took
    bool killRecords() const
    {
        return (info_.flags & TegraGM20BInfoKillRecord) != 0;
    }

    std::string error() const
    {
        std::lock_guard<gm20b::UnfairMutex> lock(errorLock_);
        return err_;
    }

    uint64_t vaInUse() const
    {
        return vaUsed_.load(std::memory_order_relaxed);
    }

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // HW Test gpu.perf.fill: uncompressed BOs allocated while set ask for direct physical addresses
    // (TegraGM20BBoProbeDirect); the BOs that asked, and those the kernel answered are reached through the SMMU
    mutable std::atomic<bool> probeDirect{false};
    std::atomic<uint64_t> probeDirectBos{0}, smmuBos{0};

    // A debug kernel's method outside TegraGM20BABI.h (TegraGM20BPathsABI.h), on this connection
    int debugCall(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
    {
        return call(method, in, inSize, out, outSize);
    }
#endif

    // The bytes of the BOs this connection allocated and holds (MTLDevice.currentAllocatedSize)
    uint64_t allocatedBytes() const
    {
        return allocated_.load(std::memory_order_relaxed);
    }

    // The CPU mappings of the connection's control memory (allocControl) it holds
    uint64_t controlMappedBytes() const
    {
        return controlMapped_.load(std::memory_order_relaxed);
    }

private:
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    std::atomic<uint64_t> jobs_{0}, released_{0};
#endif
    void setError(std::string error)
    {
        std::lock_guard<gm20b::UnfairMutex> lock(errorLock_);
        err_ = std::move(error);
    }

    int call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize);
    void notResponding(uint32_t channel, uint32_t opened);
    bool map(uint32_t token, bool readOnly, uint64_t &address, uint64_t &bytes);
    bool bind(uint64_t va, uint64_t handle, uint64_t range, uint32_t kind, bool gpuVolatile = false);
    uint64_t allocVa(uint64_t bytes, uint64_t align);
    void freeVa(uint64_t va, uint64_t bytes);
    uint32_t conn_ = 0;
    bool tunnel_ = false;
    bool volatileWrites_ = false;
    bool sameUses_ = false;
    FreeObserver *freeObserver_ = nullptr;
    // A SurfaceWrites failed: imports stop saying the GPU only samples them.
    std::atomic<bool> importsWritten_{false};
    std::atomic<uint32_t> lostChannel_{0}, lostError_{0}, opened_{0};
    std::atomic<uint64_t> admissionCookie_{1};
    gm20b::UnfairMutex lostLock_;  // a channel made, and a channel marked lost, one after the other
    std::atomic<uint64_t> lostFaultVa_{0};
    uint64_t fenceMap_ = 0;  // the connection's fence page, mapped once: every channel's word is in it
    uint64_t fenceMapBytes_ = 0;
    uint32_t selector_ = 0;
    TegraGM20BAbiInfo info_ = {};
    std::map<uint64_t, uint64_t> vaFree_;  // start -> bytes, coalesced
    gm20b::UnfairMutex vaLock_;
    gm20b::UnfairMutex eventsLock_;
    std::shared_ptr<SharedEvents> events_;
    std::atomic<uint64_t> vaUsed_{0};
    std::atomic<uint64_t> allocated_{0};
    // Kept by the threads that make and free control memory (the open, the submit worker's rings), read by the debug
    // give-back line on another: a count of their own, as allocated_ is, not their BOs' fields
    std::atomic<uint64_t> controlMapped_{0};
    std::string err_;
    mutable gm20b::UnfairMutex errorLock_;
};

// Whether the calling thread's last Kernel::submitJob, asked to end quiet, ends with its own interrupt all the same
bool jobKeptLoud();

}  // namespace gm20b
