// SPDX-License-Identifier: BSD-2-Clause-Patent
// A host1x engine job's retirement with no thread that sleeps per job. The job's syncpoint fence (host1x's interrupt)
// and, when its stream waits for a GPU producer, that producer's bridge (the bridge's retirement) finish in either
// order; then the job's physical result is published. That and the job's semantic completion (the events' retirement)
// finish in either order and end the job.
//
// A job's bound counts from when its inputs are in, the moment the engine could have started it: its GPU producer's
// bridge, its other host1x producers' fences, and the inputs of the job queued ahead of it on the same serialized
// channel. A job held up by an upstream stall is not stalled itself: the producer's own bound cancels the producer,
// whose released syncpoint lets the job's stream on and starts its bound. Each input is also a hardware step, so a
// job never ends, and its slot is never reused, before every input has come in.
//
// A job whose fence failed, or that cancel() failed, ends with kCancelled whatever its GPU producer reports. A job
// whose stream waited for another host1x engine's job that failed ends with that failure, as its semantic batch would
// later: a cancelled job's syncpoint is released by the CPU, so a job that read what it was to write ran on memory it
// never wrote. The engine records the cancellation before that release, so it is known by the time this job's fence
// is reached.
//
// Engine supplies, each called once per job, without the engine's lock and never inside start:
//   static void fault(uint32_t slot, int status, int producers);  the fence failed: its bound outlived, or (before
//                                                                  strict) its interrupt lost; on host1x's work loop,
//                                                                  so it takes no lock and never sleeps
//   static void physical(uint32_t slot, int result);              result: 1, the GPU producer's failure, a host1x
//                                                                  producer's, or kCancelled
//   static void ended(uint32_t slot, int result);                 result: physical()'s, for the engine to join with
//                                                                  its semantic outcome
#pragma once
#include <stdint.h>
#include "SwitchHost1xFence.h"
#include "TegraHost1x.h"
#include "../TegraGM20B/SwitchGpuBridge.h"

// The other host1x engines' jobs a job's stream waits for by syncpoint: the newest of each engine's, since a
// cancellation fails every job queued behind the one that stalled.
struct SwitchHost1xProducers {
    static constexpr uint32_t kMax = 3;  // NVDEC, VIC and NVENC, a syncpoint each
    switch_gpu_backing_token tokens[kMax] = {};
    uint32_t count = 0;

    void add(const switch_gpu_backing_token &token)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (tokens[i].engine == token.engine && tokens[i].queue == token.queue) {
                if (token.value > tokens[i].value)
                    tokens[i] = token;
                return;
            }
        if (count < kMax)
            tokens[count++] = token;
    }

    int failure() const
    {
        for (uint32_t i = 0; i < count; ++i) {
            const int physical = switch_gpu_backing_token_physical_status(&tokens[i], 0);
            if (physical < 0 && physical != SWITCH_GPU_BACKING_STALE)
                return physical;
        }
        return 0;
    }
};

struct SwitchHost1xInputFence {
    switch_host1x_fence fence;
    uint32_t slot;
};

struct SwitchHost1xStage {
    switch_host1x_fence fence;
    SwitchHost1xProducers producers;
    SwitchHost1xInputFence produced[SwitchHost1xProducers::kMax];
    uint32_t hardware, inputs, successor, final, boundUs;
    int physical;
    bool ended;
};

template <class Engine, uint32_t Slots> class SwitchHost1xStages {
public:
    static constexpr int kCancelled = -5;
    inline static SwitchHost1xStage stages[Slots] = {};

    // Under the engine's lock, once the job's slot is published and its batch committed; `ahead` is the slot of the
    // job queued just before it, or Slots. A job with a bridge has its bound started by the bridge's completion, so a
    // GPU producer that is slow, or killed by the GPU's recovery, fails only its own consumer (D8); it cannot end
    // before bridged() is called.
    static int start(uint32_t slot, uint32_t syncpt, uint32_t threshold, bool bridge, bool semantic, uint32_t boundUs,
                     const SwitchHost1xProducers &producers, uint32_t ahead)
    {
        auto &stage = stages[slot];
        switch_host1x_fence_init(&stage.fence, syncpt, threshold, signaled);
        stage.producers = producers;
        const uint32_t own = (bridge ? 1 : 0) + producers.count;
        stage.hardware = 1 + own;
        stage.inputs = own + 1;  // and start's own hold on them, given back once every input is armed
        stage.successor = kNoSuccessor;
        stage.final = semantic ? 2 : 1;
        stage.boundUs = boundUs;
        stage.physical = 1;
        stage.ended = false;
        bool behind = false;
        if (ahead < Slots) {
            ++stage.inputs;
            ++stage.hardware;
            uint32_t expected = kNoSuccessor;
            behind = __atomic_compare_exchange_n(&stages[ahead].successor, &expected, slot, false, __ATOMIC_ACQ_REL,
                                                 __ATOMIC_ACQUIRE);
            if (!behind) {
                --stage.inputs;
                --stage.hardware;
            }
        }
        if (!own && !behind) {
            stage.inputs = 0;
            stage.successor = kInputsReady;
            return switch_host1x_fence_add(&stage.fence, boundUs);
        }
        if (const int error = switch_host1x_fence_add(&stage.fence, 0))
            return error;
        for (uint32_t i = 0; i < producers.count; ++i) {
            auto &input = stage.produced[i];
            input.slot = slot;
            switch_host1x_fence_init(&input.fence, producers.tokens[i].queue,
                                     static_cast<uint32_t>(producers.tokens[i].value), produced);
            if (const int error = switch_host1x_fence_add(&input.fence, 0))
                return error;
        }
        inputStep(stage);
        return 0;
    }

    // After the engine's lock: the bridge's lock is held across a GPU submission's wait for channel room.
    static int notify(uint32_t slot, const switch_gpu_bridge *bridge)
    {
        return switch_gpu_bridge_notify(bridge, bridged, &stages[slot]);
    }

    static void semantic(uint32_t slot)
    {
        finalStep(stages[slot]);
    }

    // A host1x filter waiter's read (any context, no lock, host1x up): the job's syncpoint is at its fence, whose
    // callback has not run, its bridge is done and nothing failed. Its semantic step is the engine's to judge.
    static bool fenceReached(uint32_t slot)
    {
        const auto &stage = stages[slot];
        if (static_cast<int32_t>(tegra_host1x_syncpt_read(stage.fence.syncpt) - stage.fence.threshold) < 0)
            return false;
        // The syncpoint before the stage words: a cancel stores its failure before the CPU releases the syncpoint.
        __asm__ volatile("dmb sy" ::: "memory");
        return __atomic_load_n(&stage.hardware, __ATOMIC_ACQUIRE) == 1 &&
               __atomic_load_n(&stage.physical, __ATOMIC_ACQUIRE) == 1;
    }

    // Under the engine's lock, once the engine has stopped (its channel torn down, the engine reset or powered off): a
    // job whose fence the syncpoint has not reached fails. True: the engine records the failure before it releases
    // the fence, which another engine's stream may be waiting on.
    static bool cancel(uint32_t slot, uint32_t reached)
    {
        auto &stage = stages[slot];
        if (static_cast<int32_t>(reached - stage.fence.threshold) >= 0)
            return false;
        __atomic_store_n(&stage.physical, kCancelled, __ATOMIC_RELEASE);
        return true;
    }

private:
    static constexpr uint32_t kNoSuccessor = Slots, kInputsReady = Slots + 1;

    static uint32_t slotOf(const SwitchHost1xStage &stage)
    {
        return static_cast<uint32_t>(&stage - stages);
    }

    static void signaled(switch_host1x_fence *fence, int status)
    {
        auto &stage = *reinterpret_cast<SwitchHost1xStage *>(fence);
        if (status) {
            const int producers = __atomic_exchange_n(&stage.physical, kCancelled, __ATOMIC_ACQ_REL);
            Engine::fault(slotOf(stage), status, producers);
        }
        hardwareStep(stage);
    }

    static void bridged(void *context, int status)
    {
        auto &stage = *static_cast<SwitchHost1xStage *>(context);
        if (status < 0 && __atomic_load_n(&stage.physical, __ATOMIC_ACQUIRE) != kCancelled)
            __atomic_store_n(&stage.physical, status, __ATOMIC_RELEASE);
        inputStep(stage);
        hardwareStep(stage);
    }

    // A host1x producer's fence: reached, or released by the CPU when its engine cancelled it. The producer's failure
    // is folded in here already, ahead of the job's own fence: a host1x filter waiter reads the job's physical result
    // with nothing but that fence left (switch_gpu_backing_token_filter_status), before hardwareStep's fold.
    static void produced(switch_host1x_fence *fence, int)
    {
        auto &stage = stages[reinterpret_cast<SwitchHost1xInputFence *>(fence)->slot];
        foldProducerFailure(stage);
        inputStep(stage);
        hardwareStep(stage);
    }

    // The last input starts the bound, and is the next job's input from the job ahead of it.
    static void inputStep(SwitchHost1xStage &stage)
    {
        if (__atomic_sub_fetch(&stage.inputs, 1u, __ATOMIC_ACQ_REL))
            return;
        switch_host1x_fence_watch(&stage.fence, stage.boundUs);
        const uint32_t next = __atomic_exchange_n(&stage.successor, kInputsReady, __ATOMIC_ACQ_REL);
        if (next < Slots) {
            inputStep(stages[next]);
            hardwareStep(stages[next]);
        }
    }

    static void foldProducerFailure(SwitchHost1xStage &stage)
    {
        int ran = 1;
        if (__atomic_load_n(&stage.physical, __ATOMIC_ACQUIRE) == ran)
            if (const int failed = stage.producers.failure())
                __atomic_compare_exchange_n(&stage.physical, &ran, failed, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    static void hardwareStep(SwitchHost1xStage &stage)
    {
        if (__atomic_sub_fetch(&stage.hardware, 1u, __ATOMIC_ACQ_REL))
            return;
        foldProducerFailure(stage);
        Engine::physical(slotOf(stage), stage.physical);
        finalStep(stage);
    }

    static void finalStep(SwitchHost1xStage &stage)
    {
        if (__atomic_sub_fetch(&stage.final, 1u, __ATOMIC_ACQ_REL))
            return;
        Engine::ended(slotOf(stage), stage.physical);
    }
};
