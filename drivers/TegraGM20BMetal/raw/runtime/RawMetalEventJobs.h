// SPDX-License-Identifier: MIT
// Event operations no raw range's job carries, in jobs of no work on the raw channel (raw_event_job.h). Submit thread only.
#pragma once
#import "RawMetalObjects.h"

// The head range is a kernel event's signal no job carries yet, with the kernel ordering this channel after m2v's: the
// scheduler submits it once the m2v work before it is scheduled (m2vClear), not once that work completed
bool signalOrderedOnGpu(RMCommandBuffer *cb, const RMSegment &range);

enum class RMEventJob {
    None,       // nothing for a job: a signal of an event the kernel does not hold (its value is set on the CPU)
    Submitted,  // the head range's signal and the ones right after it went in jobs
    Retry,      // not submitted now (the kernel refused the Order, or the command buffer failed): visit it again
};

// At a head signal range no job carries: it and the leasable signals right after it
RMEventJob submitEventJobs(RMCommandBuffer *cb);

// At the last of a run of head waits the GPU takes (RMSegment::gpuWait) that no raw range follows (signals, m2v work or the
// command buffer's end come next): its producers' waits and the leasable signals right after it, so the queue is not held
// on the CPU. A wait only on this channel's earlier work (a promise) needs no job: the range retires with the channel's
// last submission, which is after its producer. None: nothing for the GPU (a value reached, or the next range's job takes
// the waits).
RMEventJob submitWaitJob(RMCommandBuffer *cb);

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's metal.native.shared_event and metal.native.event_waits: the jobs made since the device opened that signal,
// that wait, and the waits retired with the channel's last submission
uint64_t eventJobsSubmitted();
uint64_t waitJobsSubmitted();
uint64_t promiseWaitsRetired();
#endif
