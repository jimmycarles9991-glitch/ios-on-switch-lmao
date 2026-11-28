// SPDX-License-Identifier: MIT
// The TegraGM20B user client as gm20b::Kernel calls it (drivers/TegraGM20B/TegraGM20BABI.h, TegraGM20BJobABI.h), for the
// raw path's channel recovery and resource set tests. Channels live in the slots of one fence page: a channel takes the
// lowest free slot, and a slot's seqnos go on across the channels it holds, so a channel made again can land in a slot
// whose fence is behind the old one. Jobs (SubmitJob) complete as they are submitted, or stay in flight
// until a kill, which writes the record and then moves the fence past every seqno given out, as the kernel does, or with
// a test's GPU (model_gpu.h) complete as it runs them, a WaitFence waiting for that. Each job keeps the words of its
// pushes that lie in memory the model bound (VmBind), for a test to read the methods back, and checkJob sees the words of
// each job whose pushes all lie there (job_methods.h checks their methods). Memory a
// killed job wrote holds undefined contents until a job writes it again; a job that only reads it is counted
// (staleReads). A SubmitJob naming a set uses every member as registered, the records of one BO one member with their
// accesses OR'd, as the kernel's sorted_members merges them (SwitchGpuResourceSet.cpp). Sets leave out what the kernel
// leaves out (a BO a CPU lease named, a BO of another set: access 0 in each of its records) and end their generation,
// mirrored in the fence page, when a member is freed or leased by the CPU, a channel that used the set is destroyed
// killed, or the set is made again. A ResourceSetCreate naming a handle the connection does not hold is refused kIOReturnBadArgument, as the
// kernel's attachSet refuses it, with the reason in that record's reserved word. The first IOServiceOpen of a model is
// `model`; each later one is a connection of its own (its BOs, sets, channels and GPU VM), closed with
// IOServiceClose, as HW Test's bench is beside the app's Metal device. Its IOKit calls replace the framework's:
// include it in one unit of a test. The fence page is one page laid out as the kernel's (TegraGM20BFencePageABI.h): the
// channels' records, the channel epoch, the set generations, the clock anchors of each power-up (powerUp) and the writer
// promises' words, which a Make takes one past and the end of a job that keeps them (when the model completes it at its
// submission) or their cancel releases.
#pragma once
#include "TegraGM20BABI.h"
#include "TegraGM20BClockABI.h"
#include "TegraGM20BCompilerABI.h"
#include "TegraGM20BFencePageABI.h"
#include "TegraGM20BJobABI.h"
#include "TegraGM20BSchedABI.h"
#include "TegraGM20BVolatileABI.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/memory_object_types.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kernel_model {

constexpr uint32_t kSlots = TegraGM20BFencePageChannelsBytes / TegraGM20BFencePageRecordBytes,
                   kRecordBytes = TegraGM20BFencePageRecordBytes, kFenceToken = 0x7fff0000,
                   kFencePageBytes = TegraGM20BFencePageGpuBytes;

inline void (*checkJob)(const uint32_t *words, const TegraGM20BAbiPush *pushes, uint32_t count) = nullptr;
// Each call as it arrives, before the model takes it and outside callLock: a test holds one open there while others go on,
// or answers it (anything but KERN_SUCCESS)
inline std::atomic<int (*)(uint32_t method, const void *input)> arriving{nullptr};

enum class ParkState : uint8_t {
    Live = 0,
    Parked,
    Reclaimed,
};

struct BoVmBacking {
    mach_port_t entry = MACH_PORT_NULL;
    uint64_t va = 0;
    uint64_t bytes = 0;
    bool ownsVa = false;

    ~BoVmBacking()
    {
        if (ownsVa && va && bytes)
            vm_deallocate(mach_task_self(), vm_address_t(va), vm_size_t(bytes));
        if (entry != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), entry);
    }
};

struct BoState {
    bool import = false, cached = false, cpuReached = false;
    bool hostWritten = false;         // a CPU write lease released on it since a job last named it
    uint32_t set = 0;                 // the set it is a member of, 0: none
    uint64_t address = 0, bytes = 0;  // an import's host range (or BoAlloc bytes)
    bool control = false, coherent = false, system = false, poolVa = false;
    ParkState parkState = ParkState::Live;
    bool mapped = false, parkedGuard = false, mappedFixed = false;
    uint64_t mappedVa = 0, parkedVa = 0;
    std::shared_ptr<BoVmBacking> vm = nullptr;
};

struct SetState {
    std::vector<TegraGM20BAbiSurfaceUse> members;
    uint32_t generation = 0, used = 0;  // used: the slots whose channels ran its jobs
    bool live = false;
};

struct Job {
    uint32_t channel = 0, seqno = 0;
    uint64_t uid = 0;
    std::vector<TegraGM20BAbiSurfaceUse> uses;
    uint32_t flags = 0;           // TegraGM20BAbiJob.flags
    std::vector<uint32_t> words;  // its pushes' method stream, in push order, where the model's memory holds them
    std::vector<TegraGM20BAbiPush> pushes;  // a SubmitJob's GP entries
    bool hostWritten = false;               // the kernel drops the GPU L2's lines before the job (gpu_exec.c)
};

struct Slot {
    uint32_t submitted = 0, error = 0;
    uint64_t uid = 0;      // the channel the slot holds, 0: free
    bool foreign = false;  // another driver's channel of the connection (NVK's) holds it
    std::vector<TegraGM20BAbiSurfaceUse> cachedUses;
    uint64_t cachedBindGen = 0, cachedBoGen = 0;
    uint32_t cachedSet = 0;
    bool cachedAdmitted = false, cachedValid = false;
};

// The kernel's page is zero where no region of TegraGM20BFencePageABI.h is; the model's holds kStray there, so a word
// placed in no region reads it instead of the zero it starts at, and a write there shows (stray)
struct FencePage {
    static constexpr uint8_t kStray = 0xa5;
    alignas(64) uint8_t bytes[kFencePageBytes];

    FencePage()
    {
        std::memset(bytes, kStray, sizeof bytes);
        for (const tgm_fence_page_region &r : tgm_fence_page_regions)
            std::memset(bytes + r.first, 0, r.bytes);
    }

    // The first byte in no region that does not hold kStray, kFencePageBytes where there is none
    uint32_t stray() const
    {
        for (uint32_t at = 0; at < kFencePageBytes; at++)
            if (bytes[at] != kStray &&
                std::none_of(std::begin(tgm_fence_page_regions), std::end(tgm_fence_page_regions),
                             [at](const tgm_fence_page_region &r) {
                                 return at >= r.first && at - r.first < r.bytes;
                             }))
                return at;
        return kFencePageBytes;
    }
};

struct Model {
    FencePage fences;
    Slot slots[kSlots];
    uint64_t nextHandle = 1, nextUid = 1;
    std::map<uint32_t, std::vector<uint8_t>> memory;
    std::map<uint64_t, BoState> bos;
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> imports;  // handle: the host pages imported and their bytes
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> binds;    // va: the BO bound there and its bytes
    std::map<uint64_t, uint32_t> bindKinds;                     // va: the PTE kind it was bound with

    // Where the host holds the BO bound at `va` (its allocation, or the pages an import named), null: none bound there
    uint8_t *host(uint64_t va)
    {
        return host(va, 1);
    }

    // [va, va + bytes) in the host memory of the one bind that holds all of it; null where it does not
    uint8_t *host(uint64_t va, uint64_t bytes)
    {
        if (packMappedBytes && packImage.hostData && va >= packMappedVa && va - packMappedVa < packMappedBytes &&
            bytes <= packMappedBytes - (va - packMappedVa))
            return const_cast<uint8_t *>(packImage.hostData + (va - packMappedVa));
        auto bound = binds.upper_bound(va);
        if (bound == binds.begin())
            return nullptr;
        --bound;
        const uint64_t offset = va - bound->first, handle = bound->second.first;
        const auto within = [&](uint64_t size) {
            return offset < size && bytes <= size - offset;
        };
        if (!within(bound->second.second))
            return nullptr;
        const auto bo = bos.find(handle);
        if (bo != bos.end() && bo->second.vm && bo->second.mapped && bo->second.parkState == ParkState::Live &&
            within(bo->second.bytes))
            return reinterpret_cast<uint8_t *>(uintptr_t(bo->second.mappedVa)) + offset;
        if (const auto held = memory.find(uint32_t(handle)); held != memory.end() && within(held->second.size()))
            return held->second.data() + offset;
        if (const auto pages = imports.find(handle); pages != imports.end() && within(pages->second.second))
            return reinterpret_cast<uint8_t *>(uintptr_t(pages->second.first)) + offset;
        return nullptr;
    }

    SetState sets[TegraGM20BMaxResourceSets];
    unsigned submitJobs = 0, setJobs = 0, staleSets = 0, staleViews = 0;
    unsigned setRefusals = 0;    // ResourceSetCreates refused for a member
    uint64_t refusedHandle = 0;  // the last one's member
    unsigned setRepeats = 0;     // ResourceSetCreate records naming a BO an earlier record of the same call named
    unsigned refuseSets = 0;     // ResourceSetCreates refused from now, kIOReturnNoResources (no set left to make)
    bool opened = false;         // the first IOServiceOpen took this model
    io_connect_t nextConnection = 43;
    std::map<io_connect_t, std::unique_ptr<Model>> others;                    // the later IOServiceOpens' connections
    std::map<uint32_t, std::pair<uint64_t, std::vector<uint64_t>>> admitted;  // channel: cookie, its uses' BOs
    std::vector<uint64_t> cpuLocked;  // an IOSurfaceLock in another process: a job naming one is refused CannotLock

    bool lockedByCpu(uint64_t bo) const
    {
        return std::find(cpuLocked.begin(), cpuLocked.end(), bo) != cpuLocked.end();
    }

    // Another process's IOSurfaceLock: it waits (kIOReturnBusy here) while a command buffer's admission holds the surface
    int cpuLock(uint64_t bo)
    {
        for (const auto &[channel, held] : admitted)
            if (std::find(held.second.begin(), held.second.end(), bo) != held.second.end())
                return kIOReturnBusy;
        cpuLocked.push_back(bo);
        return KERN_SUCCESS;
    }

    std::vector<Job> jobs;
    bool inFlight = false;  // jobs run until a kill
    // A test's GPU: each job is in flight until it ran, and a WaitFence on it waits up to then (false: it timed out)
    std::function<void()> jobAdded;
    std::function<void(const TegraGM20BAbiBindOp &op)> bound;  // each VmBind's op, as the kernel takes it
    std::function<bool(uint32_t channel, uint32_t seqno)> blockUntil;
    // the shader compiler broker (SwitchGpuShaderCompiler.cpp): none while its info flags are 0; each compile's answer is
    // the next of `compileResults`, the last one again once they run out
    uint32_t compilerFlags = 0;
    std::vector<int32_t> compileResults = {TegraGM20BCompileUnavailable};
    unsigned compiles = 0;
    // The compiler's store a client maps (TegraGM20BCompilerStoreToken), of stack 0 as the harness's packs are, and the
    // compiler itself: each request's bytes, answered with its result once it put what it made in the store (unset: the
    // answers are compileResults'); each answer names a new store generation, which the client maps again
    const uint8_t *store = nullptr;
    uint64_t storeBytes = 0;
    uint32_t storeGeneration = 1;
    std::function<int32_t(const std::string &request)> compiler;
    bool asyncCompile = false;
    std::function<int(uint64_t cookie)> compileWait;
    std::vector<std::string> compileRequests;  // each compile's request bytes, as the client sent them

    struct CompileCall {
        uint32_t qos = 0, flags = 0;
    };

    std::vector<CompileCall> compileCalls;
    unsigned refuse = 0;  // submissions refused from now, with `refusal`
    // a SubmitJob use of no BO the connection holds or binds refused kIOReturnBadArgument, as the kernel refuses it (off:
    // the tests whose uses name memory the model never bound)
    bool unknownUsesRefused = false;
    // BoAllocs of control memory (TegraGM20BBoControl) made, and whether they are answered kIOReturnNoMemory
    unsigned controlAllocs = 0;
    bool refuseControl = false;
    bool refuseAllocs = false;  // every BoAlloc answered kIOReturnNoMemory
    // the next BoAllocs and BoImports refused, kIOReturnNoMemory: memory the kernel has none of, pages it cannot wire
    unsigned refuseAllocCount = 0, refuseImports = 0;
    // GetInfo's client VA range: a gm20b::Device's shader heap reserves a 4 GiB program window in its quarter
    uint64_t vaStart = 0x1000000;
    uint64_t vaEnd = uint64_t(1) << 40;
    uint32_t infoFlags = 0;  // GetInfo's flags beyond the model's own (TegraGM20BInfoTiledCache)
    bool coherentFromPool = true;
    bool forceParkBusy = false, forceUnparkBusy = false;
    uint64_t overrideUnparkBytes = 0, maxBoBytes = 0;
    uint64_t parkCapBytes = 0, parkedBytes = 0;
    unsigned refuseParkCount = 0, refuseUnparkCount = 0;
    int refuseParkError = kIOReturnBusy;
    unsigned parkCalls = 0, parkRefusals = 0, unparkResident = 0, unparkReclaimed = 0, reclaims = 0, fixedMaps = 0,
             badUnmaps = 0;
    std::vector<uint64_t> parkedOrder;

    bool boParkEnabled() const
    {
        return (infoFlags & TegraGM20BInfoBoPark) != 0;
    }

    bool reclaimParked(uint64_t handle)
    {
        const auto it = bos.find(handle);
        if (it == bos.end() || it->second.parkState != ParkState::Parked)
            return false;
        BoState &bo = it->second;
        parkedOrder.erase(std::remove(parkedOrder.begin(), parkedOrder.end(), handle), parkedOrder.end());
        parkedBytes -= bo.bytes;
        for (auto bound = binds.begin(); bound != binds.end();)
            bound = bound->second.first == handle ? binds.erase(bound) : std::next(bound);
        if (bo.vm && bo.vm->entry != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), bo.vm->entry);
            bo.vm->entry = MACH_PORT_NULL;
        }
        bo.parkState = ParkState::Reclaimed;
        ++bindGen;
        ++boGen;
        reclaims++;
        return true;
    }

    unsigned reclaimAllParked()
    {
        const std::vector<uint64_t> order = parkedOrder;
        unsigned n = 0;
        for (uint64_t handle : order)
            n += reclaimParked(handle);
        return n;
    }

    void reclaimToCap()
    {
        while (parkCapBytes && parkedBytes > parkCapBytes && !parkedOrder.empty())
            reclaimParked(parkedOrder.front());
    }

    struct PackImageConfig {
        bool enabled = false;
        uint32_t status = TegraGM20BPackStatusReady;
        int error = KERN_SUCCESS;
        bool allowCpuWire = true;
        uint64_t gpuVa = TGM_PACK_WINDOW_VA;
        uint64_t cpuAddress = 0;
        uint64_t tier0HeapBytes = 0;
        uint64_t heapBytes = 0;
        uint64_t stack = 0;
        uint64_t waitedNs = 0;
        uint64_t vmPageBytes = 0;
        const uint8_t *hostData = nullptr;
        std::function<void()> onMap;
    };

    struct PackImageCall {
        uint32_t flags = 0;
        std::vector<TegraGM20BAbiPackCpuRange> cpuRanges;
        uint32_t status = 0;
        int kr = KERN_SUCCESS;
    };

    PackImageConfig packImage;
    std::vector<PackImageCall> packImageCalls;
    uint64_t packMappedVa = 0, packMappedBytes = 0;
    // A test's answer to a method the model does not implement, its reply written into `output`; unset, every such
    // method is refused kIOReturnUnsupported, as a kernel without the feature refuses it (ZcullInfo: no zcull RAM)
    std::function<kern_return_t(uint32_t method, void *output)> otherMethod;
    // SurfaceLayout's answer to every import that asks (TegraGM20BInfoSurfaceLayout in infoFlags): 0 pitch, or
    // TegraGM20BLayoutTiled, the kernel's block-linear decision for the surface
    uint32_t surfaceLayout = 0;
    int refusal = kIOReturnNoResources;
    unsigned notResponding = 0;  // WaitFences answered kIOReturnNotResponding while GetStatus calls the channel healthy
    bool foreignTakesFreedSlot = false;
    bool volatileWrites = true;
    bool priorityCritical = false;
    bool channelProgress = true;
    bool sameUses = true;
    uint64_t bindGen = 1, boGen = 1;
    unsigned sameUseHits = 0, sameUseMisses = 0;
    unsigned creates = 0, destroys = 0, staleReads = 0, cpuAcquireCalls = 0;
    // the channel epoch's count (channelEpochMoves)
    uint32_t epoch = 0;
    std::vector<uint64_t> freed;             // BoFree's handles, in order
    std::map<uint64_t, uint64_t> undefined;  // handle: the killed channel whose job wrote it last
    // Kernel events (EventImport, EventWord): off, the kernel answers neither, as one without them does
    bool kernelEvents = false;
    std::map<uint64_t, uint32_t> events;  // handle: the port imported
    uint64_t nextEvent = 1;
    std::vector<uint64_t> eventWords;  // EventWord's handles, in order

    // WriterPromise's calls, in order; a Make is answered with the next promise, which its record's `promise` holds (an
    // Arm's is its mask, and `ns` its bound); promiseCalled runs after each is recorded, under callLock
    struct PromiseCall {
        uint32_t op = 0, promise = 0;
        std::vector<uint64_t> handles;
        uint64_t ns = 0;
    };

    std::vector<PromiseCall> promiseCalls;
    std::function<void()> promiseCalled;
    uint32_t nextPromise = 0;
    uint32_t promiseValues[TegraGM20BMaxPromises] = {};
    // The holders' bounds (TegraGM20BPromiseArm) on the model's own clock, which only advancePromiseClock moves: a
    // promise past its bound is given up as the kernel gives it up (released, so a reader goes on), and a job that keeps
    // it is refused with kIOReturnTimeout
    uint64_t promiseNs = 0;
    uint64_t promiseDue[TegraGM20BMaxPromises] = {};
    bool promiseGivenUp[TegraGM20BMaxPromises] = {}, promiseFails[TegraGM20BMaxPromises] = {};
    // The kernel's clock anchors (gpu_anchor.c), on the first connection's model, which holds them
    TegraGM20BAbiClockAnchors anchors = {};

    TegraGM20BAbiFence &fence(uint32_t slot)
    {
        return *reinterpret_cast<TegraGM20BAbiFence *>(fences.bytes + slot * kRecordBytes);
    }

    volatile uint32_t &epochWord()
    {
        return reinterpret_cast<volatile uint32_t *>(fences.bytes)[TegraGM20BChannelEpochWord];
    }

    volatile uint32_t &promiseWord(uint32_t promise)
    {
        return reinterpret_cast<volatile uint32_t *>(fences.bytes)[TegraGM20BPromiseWord + promise];
    }

    void promiseReleased(uint32_t promise)
    {
        if (promise < TegraGM20BMaxPromises && int32_t(promiseWord(promise) - promiseValues[promise]) < 0)
            promiseWord(promise) = promiseValues[promise];
    }

    // Under callLock
    void advancePromiseClock(uint64_t ms)
    {
        promiseNs += ms * 1000000;
        for (uint32_t p = 0; p < TegraGM20BMaxPromises; ++p) {
            if (!promiseDue[p] || promiseNs < promiseDue[p])
                continue;
            promiseDue[p] = 0;
            promiseGivenUp[p] = true;
            promiseReleased(p);
        }
    }

    TegraGM20BAbiClockAnchors *anchorTable()
    {
        return reinterpret_cast<TegraGM20BAbiClockAnchors *>(fences.bytes + TegraGM20BClockAnchorOffset);
    }

    // A power-up of the GPU: its anchor into every connection's page, as the kernel publishes it
    void powerUp(const TegraGM20BAbiClockAnchor &anchor)
    {
        const uint32_t n = anchors.published;
        *tgm_clock_anchor_slot(&anchors, n) = anchor;
        anchors.published = n + 1;
        tgm_clock_anchor_mirror(anchorTable(), &anchors, n);
        for (auto &[connection, other] : others)
            tgm_clock_anchor_mirror(other->anchorTable(), &anchors, n);
    }

    // The GPU's channel epoch (TegraGM20BChannelEpochWord) moving on, in every connection's page, as the kernel moves it
    // at a channel destroyed on any connection, a connection closed, or pool memory back after the pool refused the kernel
    // an allocation. On the first connection's model, which holds the count.
    void channelEpochMoves()
    {
        ++epoch;
        __atomic_store_n(&epochWord(), epoch, __ATOMIC_RELEASE);
        for (auto &[connection, other] : others)
            __atomic_store_n(&other->epochWord(), epoch, __ATOMIC_RELEASE);
    }

    Slot *live(uint32_t channel)
    {
        if (!channel || channel > kSlots || !slots[channel - 1].uid || slots[channel - 1].foreign)
            return nullptr;
        return &slots[channel - 1];
    }

    unsigned liveChannels() const
    {
        unsigned n = 0;
        for (const Slot &s : slots)
            n += s.uid && !s.foreign;
        return n;
    }

    void kill(uint32_t channel, uint32_t error)
    {
        Slot &s = slots[channel - 1];
        TegraGM20BAbiFence &f = fence(channel - 1);
        for (const Job &job : jobs)
            if (job.uid == s.uid && int32_t(job.seqno - f.completed) > 0)
                for (const auto &use : job.uses)
                    if (use.access & TegraGM20BSurfaceWrite)
                        undefined[use.handle] = s.uid;
        f.killed_first = f.completed + 1;
        f.killed_last = s.submitted;
        f.error = error;
        __atomic_store_n(&f.completed, s.submitted, __ATOMIC_RELEASE);
        s.error = error;
    }

    int create(TegraGM20BAbiChannelCreateReply &reply)
    {
        uint32_t slot = 0;
        while (slot < kSlots && (slots[slot].uid || slots[slot].foreign))
            slot++;
        if (slot == kSlots)
            return kIOReturnNoResources;
        creates++;
        slots[slot].uid = nextUid++;
        slots[slot].error = 0;
        slots[slot].cachedUses.clear();
        slots[slot].cachedValid = false;
        TegraGM20BAbiFence &f = fence(slot);
        f.killed_first = f.killed_last = f.error = 0;
        auto *identity = reinterpret_cast<TegraGM20BAbiFenceIdentity *>(
            fences.bytes + slot * kRecordBytes + sizeof(TegraGM20BAbiFence) + sizeof(TegraGM20BAbiFenceSemantic));
        identity->channel_uid = slots[slot].uid;
        reply = {slot + 1, kFenceToken, slot * kRecordBytes, 0};
        return KERN_SUCCESS;
    }

    // A use's BO: by handle, or with TegraGM20BSurfaceGpuAddress the BO bound there; 0: none
    uint64_t resolve(const TegraGM20BAbiSurfaceUse &use) const
    {
        if (!(use.access & TegraGM20BSurfaceGpuAddress))
            return bos.count(use.handle) ? use.handle : 0;
        for (const auto &[va, bound] : binds)
            if (use.handle >= va && use.handle - va < bound.second &&
                (!(use.access & TegraGM20BSurfaceRange) || use.reserved <= bound.second - (use.handle - va)))
                return bound.first;
        return 0;
    }

    // The words of `pushes` that lie whole in memory the model bound
    std::vector<uint32_t> pushWords(const TegraGM20BAbiPush *pushes, uint32_t count)
    {
        std::vector<uint32_t> words;
        bool whole = true;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *first = host(pushes[i].va), *last = host(pushes[i].va + uint64_t(pushes[i].dwords) * 4 - 1);
            if (first && last == first + uint64_t(pushes[i].dwords) * 4 - 1) {
                const auto *w = reinterpret_cast<const uint32_t *>(first);
                words.insert(words.end(), w, w + pushes[i].dwords);
            } else {
                whole = false;
            }
        }
        if (whole && checkJob)
            checkJob(words.data(), pushes, count);
        return words;
    }

    int run(Slot &s, uint32_t channel, std::vector<TegraGM20BAbiSurfaceUse> uses, TegraGM20BAbiExecReply &reply,
            uint32_t flags, std::vector<uint32_t> words = {})
    {
        for (auto &use : uses)
            use.handle = resolve(use) ? resolve(use) : use.handle;
        Job job{channel, ++s.submitted, s.uid, std::move(uses), flags, std::move(words), {}, false};
        for (const auto &use : job.uses) {
            if (use.access & TegraGM20BSurfaceWrite)
                undefined.erase(use.handle);
            else if (undefined.count(use.handle))
                staleReads++;
        }
        jobs.push_back(job);
        if (!inFlight && !jobAdded)
            __atomic_store_n(&fence(channel - 1).completed, job.seqno, __ATOMIC_RELEASE);
        reply = {job.seqno, 0};
        return KERN_SUCCESS;
    }

    int submitJob(const TegraGM20BAbiSubmitAddress &address, TegraGM20BAbiExecReply &reply)
    {
        const auto *head = reinterpret_cast<const TegraGM20BAbiJob *>(uintptr_t(address.address));
        Slot *s = live(head->channel);
        if (!s || head->set > TegraGM20BMaxResourceSets || !head->set != !head->set_generation)
            return kIOReturnBadArgument;
        if (s->error)
            return kIOReturnNotResponding;
        if (refuse) {
            refuse--;
            return refusal;
        }
        SetState *set = head->set ? &sets[head->set - 1] : nullptr;
        if (set && !set->live)
            return kIOReturnBadArgument;
        if (set && set->generation != head->set_generation) {
            staleSets++;
            return kIOReturnNotReady;
        }
        const auto *shared = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(
            reinterpret_cast<const uint8_t *>(head + 1) + head->push_count * sizeof(TegraGM20BAbiPush));
        const uint32_t admission = head->flags & TegraGM20BJobAdmission;
        const uint64_t cookie =
            admission ? reinterpret_cast<const TegraGM20BAbiSubmitAdmission *>(shared + head->shared_count)->cookie : 0;
        auto held = admitted.find(head->channel);
        if (admission & TegraGM20BJobAdmissionCancel) {
            if (head->flags & TegraGM20BJobSameUses)
                return kIOReturnBadArgument;
            if (held != admitted.end() && held->second.first == cookie)
                admitted.erase(held);
            reply = {};
            return KERN_SUCCESS;
        }
        if (bool(admission & TegraGM20BJobAdmissionContinue) != (held != admitted.end()) ||
            (held != admitted.end() && held->second.first != cookie))
            return kIOReturnBadArgument;
        const bool useCached = (head->flags & TegraGM20BJobSameUses) != 0;
        const bool isAdmitted = (admission & (TegraGM20BJobAdmissionBegin | TegraGM20BJobAdmissionContinue)) != 0;
        std::vector<TegraGM20BAbiSurfaceUse> effective;
        if (useCached) {
            if (head->shared_count != 0)
                return kIOReturnBadArgument;
            if (!s->cachedValid || s->cachedBindGen != bindGen || s->cachedBoGen != boGen ||
                s->cachedSet != head->set || s->cachedAdmitted != isAdmitted) {
                s->cachedValid = false;
                sameUseMisses++;
                return kIOReturnNotReady;
            }
            sameUseHits++;
            effective = s->cachedUses;
        } else {
            s->cachedValid = false;
            effective.assign(shared, shared + head->shared_count);
            for (const auto &u : effective)
                if (const uint64_t bo = resolve(u); (!bo && unknownUsesRefused) || (bo && bos.at(bo).set))
                    return kIOReturnBadArgument;
        }
        std::vector<uint64_t> named;
        for (const auto &u : effective)
            if (const uint64_t bo = resolve(u))
                named.push_back(bo);
        for (const uint64_t bo : named) {
            if (bos.count(bo) && bos.at(bo).parkState != ParkState::Live)
                return kIOReturnNotReady;
            if (held != admitted.end() &&
                std::find(held->second.second.begin(), held->second.second.end(), bo) == held->second.second.end())
                return kIOReturnBadArgument;
            if (held == admitted.end() && lockedByCpu(bo))
                return kIOReturnCannotLock;
        }
        if (set) {
            for (const auto &m : set->members)
                if (bos.count(m.handle) && bos.at(m.handle).parkState != ParkState::Live)
                    return kIOReturnNotReady;
        }
        if (!useCached) {
            s->cachedUses = effective;
            s->cachedBindGen = bindGen;
            s->cachedBoGen = boGen;
            s->cachedSet = head->set;
            s->cachedAdmitted = isAdmitted;
            s->cachedValid = true;
        }
        if (admission & TegraGM20BJobAdmissionBegin)
            admitted[head->channel] = {cookie, named};
        if (admission & TegraGM20BJobAdmissionEnd)
            admitted.erase(head->channel);
        if (staleViews) {
            staleViews--;
            return kIOReturnNotReady;
        }
        std::vector<TegraGM20BAbiSurfaceUse> uses = effective;
        if (set) {
            uses.insert(uses.end(), set->members.begin(), set->members.end());
            set->used |= 1u << (head->channel - 1);
            setJobs++;
        }
        bool hostWritten = (head->flags & TegraGM20BJobHostWritten) != 0;
        for (const auto &u : effective) {
            hostWritten |= (u.access & TegraGM20BSurfaceHostWritten) != 0;
            if (const auto bo = bos.find(resolve(u)); bo != bos.end())
                hostWritten |= std::exchange(bo->second.hostWritten, false);
        }
        const auto *kept = reinterpret_cast<const TegraGM20BAbiJobPromises *>(
            reinterpret_cast<const uint8_t *>(shared + head->shared_count) +
            (admission ? sizeof(TegraGM20BAbiSubmitAdmission) : 0));
        const uint32_t keeps =
            (head->flags & TegraGM20BJobFulfills) ? std::min<uint32_t>(kept->count, TegraGM20BMaxJobPromises) : 0;
        for (uint32_t i = 0; i < keeps; i++) {
            if (promiseGivenUp[kept->promise[i] % TegraGM20BMaxPromises] &&
                promiseFails[kept->promise[i] % TegraGM20BMaxPromises])
                return kIOReturnTimeout;
            promiseDue[kept->promise[i] % TegraGM20BMaxPromises] = 0;
        }
        submitJobs++;
        const auto *pushes = reinterpret_cast<const TegraGM20BAbiPush *>(head + 1);
        const int kr = run(*s, head->channel, std::move(uses), reply, head->flags, pushWords(pushes, head->push_count));
        jobs.back().hostWritten = hostWritten;
        jobs.back().pushes.assign(pushes, pushes + head->push_count);
        if ((head->flags & TegraGM20BJobFulfills) && !inFlight && !jobAdded)
            for (uint32_t i = 0; i < std::min<uint32_t>(kept->count, TegraGM20BMaxJobPromises); i++)
                promiseReleased(kept->promise[i]);
        if (jobAdded)
            jobAdded();
        return kr;
    }

    void endGeneration(uint32_t index)
    {
        __atomic_store_n(&reinterpret_cast<volatile uint32_t *>(fences.bytes)[TegraGM20BSetGenerationWord + index],
                         ++sets[index].generation, __ATOMIC_RELEASE);
    }

    int setCreate(const TegraGM20BAbiResourceSetCreate &in, TegraGM20BAbiResourceSetCreateReply &out)
    {
        if (!in.count || in.count > TegraGM20BMaxSetMembers || !in.uses_address || in.set > TegraGM20BMaxResourceSets)
            return kIOReturnBadArgument;
        if (refuseSets) {
            refuseSets--;
            return kIOReturnNoResources;
        }
        auto *uses = reinterpret_cast<TegraGM20BAbiSurfaceUse *>(uintptr_t(in.uses_address));
        constexpr uint32_t kAccess = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
        std::vector<uint64_t> handles(in.count);
        for (uint32_t i = 0; i < in.count; ++i) {
            uint32_t reason = switch_gpu_set_record_refusal(&uses[i]);
            if (!reason && !(handles[i] = resolve(uses[i])))
                reason = TegraGM20BSetRefusedGone;
            if (reason) {
                setRefusals++;
                refusedHandle = uses[i].handle;
                uses[i].reserved = reason;
                return kIOReturnBadArgument;
            }
        }
        uint32_t index = in.set ? in.set - 1 : 0;
        if (in.set && !sets[index].live)
            return kIOReturnBadArgument;
        while (!in.set && index < TegraGM20BMaxResourceSets && sets[index].live)
            ++index;
        if (index == TegraGM20BMaxResourceSets)
            return kIOReturnNoResources;
        for (const auto &member : sets[index].members)
            if (const auto it = bos.find(member.handle); it != bos.end())
                it->second.set = 0;
        auto &members = sets[index].members;
        members.clear();
        std::map<uint64_t, size_t> listed;  // handle: its member
        for (uint32_t i = 0; i < in.count; ++i) {
            const BoState &bo = bos.at(handles[i]);
            const auto [at, first] = listed.emplace(handles[i], members.size());
            setRepeats += !first;
            if (bo.cpuReached || (bo.set && bo.set != index + 1))
                uses[i].access = 0;
            else if (first)
                members.push_back({handles[i], uses[i].access & kAccess, 0});
            else
                members[at->second].access |= uses[i].access & kAccess;
        }
        for (const auto &member : sets[index].members)
            bos[member.handle].set = index + 1;
        sets[index].live = true;
        ++boGen;
        endGeneration(index);
        out = {index + 1, sets[index].generation, kFenceToken, (TegraGM20BSetGenerationWord + index) * 4};
        return KERN_SUCCESS;
    }

    int setRelease(const TegraGM20BAbiResourceSetRelease &in)
    {
        if (in.reserved || !in.set || in.set > TegraGM20BMaxResourceSets || !sets[in.set - 1].live)
            return kIOReturnBadArgument;
        for (const auto &member : sets[in.set - 1].members)
            if (const auto it = bos.find(member.handle); it != bos.end())
                it->second.set = 0;
        sets[in.set - 1].members.clear();
        sets[in.set - 1].live = false;
        ++boGen;
        return KERN_SUCCESS;
    }

    void freeBo(uint64_t handle)
    {
        const auto it = bos.find(handle);
        if (it == bos.end())
            return;
        if (it->second.parkState == ParkState::Parked) {
            parkedOrder.erase(std::remove(parkedOrder.begin(), parkedOrder.end(), handle), parkedOrder.end());
            parkedBytes -= it->second.bytes;
        }
        if (const uint32_t set = it->second.set) {
            auto &members = sets[set - 1].members;
            members.erase(std::remove_if(members.begin(), members.end(),
                                         [&](const TegraGM20BAbiSurfaceUse &m) {
                                             return m.handle == handle;
                                         }),
                          members.end());
            endGeneration(set - 1);
        }
        bos.erase(it);
        imports.erase(handle);
        for (auto bound = binds.begin(); bound != binds.end();)
            bound = bound->second.first == handle ? binds.erase(bound) : std::next(bound);
        ++bindGen;
        ++boGen;
    }

    void cpuLease(uint64_t handle)
    {
        const auto it = bos.find(handle);
        if (it == bos.end())
            return;
        it->second.cpuReached = true;
        if (const uint32_t set = it->second.set) {
            auto &members = sets[set - 1].members;
            for (auto at = members.begin(); at != members.end();)
                at = at->handle == handle ? members.erase(at) : std::next(at);
            it->second.set = 0;
            endGeneration(set - 1);
        }
    }

    // A write lease's release: the kernel's next GM20B job naming the BO is host-written (the backing's host epoch moved,
    // SwitchGpuOwnership.cpp)
    void cpuReleased(const TegraGM20BAbiCpuAccess &in)
    {
        if (const auto it = bos.find(in.handle); it != bos.end() && (in.access & TegraGM20BCpuWrite))
            it->second.hostWritten = true;
    }

    void destroyed(uint32_t channel, bool killed)
    {
        for (uint32_t i = 0; i < TegraGM20BMaxResourceSets; ++i)
            if (killed && sets[i].live && (sets[i].used & (1u << (channel - 1)))) {
                sets[i].used &= ~(1u << (channel - 1));
                endGeneration(i);
            }
    }

    int wait(const TegraGM20BAbiWaitFence &in)
    {
        Slot *s = live(in.channel);
        if (!s)
            return kIOReturnBadArgument;
        if (s->error)
            return kIOReturnNotResponding;
        if (notResponding) {
            notResponding--;
            return kIOReturnNotResponding;
        }
        const auto reached = [&] {
            return int32_t(__atomic_load_n(&fence(in.channel - 1).completed, __ATOMIC_ACQUIRE) - in.seqno) >= 0;
        };
        if (blockUntil && !reached())
            return blockUntil(in.channel, in.seqno) ? KERN_SUCCESS : kIOReturnTimeout;
        return reached() ? KERN_SUCCESS : kIOReturnTimeout;
    }

    int status(const TegraGM20BAbiChannel &in, TegraGM20BAbiStatus &out)
    {
        Slot *s = live(in.channel);
        if (!s)
            return kIOReturnBadArgument;
        out = {fence(in.channel - 1).completed, s->submitted, s->error, 0, 0};
        return KERN_SUCCESS;
    }
};

// Never destroyed: a facade's work on a global queue (growBytesChunks' control buffers) may call the kernel while a
// process that is exiting runs its static destructors, and a destroyed mutex throws (the process aborts)
inline Model &model = *new Model;
// held through every call but a WaitFence, and by a test GPU's reads of the model
inline std::mutex &callLock = *new std::mutex;
constexpr io_connect_t kFirstConnection = 42;
constexpr uint32_t kMethodCompileWait = 0x534b;

// What a test put in the model for a device's threads to call or read, under callLock: each connection's bound,
// jobAdded and compiler hooks and the compiler store it served, which the model reads and calls only under callLock, so
// none runs once this returns; arriving, a plain function the model calls before it takes the lock; and blockUntil,
// which a WaitFence reads and calls holding no lock (it waits there): cleared only where set, by then by an owner that
// ended its waits (model_gpu.h, timed_gpu.h), as an empty one written here would race that read. checkJob stays:
// job_methods.h installs it for every job, and it reaches no test's state.
inline void clearHooks()
{
    std::lock_guard<std::mutex> l(callLock);
    const auto clear = [](Model &m) {
        m.bound = nullptr;
        m.jobAdded = nullptr;
        m.promiseCalled = nullptr;
        m.compiler = nullptr;
        m.asyncCompile = false;
        m.compileWait = nullptr;
        m.compilerFlags = 0;
        m.store = nullptr;
        m.storeBytes = 0;
        m.packImage.onMap = nullptr;
        if (m.blockUntil)
            m.blockUntil = nullptr;
    };
    clear(model);
    for (auto &[connection, other] : model.others)
        clear(*other);
    arriving.store(nullptr, std::memory_order_release);
}

// clearHooks at scope exit: declared after what a test's hooks capture, it goes before them
struct HooksCleared {
    HooksCleared() = default;
    HooksCleared(const HooksCleared &) = delete;
    HooksCleared &operator=(const HooksCleared &) = delete;

    ~HooksCleared()
    {
        clearHooks();
    }
};

inline Model &connected(io_connect_t connection)
{
    return connection == kFirstConnection ? model : *model.others.at(connection);
}

}  // namespace kernel_model

extern "C" kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t method, const void *input, size_t,
                                                   void *output, size_t *)
{
    if (const auto hook = kernel_model::arriving.load(std::memory_order_acquire))
        if (const int kr = hook(method, input); kr != KERN_SUCCESS)
            return kr;
    kernel_model::Model &model = kernel_model::connected(connection);
    std::unique_lock<std::mutex> hold(kernel_model::callLock, std::defer_lock);
    if (method != TegraGM20BMethodWaitFence && method != kernel_model::kMethodCompileWait)
        hold.lock();
    switch (method) {
    case TegraGM20BMethodGetInfo: {
        auto &info = *static_cast<TegraGM20BAbiInfo *>(output);
        info = {};
        info.abi_version = TegraGM20BAbiVersion;
        info.flags = TegraGM20BInfoKernelMap | TegraGM20BInfoSubmitAddress | TegraGM20BInfoKillRecord |
                     TegraGM20BInfoCpuAccess | TegraGM20BInfoChannelEpoch | TegraGM20BInfoPriority |
                     TegraGM20BInfoPriorityReply | (model.channelProgress ? TegraGM20BInfoChannelProgress : 0u) |
                     (model.sameUses ? TegraGM20BInfoSameUses : 0u) |
                     (model.volatileWrites ? TegraGM20BInfoVolatileWrites : 0u) |
                     (model.packImage.enabled ? TegraGM20BInfoPackImage : 0u) | model.infoFlags;
        info.fence_token = kernel_model::kFenceToken;
        info.cls_copy = 0xb0b5;
        info.cls_eng3d = 0xb197;
        info.va_start = model.vaStart;
        info.va_end = model.vaEnd;
        info.page_size = 4096;
        info.max_bo_bytes = model.maxBoBytes;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        if (model.refuseAllocCount) {
            model.refuseAllocCount--;
            return kIOReturnNoMemory;
        }
        const auto &in = *static_cast<const TegraGM20BAbiBoAlloc *>(input);
        if (in.flags & TegraGM20BBoControl) {
            model.controlAllocs++;
            if (model.refuseControl)
                return kIOReturnNoMemory;
        }
        if (model.refuseAllocs)
            return kIOReturnNoMemory;
        const uint32_t token = uint32_t(model.nextHandle);
        const bool control = (in.flags & TegraGM20BBoControl) != 0;
        const bool coherent = (in.flags & TegraGM20BBoCoherent) != 0;
        const bool system = (in.flags & TegraGM20BBoSystem) != 0;
        if (system && (!coherent || control || (in.flags & (TegraGM20BBoCompressible | TegraGM20BBoContiguous))))
            return kIOReturnBadArgument;
        if (in.flags && model.maxBoBytes && in.bytes > model.maxBoBytes)
            return kIOReturnBadArgument;
        kernel_model::BoState state = {};
        state.cached = !coherent;
        state.bytes = in.bytes;
        state.control = control;
        state.coherent = coherent;
        state.system = system;
        state.poolVa = coherent && !control && !system && model.coherentFromPool;
        if (model.boParkEnabled() && !control) {
            auto vm = std::make_shared<kernel_model::BoVmBacking>();
            memory_object_size_t sz = in.bytes;
            mach_port_t entry = MACH_PORT_NULL;
            const vm_prot_t prot = MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE;
            if (mach_make_memory_entry_64(mach_task_self(), &sz, 0, prot, &entry, MACH_PORT_NULL) != KERN_SUCCESS)
                return kIOReturnNoMemory;
            vm->entry = entry;
            vm->bytes = in.bytes;
            state.vm = std::move(vm);
        } else {
            model.memory[token].assign(in.bytes, 0);
        }
        model.bos[model.nextHandle] = std::move(state);
        ++model.boGen;
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, token, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoImport: {
        if (model.refuseImports) {
            model.refuseImports--;
            return kIOReturnNoMemory;
        }
        const auto &in = *static_cast<const TegraGM20BAbiBoImport *>(input);
        kernel_model::BoState imported = {};
        imported.import = true;
        imported.cached = true;
        imported.address = in.address;
        imported.bytes = in.bytes;
        model.bos[model.nextHandle] = std::move(imported);
        model.imports[model.nextHandle] = {in.address, in.bytes};
        ++model.boGen;
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, 0, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoFree:
        model.freed.push_back(static_cast<const TegraGM20BAbiBoFree *>(input)->handle);
        model.freeBo(static_cast<const TegraGM20BAbiBoFree *>(input)->handle);
        return KERN_SUCCESS;
    case TegraGM20BMethodBoPark: {
        if (!model.boParkEnabled())
            return kIOReturnUnsupported;
        const auto &in = *static_cast<const TegraGM20BAbiBoPark *>(input);
        const auto it = model.bos.find(in.handle);
        if (it == model.bos.end() || it->second.parkState != kernel_model::ParkState::Live)
            return kIOReturnBadArgument;
        kernel_model::BoState &bo = it->second;
        if (bo.import || bo.control || bo.poolVa || !bo.vm || bo.vm->entry == MACH_PORT_NULL)
            return kIOReturnUnsupported;
        if (!bo.mapped)
            return kIOReturnNotFound;
        if (bo.set != 0 || model.inFlight || model.lockedByCpu(in.handle) || model.forceParkBusy) {
            model.parkRefusals++;
            return kIOReturnBusy;
        }
        if (model.refuseParkCount) {
            model.refuseParkCount--;
            model.parkRefusals++;
            return model.refuseParkError;
        }
        const bool guard = (in.flags & TegraGM20BParkGuard) != 0;
        vm_address_t at = vm_address_t(bo.mappedVa);
        if (vm_allocate(mach_task_self(), &at, vm_size_t(bo.bytes), VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) !=
            KERN_SUCCESS)
            return kIOReturnBusy;
        if (guard)
            vm_protect(mach_task_self(), at, vm_size_t(bo.bytes), TRUE, VM_PROT_NONE);
        bo.parkState = kernel_model::ParkState::Parked;
        bo.parkedVa = bo.mappedVa;
        bo.parkedGuard = guard;
        bo.mapped = false;
        bo.mappedVa = 0;
        ++model.boGen;
        model.parkCalls++;
        model.parkedOrder.push_back(in.handle);
        model.parkedBytes += bo.bytes;
        model.reclaimToCap();
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoUnpark: {
        if (!model.boParkEnabled())
            return kIOReturnUnsupported;
        const auto &in = *static_cast<const TegraGM20BAbiBoUnpark *>(input);
        auto &reply = *static_cast<TegraGM20BAbiBoUnparkReply *>(output);
        reply = {};
        const auto it = model.bos.find(in.handle);
        if (it == model.bos.end())
            return kIOReturnBadArgument;
        kernel_model::BoState &bo = it->second;
        if (model.forceUnparkBusy &&
            (bo.parkState == kernel_model::ParkState::Parked || bo.parkState == kernel_model::ParkState::Reclaimed))
            return kIOReturnBusy;
        if (bo.parkState == kernel_model::ParkState::Reclaimed) {
            model.unparkReclaimed++;
            reply = {bo.parkedVa, bo.bytes, TegraGM20BUnparkReclaimed, 0};
            return KERN_SUCCESS;
        }
        if (bo.parkState != kernel_model::ParkState::Parked || !bo.vm || bo.vm->entry == MACH_PORT_NULL)
            return kIOReturnBadArgument;
        if (model.refuseUnparkCount) {
            model.refuseUnparkCount--;
            return kIOReturnNoMemory;
        }
        vm_address_t at = vm_address_t(bo.parkedVa);
        if (vm_map(mach_task_self(), &at, vm_size_t(bo.bytes), 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, bo.vm->entry, 0,
                   FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE) != KERN_SUCCESS)
            return kIOReturnNoMemory;
        bo.parkState = kernel_model::ParkState::Live;
        bo.mapped = true;
        bo.mappedVa = uint64_t(at);
        bo.vm->va = uint64_t(at);
        bo.vm->ownsVa = true;
        bo.parkedVa = 0;
        bo.parkedGuard = false;
        ++model.boGen;
        model.parkedOrder.erase(std::remove(model.parkedOrder.begin(), model.parkedOrder.end(), in.handle),
                                model.parkedOrder.end());
        model.parkedBytes -= bo.bytes;
        model.unparkResident++;
        const uint64_t replyBytes = model.overrideUnparkBytes ? model.overrideUnparkBytes : bo.bytes;
        reply = {bo.mappedVa, replyBytes, TegraGM20BUnparkResident, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodVmBind: {
        const auto &b = *static_cast<const TegraGM20BAbiVmBind *>(input);
        if (model.packMappedBytes) {
            const uint64_t packEnd = model.packMappedVa + model.packMappedBytes;
            for (uint32_t i = 0; i < b.count; i++) {
                const TegraGM20BAbiBindOp &op = b.ops[i];
                if (op.range && op.va < packEnd && model.packMappedVa < op.va + op.range)
                    return kIOReturnBadArgument;
            }
        }
        for (uint32_t i = 0; i < b.count; i++) {
            const TegraGM20BAbiBindOp &op = b.ops[i];
            if ((op.op == TegraGM20BBindMap || op.op == TegraGM20BBindMapVolatile) && model.bos.count(op.handle) &&
                model.bos.at(op.handle).parkState != kernel_model::ParkState::Live)
                return kIOReturnNotReady;
            if (op.range) {
                for (const auto &[bva, entry] : model.binds) {
                    const uint64_t bhandle = entry.first, brange = entry.second;
                    if (!brange || op.va >= bva + brange || bva >= op.va + op.range)
                        continue;
                    const auto bit = model.bos.find(bhandle);
                    if (bit != model.bos.end() && bit->second.parkState != kernel_model::ParkState::Live)
                        return kIOReturnNotReady;
                }
            }
        }
        for (uint32_t i = 0; i < b.count; i++) {
            const TegraGM20BAbiBindOp &op = b.ops[i];
            model.binds[op.va] = {op.handle, op.range};
            model.bindKinds[op.va] = op.kind;
            if (model.bound)
                model.bound(op);
        }
        if (b.count)
            ++model.bindGen;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCpuAcquire: {
        const uint64_t handle = static_cast<const TegraGM20BAbiCpuAccess *>(input)->handle;
        if (model.bos.count(handle) && model.bos.at(handle).parkState != kernel_model::ParkState::Live)
            return kIOReturnNotReady;
        model.cpuAcquireCalls++;
        model.cpuLease(handle);
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCpuRelease:
        model.cpuReleased(*static_cast<const TegraGM20BAbiCpuAccess *>(input));
        return KERN_SUCCESS;
    case TegraGM20BMethodResourceSetCreate:
        return model.setCreate(*static_cast<const TegraGM20BAbiResourceSetCreate *>(input),
                               *static_cast<TegraGM20BAbiResourceSetCreateReply *>(output));
    case TegraGM20BMethodResourceSetRelease:
        return model.setRelease(*static_cast<const TegraGM20BAbiResourceSetRelease *>(input));
    case TegraGM20BMethodSubmitJob:
        return model.submitJob(*static_cast<const TegraGM20BAbiSubmitAddress *>(input),
                               *static_cast<TegraGM20BAbiExecReply *>(output));
    case TegraGM20BMethodMap: {
        const auto &in = *static_cast<const TegraGM20BAbiMap *>(input);
        auto &reply = *static_cast<TegraGM20BAbiMapReply *>(output);
        if (in.token == kernel_model::kFenceToken) {
            reply = {uint64_t(uintptr_t(model.fences.bytes)), sizeof model.fences.bytes};
            return KERN_SUCCESS;
        }
        if (in.token == TegraGM20BCompilerStoreToken) {
            reply = {uint64_t(uintptr_t(model.store)), model.storeBytes};
            return KERN_SUCCESS;
        }
        if (const auto it = model.bos.find(in.token); it != model.bos.end() && it->second.vm) {
            kernel_model::BoState &bo = it->second;
            if (bo.parkState != kernel_model::ParkState::Live || bo.vm->entry == MACH_PORT_NULL)
                return kIOReturnNotReady;
            const vm_prot_t prot = (in.flags & TegraGM20BMapReadOnly) ? VM_PROT_READ : (VM_PROT_READ | VM_PROT_WRITE);
            if (in.flags & TegraGM20BMapFixed) {
                if (!in.address || bo.mapped)
                    return kIOReturnBadArgument;
                for (const auto &[otherHandle, other] : model.bos)
                    if (other.parkState == kernel_model::ParkState::Live && other.mapped &&
                        in.address < other.mappedVa + other.bytes && other.mappedVa < in.address + bo.bytes)
                        return kIOReturnBusy;
                vm_address_t regionAddr = vm_address_t(in.address);
                vm_size_t regionSize = 0;
                vm_region_basic_info_data_64_t info = {};
                mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t obj = MACH_PORT_NULL;
                if (vm_region_64(mach_task_self(), &regionAddr, &regionSize, VM_REGION_BASIC_INFO_64,
                                 reinterpret_cast<vm_region_info_t>(&info), &count, &obj) != KERN_SUCCESS ||
                    regionAddr > in.address || regionAddr + regionSize < in.address + bo.bytes)
                    return kIOReturnNotFound;
                vm_address_t at = vm_address_t(in.address);
                if (vm_map(mach_task_self(), &at, vm_size_t(bo.bytes), 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                           bo.vm->entry, 0, FALSE, prot, prot, VM_INHERIT_NONE) != KERN_SUCCESS)
                    return kIOReturnNoMemory;
                for (auto &[otherHandle, other] : model.bos)
                    if (otherHandle != it->first && other.vm && other.parkState != kernel_model::ParkState::Live &&
                        other.parkedVa == in.address)
                        other.vm->ownsVa = false;
                bo.vm->va = uint64_t(at);
                bo.vm->ownsVa = true;
                bo.mapped = true;
                bo.mappedVa = uint64_t(at);
                bo.mappedFixed = true;
                model.fixedMaps++;
                reply = {bo.mappedVa, bo.bytes};
                return KERN_SUCCESS;
            }
            vm_address_t at = 0;
            if (vm_map(mach_task_self(), &at, vm_size_t(bo.bytes), 0, VM_FLAGS_ANYWHERE, bo.vm->entry, 0, FALSE, prot,
                       prot, VM_INHERIT_NONE) != KERN_SUCCESS)
                return kIOReturnNoMemory;
            bo.vm->va = uint64_t(at);
            bo.vm->ownsVa = true;
            bo.mapped = true;
            bo.mappedVa = uint64_t(at);
            reply = {bo.mappedVa, bo.bytes};
            return KERN_SUCCESS;
        }
        reply = {uint64_t(uintptr_t(model.memory[in.token].data())), model.memory[in.token].size()};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodChannelCreate:
        return model.create(*static_cast<TegraGM20BAbiChannelCreateReply *>(output));
    case TegraGM20BMethodChannelDestroy: {
        const auto &in = *static_cast<const TegraGM20BAbiChannel *>(input);
        kernel_model::Slot *s = model.live(in.channel);
        if (!s)
            return kIOReturnBadArgument;
        model.destroys++;
        model.destroyed(in.channel, s->error != 0);
        s->uid = 0;
        s->foreign = model.foreignTakesFreedSlot;
        model.foreignTakesFreedSlot = false;
        kernel_model::model.channelEpochMoves();
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodWaitFence:
        return model.wait(*static_cast<const TegraGM20BAbiWaitFence *>(input));
    case TegraGM20BMethodGetStatus:
        return model.status(*static_cast<const TegraGM20BAbiChannel *>(input),
                            *static_cast<TegraGM20BAbiStatus *>(output));
    case TegraGM20BMethodChannelProgress: {
        if (!model.channelProgress)
            return kIOReturnUnsupported;
        const auto &in = *static_cast<const TegraGM20BAbiChannel *>(input);
        if (!model.live(in.channel))
            return kIOReturnBadArgument;
        const uint32_t slot = in.channel - 1;
        *static_cast<TegraGM20BAbiChannelProgressReply *>(output) = {uint64_t(kernel_model::kFenceToken) +
                                                                     slot * kernel_model::kRecordBytes +
                                                                     TegraGM20BFencePageChannelProgressOffset};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCpuComplete: {
        const auto &in = *static_cast<const TegraGM20BAbiCpuComplete *>(input);
        *static_cast<TegraGM20BAbiCpuCompleteReply *>(output) = {in.range_count, 0, 0, 0, 0, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodUnmap: {
        const auto &in = *static_cast<const TegraGM20BAbiUnmap *>(input);
        if (const auto it = model.bos.find(in.token); it != model.bos.end() && it->second.vm) {
            kernel_model::BoState &bo = it->second;
            if (bo.parkState != kernel_model::ParkState::Live || !bo.mapped || bo.mappedVa != in.address) {
                model.badUnmaps++;
                return kIOReturnBadArgument;
            }
            if (bo.vm->ownsVa)
                vm_deallocate(mach_task_self(), vm_address_t(in.address), vm_size_t(bo.bytes));
            bo.vm->va = 0;
            bo.vm->ownsVa = false;
            bo.mapped = false;
            bo.mappedVa = 0;
        }
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodEventImport: {
        if (!model.kernelEvents)
            return kIOReturnUnsupported;
        model.events[model.nextEvent] = static_cast<const TegraGM20BAbiEventImport *>(input)->port;
        *static_cast<TegraGM20BAbiEventImportReply *>(output) = {model.nextEvent++, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodEventRelease:
        return model.events.erase(static_cast<const TegraGM20BAbiEventRelease *>(input)->handle) ? KERN_SUCCESS
                                                                                                 : kIOReturnBadArgument;
    case TegraGM20BMethodEventWord: {
        const auto &in = *static_cast<const TegraGM20BAbiEventWord *>(input);
        if (!model.kernelEvents)
            return kIOReturnUnsupported;
        if (!model.events.count(in.handle) || in.handle >= TegraGM20BEventWords)
            return kIOReturnNoResources;
        model.eventWords.push_back(in.handle);
        const uint64_t word = kernel_model::kFenceToken + 4 * (TegraGM20BEventWordFirst + in.handle);
        *static_cast<TegraGM20BAbiEventWordReply *>(output) = {word, 0, 0, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCompilerInfo: {
        if (!model.compilerFlags)
            return kIOReturnUnsupported;
        auto &info = *static_cast<TegraGM20BAbiCompilerInfoReply *>(output);
        info = {};
        info.flags = model.compilerFlags;
        info.store_bytes = model.storeBytes;
        info.generation = model.storeGeneration;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCompile: {
        const auto &in = *static_cast<const TegraGM20BAbiCompile *>(input);
        model.compileCalls.push_back({in.qos, in.flags});
        model.compileRequests.emplace_back(reinterpret_cast<const char *>(uintptr_t(in.address)), size_t(in.bytes));
        const size_t at = std::min<size_t>(model.compiles++, model.compileResults.size() - 1);
        auto &reply = *static_cast<TegraGM20BAbiCompileReply *>(output);
        reply = {};
        if (model.compiler) {
            const auto &in = *static_cast<const TegraGM20BAbiCompile *>(input);
            reply.result = model.compiler(std::string(reinterpret_cast<const char *>(uintptr_t(in.address)), in.bytes));
            reply.generation = ++model.storeGeneration;
            return KERN_SUCCESS;
        }
        reply.result = model.compileResults[at];
        return KERN_SUCCESS;
    }
    case kernel_model::kMethodCompileWait: {
        std::function<int(uint64_t)> wait;
        {
            std::lock_guard<std::mutex> l(kernel_model::callLock);
            wait = model.compileWait;
        }
        if (!wait || !input)
            return kIOReturnNotFound;
        return wait(*static_cast<const uint64_t *>(input));
    }
    case TegraGM20BMethodPriority: {
        const auto &in = *static_cast<const TegraGM20BAbiPriority *>(input);
        if (in.priority == 3 || in.priority > TegraGM20BMaxPriority || in.background_priority > TegraGM20BMaxPriority)
            return kIOReturnBadArgument;
        if (output) {
            const bool top = in.priority == 0 || in.priority == 5;
            *static_cast<TegraGM20BAbiPriorityReply *>(output) = {
                top && model.priorityCritical ? uint32_t(TegraGM20BPriorityCritical) : 0u, 0};
        }
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSurfaceLayout:
        *static_cast<TegraGM20BAbiSurfaceLayoutReply *>(output) = {model.surfaceLayout, 0};
        return KERN_SUCCESS;
    case TegraGM20BMethodWriterPromise: {
        const auto &in = *static_cast<const TegraGM20BAbiWriterPromise *>(input);
        const auto *uses = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(uintptr_t(in.uses_address));
        kernel_model::Model::PromiseCall call{in.op, in.promise, {}};
        for (uint32_t i = 0; i < in.count; i++)
            call.handles.push_back(uses[i].handle);
        if (in.op == TegraGM20BPromiseMake) {
            call.promise = model.nextPromise++ % TegraGM20BMaxPromises;
            model.promiseValues[call.promise] = model.promiseWord(call.promise) + 1;
            model.promiseDue[call.promise] = 0;
            model.promiseGivenUp[call.promise] = false;
        } else if (in.op == TegraGM20BPromiseArm) {
            call.ns = in.uses_address;
            for (uint32_t p = 0; p < TegraGM20BMaxPromises; ++p) {
                if (!(in.promise & (1u << p)))
                    continue;
                model.promiseDue[p] = model.promiseNs + std::max<uint64_t>(in.uses_address, 1);
                model.promiseFails[p] = (in.channel & TegraGM20BPromiseArmFails) != 0;
            }
        } else if (!in.count) {
            model.promiseDue[in.promise % TegraGM20BMaxPromises] = 0;
            model.promiseReleased(in.promise);
        }
        *static_cast<TegraGM20BAbiWriterPromiseReply *>(output) = {call.promise, 0};
        model.promiseCalls.push_back(std::move(call));
        if (model.promiseCalled)
            model.promiseCalled();
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodClockAnchors:
        *static_cast<TegraGM20BAbiClockAnchorsReply *>(output) = {
            kernel_model::kFenceToken + TegraGM20BClockAnchorOffset, kernel_model::kFenceToken, 0};
        return KERN_SUCCESS;
    case TegraGM20BMethodPackImageMap: {
        if (!model.packImage.enabled)
            return kIOReturnUnsupported;
        const auto &in = *static_cast<const TegraGM20BAbiPackImageMap *>(input);
        auto &out = *static_cast<TegraGM20BAbiPackImageMapReply *>(output);
        out = {};
        const auto record = [&](int kr) {
            kernel_model::Model::PackImageCall call;
            call.flags = in.flags;
            const uint32_t n = std::min<uint32_t>(in.cpu_range_count, TegraGM20BMaxPackCpuRanges);
            call.cpuRanges.assign(in.cpu_ranges, in.cpu_ranges + n);
            call.status = out.status;
            call.kr = kr;
            model.packImageCalls.push_back(std::move(call));
            return kr;
        };
        if (in.flags != 0 || in.cpu_range_count > TegraGM20BMaxPackCpuRanges)
            return record(kIOReturnBadArgument);
        for (uint32_t i = in.cpu_range_count; i < TegraGM20BMaxPackCpuRanges; ++i)
            if (in.cpu_ranges[i].address || in.cpu_ranges[i].bytes)
                return record(kIOReturnBadArgument);
        const uint64_t vmPage = model.packImage.vmPageBytes ? model.packImage.vmPageBytes : uint64_t(getpagesize());
        uint64_t wiredBytes = 0;
        for (uint32_t i = 0; i < in.cpu_range_count; ++i) {
            const uint64_t addr = in.cpu_ranges[i].address, bytes = in.cpu_ranges[i].bytes;
            if (!addr || !bytes || (addr & (vmPage - 1)) || (bytes & (vmPage - 1)) || addr > UINT64_MAX - bytes ||
                bytes > (64ULL << 20) - wiredBytes)
                return record(kIOReturnBadArgument);
            wiredBytes += bytes;
        }
        if (model.packMappedBytes)
            return record(kIOReturnExclusiveAccess);
        if (in.cpu_range_count && !model.packImage.allowCpuWire)
            return record(kIOReturnNotPermitted);
        if (const auto hook = model.packImage.onMap) {
            hold.unlock();
            hook();
            hold.lock();
        }
        if (model.packImage.error != KERN_SUCCESS)
            return record(model.packImage.error);
        if (model.packImage.status != TegraGM20BPackStatusReady) {
            out.status = model.packImage.status;
            out.waited_ns = model.packImage.waitedNs;
            return record(KERN_SUCCESS);
        }
        if (model.packMappedBytes)
            return record(kIOReturnExclusiveAccess);
        const uint64_t va = model.packImage.gpuVa;
        const uint64_t bytes = model.packImage.tier0HeapBytes;
        const uint64_t packEnd = va + bytes;
        for (const auto &[bva, bound] : model.binds)
            if (bva < packEnd && va < bva + bound.second)
                return record(kIOReturnBadArgument);
        model.packMappedVa = va;
        model.packMappedBytes = bytes;
        out.status = TegraGM20BPackStatusReady;
        out.gpu_va = va;
        out.cpu_address = model.packImage.cpuAddress;
        out.tier0_heap_bytes = bytes;
        out.heap_bytes = model.packImage.heapBytes;
        out.stack = model.packImage.stack;
        out.cpu_wired_bytes = wiredBytes;
        out.waited_ns = model.packImage.waitedNs;
        return record(KERN_SUCCESS);
    }
    default:
        return model.otherMethod ? model.otherMethod(method, output) : kIOReturnUnsupported;
    }
}

extern "C" io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef matching)
{
    CFRelease(matching);
    return 111;
}

extern "C" kern_return_t IOServiceOpen(io_service_t, task_port_t, uint32_t, io_connect_t *connection)
{
    using kernel_model::model;
    std::lock_guard<std::mutex> hold(kernel_model::callLock);
    if (!std::exchange(model.opened, true)) {
        *connection = kernel_model::kFirstConnection;
        return KERN_SUCCESS;
    }
    *connection = model.nextConnection++;
    auto &other = model.others[*connection] = std::make_unique<kernel_model::Model>();
    other->vaStart = model.vaStart;
    other->vaEnd = model.vaEnd;
    other->packImage = model.packImage;
    other->opened = true;
    other->epochWord() = model.epoch;
    const uint32_t published = model.anchors.published;
    for (uint32_t n = published > TegraGM20BClockAnchorSlots ? published - TegraGM20BClockAnchorSlots : 0;
         n < published; n++)
        tgm_clock_anchor_mirror(other->anchorTable(), &model.anchors, n);
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOObjectRelease(io_object_t)
{
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOServiceClose(io_connect_t connection)
{
    std::lock_guard<std::mutex> hold(kernel_model::callLock);
    kernel_model::model.others.erase(connection);
    kernel_model::model.channelEpochMoves();
    return KERN_SUCCESS;
}
