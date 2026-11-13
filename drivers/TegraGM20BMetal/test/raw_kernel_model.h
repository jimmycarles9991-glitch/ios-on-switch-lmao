// SPDX-License-Identifier: MIT
// The TegraGM20B user client as raw::Kernel calls it (drivers/TegraGM20B/TegraGM20BABI.h, TegraGM20BJobABI.h), for the
// raw path's channel recovery and resource set tests. Channels live in the slots of one fence page: a channel takes the
// lowest free slot, and a slot's seqnos go on across the channels it holds, so a channel made again can land in a slot
// whose fence is behind the old one. Jobs (SubmitAddress, SubmitJob) complete as they are submitted, or stay in flight
// until a kill, which writes the record and then moves the fence past every seqno given out, as the kernel does, or with
// a test's GPU (model_gpu.h) complete as it runs them, a WaitFence waiting for that. Each job keeps the words of its
// pushes that lie in memory the model bound (VmBind), for a test to read the methods back, and checkJob sees the words of
// each job whose pushes all lie there (raw_job_methods.h checks their methods). Memory a
// killed job wrote holds undefined contents until a job writes it again; a job that only reads it is counted
// (staleReads). A SubmitJob naming a set uses every member as registered, the records of one BO one member with their
// accesses OR'd, as the kernel's sorted_members merges them (SwitchGpuResourceSet.cpp). Sets leave out what the kernel
// leaves out (a BO a CPU lease named, a BO of another set: access 0 in each of its records) and end their generation,
// mirrored in the fence page, when a member is freed or leased by the CPU, a channel that used the set is destroyed
// killed, or the set is made again. A ResourceSetCreate naming a handle the connection does not hold is refused kIOReturnBadArgument, as the
// kernel's attachSet refuses it, with the reason in that record's reserved word. The first IOServiceOpen of a model is
// `model`; each later one is a connection of its own (its BOs, sets, channels and GPU VM), closed with
// IOServiceClose, as HW Test's bench is beside the app's Metal device. Its IOKit calls replace the framework's:
// include it in one unit of a test.
#pragma once
#include "TegraGM20BABI.h"
#include "TegraGM20BCompilerABI.h"
#include "TegraGM20BJobABI.h"
#include <IOKit/IOKitLib.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace kernel_model {

constexpr uint32_t kSlots = 16, kRecordBytes = 64, kFenceToken = 0x7fff0000, kFencePageBytes = 4096;

inline void (*checkJob)(const uint32_t *words, const TegraGM20BAbiPush *pushes, uint32_t count) = nullptr;

struct BoState {
    bool import = false, cached = false, cpuReached = false;
    bool hostWritten = false;         // a CPU write lease released on it since a job last named it
    uint32_t set = 0;                 // the set it is a member of, 0: none
    uint64_t address = 0, bytes = 0;  // an import's host range
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
    uint32_t flags = 0;           // SubmitAddress: TegraGM20BAbiSubmit.surface_flags; SubmitJob: TegraGM20BAbiJob.flags
    std::vector<uint32_t> words;  // its pushes' method stream, in push order, where the model's memory holds them
    std::vector<TegraGM20BAbiPush> pushes;  // a SubmitJob's GP entries
    bool hostWritten = false;               // the kernel drops the GPU L2's lines before the job (gpu_exec.c)
};

struct Slot {
    uint32_t submitted = 0, error = 0;
    uint64_t uid = 0;      // the channel the slot holds, 0: free
    bool foreign = false;  // another driver's channel of the connection (NVK's) holds it
};

struct Model {
    alignas(64) uint8_t fences[kFencePageBytes] = {};
    Slot slots[kSlots];
    uint64_t nextHandle = 1, nextUid = 1;
    std::map<uint32_t, std::vector<uint8_t>> memory;
    std::map<uint64_t, BoState> bos;
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> imports;  // handle: the host pages imported and their bytes
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> binds;    // va: the BO bound there and its bytes

    // Where the host holds the BO bound at `va` (its allocation, or the pages an import named), null: none bound there
    uint8_t *host(uint64_t va)
    {
        return host(va, 1);
    }

    // [va, va + bytes) in the host memory of the one bind that holds all of it; null where it does not
    uint8_t *host(uint64_t va, uint64_t bytes)
    {
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
        if (const auto held = memory.find(uint32_t(handle)); held != memory.end() && within(held->second.size()))
            return held->second.data() + offset;
        if (const auto pages = imports.find(handle); pages != imports.end() && within(pages->second.second))
            return reinterpret_cast<uint8_t *>(uintptr_t(pages->second.first)) + offset;
        return nullptr;
    }

    SetState sets[TegraGM20BMaxResourceSets];
    unsigned submitJobs = 0, setJobs = 0, staleSets = 0, addressJobs = 0, staleViews = 0;
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
    unsigned refuse = 0;  // submissions refused from now, with `refusal`
    // GetInfo's client VA end: a raw::Device's shader heap reserves a 4 GiB program window in its quarter
    uint64_t vaEnd = uint64_t(1) << 40;
    uint32_t infoFlags = 0;  // GetInfo's flags beyond the model's own (TegraGM20BInfoTiledCache)
    int refusal = kIOReturnNoResources;
    unsigned notResponding = 0;  // WaitFences answered kIOReturnNotResponding while GetStatus calls the channel healthy
    bool foreignTakesFreedSlot = false;
    unsigned creates = 0, destroys = 0, staleReads = 0;
    std::vector<uint64_t> freed;             // BoFree's handles, in order
    std::map<uint64_t, uint64_t> undefined;  // handle: the killed channel whose job wrote it last
    // Kernel events (EventImport, EventWord): off, the kernel answers neither, as one without them does
    bool kernelEvents = false;
    std::map<uint64_t, uint32_t> events;  // handle: the port imported
    uint64_t nextEvent = 1;
    std::vector<uint64_t> eventWords;  // EventWord's handles, in order

    TegraGM20BAbiFence &fence(uint32_t slot)
    {
        return *reinterpret_cast<TegraGM20BAbiFence *>(fences + slot * kRecordBytes);
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
        f.completed = s.submitted;
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
        TegraGM20BAbiFence &f = fence(slot);
        f.killed_first = f.killed_last = f.error = 0;
        reply = {slot + 1, kFenceToken, slot * kRecordBytes, 0};
        return KERN_SUCCESS;
    }

    // A use's BO: by handle, or with TegraGM20BSurfaceGpuAddress the BO bound there; 0: none
    uint64_t resolve(const TegraGM20BAbiSurfaceUse &use) const
    {
        if (!(use.access & TegraGM20BSurfaceGpuAddress))
            return bos.count(use.handle) ? use.handle : 0;
        for (const auto &[va, bound] : binds)
            if (use.handle >= va && use.handle - va < bound.second)
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
            fence(channel - 1).completed = job.seqno;
        reply = {job.seqno, 0};
        return KERN_SUCCESS;
    }

    int submit(const TegraGM20BAbiSubmitAddress &address, TegraGM20BAbiExecReply &reply)
    {
        const auto *head = reinterpret_cast<const TegraGM20BAbiSubmit *>(uintptr_t(address.address));
        Slot *s = live(head->channel);
        if (!s)
            return kIOReturnBadArgument;
        if (s->error)
            return kIOReturnNotResponding;
        if (refuse) {
            refuse--;
            return refusal;
        }
        const auto *uses = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(
            reinterpret_cast<const uint8_t *>(head + 1) + head->push_count * sizeof(TegraGM20BAbiPush));
        addressJobs++;
        const int kr = run(*s, head->channel, {uses, uses + head->surface_count}, reply, head->surface_flags,
                           pushWords(reinterpret_cast<const TegraGM20BAbiPush *>(head + 1), head->push_count));
        if (jobAdded)
            jobAdded();
        return kr;
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
        for (uint32_t i = 0; i < head->shared_count; ++i)
            if (const uint64_t bo = resolve(shared[i]); bo && bos.at(bo).set)
                return kIOReturnBadArgument;
        const uint32_t admission = head->flags & TegraGM20BJobAdmission;
        const uint64_t cookie =
            admission ? reinterpret_cast<const TegraGM20BAbiSubmitAdmission *>(shared + head->shared_count)->cookie : 0;
        auto held = admitted.find(head->channel);
        if (admission & TegraGM20BJobAdmissionCancel) {
            if (held != admitted.end() && held->second.first == cookie)
                admitted.erase(held);
            reply = {};
            return KERN_SUCCESS;
        }
        if (bool(admission & TegraGM20BJobAdmissionContinue) != (held != admitted.end()) ||
            (held != admitted.end() && held->second.first != cookie))
            return kIOReturnBadArgument;
        std::vector<uint64_t> named;
        for (uint32_t i = 0; i < head->shared_count; ++i)
            if (const uint64_t bo = resolve(shared[i]))
                named.push_back(bo);
        for (const uint64_t bo : named) {
            if (held != admitted.end() &&
                std::find(held->second.second.begin(), held->second.second.end(), bo) == held->second.second.end())
                return kIOReturnBadArgument;
            if (held == admitted.end() && lockedByCpu(bo))
                return kIOReturnCannotLock;
        }
        if (admission & TegraGM20BJobAdmissionBegin)
            admitted[head->channel] = {cookie, named};
        if (admission & TegraGM20BJobAdmissionEnd)
            admitted.erase(head->channel);
        if (staleViews) {
            staleViews--;
            return kIOReturnNotReady;
        }
        std::vector<TegraGM20BAbiSurfaceUse> uses(shared, shared + head->shared_count);
        if (set) {
            uses.insert(uses.end(), set->members.begin(), set->members.end());
            set->used |= 1u << (head->channel - 1);
            setJobs++;
        }
        bool hostWritten = (head->flags & TegraGM20BJobHostWritten) != 0;
        for (uint32_t i = 0; i < head->shared_count; ++i) {
            hostWritten |= (shared[i].access & TegraGM20BSurfaceHostWritten) != 0;
            if (const auto bo = bos.find(resolve(shared[i])); bo != bos.end())
                hostWritten |= std::exchange(bo->second.hostWritten, false);
        }
        submitJobs++;
        const auto *pushes = reinterpret_cast<const TegraGM20BAbiPush *>(head + 1);
        const int kr = run(*s, head->channel, std::move(uses), reply, head->flags, pushWords(pushes, head->push_count));
        jobs.back().hostWritten = hostWritten;
        jobs.back().pushes.assign(pushes, pushes + head->push_count);
        if (jobAdded)
            jobAdded();
        return kr;
    }

    void endGeneration(uint32_t index)
    {
        reinterpret_cast<volatile uint32_t *>(fences)[TegraGM20BSetGenerationWord + index] = ++sets[index].generation;
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
        return KERN_SUCCESS;
    }

    void freeBo(uint64_t handle)
    {
        const auto it = bos.find(handle);
        if (it == bos.end())
            return;
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
        if (blockUntil && int32_t(fence(in.channel - 1).completed - in.seqno) < 0)
            return blockUntil(in.channel, in.seqno) ? KERN_SUCCESS : kIOReturnTimeout;
        return int32_t(fence(in.channel - 1).completed - in.seqno) >= 0 ? KERN_SUCCESS : kIOReturnTimeout;
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

inline Model &connected(io_connect_t connection)
{
    return connection == kFirstConnection ? model : *model.others.at(connection);
}

}  // namespace kernel_model

extern "C" kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t method, const void *input, size_t,
                                                   void *output, size_t *)
{
    kernel_model::Model &model = kernel_model::connected(connection);
    std::unique_lock<std::mutex> hold(kernel_model::callLock, std::defer_lock);
    if (method != TegraGM20BMethodWaitFence)
        hold.lock();
    switch (method) {
    case TegraGM20BMethodGetInfo: {
        auto &info = *static_cast<TegraGM20BAbiInfo *>(output);
        info = {};
        info.abi_version = TegraGM20BAbiVersion;
        info.flags = TegraGM20BInfoKernelMap | TegraGM20BInfoSubmitAddress | TegraGM20BInfoKillRecord |
                     TegraGM20BInfoCpuAccess | model.infoFlags;
        info.cls_copy = 0xb0b5;
        info.cls_eng3d = 0xb197;
        info.va_start = 0x1000000;
        info.va_end = model.vaEnd;
        info.page_size = 4096;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        const auto &in = *static_cast<const TegraGM20BAbiBoAlloc *>(input);
        const uint32_t token = uint32_t(model.nextHandle);
        model.memory[token].assign(in.bytes, 0);
        model.bos[model.nextHandle] = {false, !(in.flags & TegraGM20BBoCoherent), 0};
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, token, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoImport: {
        const auto &in = *static_cast<const TegraGM20BAbiBoImport *>(input);
        model.bos[model.nextHandle] = {true, true, false, false, 0, in.address, in.bytes};
        model.imports[model.nextHandle] = {in.address, in.bytes};
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, 0, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoFree:
        model.freed.push_back(static_cast<const TegraGM20BAbiBoFree *>(input)->handle);
        model.freeBo(static_cast<const TegraGM20BAbiBoFree *>(input)->handle);
        return KERN_SUCCESS;
    case TegraGM20BMethodVmBind: {
        const auto &op = static_cast<const TegraGM20BAbiVmBind *>(input)->ops[0];
        model.binds[op.va] = {op.handle, op.range};
        if (model.bound)
            model.bound(op);
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCpuAcquire:
        model.cpuLease(static_cast<const TegraGM20BAbiCpuAccess *>(input)->handle);
        return KERN_SUCCESS;
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
        if (in.token == kernel_model::kFenceToken)
            reply = {uint64_t(uintptr_t(model.fences)), sizeof model.fences};
        else
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
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSubmitAddress:
        return model.submit(*static_cast<const TegraGM20BAbiSubmitAddress *>(input),
                            *static_cast<TegraGM20BAbiExecReply *>(output));
    case TegraGM20BMethodWaitFence:
        return model.wait(*static_cast<const TegraGM20BAbiWaitFence *>(input));
    case TegraGM20BMethodGetStatus:
        return model.status(*static_cast<const TegraGM20BAbiChannel *>(input),
                            *static_cast<TegraGM20BAbiStatus *>(output));
    case TegraGM20BMethodCpuComplete: {
        const auto &in = *static_cast<const TegraGM20BAbiCpuComplete *>(input);
        *static_cast<TegraGM20BAbiCpuCompleteReply *>(output) = {in.range_count, 0, 0, 0, 0, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodUnmap:
        return KERN_SUCCESS;
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
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCompile: {
        const size_t at = std::min<size_t>(model.compiles++, model.compileResults.size() - 1);
        auto &reply = *static_cast<TegraGM20BAbiCompileReply *>(output);
        reply = {};
        reply.result = model.compileResults[at];
        return KERN_SUCCESS;
    }
    default:
        return kIOReturnUnsupported;
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
    other->vaEnd = model.vaEnd;
    other->opened = true;
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
    return KERN_SUCCESS;
}
