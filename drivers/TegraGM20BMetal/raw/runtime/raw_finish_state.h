// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>

namespace raw {

// A command buffer's ranges in flight and whether it will submit no more (its last range submitted, or it failed), in one
// word with no lock: of the submitter's ready() and the completions of its ranges, exactly one sees nothing in flight after
// ready, and that one finishes it.
class FinishState {
public:
    // A range handed to a GPU, before its completion can run
    void submitted()
    {
        word_.fetch_add(1, std::memory_order_relaxed);
    }

    // It submits no more: true where nothing it handed over is in flight and this is the first ready(), so the caller
    // finishes it now
    bool ready()
    {
        const uint32_t old = word_.fetch_or(kReady, std::memory_order_acq_rel);
        return !(old & kReady) && !(old & kCount);
    }

    // A range of it completed: true where it was the last in flight after ready(), so the caller finishes it
    bool completed()
    {
        return word_.fetch_sub(1, std::memory_order_acq_rel) == (kReady | 1);
    }

    uint32_t pending() const
    {
        return word_.load(std::memory_order_acquire) & kCount;
    }

private:
    static constexpr uint32_t kReady = 1u << 31, kCount = kReady - 1;
    std::atomic<uint32_t> word_{0};
};

}  // namespace raw
