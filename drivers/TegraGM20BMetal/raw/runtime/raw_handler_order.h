// SPDX-License-Identifier: MIT
// A Metal queue's completed handlers run in the order its command buffers were enqueued, as Metal's per-queue completion
// queue runs them, though the raw path finishes them on different threads: an m2v range's on NVK's completion thread, a raw
// range's on the fence thread, a refused one on the submit thread. With no lock and no wait: a command buffer whose
// predecessor's handlers have not run hands its own to that predecessor, which runs them once its own are done.
#pragma once
#include <atomic>

namespace raw {

class HandlerTurn {
public:
    bool ran() const
    {
        return next_.load(std::memory_order_acquire) == ranMark();
    }

    // `work` (the successor's, owned by this turn from now) runs after this turn's handlers: false when they ran already,
    // and the caller runs it
    bool hand(void *work)
    {
        void *expected = nullptr;
        return next_.compare_exchange_strong(expected, work, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // This turn's handlers ran: the work handed to it meanwhile, for the caller to run (null: none)
    void *finished()
    {
        void *next = next_.exchange(ranMark(), std::memory_order_acq_rel);
        return next == ranMark() ? nullptr : next;
    }

private:
    static void *ranMark()
    {
        static char mark;
        return &mark;
    }

    std::atomic<void *> next_{nullptr};
};

}  // namespace raw
