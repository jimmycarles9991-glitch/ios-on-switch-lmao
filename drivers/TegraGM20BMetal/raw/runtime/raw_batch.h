// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "../../../TegraGM20B/TegraGM20BABI.h"

namespace raw {

// Admission estimates, not elapsed-time guarantees: loops, texture latency and clocks are not represented by static
// instruction counts. Keep small work together; expose idle/preempt opportunities in long sequences without CPU waits.
// A single draw/dispatch remains indivisible, including an MME header and its GPU-owned indirect arguments.
// A client's batches expose preemption boundaries for the compositor's frame. Nothing preempts the render server, so its
// batches only keep the kernel's stuck rule (TegraGM20BRecover.h: 500 ms without a completed seqno) fed, while each
// internal fence drains the pipeline mid-pass and raises a CPU interrupt.
struct BatchWork {
    static constexpr uint64_t kLimit = 64ull * 1024 * 1024;
    static constexpr uint64_t kRenderServerLimit = 16 * kLimit;
    static constexpr uint64_t kSaturated = kRenderServerLimit;  // at least every limit: ends the batch
    static constexpr uint32_t kOperations = 128;
    uint64_t work = 0;
    uint32_t operations = 0;

    static uint64_t product(uint64_t a, uint64_t b)
    {
        return b && a > kSaturated / b ? kSaturated : std::min(kSaturated, a * b);
    }

    bool add(uint64_t estimate, uint64_t limit = kLimit)
    {
        work = std::min(kSaturated, work + std::min(kSaturated, estimate));
        return ++operations >= kOperations || work >= limit;
    }
};

// ABI records remain contiguous. Boundaries are local metadata until a submission carries several batches: then each
// boundary inside it is a TegraGM20BPushFence. Prefix insertion (SLM setup or hoisted split prologues) preserves every
// recorded cut; merging cannot erase one.
class PushBatches {
    std::vector<TegraGM20BAbiPush> pushes_;
    std::vector<size_t> ends_;

public:
    void clear()
    {
        pushes_.clear();
        ends_.clear();
    }

    bool empty() const
    {
        return pushes_.empty();
    }

    size_t size() const
    {
        return pushes_.size();
    }

    const TegraGM20BAbiPush *data() const
    {
        return pushes_.data();
    }

    TegraGM20BAbiPush &operator[](size_t at)
    {
        return pushes_[at];
    }

    const TegraGM20BAbiPush &operator[](size_t at) const
    {
        return pushes_[at];
    }

    TegraGM20BAbiPush &back()
    {
        return pushes_.back();
    }

    void append(TegraGM20BAbiPush push, bool merge = true)
    {
        if (merge && !push.flags && !pushes_.empty() && !pushes_.back().flags &&
            (ends_.empty() || ends_.back() != pushes_.size())) {
            auto &last = pushes_.back();
            if (last.va + uint64_t(last.dwords) * 4 == push.va &&
                uint64_t(last.dwords) + push.dwords < (1u << 21) - 1) {
                last.dwords += push.dwords;
                return;
            }
        }
        pushes_.push_back(push);
    }

    void finishBatch()
    {
        if (!pushes_.empty() && !(pushes_.back().flags & TegraGM20BPushIncomplete) &&
            (ends_.empty() || ends_.back() != pushes_.size()))
            ends_.push_back(pushes_.size());
    }

    // End-of-pass stores, visibility operations and the final timestamp belong to the last work batch when no later
    // draw/dispatch followed its cut. They do not need a separate empty-work Exec and idle barrier.
    void includeTail()
    {
        if (!ends_.empty())
            ends_.pop_back();
    }

    void insert(size_t at, TegraGM20BAbiPush push)
    {
        pushes_.insert(pushes_.begin() + at, push);
        for (size_t &end : ends_)
            if (end >= at)
                ++end;
    }

    size_t next(size_t at, uint32_t maxPushes) const
    {
        if (at >= pushes_.size())
            return 0;
        size_t end = at + std::min<size_t>(maxPushes, pushes_.size() - at);
        const auto cut = std::upper_bound(ends_.begin(), ends_.end(), at);
        if (cut != ends_.end())
            end = std::min(end, *cut);
        while (end > at && (pushes_[end - 1].flags & TegraGM20BPushIncomplete))
            --end;
        return end - at;
    }

    // One submission from `at`: consecutive next() spans while they fit maxPushes, at most maxFences + 1 of them. Each
    // span's end inside it becomes an internal fence (fence()): every span still ends with the channel's fence.
    size_t submission(size_t at, uint32_t maxPushes, uint32_t maxFences) const
    {
        size_t n = next(at, maxPushes);
        for (uint32_t fences = 0; n && fences < maxFences && at + n < pushes_.size(); ++fences) {
            const size_t more = next(at + n, maxPushes);
            if (!more || n + more > maxPushes)
                break;
            n += more;
        }
        return n;
    }

    void fence(size_t at, size_t n, uint32_t maxPushes)
    {
        for (size_t end = at + next(at, maxPushes); end > at && end < at + n;) {
            pushes_[end - 1].flags |= TegraGM20BPushFence;
            const size_t step = next(end, maxPushes);
            if (!step)
                return;
            end += step;
        }
    }

    template <class Submit> bool submit(uint32_t maxPushes, uint32_t maxFences, Submit &&submitOne)
    {
        for (size_t at = 0; at < pushes_.size();) {
            const size_t n = submission(at, maxPushes, maxFences);
            fence(at, n, maxPushes);
            if (!submitOne(at, n))
                return false;
            at += n;
        }
        return true;
    }

    bool valid(uint32_t maxPushes) const
    {
        for (size_t at = 0; at < pushes_.size();) {
            const size_t n = next(at, maxPushes);
            if (!n)
                return false;
            at += n;
        }
        return true;
    }
};

}  // namespace raw
