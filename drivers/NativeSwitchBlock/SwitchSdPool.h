// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stddef.h>
#include <stdint.h>

template <typename Node> struct SwitchSdPool {
    Node *slab = nullptr;
    Node *freeHead = nullptr;
    uint32_t capacity = 0;
    uint32_t inUse = 0;

    void init(Node *storage, uint32_t count)
    {
        slab = nullptr;
        freeHead = nullptr;
        capacity = 0;
        inUse = 0;
        if (!storage || !count)
            return;
        slab = storage;
        capacity = count;
        freeHead = storage;
        for (uint32_t i = 0; i < count; ++i) {
            storage[i].next = i + 1 < count ? &storage[i + 1] : nullptr;
            storage[i].pooled = true;
        }
    }

    Node *detachIfIdle()
    {
        if (!slab || inUse != 0)
            return nullptr;
        Node *const old = slab;
        slab = nullptr;
        freeHead = nullptr;
        capacity = 0;
        return old;
    }

    bool owns(const Node *node) const
    {
        if (!slab || !capacity || !node)
            return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(slab);
        const uintptr_t addr = reinterpret_cast<uintptr_t>(node);
        const uintptr_t span = static_cast<uintptr_t>(capacity) * sizeof(Node);
        return addr >= base && addr - base < span && ((addr - base) % sizeof(Node)) == 0;
    }

    Node *pop()
    {
        Node *const node = freeHead;
        if (!node)
            return nullptr;
        freeHead = node->next;
        node->next = nullptr;
        node->pooled = true;
        ++inUse;
        return node;
    }

    void push(Node *node)
    {
        if (!node)
            return;
        node->next = freeHead;
        node->pooled = true;
        freeHead = node;
        --inUse;
    }

    void pushBatch(Node *first, Node *last, uint32_t count)
    {
        if (!first || !last || !count)
            return;
        last->next = freeHead;
        freeHead = first;
        inUse -= count;
    }

    uint32_t size() const
    {
        return capacity;
    }

    uint32_t active() const
    {
        return inUse;
    }

    uint32_t available() const
    {
        return capacity - inUse;
    }

    bool empty() const
    {
        return freeHead == nullptr;
    }
};

enum class SwitchSdPoolSubmitResult : uint8_t {
    kQueued,
    kNotReady,
    kNoMemory,
};

// Pops a pooled node and enqueues under a single lock hold. If the pool is empty, drops the lock before heap
// allocation and re-checks shutdown on re-acquire; a node allocated across shutdown is freed outside the lock.
template <typename Node, typename Ops>
inline SwitchSdPoolSubmitResult switchSdPoolSubmit(SwitchSdPool<Node> &pool, const Node &spec, Ops &&ops)
{
    const uint64_t now = ops.nowNs();
    ops.lock();
    if (ops.isShutdown()) {
        ops.unlock();
        return SwitchSdPoolSubmitResult::kNotReady;
    }
    if (Node *node = pool.pop()) {
        *node = spec;
        node->next = nullptr;
        node->queuedNs = now;
        node->chained = false;
        node->pooled = true;
        ops.enqueueLocked(node);
        ops.unlock();
        return SwitchSdPoolSubmitResult::kQueued;
    }
    const uint32_t capacity = pool.size();
    ops.unlock();
    if (capacity)
        ops.onExhausted(capacity);
    Node *const node = ops.allocHeap();
    if (!node)
        return SwitchSdPoolSubmitResult::kNoMemory;
    *node = spec;
    node->next = nullptr;
    node->queuedNs = ops.nowNs();
    node->chained = false;
    node->pooled = false;
    ops.lock();
    if (ops.isShutdown()) {
        ops.unlock();
        ops.freeHeap(node);
        return SwitchSdPoolSubmitResult::kNotReady;
    }
    ops.enqueueLocked(node);
    ops.unlock();
    return SwitchSdPoolSubmitResult::kQueued;
}

// Drains `first` in bounded stack chunks: copies metadata, splices pooled nodes back under one lock hold per chunk,
// then unlocks before freeing overflow heap nodes and running completion callbacks so re-entrant I/O never deadlocks.
template <size_t Chunk, typename Node, typename Meta, typename Ops>
inline void switchSdPoolComplete(SwitchSdPool<Node> &pool, Node *first, Ops &&ops)
{
    static_assert(Chunk > 0, "Chunk must be nonzero");
    Meta done[Chunk];
    for (Node *r = first; r;) {
        size_t n = 0;
        Node *pooledHead = nullptr;
        Node *pooledTail = nullptr;
        uint32_t pooledCount = 0;
        Node *heapHead = nullptr;
        while (r && n < Chunk) {
            Node *const next = r->next;
            done[n++] = ops.capture(*r);
            if (r->pooled) {
                r->next = pooledHead;
                if (!pooledTail)
                    pooledTail = r;
                pooledHead = r;
                ++pooledCount;
            } else {
                r->next = heapHead;
                heapHead = r;
            }
            r = next;
        }
        if (pooledHead) {
            ops.lock();
            pool.pushBatch(pooledHead, pooledTail, pooledCount);
            ops.unlock();
        }
        while (heapHead) {
            Node *const next = heapHead->next;
            ops.freeHeap(heapHead);
            heapHead = next;
        }
        for (size_t i = 0; i < n; ++i)
            ops.finish(done[i]);
    }
}
