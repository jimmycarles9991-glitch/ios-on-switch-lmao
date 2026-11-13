// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct SwitchHost1xResult;
struct switch_gpu_bridge;
struct switch_gpu_event_waits;

struct SwitchVICQueue {
    static constexpr uint32_t kSlots = 16;
    using Completion = void (*)(void *, int);

    struct Job {
        uint64_t fence = 0;
        uint64_t submittedUs = 0;
        Completion completion = nullptr;
        void *context = nullptr;
        switch_gpu_bridge *bridge = nullptr;
        switch_gpu_event_waits *semantics = nullptr;
        SwitchHost1xResult *outcome = nullptr;
        uint32_t toBatchEnd = 0;  // jobs after this one in its batch: the last holds the batch's bridge
    };

    Job jobs[kSlots] = {};
    uint32_t head = 0, count = 0;
    uint64_t submitted = 0, completed = 0;

    static bool reached(uint32_t value, uint32_t threshold)
    {
        return static_cast<int32_t>(value - threshold) >= 0;
    }

    uint32_t tail() const
    {
        return (head + count) % kSlots;
    }

    bool full() const
    {
        return count == kSlots;
    }

    bool canPublish(uint32_t amount) const
    {
        return amount && amount <= kSlots - count && submitted <= UINT64_MAX - amount;
    }

    uint32_t batchEnd() const
    {
        return (head + jobs[head].toBatchEnd) % kSlots;
    }

    uint32_t last() const
    {
        return (tail() + kSlots - 1) % kSlots;
    }

    uint32_t configurationSlot(uint32_t job) const
    {
        return (tail() + job) % kSlots;
    }

    bool publish(Job job)
    {
        if (!canPublish(1))
            return false;
        job.fence = ++submitted;
        jobs[tail()] = job;
        ++count;
        return true;
    }

    bool retire(uint32_t value)
    {
        if (!count || !reached(value, static_cast<uint32_t>(jobs[head].fence)))
            return false;
        completed = jobs[head].fence;
        jobs[head] = {};
        head = (head + 1) % kSlots;
        --count;
        return true;
    }

    bool rebase(uint32_t hardware)
    {
        const uint32_t advance = hardware - static_cast<uint32_t>(submitted);
        if (count || advance > INT32_MAX || submitted > UINT64_MAX - advance)
            return false;
        submitted += advance;
        completed = submitted;
        return true;
    }

    bool legacySequence(uint32_t threshold, uint64_t *sequence) const
    {
        const uint32_t behind = static_cast<uint32_t>(submitted) - threshold;
        if (!sequence || behind > INT32_MAX || behind > submitted)
            return false;
        *sequence = submitted - behind;
        return true;
    }

    bool reset(uint32_t value)
    {
        if (count)
            return false;
        submitted = completed = value;
        return true;
    }
};
