// SPDX-License-Identifier: MIT
#pragma once
#include <dispatch/dispatch.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "raw_lock.h"

namespace raw {

// The submit worker's submissions and any thread's moves (a compressed texture made plain) exclude each other, with no
// lock on the worker while no move waits. The worker marks a submission and takes the lock only where a move waits; a
// move takes the lock, counts itself waiting and sleeps until the submission it found marked ends. Each stores before it
// loads the other's word (seq_cst), so at least one sees the other.
class SubmitGate {
public:
    SubmitGate() : ended_(dispatch_semaphore_create(0))
    {}

    ~SubmitGate()
    {
#if !__has_feature(objc_arc)
        dispatch_release(ended_);
#endif
    }

    SubmitGate(const SubmitGate &) = delete;
    SubmitGate &operator=(const SubmitGate &) = delete;

    // The submit worker's
    template <typename Submit> auto submit(Submit &&run) -> decltype(run())
    {
        submitting_.store(true);
        if (!movesWaiting_.load()) {
            const Ending ending(*this);
            return run();
        }
        submitting_.store(false);
        dispatch_semaphore_signal(ended_);
        std::lock_guard<RecursiveMutex> l(lock_);
        return run();
    }

    // Any thread's but the worker's inside its own submission (that one moves at once)
    template <typename Move> auto exclude(Move &&move) -> decltype(move())
    {
        std::lock_guard<RecursiveMutex> l(lock_);
        movesWaiting_.fetch_add(1);
        while (submitting_.load())
            dispatch_semaphore_wait(ended_, DISPATCH_TIME_FOREVER);
        const Waited waited(*this);
        return move();
    }

private:
    struct Ending {
        explicit Ending(SubmitGate &gate) : gate(gate)
        {}

        ~Ending()
        {
            gate.submitting_.store(false);
            if (gate.movesWaiting_.load())
                dispatch_semaphore_signal(gate.ended_);
        }

        SubmitGate &gate;
    };

    struct Waited {
        explicit Waited(SubmitGate &gate) : gate(gate)
        {}

        ~Waited()
        {
            gate.movesWaiting_.fetch_sub(1);
        }

        SubmitGate &gate;
    };

    RecursiveMutex lock_;
    std::atomic<bool> submitting_{false};
    std::atomic<uint32_t> movesWaiting_{0};
    dispatch_semaphore_t ended_;
};

}  // namespace raw
