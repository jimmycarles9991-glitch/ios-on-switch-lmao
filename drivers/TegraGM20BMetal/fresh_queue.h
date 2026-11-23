// SPDX-License-Identifier: MIT
// The new textures whose first contents (fresh.h) the raw channel has not written yet. A texture is added as it is made,
// before any command buffer can name it; the next raw submission takes every one and writes them ahead of its own commands, on
// its own channel, so every later command on that channel (that submission's included) finds them written. A submission that
// fails gives them back for the next. A CPU transfer (replaceRegion, getBytes) extracts its texture and writes it in a raw
// submission of its own first, which the transfer follows.
#pragma once
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace gm20b {

template <class Entry> class FreshQueue {
public:
    void add(Entry entry)
    {
        std::lock_guard<std::mutex> l(lock_);
        pending_.push_back(std::move(entry));
        any_.store(true, std::memory_order_release);
    }

    bool any() const
    {
        return any_.load(std::memory_order_acquire);
    }

    std::vector<Entry> take()
    {
        std::vector<Entry> taken;
        if (!any())
            return taken;
        std::lock_guard<std::mutex> l(lock_);
        taken.swap(pending_);
        any_.store(false, std::memory_order_release);
        return taken;
    }

    // The pending entries `match` holds for, out of the queue: true where there was one
    template <class Match> bool extract(Match match)
    {
        if (!any())
            return false;
        std::lock_guard<std::mutex> l(lock_);
        const auto kept = std::remove_if(pending_.begin(), pending_.end(), match);
        const bool found = kept != pending_.end();
        pending_.erase(kept, pending_.end());
        any_.store(!pending_.empty(), std::memory_order_release);
        return found;
    }

    void settle(std::vector<Entry> &&taken, bool submitted)
    {
        if (submitted || taken.empty())
            return;
        std::lock_guard<std::mutex> l(lock_);
        taken.insert(taken.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
        pending_.swap(taken);
        any_.store(true, std::memory_order_release);
    }

private:
    std::mutex lock_;
    std::vector<Entry> pending_;
    std::atomic<bool> any_{false};
};

}  // namespace gm20b
