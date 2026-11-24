// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

namespace display_promise_test {

struct PromiseKernel {
    static constexpr uint64_t kChannelUid = 9;
    volatile uint32_t word = 0;
    uint32_t value = 1;
    uint32_t jobSeqCompleted = 0, jobSeqVisible = 0;
    bool abandoned = false;
    unsigned steppedRaising = 0, steppedRaised = 0, steppedKeptFirst = 0;
    int lastClaim = SWITCH_GPU_PRESENT_WATCH_PENDING;
    switch_gpu_present_watch watch{};
    switch_gpu_giveups giveups{};
    Display *display = nullptr;
    switch_gpu_backing_token promiseToken{};

    static inline PromiseKernel *current = nullptr;

    static volatile uint32_t *fenceWord(uint32_t offset)
    {
        return offset == kPromiseOffset ? &current->word : nullptr;
    }

    static int fenceRaise(uint32_t offset, volatile uint32_t *w, uint32_t val)
    {
        if (offset != kPromiseOffset || static_cast<int32_t>(*w - val) >= 0)
            return 0;
        *w = val;
        return 1;
    }

    static uint32_t fenceOwner(uint32_t offset)
    {
        return offset == kPromiseOffset ? kPromiseOwner : 0;
    }

    static int slotOf(uint32_t offset)
    {
        return offset == kPromiseOffset ? 0 : -1;
    }

    static int readerParked(const switch_gpu_giveup_job *)
    {
        return 0;
    }

    static int jobAhead(const switch_gpu_giveup_job *, uint32_t, const volatile uint32_t **fence, uint32_t *,
                        uint64_t *checkNs)
    {
        *fence = nullptr;
        *checkNs = SWITCH_GPU_GIVEUP_LOOK_NS;
        return SWITCH_GPU_GIVEUP_HEAD;
    }

    static void need(uint32_t, uint32_t)
    {}

    static void moved(uint32_t, uint32_t)
    {}

    static void recall(switch_gpu_giveup *)
    {}

    static int killed(const switch_gpu_giveup_job *)
    {
        return 0;
    }

    static int tokenCompleted(const switch_gpu_backing_token *t)
    {
        if (t->engine == SWITCH_GPU_ENGINE_PROMISE)
            return current->abandoned ? SWITCH_GPU_BACKING_WRITE_REFUSED : (current->word >= t->value ? 1 : 0);
        if (t->engine == SWITCH_GPU_ENGINE_GM20B)
            return current->jobSeqCompleted >= t->value ? 1 : 0;
        return 0;
    }

    static int tokenExternal(const switch_gpu_backing_token *t)
    {
        if (t->engine == SWITCH_GPU_ENGINE_GM20B)
            return current->jobSeqVisible >= t->value ? 1 : 0;
        return 0;
    }

    static void stepped(uint32_t offset, uint32_t val, int step, void *)
    {
        if (step == SWITCH_GPU_PROMISE_RAISING) {
            ++current->steppedRaising;
            current->abandoned = true;
        } else if (step == SWITCH_GPU_PROMISE_RAISED) {
            ++current->steppedRaised;
            current->releaseWatch(offset, val, true);
        } else if (step == SWITCH_GPU_PROMISE_KEPT_FIRST) {
            ++current->steppedKeptFirst;
            current->abandoned = false;
        }
    }

    static int expire(switch_gpu_giveup *e, uint64_t now)
    {
        return switch_gpu_giveup_held_expire(&current->giveups, e, now);
    }

    const switch_gpu_promise_fence fenceOps{fenceWord, fenceRaise, fenceOwner};
    const switch_gpu_giveup_ops ops{&fenceOps, slotOf, readerParked, jobAhead, need,
                                    stepped,   moved,  recall,       killed,   expire};

    void reset(Display *d)
    {
        display = d;
        current = this;
        word = 0;
        value = 1;
        jobSeqCompleted = jobSeqVisible = 0;
        abandoned = false;
        steppedRaising = steppedRaised = steppedKeptFirst = 0;
        lastClaim = SWITCH_GPU_PRESENT_WATCH_PENDING;
        watch = {};
        giveups = {};
        promiseToken = {value, 0, SWITCH_GPU_ENGINE_PROMISE, kPromiseOffset};
    }

    int parkPromisedOffer()
    {
        switch_gpu_present_watch_promise(&watch, &promiseToken);
        const int slot =
            switch_gpu_giveup_park(&giveups, &ops, kPromiseOffset, value, "display", "backboardd", nullptr, 1, gNowNs);
        if (slot >= 0) {
            switch_gpu_giveup *e = &giveups.entry[slot];
            e->due_ns = switch_gpu_giveup_bound(&giveups, e, gNowNs + TegraGM20BPromiseBoundNs);
        }
        return slot;
    }

    int keepLate(uint32_t jobSeq, bool useFilter)
    {
        const switch_gpu_backing_token job{jobSeq, 1, SWITCH_GPU_ENGINE_GM20B, uint32_t(kChannelUid)};
        const int kept = switch_gpu_present_watch_kept(&watch, kPromiseOffset, value, &job);
        if (!kept)
            return 0;
        switch_gpu_present_watch_rearm(&watch);
        const switch_gpu_giveup_job gj{uint32_t(kChannelUid), jobSeq};
        switch_gpu_giveup_kept(&giveups, &ops, kPromiseOffset, value, &gj, jobSeq, gNowNs);
        word = value;
        jobSeqCompleted = jobSeqVisible = jobSeq;
        switch_gpu_giveup_leave(&giveups, &ops, kPromiseOffset, value, 1);
        if (useFilter) {
            if (!switch_gpu_present_watch_fenced(&watch, kChannelUid, jobSeq))
                return -1;
            if (!switch_gpu_present_watch_take_visible(&watch, tokenCompleted, tokenExternal))
                return -2;
            lastClaim = SWITCH_GPU_PRESENT_WATCH_VISIBLE;
            switch_gpu_present_watch_reset(&watch);
            display->gpuFilter();
            display->gpuComplete(0);
            return 1;
        }
        lastClaim = switch_gpu_present_watch_claim(&watch, tokenCompleted, tokenExternal);
        if (lastClaim != SWITCH_GPU_PRESENT_WATCH_VISIBLE)
            return -3;
        switch_gpu_present_watch_reset(&watch);
        display->gpuComplete(0);
        return 1;
    }

    void releaseWatch(uint32_t offset, uint32_t val, bool failed)
    {
        if (!switch_gpu_present_watch_promised(&watch, offset, val))
            return;
        if (failed)
            abandoned = true;
        lastClaim = switch_gpu_present_watch_claim(&watch, tokenCompleted, tokenExternal);
        switch_gpu_present_watch_reset(&watch);
        display->gpuComplete(lastClaim == SWITCH_GPU_PRESENT_WATCH_FAILED ? TEGRA_SCANOUT_SOURCE_REFUSED : 0);
    }

    void withdrawBeforeBound()
    {
        abandoned = true;
        word = value;
        switch_gpu_giveup_leave(&giveups, &ops, kPromiseOffset, value, 1);
        releaseWatch(kPromiseOffset, value, true);
    }

    void runGiveUp()
    {
        switch_gpu_giveup_run(&giveups, &ops, gNowNs);
    }
};

}  // namespace display_promise_test
