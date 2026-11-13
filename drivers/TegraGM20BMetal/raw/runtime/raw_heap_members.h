// SPDX-License-Identifier: MIT
// What a job names of an MTLHeap an encoder used (useHeap): its arena, one BO named whole, and the resources the heap made
// outside it (raw/runtime/RawMetalHeap.mm), each its storage range. The set changes as the heap makes and frees resources,
// never at useHeap: a command buffer names a heap once however often its encoders use it, and its jobs name what the heap
// holds when they are submitted.
#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace raw {

struct HeapMember {
    uint64_t va = 0, bytes = 0;
    bool hostVisible = false;  // the CPU writes it: a job names it host-written
};

class HeapMembers {
public:
    static constexpr uint32_t kNoArena = ~0u;

    explicit HeapMembers(uint32_t arena = kNoArena, bool hostVisible = false) : arena_(arena), hostVisible_(hostVisible)
    {}

    uint32_t arena() const
    {
        return arena_;
    }

    bool hostVisible() const
    {
        return hostVisible_;
    }

    void add(const HeapMember &m)
    {
        if (!m.va || !m.bytes)
            return;
        std::lock_guard<std::mutex> l(lock_);
        members_.push_back(m);
        generation_++;
    }

    void remove(uint64_t va)
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto at = std::find_if(members_.begin(), members_.end(), [&](const HeapMember &m) {
            return m.va == va;
        });
        if (at == members_.end())
            return;
        *at = members_.back();
        members_.pop_back();
        generation_++;
    }

    // `each` over the members made outside the arena, as they are now
    template <typename F> void each(F &&each) const
    {
        std::lock_guard<std::mutex> l(lock_);
        for (const HeapMember &m : members_)
            each(m);
    }

    uint64_t generation() const
    {
        std::lock_guard<std::mutex> l(lock_);
        return generation_;
    }

    size_t outside() const
    {
        std::lock_guard<std::mutex> l(lock_);
        return members_.size();
    }

private:
    const uint32_t arena_;
    const bool hostVisible_;
    mutable std::mutex lock_;
    std::vector<HeapMember> members_;
    uint64_t generation_ = 0;
};

}  // namespace raw
