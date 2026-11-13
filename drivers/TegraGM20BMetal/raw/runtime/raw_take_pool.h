// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace raw {

// Up to N items any thread keeps and any thread takes, with no lock. Each item lives in a node of the pool's own array,
// on one of two stacks: kept (an item to take) and spare (an emptied one, to keep the next item in). A stack's head is a
// node index and a count its compare-and-swap moves with it, so a node taken and kept again between a taker's load and
// its swap fails that swap. Nodes are never freed: a taker may read the next index of a node another just took.
template <typename T, uint32_t N> class TakePool {
public:
    static constexpr uint32_t kNone = ~0u;

    // An item kept before, now the caller's until it gives the node back (`node` names it); null: none kept
    T *take(uint32_t &node)
    {
        node = pop(kept_);
        if (node == kNone)
            return nullptr;
        count_.fetch_sub(1, std::memory_order_relaxed);
        return &nodes_[node].item;
    }

    // The node of an item the caller took, emptied, for a later keep
    void giveBack(uint32_t node)
    {
        push(spare_, node);
    }

    // A node to keep an item in, its item the caller's until keep(); null: all N hold items
    T *room(uint32_t &node)
    {
        node = pop(spare_);
        if (node == kNone) {
            node = made_.fetch_add(1, std::memory_order_relaxed);
            if (node >= N) {
                made_.fetch_sub(1, std::memory_order_relaxed);
                node = kNone;
                return nullptr;
            }
        }
        return &nodes_[node].item;
    }

    void keep(uint32_t node)
    {
        count_.fetch_add(1, std::memory_order_relaxed);
        push(kept_, node);
    }

    // Items kept, as any thread sees it
    uint32_t kept() const
    {
        return count_.load(std::memory_order_relaxed);
    }

private:
    struct Node {
        T item{};
        std::atomic<uint32_t> next{kNone};
    };

    static uint64_t pack(uint32_t node, uint32_t count)
    {
        return uint64_t(count) << 32 | node;
    }

    uint32_t pop(std::atomic<uint64_t> &head)
    {
        uint64_t h = head.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t node = uint32_t(h);
            if (node == kNone)
                return kNone;
            const uint32_t next = nodes_[node].next.load(std::memory_order_relaxed);
            if (head.compare_exchange_weak(h, pack(next, uint32_t(h >> 32) + 1), std::memory_order_acq_rel,
                                           std::memory_order_acquire))
                return node;
        }
    }

    void push(std::atomic<uint64_t> &head, uint32_t node)
    {
        uint64_t h = head.load(std::memory_order_relaxed);
        do
            nodes_[node].next.store(uint32_t(h), std::memory_order_relaxed);
        while (!head.compare_exchange_weak(h, pack(node, uint32_t(h >> 32) + 1), std::memory_order_release,
                                           std::memory_order_relaxed));
    }

    std::array<Node, N> nodes_{};
    std::atomic<uint64_t> kept_{pack(kNone, 0)}, spare_{pack(kNone, 0)};
    std::atomic<uint32_t> made_{0}, count_{0};
};

}  // namespace raw
