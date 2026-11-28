// SPDX-License-Identifier: MIT
#include "lock.h"
#include "resource_set.h"
#include "encode_profile.h"
#include "TegraGM20BJobABI.h"

#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>

namespace gm20b {
namespace {
os_log_t setLog()
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b");
    return log;
}

const char *refusedWhy(uint32_t reason)
{
    switch (reason) {
    case TegraGM20BSetRefusedReserved:
        return "its reserved word is not zero";
    case TegraGM20BSetRefusedAccess:
        return "its access is neither read nor write";
    case TegraGM20BSetRefusedGone:
        return "the connection holds no such BO";
    default:
        return "no reason given";
    }
}

bool byHandle(const TegraGM20BAbiSurfaceUse &a, const TegraGM20BAbiSurfaceUse &b)
{
    return a.handle < b.handle;
}

// One record per BO, by handle, with the accesses of every member of it OR'd: an arena's view aliases are members of its
// handle at other VAs
void registration(const std::vector<SetMember> &members, std::vector<TegraGM20BAbiSurfaceUse> &uses)
{
    uses.clear();
    for (const SetMember &m : members)
        uses.push_back({m.handle, m.access, 0});
    std::sort(uses.begin(), uses.end(), byHandle);
    size_t unique = 0;
    for (size_t i = 0; i < uses.size(); ++i) {
        if (unique && uses[unique - 1].handle == uses[i].handle)
            uses[unique - 1].access |= uses[i].access;
        else
            uses[unique++] = uses[i];
    }
    uses.resize(unique);
}

// What the kernel answered for `handle`'s record in a registration: 0 when it kept the BO out
uint32_t registered(const std::vector<TegraGM20BAbiSurfaceUse> &uses, uint64_t handle)
{
    const auto at = std::lower_bound(uses.begin(), uses.end(), TegraGM20BAbiSurfaceUse{handle, 0, 0}, byHandle);
    return at != uses.end() && at->handle == handle ? at->access : 0;
}
}  // namespace

const SetMember *SetView::find(uint64_t va, uint64_t bytes) const
{
    const auto after = std::upper_bound(members.begin(), members.end(), va, [](uint64_t at, const SetMember &m) {
        return at < m.va;
    });
    if (after == members.begin())
        return nullptr;
    const SetMember &m = *std::prev(after);
    return va - m.va < m.bytes && bytes <= m.bytes - (va - m.va) ? &m : nullptr;
}

bool SetView::named(uint64_t handle) const
{
    for (const SetMember &m : members)
        if (m.handle == handle)
            return true;
    return false;
}

bool SetView::UseCache::matches(uint32_t ch, uint32_t setId, bool isAdmitted, const TegraGM20BAbiSurfaceUse *items,
                                uint32_t count) const
{
    if (!valid || channel != ch || set != setId || admitted != isAdmitted || uses.size() != count)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (uses[i].handle != items[i].handle || uses[i].access != items[i].access ||
            uses[i].reserved != items[i].reserved)
            return false;
    return true;
}

void SetView::UseCache::save(uint32_t ch, uint32_t setId, bool isAdmitted, const TegraGM20BAbiSurfaceUse *items,
                             uint32_t count)
{
    channel = ch;
    set = setId;
    admitted = isAdmitted;
    uses.assign(items, items + count);
    valid = true;
}

class ResourceSet::SyncGuard {
public:
    explicit SyncGuard(ResourceSet &set) : set_(set)
    {
        set_.syncing_ = true;
    }

    ~SyncGuard()
    {
        set_.syncing_ = false;
        set_.leftDuringSync_.clear();
        if (set_.syncWaiters_)
            set_.syncDone_.notify_all();
    }

    SyncGuard(const SyncGuard &) = delete;
    SyncGuard &operator=(const SyncGuard &) = delete;

private:
    ResourceSet &set_;
};

void ResourceSet::waitSyncLocked(std::unique_lock<gm20b::Mutex> &l)
{
    if (!syncing_)
        return;
    ++syncWaiters_;
    syncDone_.wait(l, [&] {
        return !syncing_;
    });
    --syncWaiters_;
}

void ResourceSet::insertInto(std::vector<SetMember> &list, const Bo &bo, uint32_t access, bool always)
{
    const uint64_t va = bo.vaBase ? bo.vaBase : bo.va;
    for (SetMember &m : list)
        if (m.handle == bo.handle && m.va == va) {
            m.access |= access;
            return;
        }
    const SetMember made = {bo.handle, va, bo.bytes, access, always};
    list.insert(std::upper_bound(list.begin(), list.end(), made,
                                 [](const SetMember &a, const SetMember &b) {
                                     return a.va < b.va;
                                 }),
                made);
}

bool ResourceSet::eraseLocked(uint64_t handle)
{
    const auto gone = [&](const SetMember &m) {
        return m.handle == handle;
    };
    const size_t before = members_.size() + outside_.size();
    members_.erase(std::remove_if(members_.begin(), members_.end(), gone), members_.end());
    outside_.erase(std::remove_if(outside_.begin(), outside_.end(), gone), outside_.end());
    return members_.size() + outside_.size() != before;
}

// A refused registration: refusal_ and one error line say which member the kernel named and why, and that member leaves
// the list with every record of its BO (a use of the jobs that touch it from now on). True: it named one.
bool ResourceSet::refusedLocked(const std::vector<TegraGM20BAbiSurfaceUse> &uses, std::vector<SetMember> &trial,
                                const char *when, bool again)
{
    refusals_.fetch_add(1, std::memory_order_relaxed);
    const auto named = std::find_if(uses.begin(), uses.end(), [](const TegraGM20BAbiSurfaceUse &u) {
        return u.reserved != 0;
    });
    char line[256];
    if (named == uses.end())
        snprintf(line, sizeof line, "the kernel refused resource set %u (%zu members) %s and named no member", set_,
                 uses.size(), when);
    else
        snprintf(line, sizeof line, "the kernel refused resource set %u's member %zu of %zu %s (BO %llu: %s)", set_,
                 size_t(named - uses.begin()), uses.size(), when, (unsigned long long)named->handle,
                 refusedWhy(named->reserved));
    refusal_ = lastRefusal_ = line;
    os_log_error(setLog(), "SwitchMetal: %{public}s; %{public}s", line,
                 again                 ? "the set was not made"
                 : named == uses.end() ? "made anew"
                                       : "made again without it");
    if (named == uses.end())
        return false;
    const uint64_t handle = named->handle;
    const auto match = [&](const SetMember &m) {
        return m.handle == handle;
    };
    trial.erase(std::remove_if(trial.begin(), trial.end(), match), trial.end());
    const size_t before = members_.size();
    members_.erase(std::remove_if(members_.begin(), members_.end(), match), members_.end());
    if (members_.size() != before) {
        removals_ += uint32_t(before - members_.size());
        version_.fetch_add(1, std::memory_order_release);
    }
    return true;
}

// Every member out, as uses of the jobs that touch them (always ones of every job): a set the kernel holds under no id
// names nothing.
void ResourceSet::leaveAllLocked(const std::vector<SetMember> &trial)
{
    for (const SetMember &m : trial) {
        if (!m.always)
            continue;
        if (std::find(leftDuringSync_.begin(), leftDuringSync_.end(), m.handle) != leftDuringSync_.end())
            continue;
        const bool already = std::any_of(outside_.begin(), outside_.end(), [&](const SetMember &o) {
            return o.handle == m.handle && o.va == m.va;
        });
        if (!already)
            outside_.push_back(m);
    }
    removals_ += uint32_t(members_.size());
    members_.clear();
    version_.fetch_add(1, std::memory_order_release);
}

// lock_ held and syncing_ owned: makes the kernel set with `trial` while dropping lock_ across ResourceSetCreate and
// ResourceSetRelease. Uncommitted candidates stay out of members_ until the kernel accepts them so concurrent view()
// never omits an unregistered BO from job uses.
bool ResourceSet::syncLocked(std::unique_lock<gm20b::Mutex> &l, Kernel &k, std::vector<SetMember> &trial,
                             const char *when)
{
    const auto pruneLeft = [&](std::vector<SetMember> &list) {
        if (leftDuringSync_.empty())
            return;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const SetMember &m) {
                                      return std::find(leftDuringSync_.begin(), leftDuringSync_.end(), m.handle) !=
                                             leftDuringSync_.end();
                                  }),
                   list.end());
    };
    pruneLeft(trial);
    if (trial.empty())
        return true;
    std::vector<TegraGM20BAbiSurfaceUse> uses;
    uses.reserve(trial.size());
    bool again = false;
    for (;;) {
        registration(trial, uses);
        uint32_t set = set_, generation = 0;
        const volatile uint32_t *mirror = nullptr;
        leftDuringSync_.clear();
        l.unlock();
        const int kr = k.resourceSet(uses, set, generation, mirror);
        l.lock();
        if (kr == KERN_SUCCESS) {
            std::vector<SetMember> kept;
            kept.reserve(trial.size());
            uint32_t droppedExisting = 0;
            for (const SetMember &m : trial) {
                if (std::find(leftDuringSync_.begin(), leftDuringSync_.end(), m.handle) != leftDuringSync_.end())
                    continue;
                if (registered(uses, m.handle)) {
                    kept.push_back(m);
                    continue;
                }
                const bool existed = std::any_of(members_.begin(), members_.end(), [&](const SetMember &e) {
                    return e.handle == m.handle && e.va == m.va;
                });
                if (existed)
                    ++droppedExisting;
                if (m.always) {
                    os_log_error(setLog(),
                                 "SwitchMetal: the kernel keeps BO %llu (%llu KiB at 0x%llx), which every job uses, "
                                 "out of resource set %u: a use of every job from now on",
                                 (unsigned long long)m.handle, (unsigned long long)(m.bytes >> 10),
                                 (unsigned long long)m.va, set);
                    outside_.push_back(m);
                }
            }
            removals_ += droppedExisting;
            members_.swap(kept);
            set_ = set;
            generation_ = generation;
            mirror_ = mirror;
            version_.fetch_add(1, std::memory_order_release);
            if (again)
                rebuilds_.fetch_add(1, std::memory_order_relaxed);
            refusal_.clear();
            return true;
        }
        if (kr != kIOReturnBadArgument) {
            refusal_ = "the resource set was not made " + std::string(when) + " (" + k.error() + ")";
            os_log_error(setLog(), "SwitchMetal: %{public}s", refusal_.c_str());
            break;
        }
        const auto named = std::find_if(uses.begin(), uses.end(), [](const TegraGM20BAbiSurfaceUse &u) {
            return u.reserved != 0;
        });
        if (named != uses.end() &&
            std::find(leftDuringSync_.begin(), leftDuringSync_.end(), named->handle) != leftDuringSync_.end()) {
            pruneLeft(trial);
            if (trial.empty())
                return true;
            continue;
        }
        const bool wasNamed = refusedLocked(uses, trial, when, again);
        if (again || (!wasNamed && !set_))
            break;
        if (!wasNamed) {
            const uint32_t oldSet = set_;
            set_ = 0;
            l.unlock();
            k.releaseSet(oldSet);
            l.lock();
        }
        pruneLeft(trial);
        if (trial.empty())
            return true;
        again = true;
    }
    if (!set_)
        leaveAllLocked(trial);
    return false;
}

bool ResourceSet::add(Kernel &k, std::initializer_list<Candidate> candidates, bool always)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitSyncLocked(l);
    SyncGuard guard(*this);
    return addSyncLocked(l, k, candidates.begin(), candidates.end(), always, "for a new member");
}

void ResourceSet::addLater(std::initializer_list<Candidate> candidates)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    for (const Candidate &c : candidates)
        pending_.push_back({*c.bo, c.access});
    hasPending_.store(true, std::memory_order_release);
}

void ResourceSet::addPending(Kernel &k)
{
    if (!hasPending_.load(std::memory_order_acquire))
        return;
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitSyncLocked(l);
    std::vector<std::pair<Bo, uint32_t>> batch;
    batch.swap(pending_);
    hasPending_.store(false, std::memory_order_relaxed);
    if (batch.empty())
        return;
    std::vector<Candidate> candidates;
    candidates.reserve(batch.size());
    for (const auto &[bo, access] : batch)
        candidates.push_back({&bo, access});
    SyncGuard guard(*this);
    addSyncLocked(l, k, candidates.data(), candidates.data() + candidates.size(), false, "for an unparked member");
}

// A BO that leaves before the next begin is not added then
void ResourceSet::dropPendingLocked(uint64_t handle)
{
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [handle](const std::pair<Bo, uint32_t> &p) {
                                      return p.first.handle == handle;
                                  }),
                   pending_.end());
}

bool ResourceSet::addSyncLocked(std::unique_lock<gm20b::Mutex> &l, Kernel &k, const Candidate *first,
                                const Candidate *last, bool always, const char *when)
{
    std::vector<uint64_t> joining;
    for (const Candidate *c = first; c != last; ++c)
        if (std::none_of(members_.begin(), members_.end(), [&](const SetMember &m) {
                return m.handle == c->bo->handle;
            }))
            joining.push_back(c->bo->handle);
    std::vector<SetMember> trial = members_;
    for (const Candidate *c = first; c != last; ++c)
        insertInto(trial, *c->bo, c->access, always);
    if (!syncLocked(l, k, trial, when)) {
        if (set_) {
            // A candidate of a BO that was a member already (an arena's view alias) stays: the kernel's set still
            // holds that BO, at every VA, until a free or a refusal takes it out.
            bool keptAlias = false;
            for (const Candidate *c = first; c != last; ++c) {
                if (std::find(joining.begin(), joining.end(), c->bo->handle) != joining.end())
                    continue;
                if (std::none_of(members_.begin(), members_.end(), [&](const SetMember &m) {
                        return m.handle == c->bo->handle;
                    }))
                    continue;
                insertInto(members_, *c->bo, c->access, always);
                keptAlias = true;
            }
            if (keptAlias)
                version_.fetch_add(1, std::memory_order_release);
        }
        return false;
    }
    for (const Candidate *c = first; c != last; ++c)
        if (std::none_of(members_.begin(), members_.end(), [&](const SetMember &m) {
                return m.handle == c->bo->handle;
            }))
            return false;
    return true;
}

void ResourceSet::promote(Kernel &k, uint64_t handle)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitSyncLocked(l);
    bool changed = false;
    std::vector<SetMember> trial = members_;
    for (SetMember &m : trial)
        if (m.handle == handle && !(m.access & TegraGM20BSurfaceWrite)) {
            m.access |= TegraGM20BSurfaceWrite;
            changed = true;
        }
    if (!changed)
        return;
    SyncGuard guard(*this);
    syncLocked(l, k, trial, "for a written member");
}

void ResourceSet::remove(Kernel &k, const std::vector<uint64_t> &handles)
{
    if (handles.empty())
        return;
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitSyncLocked(l);
    bool changed = false;
    for (uint64_t handle : handles) {
        dropPendingLocked(handle);
        if (eraseLocked(handle)) {
            ++removals_;
            changed = true;
        }
    }
    if (!changed)
        return;
    version_.fetch_add(1, std::memory_order_release);
    SyncGuard guard(*this);
    if (members_.empty()) {
        if (const uint32_t oldSet = set_) {
            set_ = 0;
            generation_ = 0;
            mirror_ = nullptr;
            l.unlock();
            k.releaseSet(oldSet);
            l.lock();
        }
        return;
    }
    std::vector<SetMember> trial = members_;
    syncLocked(l, k, trial, "before parking");
}

bool ResourceSet::leaving(uint64_t handle)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (syncing_)
        leftDuringSync_.push_back(handle);
    dropPendingLocked(handle);
    if (!eraseLocked(handle))
        return false;
    ++removals_;
    version_.fetch_add(1, std::memory_order_release);
    return true;
}

void ResourceSet::left(Kernel &k)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitSyncLocked(l);
    if (members_.empty())
        return;
    SyncGuard guard(*this);
    std::vector<SetMember> trial = members_;
    syncLocked(l, k, trial, "after a member's free");
}

ResourceSet::Renewal ResourceSet::renew(Kernel &k, uint32_t generation)
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    waitSyncLocked(l);
    if (generation != generation_)
        return Renewal::renewed;
    if (!mirror_ || __atomic_load_n(mirror_, __ATOMIC_ACQUIRE) == generation_)
        return Renewal::current;
    SyncGuard guard(*this);
    std::vector<SetMember> trial = members_;
    return syncLocked(l, k, trial, "for an ended generation") ? Renewal::renewed : Renewal::refused;
}

void ResourceSet::view(SetView &v) const
{
    if (v.version == version_.load(std::memory_order_acquire))
        return;
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    v.set = members_.empty() ? 0 : set_;
    v.generation = generation_;
    v.removals = removals_;
    v.members = members_;
    v.outside = outside_;
    v.version = version_.load(std::memory_order_relaxed);
}

std::string ResourceSet::refusal() const
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    return refusal_;
}

std::string ResourceSet::lastRefusal() const
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    return lastRefusal_;
}

bool submitInSet(Kernel &k, ResourceSet &set, SetView &view, bool named, KernelJob job, uint32_t &seqno, int *result,
                 SurfaceList *uses, const std::vector<SetUse> *recorded, uint32_t removals)
{
    set.view(view);
    view.refused.clear();
    const bool isCancel = (job.admissionFlags & TegraGM20BJobAdmissionCancel) != 0;
    const bool admitted = (job.admissionFlags & (TegraGM20BJobAdmissionBegin | TegraGM20BJobAdmissionContinue)) != 0;
    bool retriedSameUses = false;
    for (;;) {
        if (uses) {
            for (const SetMember &m : view.outside)
                uses->addAccess(m.handle, m.access);
            if (recorded && view.removals != removals)
                for (const SetUse &u : *recorded)
                    if (!view.named(u.handle))
                        uses->addAccess(u.handle, u.access);
            if (!uses->valid()) {
                if (result)
                    *result = kIOReturnBadArgument;
                return false;
            }
            job.uses = uses->uses().data();
            job.useCount = uint32_t(uses->uses().size());
        }
        job.set = named ? view.set : 0;
        job.generation = job.set ? view.generation : 0;
        const bool canSameUses = !isCancel && !retriedSameUses && k.supportsSameUses() && job.useCount > 0 &&
                                 job.uses != nullptr &&
                                 view.useCache.matches(job.channel, job.set, admitted, job.uses, job.useCount);
        KernelJob attempt = job;
        if (canSameUses) {
            attempt.sameUses = true;
            attempt.uses = nullptr;
            attempt.useCount = 0;
        }
        int kr = KERN_SUCCESS;
        const bool submitted = k.submitJob(attempt, seqno, &kr);
        if (result)
            *result = kr;
        if (submitted) {
            if (!isCancel && !canSameUses) {
                if (k.supportsSameUses() && job.useCount > 0 && job.uses != nullptr)
                    view.useCache.save(job.channel, job.set, admitted, job.uses, job.useCount);
                else
                    view.useCache.clear();
            }
            return true;
        }
        if (canSameUses && kr == kIOReturnNotReady) {
            view.useCache.clear();
            retriedSameUses = true;
            if (job.set) {
                switch (set.renew(k, job.generation)) {
                case ResourceSet::Renewal::renewed:
                    set.view(view);
                    break;
                case ResourceSet::Renewal::current:
                    break;
                case ResourceSet::Renewal::refused:
                    view.refused = set.refusal();
                    if (result)
                        *result = kIOReturnBadArgument;
                    return false;
                }
            }
            continue;
        }
        if (!isCancel)
            view.useCache.clear();
        if (kr != kIOReturnNotReady || !job.set)
            return false;
        switch (set.renew(k, job.generation)) {
        case ResourceSet::Renewal::renewed:
            break;
        case ResourceSet::Renewal::current:
            return false;
        case ResourceSet::Renewal::refused:
            view.refused = set.refusal();
            if (result)
                *result = kIOReturnBadArgument;
            return false;
        }
        set.view(view);
    }
}

}  // namespace gm20b
