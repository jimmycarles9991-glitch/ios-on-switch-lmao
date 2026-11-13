// SPDX-License-Identifier: BSD-2-Clause-Patent
// What each NVENC job reads that another engine's job wrote: VIC's input picture, or a native picture's NVDEC and VIC
// producers. The job's stream waits for them by syncpoint, and a producer its engine cancelled reaches its syncpoint
// all the same (the CPU releases it), so the job's Wait answers with the producer's failure instead of the frame coded
// from memory nobody wrote. The cancelled job is recorded failed before that release; a semantic failure counts once
// it is retired.
//
// The same producers decide when NVENC's stall bound counts. NVENC runs its jobs in order, each behind the producers
// its stream waits for, so a wait on a fence whose jobs still have a producer running is held up upstream, not
// stalled: the producer's own bound cancels the producer, which releases its syncpoint and NVENC's stream. wait()
// gives NVENC's bound a full round once every input is in, and only a round that began that way and ran out is
// NVENC's stall.
#pragma once
#include <IOKit/IOLocks.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

class TegraNVENCInputs {
public:
    static constexpr uint32_t kJobs = 16;  // the queue's 4 and the fences a client may still wait for after them
    static constexpr uint32_t kProducers = 5;

    bool start()
    {
        lock_ = IOLockAlloc();
        return lock_ != nullptr;
    }

    // Under the engine lock, once the job is queued and before its fence is handed out.
    void record(uint32_t fence, const switch_gpu_backing_token *producers, uint32_t count)
    {
        Job job = {fence, 0, {}};
        for (uint32_t i = 0; i < count && job.count < kProducers; ++i)
            if (producers[i].engine == SWITCH_GPU_ENGINE_VIC || producers[i].engine == SWITCH_GPU_ENGINE_NVDEC)
                job.producers[job.count++] = producers[i];
        IOLockLock(lock_);
        jobs_[fence % kJobs] = job;
        IOLockUnlock(lock_);
    }

    // Once the job's fence is reached: 0, or the first failure of what it read. A job kJobs fences back is no longer
    // known and reads as 0.
    int failure(uint32_t fence) const
    {
        IOLockLock(lock_);
        const Job job = jobs_[fence % kJobs];
        IOLockUnlock(lock_);
        if (job.fence != fence)
            return 0;
        for (uint32_t i = 0; i < job.count; ++i) {
            const int physical = switch_gpu_backing_token_physical_status(&job.producers[i], 0);
            const int result = physical < 0 ? physical : switch_gpu_backing_token_status(&job.producers[i]);
            if (result < 0 && result != SWITCH_GPU_BACKING_STALE)
                return result;
        }
        return 0;
    }

    // The jobs after `reached` up to `fence`: true once every producer they read has run or failed on its engine.
    bool ready(uint32_t reached, uint32_t fence) const
    {
        bool in = true;
        each(reached, fence, [&](const switch_gpu_backing_token &producer) {
            in = in && switch_gpu_backing_token_physical_status(&producer, 0) != 0;
        });
        return in;
    }

    // Sleeps until those producers have ended on their engines, as each one's own bound and cancellation see to.
    void awaitProducers(uint32_t reached, uint32_t fence) const
    {
        each(reached, fence, [](const switch_gpu_backing_token &producer) {
            if (!switch_gpu_backing_token_physical_status(&producer, 0))
                (void)switch_gpu_backing_token_wait(&producer);
        });
    }

    // NVENC's bounded wait for `fence` (bounded() returns `timedOut` once its bound ran out), its bound counted from
    // when the inputs are in.
    template <class Bounded> int wait(uint32_t reached, uint32_t fence, int timedOut, Bounded bounded) const
    {
        for (;;) {
            const bool in = ready(reached, fence);
            const int result = bounded();
            if (result != timedOut || in)
                return result;
            awaitProducers(reached, fence);
        }
    }

private:
    template <class Visit> void each(uint32_t reached, uint32_t fence, Visit visit) const
    {
        const auto queued = static_cast<int32_t>(fence - reached);
        for (int32_t k = queued > static_cast<int32_t>(kJobs) ? queued - static_cast<int32_t>(kJobs) : 0; k < queued;
             ++k) {
            const uint32_t at = reached + 1 + static_cast<uint32_t>(k);
            IOLockLock(lock_);
            const Job job = jobs_[at % kJobs];
            IOLockUnlock(lock_);
            for (uint32_t i = 0; job.fence == at && i < job.count; ++i)
                visit(job.producers[i]);
        }
    }

    struct Job {
        uint32_t fence, count;
        switch_gpu_backing_token producers[kProducers];
    };

    IOLock *lock_ = nullptr;
    Job jobs_[kJobs] = {};
};
