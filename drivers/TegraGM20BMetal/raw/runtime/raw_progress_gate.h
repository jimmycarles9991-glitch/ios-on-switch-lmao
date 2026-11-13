// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>

namespace raw {

// Whether a completion wakes the submit worker: only while a queue head is parked, so a steady stream of command buffers
// wakes it once for each commit and not again for each completion.
//
// No lost wake: the worker reads the epoch before it judges a head (visit) and, deciding to park it, marks itself parked
// and reads the epoch again (mayPark); a completion makes its progress, counts the epoch and reads the mark (progressed).
// All four are sequentially consistent, so of the worker's mark-then-read and the completion's count-then-read one sees
// the other's write: either the completion sees the mark and wakes the worker, or the worker sees the epoch move and judges
// the head again (and a completion counted before visit is one whose progress the judging already saw).
class ProgressGate {
public:
    // The worker, before it judges a head
    uint64_t visit() const
    {
        return epoch_.load(std::memory_order_seq_cst);
    }

    // The worker, deciding to park the head it judged at `seen`: false where a completion came since (judge it again)
    bool mayPark(uint64_t seen)
    {
        parked_.store(true, std::memory_order_seq_cst);
        return epoch_.load(std::memory_order_seq_cst) == seen;
    }

    // The worker, before it sleeps: with no head parked, completions need not wake it
    void idle(bool anyParked)
    {
        if (!anyParked)
            parked_.store(false, std::memory_order_seq_cst);
    }

    // A completion, after its progress: whether to wake the worker
    bool progressed()
    {
        epoch_.fetch_add(1, std::memory_order_seq_cst);
        return parked_.load(std::memory_order_seq_cst);
    }

private:
    std::atomic<uint64_t> epoch_{0};
    std::atomic<bool> parked_{false};
};

}  // namespace raw
