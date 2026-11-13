// SPDX-License-Identifier: GPL-2.0-only
// Per-syncpoint fence lists in threshold order, each syncpoint's threshold interrupt armed at its earliest fence.
// Ported from Linux v6.12 drivers/gpu/host1x/intr.c (host1x_intr_add_fence_to_list, host1x_intr_update_hw_state,
// host1x_intr_handle_interrupt, host1x_intr_remove_fence) and drivers/gpu/host1x/hw/intr_hw.c (syncpt_thresh_isr and
// the threshold, enable and disable writes):
//   Copyright (c) 2010-2021, NVIDIA Corporation.
//   Copyright (C) 2010 Google, Inc.
// Not ported: dma_fence (a fence is a caller-owned node and its callback). Added: every arm reads the value back, so a
// fence the syncpoint passed before its threshold was written expires here instead of waiting for an interrupt that
// the cleared status bit will not raise; and the bound check (overdue).
#pragma once
#include <stdint.h>
#include "SwitchHost1xFence.h"

// Expired fences in the order they expired, linked through next.
struct SwitchHost1xExpired {
    switch_host1x_fence *first = nullptr;
    switch_host1x_fence **last = &first;

    SwitchHost1xExpired() = default;
    SwitchHost1xExpired(const SwitchHost1xExpired &) = delete;
    SwitchHost1xExpired &operator=(const SwitchHost1xExpired &) = delete;

    void push(switch_host1x_fence *fence)
    {
        fence->next = nullptr;
        *last = fence;
        last = &fence->next;
    }

    switch_host1x_fence *take()
    {
        auto *taken = first;
        first = nullptr;
        last = &first;
        return taken;
    }

    void append(SwitchHost1xExpired &from)
    {
        if (!from.first)
            return;
        *last = from.first;
        last = from.last;
        from.first = nullptr;
        from.last = &from.first;
    }

    bool erase(switch_host1x_fence *fence)
    {
        for (auto **at = &first; *at; at = &(*at)->next) {
            if (*at != fence)
                continue;
            *at = fence->next;
            if (last == &fence->next)
                last = at;
            fence->next = nullptr;
            return true;
        }
        return false;
    }
};

inline bool switch_host1x_reached(uint32_t value, uint32_t threshold)
{
    return static_cast<int32_t>(value - threshold) >= 0;
}

// Registers: read(offset) and write(offset, value) on the sync unit (offsets from its base, host1x05).
template <class Registers> class SwitchHost1xFenceList {
public:
    static constexpr uint32_t kSyncpts = 192;

    // A fence its syncpoint already passed expires at once; the rest are queued and the head armed.
    void add(Registers &r, switch_host1x_fence *fence, SwitchHost1xExpired &done)
    {
        if (switch_host1x_reached(value(r, fence->syncpt), fence->threshold)) {
            expire(fence, 0, done);
            return;
        }
        insert(fence);
        if (head[fence->syncpt] == fence)
            update(r, fence->syncpt, done);
    }

    bool remove(Registers &r, switch_host1x_fence *fence, SwitchHost1xExpired &done)
    {
        if (fence->state != SWITCH_HOST1X_FENCE_QUEUED)
            return false;
        const bool first = head[fence->syncpt] == fence;
        unlink(fence);
        fence->state = SWITCH_HOST1X_FENCE_IDLE;
        if (first)
            update(r, fence->syncpt, done);
        return true;
    }

    // syncpt_thresh_isr: each armed syncpoint whose status is set has its interrupt disabled and cleared, then its
    // reached fences expire and the next one is armed. Status bits of syncpoints nobody armed are sticky but do not
    // drive the line, and are left alone.
    void interrupt(Registers &r, SwitchHost1xExpired &done)
    {
        for (uint32_t word = 0; word < kSyncpts / 32; ++word) {
            const uint32_t status = r.read(kIntStatus + 4 * word) & armed[word];
            if (!status)
                continue;
            r.write(kIntDisable + 4 * word, status);
            r.write(kIntStatus + 4 * word, status);
            armed[word] &= ~status;
            for (uint32_t bits = status; bits; bits &= bits - 1) {
                const uint32_t id = word * 32 + static_cast<uint32_t>(__builtin_ctz(bits));
                expireReached(r, id, done);
                update(r, id, done);
            }
        }
    }

    // The sync unit came back from a reset with every threshold interrupt off.
    void rearm(Registers &r, SwitchHost1xExpired &done)
    {
        for (uint32_t &word : armed)
            word = 0;
        for (uint32_t id = 0; id < kSyncpts; ++id)
            if (head[id])
                update(r, id, done);
    }

    // Fences past their deadline: one its syncpoint has not reached fails with timedOut. One it has reached was missed
    // by the interrupt; it gets one grace period (the work loop may be about to run), then fails with lost, or, when
    // strict, is returned still queued for the caller's panic. next: the earliest deadline left, 0 for none.
    switch_host1x_fence *overdue(Registers &r, uint64_t now, uint64_t grace, bool strict, int timedOut, int lost,
                                 SwitchHost1xExpired &done, uint64_t *next)
    {
        *next = 0;
        for (uint32_t id = 0; id < kSyncpts; ++id) {
            // A removal re-arms the syncpoint, which may expire the fences after it: the walk starts again.
            for (bool removed = true; removed;) {
                removed = false;
                for (auto *fence = head[id]; fence && !removed; fence = fence->next) {
                    if (!fence->deadline || fence->deadline > now)
                        continue;
                    const bool reached = switch_host1x_reached(value(r, id), fence->threshold);
                    if (reached && !fence->graced) {
                        fence->graced = 1;
                        fence->deadline = now + grace;
                        continue;
                    }
                    if (reached && strict)
                        return fence;
                    remove(r, fence, done);
                    expire(fence, reached ? lost : timedOut, done);
                    removed = true;
                }
            }
            for (auto *fence = head[id]; fence; fence = fence->next)
                if (fence->deadline && (!*next || fence->deadline < *next))
                    *next = fence->deadline;
        }
        return nullptr;
    }

    uint32_t status(Registers &r, uint32_t id) const
    {
        return r.read(kIntStatus + 4 * (id / 32));
    }

    uint32_t value(Registers &r, uint32_t id) const
    {
        return r.read(kSyncpt + 4 * id);
    }

private:
    enum : uint32_t {
        kIntStatus = 0xe80,
        kIntDisable = 0xf00,
        kIntEnableCpu0 = 0xf20,
        kSyncpt = 0xf80,
        kIntThresh = 0x1380,
    };

    switch_host1x_fence *head[kSyncpts] = {}, *tail[kSyncpts] = {};
    uint32_t armed[kSyncpts / 32] = {};

    static void expire(switch_host1x_fence *fence, int status, SwitchHost1xExpired &done)
    {
        fence->status = status;
        done.push(fence);
    }

    // host1x_intr_add_fence_to_list: from the tail, after the last fence at or before this threshold, so fences
    // added in order go on the end in one step.
    void insert(switch_host1x_fence *fence)
    {
        const uint32_t id = fence->syncpt;
        auto *before = tail[id];
        while (before && static_cast<int32_t>(before->threshold - fence->threshold) > 0)
            before = before->prev;
        fence->prev = before;
        fence->next = before ? before->next : head[id];
        if (fence->next)
            fence->next->prev = fence;
        else
            tail[id] = fence;
        if (before)
            before->next = fence;
        else
            head[id] = fence;
        fence->state = SWITCH_HOST1X_FENCE_QUEUED;
    }

    void unlink(switch_host1x_fence *fence)
    {
        const uint32_t id = fence->syncpt;
        if (fence->prev)
            fence->prev->next = fence->next;
        else
            head[id] = fence->next;
        if (fence->next)
            fence->next->prev = fence->prev;
        else
            tail[id] = fence->prev;
        fence->prev = fence->next = nullptr;
    }

    // host1x_intr_handle_interrupt's walk: the list is sorted, so the first fence not reached ends it.
    bool expireReached(Registers &r, uint32_t id, SwitchHost1xExpired &done)
    {
        const uint32_t current = value(r, id);
        bool any = false;
        while (head[id] && switch_host1x_reached(current, head[id]->threshold)) {
            auto *fence = head[id];
            unlink(fence);
            expire(fence, 0, done);
            any = true;
        }
        return any;
    }

    // host1x_intr_update_hw_state. The status bit is cleared before the threshold is written so a stale bit from an
    // earlier threshold raises nothing; the value is read back after the enable because an increment that landed
    // before the threshold write sets no status bit.
    void update(Registers &r, uint32_t id, SwitchHost1xExpired &done)
    {
        const uint32_t word = id / 32, bit = 1u << (id % 32);
        do {
            if (!head[id]) {
                r.write(kIntDisable + 4 * word, bit);
                r.write(kIntStatus + 4 * word, bit);
                armed[word] &= ~bit;
                return;
            }
            r.write(kIntStatus + 4 * word, bit);
            r.write(kIntThresh + 4 * id, head[id]->threshold);
            r.write(kIntEnableCpu0 + 4 * word, bit);
            armed[word] |= bit;
        } while (expireReached(r, id, done));
    }
};
