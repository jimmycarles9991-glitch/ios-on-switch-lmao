// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#include "raw_resource_set.h"
#include "raw_encode_profile.h"
#include "TegraGM20BJobABI.h"

#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>

namespace raw {
namespace {
os_log_t setLog()
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
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

void ResourceSet::insertLocked(const Bo &bo, uint32_t access, bool always)
{
    const uint64_t va = bo.vaBase ? bo.vaBase : bo.va;
    for (SetMember &m : members_)
        if (m.handle == bo.handle && m.va == va) {
            m.access |= access;
            return;
        }
    const SetMember made = {bo.handle, va, bo.bytes, access, always};
    members_.insert(std::upper_bound(members_.begin(), members_.end(), made,
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

// One ResourceSetCreate of every member's BO, once. What the kernel refused comes back with access 0, and every member of
// that BO leaves members_ for good (always ones for outside_).
bool ResourceSet::attempt(Kernel &k, std::vector<TegraGM20BAbiSurfaceUse> &uses, int &kr)
{
    registration(members_, uses);
    uint32_t set = set_, generation = 0;
    const volatile uint32_t *mirror = nullptr;
    kr = k.resourceSet(uses, set, generation, mirror);
    if (kr != KERN_SUCCESS)
        return false;
    size_t kept = 0;
    for (size_t i = 0; i < members_.size(); ++i) {
        if (registered(uses, members_[i].handle))
            members_[kept++] = members_[i];
        else if (members_[i].always)
            outside_.push_back(members_[i]);
    }
    removals_ += uint32_t(members_.size() - kept);
    members_.resize(kept);
    set_ = set;
    generation_ = generation;
    mirror_ = mirror;
    version_.fetch_add(1, std::memory_order_release);
    return true;
}

// A refused registration: refusal_ and one error line say which member the kernel named and why, and that member leaves
// the list with every record of its BO (a use of the jobs that touch it from now on). True: it named one.
bool ResourceSet::refusedLocked(const std::vector<TegraGM20BAbiSurfaceUse> &uses, const char *when, bool again)
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
    os_log_error(setLog(), "raw: %{public}s; %{public}s", line,
                 again                 ? "the set was not made"
                 : named == uses.end() ? "made anew"
                                       : "made again without it");
    if (named == uses.end())
        return false;
    const uint64_t handle = named->handle;
    const size_t before = members_.size();
    members_.erase(std::remove_if(members_.begin(), members_.end(),
                                  [&](const SetMember &m) {
                                      return m.handle == handle;
                                  }),
                   members_.end());
    removals_ += uint32_t(before - members_.size());
    version_.fetch_add(1, std::memory_order_release);
    return true;
}

// Every member out, as uses of the jobs that touch them (always ones of every job): a set the kernel holds under no id
// names nothing.
void ResourceSet::leaveAllLocked()
{
    for (const SetMember &m : members_)
        if (m.always)
            outside_.push_back(m);
    removals_ += uint32_t(members_.size());
    members_.clear();
    version_.fetch_add(1, std::memory_order_release);
}

// lock_ held: the set made again with every member. One the kernel refuses for a member it names is made once more
// without it; one it refuses naming none (the set's id) is made once more as a new set. A second refusal takes its
// member out too and gives up, so no list the kernel refused is sent again. `when`: what made it again, for the error
// line.
bool ResourceSet::registerLocked(Kernel &k, const char *when)
{
    if (members_.empty())
        return true;
    std::vector<TegraGM20BAbiSurfaceUse> uses;
    uses.reserve(members_.size());
    int kr = KERN_SUCCESS;
    for (const bool again : {false, true}) {
        if (attempt(k, uses, kr)) {
            if (again)
                rebuilds_.fetch_add(1, std::memory_order_relaxed);
            refusal_.clear();
            return true;
        }
        if (kr != kIOReturnBadArgument) {
            refusal_ = "the resource set was not made " + std::string(when) + " (" + k.error() + ")";
            os_log_error(setLog(), "raw: %{public}s", refusal_.c_str());
            break;
        }
        const bool named = refusedLocked(uses, when, again);
        if (again || (!named && !set_))
            break;
        if (!named) {
            k.releaseSet(set_);
            set_ = 0;
        }
        if (members_.empty())
            return true;
    }
    if (!set_)
        leaveAllLocked();
    return false;
}

bool ResourceSet::add(Kernel &k, std::initializer_list<Candidate> candidates, bool always)
{
    std::lock_guard<raw::Mutex> l(lock_);
    std::vector<uint64_t> joining;
    for (const Candidate &c : candidates)
        if (std::none_of(members_.begin(), members_.end(), [&](const SetMember &m) {
                return m.handle == c.bo->handle;
            }))
            joining.push_back(c.bo->handle);
    for (const Candidate &c : candidates)
        insertLocked(*c.bo, c.access, always);
    if (!registerLocked(k, "for a new member")) {
        // Only the BOs this add brought take their members out. A candidate of a BO that was a member already (an arena's
        // view alias) stays: the kernel's set still holds that BO, at every VA, until a free or a refusal takes it out.
        members_.erase(std::remove_if(members_.begin(), members_.end(),
                                      [&](const SetMember &m) {
                                          return std::find(joining.begin(), joining.end(), m.handle) != joining.end();
                                      }),
                       members_.end());
        version_.fetch_add(1, std::memory_order_release);
        return false;
    }
    for (const Candidate &c : candidates)
        if (std::none_of(members_.begin(), members_.end(), [&](const SetMember &m) {
                return m.handle == c.bo->handle;
            }))
            return false;
    return true;
}

void ResourceSet::promote(Kernel &k, uint64_t handle)
{
    std::lock_guard<raw::Mutex> l(lock_);
    bool changed = false;
    for (SetMember &m : members_)
        if (m.handle == handle && !(m.access & TegraGM20BSurfaceWrite)) {
            m.access |= TegraGM20BSurfaceWrite;
            changed = true;
        }
    if (changed)
        registerLocked(k, "for a written member");
}

bool ResourceSet::leaving(uint64_t handle)
{
    std::lock_guard<raw::Mutex> l(lock_);
    if (!eraseLocked(handle))
        return false;
    ++removals_;
    version_.fetch_add(1, std::memory_order_release);
    return true;
}

void ResourceSet::left(Kernel &k)
{
    std::lock_guard<raw::Mutex> l(lock_);
    registerLocked(k, "after a member's free");
}

ResourceSet::Renewal ResourceSet::renew(Kernel &k, uint32_t generation)
{
    std::lock_guard<raw::Mutex> l(lock_);
    if (generation != generation_)
        return Renewal::renewed;
    if (!mirror_ || *mirror_ == generation_)
        return Renewal::current;
    return registerLocked(k, "for an ended generation") ? Renewal::renewed : Renewal::refused;
}

void ResourceSet::view(SetView &v) const
{
    if (v.version == version_.load(std::memory_order_acquire))
        return;
    RAW_ENCODE_LOCK(l, lock_);
    v.set = members_.empty() ? 0 : set_;
    v.generation = generation_;
    v.removals = removals_;
    v.members = members_;
    v.outside = outside_;
    v.version = version_.load(std::memory_order_relaxed);
}

std::string ResourceSet::refusal() const
{
    std::lock_guard<raw::Mutex> l(lock_);
    return refusal_;
}

std::string ResourceSet::lastRefusal() const
{
    std::lock_guard<raw::Mutex> l(lock_);
    return lastRefusal_;
}

bool submitInSet(Kernel &k, ResourceSet &set, SetView &view, bool named, KernelJob job, uint32_t &seqno, int *result,
                 SurfaceList *uses, const std::vector<SetUse> *recorded, uint32_t removals)
{
    set.view(view);
    view.refused.clear();
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
        int kr = KERN_SUCCESS;
        const bool submitted = k.submitJob(job, seqno, &kr);
        if (result)
            *result = kr;
        if (submitted)
            return true;
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

}  // namespace raw
