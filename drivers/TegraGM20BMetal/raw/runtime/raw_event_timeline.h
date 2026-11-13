// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace raw {

struct EventFence {
    uint32_t sequence = 0;
    uint64_t generation = 0;

    explicit operator bool() const
    {
        return generation != 0;
    }
};

enum class EventOutcome : uint8_t {
    pending,
    succeeded,
    failed
};

class EventPromise {
public:
    const EventFence fence;
    const uint64_t value;

    EventPromise(EventFence fence, uint64_t value) : fence(fence), value(value)
    {}

    EventOutcome outcome() const
    {
        return outcome_.load(std::memory_order_acquire);
    }

    bool orderedOn(uint64_t generation) const
    {
        const EventOutcome state = outcome();
        return state == EventOutcome::succeeded || (state == EventOutcome::pending && fence.generation == generation);
    }

    bool settle(bool success)
    {
        auto expected = EventOutcome::pending;
        return outcome_.compare_exchange_strong(expected, success ? EventOutcome::succeeded : EventOutcome::failed,
                                                std::memory_order_release, std::memory_order_relaxed);
    }

private:
    friend class EventTimeline;
    std::vector<uintptr_t> consumers_;
    std::atomic<EventOutcome> outcome_{EventOutcome::pending};
};

// The caller serializes the registry. Promises outlive its entries so consumers retain the exact prefix outcome.
class EventTimeline {
public:
    using Key = uintptr_t;
    using Promise = std::shared_ptr<EventPromise>;

    Promise observe(Key event, uint64_t value, Key queue, uint64_t generation)
    {
        auto &state = events_[event];
        for (auto it = state.signals.lower_bound(value); it != state.signals.end(); ++it) {
            if (it->second->orderedOn(generation)) {
                auto &consumers = it->second->consumers_;
                if (std::find(consumers.begin(), consumers.end(), queue) == consumers.end())
                    consumers.push_back(queue);
                state.waiters.erase(queue);
                return it->second;
            }
        }
        state.waiters[queue] = value;
        return {};
    }

    Promise publish(Key event, uint64_t value, EventFence fence, std::vector<Key> &wake)
    {
        if (!fence)
            return {};
        auto promise = std::make_shared<EventPromise>(fence, value);
        auto &state = events_[event];
        state.signals.emplace(value, promise);
        for (auto it = state.waiters.begin(); it != state.waiters.end();) {
            if (it->second <= value) {
                wake.push_back(it->first);
                it = state.waiters.erase(it);
            } else {
                ++it;
            }
        }
        return promise;
    }

    // A value a submitted command buffer's signal reaches once its work completes, not settled yet
    bool promised(Key event, uint64_t value) const
    {
        const auto it = events_.find(event);
        if (it == events_.end())
            return false;
        for (auto at = it->second.signals.lower_bound(value); at != it->second.signals.end(); ++at)
            if (at->second->outcome() == EventOutcome::pending)
                return true;
        return false;
    }

    void unobserve(Key event, Key queue)
    {
        const auto it = events_.find(event);
        if (it == events_.end())
            return;
        it->second.waiters.erase(queue);
        prune(it);
    }

    bool retire(Key event, const Promise &promise, bool success, std::vector<Key> *wake = nullptr)
    {
        if (!promise || !promise->settle(success))
            return false;
        if (wake)
            wake->insert(wake->end(), promise->consumers_.begin(), promise->consumers_.end());
        promise->consumers_.clear();
        const auto it = events_.find(event);
        if (it == events_.end())
            return true;
        auto [begin, end] = it->second.signals.equal_range(promise->value);
        for (auto at = begin; at != end; ++at) {
            if (at->second == promise) {
                it->second.signals.erase(at);
                break;
            }
        }
        prune(it);
        return true;
    }

    size_t size() const
    {
        return events_.size();
    }

private:
    struct State {
        std::multimap<uint64_t, Promise> signals;
        std::map<Key, uint64_t> waiters;
    };

    std::unordered_map<Key, State> events_;

    void prune(decltype(events_)::iterator it)
    {
        if (it->second.signals.empty() && it->second.waiters.empty())
            events_.erase(it);
    }
};

enum class EventDependencyState {
    ready,
    blocked,
    failed
};

inline EventDependencyState eventDependencies(const std::vector<EventTimeline::Promise> &promises, uint64_t generation,
                                              bool gpuOrdered)
{
    bool blocked = false;
    for (const auto &promise : promises) {
        const auto state = promise->outcome();
        if (state == EventOutcome::failed)
            return EventDependencyState::failed;
        if (state == EventOutcome::pending && (!gpuOrdered || !promise->orderedOn(generation)))
            blocked = true;
    }
    return blocked ? EventDependencyState::blocked : EventDependencyState::ready;
}

}  // namespace raw
