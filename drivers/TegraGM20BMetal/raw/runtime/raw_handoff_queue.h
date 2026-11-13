// SPDX-License-Identifier: MIT
#pragma once
#include <dispatch/dispatch.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace raw {

// Items any threads hand to one taker, each giver's taken in the order it gave them, with no lock on any side. An item
// moves into a spare node of the queue's own (nodes are made only while the queue grows to the most ever in flight, in
// chunks that never move or free), the node goes onto a stack the taker exchanges whole and appends to its own list. The
// spare stack's head is a node index and a count its compare-and-swap moves with it: a node one giver takes and the
// taker gives back between another giver's load and its swap fails that swap. The wake counts: a hand-off that wakes
// signals once after its push, as does wake(), and the taker sleeps only after a look found its condition false, so no
// wake is slept through; a wake whose change the taker saw with an earlier one costs it one more look.
template <typename T> class HandoffQueue {
    static constexpr uint32_t kNone = ~0u, kFirstChunk = 8, kChunks = 24;

    struct Node {
        T item{};
        Node *next = nullptr;
        std::atomic<uint32_t> spareNext{kNone};
        uint32_t index = 0;
    };

public:
    HandoffQueue() : wake_(dispatch_semaphore_create(0))
    {}

    ~HandoffQueue()
    {
        for (std::atomic<Node *> &chunk : chunks_)
            delete[] chunk.load();
#if !__has_feature(objc_arc)
        dispatch_release(wake_);
#endif
    }

    HandoffQueue(const HandoffQueue &) = delete;
    HandoffQueue &operator=(const HandoffQueue &) = delete;

    // A giving thread's. An item handed over with wake false leaves a sleeping taker asleep.
    void push(T &&item, bool wake = true)
    {
        Node *node = spare();
        node->item = std::move(item);
        count_.fetch_add(1, std::memory_order_relaxed);
        node->next = given_.load(std::memory_order_relaxed);
        while (!given_.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed))
            ;
        if (wake)
            dispatch_semaphore_signal(wake_);
    }

    // Any thread's, after it changed what the taker's condition reads
    void wake()
    {
        dispatch_semaphore_signal(wake_);
    }

    // The taking thread's: every item given so far appended to its own list, until `ready(*this)` holds of them
    template <typename Ready> void waitFor(Ready &&ready)
    {
        take();
        while (!ready(static_cast<const HandoffQueue &>(*this))) {
            dispatch_semaphore_wait(wake_, DISPATCH_TIME_FOREVER);
            take();
        }
    }

    // The taking thread's: the oldest item not popped, waiting for one where there is none
    T &front()
    {
        waitFor([](const HandoffQueue &q) {
            return q.head_ != nullptr;
        });
        return head_->item;
    }

    // The taking thread's: the oldest item not popped, null where none was given (no wait)
    T *peek()
    {
        take();
        return head_ ? &head_->item : nullptr;
    }

    // The taking thread's: asleep until the next hand-off that wakes, a wake(), or `until`
    void sleep(dispatch_time_t until)
    {
        dispatch_semaphore_wait(wake_, until);
    }

    // The taking thread's, once it is done with front(): the item counts as given no more
    void pop()
    {
        Node *node = head_;
        head_ = node->next;
        if (!head_)
            tail_ = nullptr;
        node->item = T();
        uint64_t h = spare_.load(std::memory_order_relaxed);
        do
            node->spareNext.store(uint32_t(h), std::memory_order_relaxed);
        while (!spare_.compare_exchange_weak(h, pack(node->index, uint32_t(h >> 32) + 1), std::memory_order_release,
                                             std::memory_order_relaxed));
        count_.fetch_sub(1, std::memory_order_release);
    }

    // Items given and not popped, as any thread sees it
    size_t size() const
    {
        return count_.load(std::memory_order_acquire);
    }

    // The taking thread's own list (the items the last take found), oldest first
    class Iterator {
    public:
        explicit Iterator(const Node *node) : node_(node)
        {}

        const T &operator*() const
        {
            return node_->item;
        }

        Iterator &operator++()
        {
            node_ = node_->next;
            return *this;
        }

        bool operator!=(const Iterator &other) const
        {
            return node_ != other.node_;
        }

    private:
        const Node *node_;
    };

    Iterator begin() const
    {
        return Iterator(head_);
    }

    Iterator end() const
    {
        return Iterator(nullptr);
    }

    // The taking thread's: its list's item `at` places from the oldest (at < the list's length)
    const T &at(size_t at) const
    {
        const Node *node = head_;
        while (at--)
            node = node->next;
        return node->item;
    }

private:
    static uint64_t pack(uint32_t index, uint32_t count)
    {
        return uint64_t(count) << 32 | index;
    }

    // Chunk c holds kFirstChunk << c nodes, from index kFirstChunk * (2^c - 1)
    static uint32_t chunkOf(uint32_t index)
    {
        return 31 - uint32_t(__builtin_clz(index / kFirstChunk + 1));
    }

    static uint32_t chunkStart(uint32_t chunk)
    {
        return kFirstChunk * ((1u << chunk) - 1);
    }

    Node *nodeAt(uint32_t index)
    {
        const uint32_t chunk = chunkOf(index);
        return &chunks_[chunk].load(std::memory_order_acquire)[index - chunkStart(chunk)];
    }

    Node *spare()
    {
        uint64_t h = spare_.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t index = uint32_t(h);
            if (index == kNone)
                return made();
            Node *node = nodeAt(index);
            const uint32_t next = node->spareNext.load(std::memory_order_relaxed);
            if (spare_.compare_exchange_weak(h, pack(next, uint32_t(h >> 32) + 1), std::memory_order_acq_rel,
                                             std::memory_order_acquire))
                return node;
        }
    }

    Node *made()
    {
        const uint32_t index = made_.fetch_add(1, std::memory_order_relaxed);
        const uint32_t chunk = chunkOf(index);
        Node *nodes = chunks_[chunk].load(std::memory_order_acquire);
        if (!nodes) {
            Node *fresh = new Node[kFirstChunk << chunk];
            if (chunks_[chunk].compare_exchange_strong(nodes, fresh, std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
                nodes = fresh;
            else
                delete[] fresh;
        }
        Node *node = &nodes[index - chunkStart(chunk)];
        node->index = index;
        return node;
    }

    void take()
    {
        Node *given = given_.exchange(nullptr, std::memory_order_acquire);
        if (!given)
            return;
        Node *first = nullptr, *last = given;
        while (given) {
            Node *next = given->next;
            given->next = first;
            first = given;
            given = next;
        }
        if (tail_)
            tail_->next = first;
        else
            head_ = first;
        tail_ = last;
    }

    std::atomic<Node *> given_{nullptr};
    std::atomic<uint64_t> spare_{pack(kNone, 0)};
    std::atomic<uint32_t> made_{0};
    std::atomic<Node *> chunks_[kChunks]{};
    std::atomic<size_t> count_{0};
    Node *head_ = nullptr, *tail_ = nullptr;  // the taker's own list, oldest first
    dispatch_semaphore_t wake_;
};

}  // namespace raw
