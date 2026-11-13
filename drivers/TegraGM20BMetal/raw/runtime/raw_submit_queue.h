// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace raw {
template <class Key, class Value> class SubmitQueue {
public:
    using Time = std::chrono::steady_clock::time_point;

    struct Reservation {
        uint64_t generation = 0, position = 0;
    };

    struct Ticket {
        Key key{};
        Value value{};
        uint64_t revision = 0, head = 0;
        std::array<uint64_t, 2> dependencies{};

        explicit operator bool() const
        {
            return revision != 0;
        }
    };

private:
    struct Queue;

    struct Link {
        Queue *before = nullptr, *after = nullptr;
        bool linked = false;
    };

    struct Chain {
        Queue *first = nullptr, *last = nullptr;
    };

    using Deadlines = std::multimap<Time, Queue *>;

    struct Entry {
        Value value;
        bool committed = false;
        bool held = false;
    };

    struct Queue {
        Key key{};
        std::deque<Entry> entries;
        std::array<Link, 4> links{};
        typename Deadlines::iterator deadline;
        uint64_t revision = 0, head = 0;
        uint64_t reservationGeneration = 0, reserved = 0, retired = 0;
        bool visiting = false, timed = false, parked = false;
        bool held = false;  // its head waits for what Metal's GPU, not its scheduler, waits for (hold)
    };

    std::list<Queue> storage_;
    std::unordered_map<Key, Queue *> lookup_;
    std::array<Chain, 4> chains_{};
    Deadlines deadlines_;
    std::array<uint64_t, 2> dependencies_{};
    uint64_t revision_ = 0;
    uint64_t reservationGeneration_ = 0;
    size_t size_ = 0, parked_ = 0;

    void unlink(Queue &q, unsigned chain)
    {
        auto &link = q.links[chain];
        if (!link.linked)
            return;
        if (link.before)
            link.before->links[chain].after = link.after;
        else
            chains_[chain].first = link.after;
        if (link.after)
            link.after->links[chain].before = link.before;
        else
            chains_[chain].last = link.before;
        link = {};
    }

    void append(Queue &q, unsigned chain)
    {
        if (q.links[chain].linked)
            return;
        auto &list = chains_[chain];
        q.links[chain] = {list.last, nullptr, true};
        if (list.last)
            list.last->links[chain].after = &q;
        else
            list.first = &q;
        list.last = &q;
    }

    void clearWait(Queue &q)
    {
        unlink(q, 1);
        unlink(q, 2);
        if (q.timed) {
            deadlines_.erase(q.deadline);
            q.timed = false;
        }
    }

    void activate(Queue &q)
    {
        if (q.parked) {
            q.parked = false;
            --parked_;
        }
        clearWait(q);
        if (!q.visiting && !q.entries.empty() && q.entries.front().committed)
            append(q, 0);
    }

    Queue *find(Key key)
    {
        auto at = lookup_.find(key);
        return at == lookup_.end() ? nullptr : at->second;
    }

    Queue *visiting(const Ticket &ticket)
    {
        auto *q = find(ticket.key);
        return q && q->visiting && !q->entries.empty() && q->head == ticket.head &&
                       q->entries.front().value == ticket.value
                   ? q
                   : nullptr;
    }

public:
    SubmitQueue() = default;
    SubmitQueue(const SubmitQueue &) = delete;
    SubmitQueue &operator=(const SubmitQueue &) = delete;
    SubmitQueue(SubmitQueue &&) = delete;
    SubmitQueue &operator=(SubmitQueue &&) = delete;

    Reservation reserve(Key key, Value value)
    {
        auto *q = find(key);
        if (!q) {
            q = chains_[3].first;
            if (q) {
                lookup_.erase(q->key);
                unlink(*q, 3);
            } else {
                storage_.emplace_back();
                q = &storage_.back();
            }
            q->key = key;
            q->held = false;
            q->reservationGeneration = ++reservationGeneration_;
            q->reserved = q->retired = 0;
            lookup_.emplace(key, q);
        }
        unlink(*q, 3);
        q->entries.push_back({value});
        ++size_;
        if (q->entries.size() == 1)
            q->head = q->revision = ++revision_;
        return {q->reservationGeneration, ++q->reserved};
    }

    // `held`: the queue is held (hold), so this job counts as scheduled at once
    bool commit(Key key, Value value, Reservation reservation, bool *held = nullptr)
    {
        auto *q = find(key);
        if (!q || reservation.generation != q->reservationGeneration || reservation.position <= q->retired ||
            reservation.position > q->reserved)
            return false;
        auto &entry = q->entries[reservation.position - q->retired - 1];
        if (entry.value != value)
            return false;
        entry.committed = true;
        entry.held = q->held;
        if (held)
            *held = q->held;
        if (&entry == &q->entries.front())
            activate(*q);
        return true;
    }

    // The queue's head waits for what Metal's GPU, not its scheduler, waits for (raw_scheduled_wait.h holds): it and every
    // job committed behind it, now or later, count as scheduled. `held`: those not held before.
    void hold(Key key, std::vector<Value> &held)
    {
        auto *q = find(key);
        if (!q)
            return;
        q->held = true;
        for (Entry &e : q->entries)
            if (e.committed && !e.held) {
                e.held = true;
                held.push_back(e.value);
            }
    }

    // The queue's head goes on: `released`, the jobs its hold held
    void release(Key key, std::vector<Value> &released)
    {
        auto *q = find(key);
        if (!q || !q->held)
            return;
        q->held = false;
        for (Entry &e : q->entries)
            if (e.held) {
                e.held = false;
                released.push_back(e.value);
            }
    }

    Ticket next()
    {
        auto *q = chains_[0].first;
        if (!q)
            return {};
        unlink(*q, 0);
        q->visiting = true;
        return {q->key, q->entries.front().value, q->revision, q->head, dependencies_};
    }

    void wake(Key key)
    {
        if (auto *q = find(key)) {
            q->revision = ++revision_;
            activate(*q);
        }
    }

    void wakeDependencies(uint32_t mask)
    {
        for (unsigned i = 0; i < 2; ++i) {
            if (!(mask & (1u << i)))
                continue;
            ++dependencies_[i];
            while (auto *q = chains_[i + 1].first) {
                q->revision = ++revision_;
                activate(*q);
            }
        }
    }

    bool park(const Ticket &ticket, uint32_t mask = 0, Time due = Time::max())
    {
        auto *q = visiting(ticket);
        if (!q)
            return false;
        bool changed = q->revision != ticket.revision;
        for (unsigned i = 0; i < 2; ++i)
            changed |= (mask & (1u << i)) && ticket.dependencies[i] != dependencies_[i];
        q->visiting = false;
        if (changed) {
            activate(*q);
            return false;
        }
        for (unsigned i = 0; i < 2; ++i)
            if (mask & (1u << i))
                append(*q, i + 1);
        if (due != Time::max()) {
            q->deadline = deadlines_.emplace(due, q);
            q->timed = true;
        }
        q->parked = true;
        ++parked_;
        return true;
    }

    bool yield(const Ticket &ticket)
    {
        auto *q = visiting(ticket);
        if (!q)
            return false;
        q->visiting = false;
        activate(*q);
        return true;
    }

    bool retire(const Ticket &ticket)
    {
        auto *q = visiting(ticket);
        if (!q)
            return false;
        q->entries.pop_front();
        ++q->retired;
        --size_;
        q->visiting = false;
        q->head = q->revision = ++revision_;
        if (q->entries.empty())
            append(*q, 3);
        else
            activate(*q);
        return true;
    }

    void expire(Time now)
    {
        while (!deadlines_.empty() && deadlines_.begin()->first <= now) {
            auto *q = deadlines_.begin()->second;
            q->revision = ++revision_;
            activate(*q);
        }
    }

    Time deadline() const
    {
        return deadlines_.empty() ? Time::max() : deadlines_.begin()->first;
    }

    bool ready() const
    {
        return chains_[0].first != nullptr;
    }

    // Another committed job waits behind the visited one on its own queue
    bool behind(const Ticket &ticket)
    {
        auto *q = visiting(ticket);
        return q && q->entries.size() > 1 && q->entries[1].committed;
    }

    size_t size() const
    {
        return size_;
    }

    // queue heads parked (park), until a wake, a dependency's progress or their deadline activates them
    size_t parked() const
    {
        return parked_;
    }

    // jobs committed and not retired (reserved ones not committed yet are no work of the scheduler's)
    size_t committed() const
    {
        size_t n = 0;
        for (const Queue &q : storage_)
            for (const Entry &e : q.entries)
                n += e.committed;
        return n;
    }
};

enum class EventReadiness {
    ready,
    blocked
};

class EventDeadline {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    EventDeadline(Time start, uint32_t timeoutMs)
        : until_(timeoutMs ? start + std::chrono::milliseconds(timeoutMs) : Time::max()), timed_(timeoutMs != 0)
    {}

    // An untimed wait waits as long as Metal's GPU would: until the value is reached, or its producer fails
    EventReadiness state(bool covered, Time now) const
    {
        if (covered || (timed_ && now >= until_))
            return EventReadiness::ready;
        return EventReadiness::blocked;
    }

    Time until() const
    {
        return until_;
    }

private:
    Time until_;
    bool timed_;
};

}  // namespace raw
