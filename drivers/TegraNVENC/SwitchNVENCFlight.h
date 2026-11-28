// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "../TegraHost1x/SwitchHost1xFence.h"
#include "../TegraHost1x/TegraHost1x.h"

constexpr uint32_t kSwitchNVENCInFlight = 4;
constexpr uint32_t kSwitchNVENCSlots = 8;

struct SwitchNVENCCompletion {
    void (*done)(void *context, uint32_t fence, int result) = nullptr;
    void *context = nullptr;
};

struct SwitchNVENCFlightSlot {
    switch_host1x_fence fence = {};
    uint32_t threshold = 0;
    SwitchNVENCCompletion completion = {};
    bool active = false;
    bool overdue = false;
};

// NVENC's in-flight job slots under one engine lock. Waits for queue room drop the engine lock across boundedWait and
// re-check slots, suspend, cancellation and wedge state on wakeup; retiring callbacks take the engine lock once.
template <class Platform> class SwitchNVENCFlight {
public:
    static constexpr uint32_t kInFlight = kSwitchNVENCInFlight;
    static constexpr uint32_t kSlots = kSwitchNVENCSlots;

    static bool reached(uint32_t value, uint32_t threshold)
    {
        return static_cast<int32_t>(value - threshold) >= 0;
    }

    Platform &platform()
    {
        return platform_;
    }

    uint32_t inFlight() const
    {
        return inFlight_;
    }

    uint32_t epoch() const
    {
        return epoch_;
    }

    bool cancelling() const
    {
        return cancelling_;
    }

    void setCancellingLocked(bool value)
    {
        cancelling_ = value;
        if (!value)
            wakeLocked();
    }

    void bumpEpochLocked()
    {
        ++epoch_;
    }

    void wakeLocked()
    {
        if (waiters_)
            platform_.wake(&waiters_);
    }

    void sleepLocked()
    {
        ++waiters_;
        platform_.sleep(&waiters_);
        --waiters_;
    }

    // Engine lock held on entry and exit. Returns 0 with *outJob ready to submit, or a negative errno.
    int waitSlotLocked(uint32_t *outJob)
    {
        for (;;) {
            while (platform_.suspended() || cancelling_)
                sleepLocked();
            if (platform_.wedged())
                return -19;
            const uint32_t job = platform_.issued() + 1;
            if (job > kInFlight) {
                const uint32_t waitFence = platform_.base() + job - kInFlight;
                if (!reached(platform_.syncptRead(), waitFence)) {
                    const uint32_t seenEpoch = epoch_;
                    platform_.unlock();
                    int ret = platform_.boundedWait(waitFence);
                    platform_.lock();
                    if (seenEpoch != epoch_)
                        continue;
                    if (ret == TEGRA_HOST1X_ETIMEDOUT || ret == TEGRA_HOST1X_EFALCON)
                        ret = platform_.stalledLocked(waitFence);
                    if (ret && ret != -5)
                        return ret;
                    continue;
                }
            }
            if (inFlight_ >= kInFlight || slots_[job % kSlots].active) {
                sleepLocked();
                continue;
            }
            *outJob = job;
            return 0;
        }
    }

    SwitchNVENCFlightSlot *armLocked(uint32_t job, uint32_t threshold, const SwitchNVENCCompletion &completion)
    {
        auto &slot = slots_[job % kSlots];
        slot = {};
        slot.threshold = threshold;
        slot.completion = completion;
        slot.active = true;
        ++inFlight_;
        return &slot;
    }

    void cancelArmLocked(SwitchNVENCFlightSlot *slot)
    {
        if (!slot || !slot->active)
            return;
        *slot = {};
        --inFlight_;
        wakeLocked();
    }

    // Single-owner normal completion: slot stays owned until active is cleared under the engine lock.
    bool retireSlot(SwitchNVENCFlightSlot *slot, int result)
    {
        const uint32_t threshold = slot->threshold;
        const SwitchNVENCCompletion completion = slot->completion;
        if (completion.done)
            completion.done(completion.context, threshold, result);
        platform_.lock();
        *slot = {};
        --inFlight_;
        const bool idle = inFlight_ == 0;
        wakeLocked();
        platform_.unlock();
        return idle;
    }

    void markOverdueLocked(SwitchNVENCFlightSlot *slot)
    {
        slot->overdue = true;
        wakeLocked();
    }

    bool findWatchdogLocked(bool falconError, uint32_t *threshold, uint32_t *seenEpoch, bool *wasOverdue) const
    {
        const SwitchNVENCFlightSlot *oldest = nullptr;
        for (const auto &slot : slots_) {
            if (!slot.active || (!slot.overdue && !falconError))
                continue;
            if (!oldest || static_cast<int32_t>(slot.threshold - oldest->threshold) < 0)
                oldest = &slot;
        }
        if (!oldest)
            return false;
        *threshold = oldest->threshold;
        *seenEpoch = epoch_;
        if (wasOverdue)
            *wasOverdue = oldest->overdue;
        return true;
    }

    bool retireOverdue(uint32_t threshold, int result)
    {
        SwitchNVENCCompletion completion = {};
        bool found = false;
        platform_.lock();
        for (auto &slot : slots_) {
            if (slot.active && slot.overdue && slot.threshold == threshold) {
                completion = slot.completion;
                slot = {};
                --inFlight_;
                found = true;
                wakeLocked();
                break;
            }
        }
        const bool idle = found && inFlight_ == 0;
        platform_.unlock();
        if (found && completion.done)
            completion.done(completion.context, threshold, result);
        return idle;
    }

    // Engine lock held on entry and exit. Retires overdue slots unlocked and sleeps until remaining fences retire.
    void drainOverdueAndWaitLocked(int cancelResult)
    {
        for (;;) {
            SwitchNVENCCompletion completions[kSlots] = {};
            uint32_t thresholds[kSlots] = {};
            uint32_t count = 0;
            for (auto &slot : slots_) {
                if (slot.active && slot.overdue) {
                    completions[count] = slot.completion;
                    thresholds[count] = slot.threshold;
                    ++count;
                    slot = {};
                    --inFlight_;
                }
            }
            if (count) {
                wakeLocked();
                platform_.unlock();
                for (uint32_t i = 0; i < count; ++i)
                    if (completions[i].done)
                        completions[i].done(completions[i].context, thresholds[i], cancelResult);
                platform_.lock();
                continue;
            }
            if (!inFlight_)
                return;
            sleepLocked();
        }
    }

private:
    Platform platform_;
    SwitchNVENCFlightSlot slots_[kSlots] = {};
    uint32_t inFlight_ = 0;
    uint32_t epoch_ = 0;
    uint32_t waiters_ = 0;
    bool cancelling_ = false;
};
