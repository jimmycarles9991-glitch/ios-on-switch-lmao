// SPDX-License-Identifier: MIT
// The raw path's channel across the kernel's kills (raw::Device::reopen): which of the channels made since the device opened
// gave out a seqno (its generation), how a channel the device gave up ended the seqnos it gave out, and the making of the
// next channel with its context.
#pragma once
#include "raw_lock.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>
#include "raw_kernel.h"

namespace raw {

// The seqnos a kill took (TegraGM20BAbiFence), or ones the device counts failed itself
struct KillRange {
    uint32_t first = 0, last = 0, error = 0;

    bool covers(uint32_t seqno) const
    {
        return error && int32_t(seqno - first) >= 0 && int32_t(last - seqno) >= 0;
    }
};

class ChannelGenerations {
public:
    uint64_t current() const
    {
        return current_.load(std::memory_order_acquire);
    }

    void opened()
    {
        current_.fetch_add(1, std::memory_order_release);
    }

    // The current channel is given up, before its slot can name another channel: `failed`, the seqnos of it that failed (its
    // kill record and those the device counted failed itself). The generation it was. One thread retires.
    uint64_t retire(const std::vector<KillRange> &failed)
    {
        const uint64_t generation = current();
        {
            std::lock_guard<raw::Mutex> l(lock_);
            retired_[next_++ % retired_.size()] = {generation, failed};
        }
        current_.fetch_add(1, std::memory_order_release);
        return generation;
    }

    // `generation`'s channel is destroyed: nothing of it runs any more
    void closed(uint64_t generation)
    {
        {
            std::lock_guard<raw::Mutex> l(lock_);
            closed_ = generation;
        }
        changed_.notify_all();
    }

    // A seqno `generation` gave out, that generation retired: whether it completed, else `error` its failure's. Once its
    // channel is destroyed. Every seqno of a retired channel is final by then (the kernel killed it and released its fence
    // past them, or it ran out), so the answer is what it failed as retire() kept it; a generation retired too long ago to
    // be kept reads as failed (error 0).
    bool completed(uint64_t generation, uint32_t seqno, uint32_t *error = nullptr)
    {
        std::unique_lock<raw::Mutex> l(lock_);
        changed_.wait(l, [&] {
            return closed_ >= generation;
        });
        for (const Retired &r : retired_) {
            if (r.generation != generation)
                continue;
            for (const KillRange &k : r.failed)
                if (k.covers(seqno)) {
                    if (error)
                        *error = k.error;
                    return false;
                }
            return true;
        }
        if (error)
            *error = 0;
        return false;
    }

private:
    struct Retired {
        uint64_t generation = 0;
        std::vector<KillRange> failed;
    };

    std::atomic<uint64_t> current_{0};
    raw::Mutex lock_;
    raw::CondVar changed_;
    uint64_t closed_ = 0;
    std::array<Retired, 8> retired_{};
    size_t next_ = 0;
};

// A ring a larger one replaced, freed once nothing reads it: the work of its channel up to `seqno` completed, or that
// channel is gone (a later generation's runs; the dead channel's work never runs again, and its seqnos are not the new
// channel's, whose slot's fence may be behind them)
struct OutgrownRing {
    Bo bo;
    uint32_t seqno = 0;
    uint64_t generation = 0;

    template <class Done> bool unused(uint64_t current, Done done) const
    {
        return generation != current || done(seqno);
    }
};

// The next channel of `engines`, its context submitted by `context(channel, seqno)` before anything else runs on it. False:
// the kernel made none, or refused its context, and no channel is left: `fence` is `none`, which every seqno has passed, so
// nothing waits for `last` (a seqno of the dead channel) on a channel that never runs, whose slot's fence may be behind.
template <class Context>
bool makeChannel(Kernel &k, uint32_t engines, uint32_t &channel, const volatile uint32_t *&fence, uint32_t &last,
                 const volatile uint32_t *none, Context &&context)
{
    channel = 0;
    fence = none;
    if (!k.channel(engines, channel, fence))
        return false;
    uint32_t seqno = 0;
    if (!context(channel, seqno)) {
        k.closeChannel(channel);
        channel = 0;
        fence = none;
        return false;
    }
    last = seqno;
    return true;
}

}  // namespace raw
