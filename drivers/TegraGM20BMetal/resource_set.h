// SPDX-License-Identifier: MIT
// The device's persistent resource set (drivers/TegraGM20B/TegraGM20BJobABI.h, methods 31-33): every BO of the device
// and every page range it imported of its own memory, registered once, which a job names by the set's id and generation
// instead of one use each. The kernel keeps out (and later takes out) what another process, engine or a CPU lease or
// CpuComplete reaches; that BO stays a use of every job that touches it.
//
// Registration (add, promote, a member's free, renew) serializes kernel set updates through a single-owner `syncing_`
// state while dropping `lock_` across ResourceSetCreate and ResourceSetRelease ioctls, so view(), leaving() and
// addLater() never stall behind the kernel. Submitters never take `lock_` while the set is unchanged: each keeps a
// SetView, refreshed when the version moved. The set hears every free of its kernel (Kernel::free, FreeObserver), whoever
// frees the BO: a member leaves the list and every view before its handle dies and its VA is given back, so no view names
// an address another BO may be bound at next and no registration names a BO the kernel no longer holds. The kernel keeps
// the set's pending jobs as its users.
#pragma once
#include "lock.h"
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "kernel.h"
#include "surfaces.h"

namespace gm20b {

struct SetMember {
    uint64_t handle = 0, va = 0, bytes = 0;
    uint32_t access = 0;  // TegraGM20BSurfaceRead, or TegraGM20BSurfaceWrite: the set's jobs may write it
    bool always = false;  // every job of the device uses it (its control memory, rings, shader heap, local memory)
};

// A member a recorded use skipped: named as a use by handle should it leave the set before the job is submitted
struct SetUse {
    uint64_t handle;
    uint32_t access;
};

struct SetView {
    uint32_t set = 0, generation = 0, version = 0;
    uint32_t removals = 0;           // members gone so far (freed, or taken out by the kernel)
    std::vector<SetMember> members;  // by va
    std::vector<SetMember> outside;  // `always` BOs the kernel keeps out of the set: a use of every job
    // Why submitInSet's job was not submitted, when it was the set: its generation had ended and the kernel refused to
    // make it again (the member and the reason). Empty otherwise; each submitInSet sets it.
    std::string refused;

    struct UseCache {
        uint32_t channel = 0, set = 0;
        bool admitted = false, valid = false;
        std::vector<TegraGM20BAbiSurfaceUse> uses;

        void clear()
        {
            valid = false;
        }

        bool matches(uint32_t ch, uint32_t setId, bool isAdmitted, const TegraGM20BAbiSurfaceUse *items,
                     uint32_t count) const;
        void save(uint32_t ch, uint32_t setId, bool isAdmitted, const TegraGM20BAbiSurfaceUse *items, uint32_t count);
    } useCache;

    // The member [va, va + bytes) lies in, else null
    const SetMember *find(uint64_t va, uint64_t bytes) const;

    bool holds(uint64_t va, uint64_t bytes) const
    {
        return find(va, bytes) != nullptr;
    }

    bool holds(const Bo &bo) const
    {
        return bo.handle && holds(bo.vaBase ? bo.vaBase : bo.va, bo.bytes);
    }

    bool named(uint64_t handle) const;
};

class ResourceSet final : public FreeObserver {
public:
    struct Candidate {
        const Bo *bo;
        uint32_t access;
    };

    struct Counts {
        uint32_t refusals = 0;  // registrations the kernel refused for a member (or for the set's id)
        uint32_t rebuilds = 0;  // registrations it took once made again without what it refused
    };

    // The set made again with these BOs among its members (`always`: every job uses them); each the kernel refuses stays out
    // (a use of the jobs that touch it). False when any was.
    bool add(Kernel &k, std::initializer_list<Candidate> candidates, bool always = false);

    bool add(Kernel &k, const Bo &bo, uint32_t access, bool always = false)
    {
        return add(k, {{&bo, access}}, always);
    }

    // As add, made at the encoding thread's next Device::begin (addPending) instead of on the caller: a set made again on
    // another thread mid-frame ends its generation under an open command buffer, and the ResourceSetCreate is what an
    // unpark paid on the caller. Until then each BO is named per job by address, as any BO outside the set is.
    void addLater(std::initializer_list<Candidate> candidates);
    // The adds addLater kept, as one add: the encoding thread's, between command buffers
    void addPending(Kernel &k);

    // A read member a job is about to write becomes a written one (the set made again). Members start as read where the CPU
    // may lease them, so a CPU read lease of a member the GPU never wrote does not wait for the set's jobs.
    void promote(Kernel &k, uint64_t handle);
    void remove(Kernel &k, const std::vector<uint64_t> &handles);

    // Kernel::free of a BO (the kernel this set registers with observes frees through it): a member leaves the list and
    // every view, then, once the BO is gone, the set is made again without it.
    bool leaving(uint64_t handle) override;
    void left(Kernel &k) override;

    enum class Renewal {
        renewed,  // a newer generation is current (made here or by another submitter), without what the kernel took out
        current,  // the job's generation was current: something else of the job was stale (a native view)
        refused,  // the set could not be made again (refusal() says why)
    };

    // A job naming `generation` came back kIOReturnNotReady: the set made again where its mirror word says the
    // generation ended (a member freed, reached from outside or by the CPU, or a channel killed).
    Renewal renew(Kernel &k, uint32_t generation);

    // `v` as the set is now; one load when nothing changed since it was taken
    void view(SetView &v) const;

    // Why the kernel refused the set's last registration; empty once one is taken
    std::string refusal() const;
    // The latest refusal's member and reason, kept after the set is made again; empty while none was refused
    std::string lastRefusal() const;

    Counts counts() const
    {
        return {refusals_.load(std::memory_order_relaxed), rebuilds_.load(std::memory_order_relaxed)};
    }

private:
    class SyncGuard;

    void waitSyncLocked(std::unique_lock<gm20b::Mutex> &l);
    bool syncLocked(std::unique_lock<gm20b::Mutex> &l, Kernel &k, std::vector<SetMember> &trial, const char *when);
    bool refusedLocked(const std::vector<TegraGM20BAbiSurfaceUse> &uses, std::vector<SetMember> &trial,
                       const char *when, bool again);
    void leaveAllLocked(const std::vector<SetMember> &trial);
    static void insertInto(std::vector<SetMember> &list, const Bo &bo, uint32_t access, bool always);
    bool eraseLocked(uint64_t handle);

    bool addSyncLocked(std::unique_lock<gm20b::Mutex> &l, Kernel &k, const Candidate *first, const Candidate *last,
                       bool always, const char *when);
    void dropPendingLocked(uint64_t handle);

    mutable gm20b::Mutex lock_;
    gm20b::CondVar syncDone_;
    bool syncing_ = false;
    uint32_t syncWaiters_ = 0;
    std::vector<uint64_t> leftDuringSync_;
    std::vector<SetMember> members_;                // by va
    std::vector<std::pair<Bo, uint32_t>> pending_;  // addLater's, under lock_
    std::atomic<bool> hasPending_{false};
    std::vector<SetMember> outside_;
    uint32_t set_ = 0, generation_ = 0, removals_ = 0;
    const volatile uint32_t *mirror_ = nullptr;
    std::atomic<uint32_t> version_{0};
    std::string refusal_, lastRefusal_;
    std::atomic<uint32_t> refusals_{0}, rebuilds_{0};
};

// One job of `channel`, naming the set when `named` (the view's generation, made again and the job submitted again while
// the kernel finds it ended). `uses` (null: the job's own), what no member holds: it gains the set's `outside` BOs, and
// each of `recorded` (taken under `removals`) the set no longer holds. False: refused; `result` the kern_return_t (the
// ResourceSetCreate's, with view.refused naming the member, where the set could not be made again).
bool submitInSet(Kernel &k, ResourceSet &set, SetView &view, bool named, KernelJob job, uint32_t &seqno,
                 int *result = nullptr, SurfaceList *uses = nullptr, const std::vector<SetUse> *recorded = nullptr,
                 uint32_t removals = 0);

}  // namespace gm20b
