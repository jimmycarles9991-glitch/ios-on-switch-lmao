// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct SwitchHost1xResult;
struct switch_gpu_bridge;
struct switch_gpu_event_waits;

// Runs once per job on host1x's interrupt work loop (it must not sleep), with the job's fence and result (0, or a
// negative errno), after the hardware and the semantic outcome are in and before the job's Results record completes.
struct SwitchNVDECCompletion {
    void (*done)(void *context, uint32_t fence, int result) = nullptr;
    void *context = nullptr;
};

struct SwitchNVDECQueue {
    static constexpr uint32_t kSlots = 8;

    struct Job {
        uint64_t sequence = 0;
        uint64_t submittedUs = 0;
        switch_gpu_bridge *bridge = nullptr;
        switch_gpu_event_waits *semantics = nullptr;
        SwitchHost1xResult *outcome = nullptr;
        SwitchNVDECCompletion completion = {};
    };

    Job jobs[kSlots] = {};
    uint64_t submitted = 0, completed = 0;
    uint32_t head = 0, count = 0;

    uint32_t tail() const
    {
        return (head + count) % kSlots;
    }

    bool full() const
    {
        return count == kSlots;
    }

    uint64_t next() const
    {
        return submitted + 1;
    }

    static bool reached(uint32_t value, uint32_t threshold)
    {
        return static_cast<int32_t>(value - threshold) >= 0;
    }

    bool publish(uint64_t submittedUs, switch_gpu_bridge *bridge = nullptr, switch_gpu_event_waits *semantics = nullptr,
                 SwitchHost1xResult *outcome = nullptr, SwitchNVDECCompletion completion = {})
    {
        if (full() || submitted == UINT64_MAX)
            return false;
        jobs[tail()] = {++submitted, submittedUs, bridge, semantics, outcome, completion};
        ++count;
        return true;
    }

    bool retire(uint32_t hardware)
    {
        if (!count || !reached(hardware, static_cast<uint32_t>(jobs[head].sequence)))
            return false;
        completed = jobs[head].sequence;
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
};
