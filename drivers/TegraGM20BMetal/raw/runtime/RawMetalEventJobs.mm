// SPDX-License-Identifier: MIT
#import "RawMetalEventJobs.h"
#import "RawMetalInternal.h"
#include "raw_event_job.h"
#include "raw_order.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static std::atomic<uint64_t> gSignalJobs{0}, gWaitJobs{0}, gPromiseWaits{0};

uint64_t eventJobsSubmitted()
{
    return gSignalJobs.load(std::memory_order_relaxed);
}

uint64_t waitJobsSubmitted()
{
    return gWaitJobs.load(std::memory_order_relaxed);
}

uint64_t promiseWaitsRetired()
{
    return gPromiseWaits.load(std::memory_order_relaxed);
}
#endif

static raw::EventRangeView viewOf(RMCommandBuffer *cb, size_t at)
{
    const RMSegment &range = cb->_segs[at];
    raw::EventRangeView view;
    view.event = range.event;
    view.carried = range.gpuSignal;
    if (range.event) {
        const Cmd c = cb->_plan.legacy(range.from);
        view.signal = c.op == OpSignalEvent;
        view.leasable = view.signal && sharedEventLease((__bridge id)c.obj) != nullptr;
    }
    return view;
}

bool signalOrderedOnGpu(RMCommandBuffer *cb, const RMSegment &range)
{
    if (!range.event || range.gpuSignal)
        return false;
    const Cmd c = cb->_plan.legacy(range.from);
    if (c.op != OpSignalEvent || !sharedEventLease((__bridge id)c.obj))
        return false;
    std::lock_guard<raw::Mutex> m(gRM->m2vLock);
    return gRM->gpuOrder;
}

// `setRefused`: the job's resource set refusal (raw::Device::setRefused), empty for none
static void eventJobFailure(RMCommandBuffer *cb, int result, const std::string &setRefused = {})
{
    NSString *why =
        setRefused.empty()
            ? [NSString
                  stringWithFormat:@"The kernel refused an event job (0x%x): a producer failed or is lost", result]
            : [NSString stringWithUTF8String:setRefused.c_str()];
    std::lock_guard<raw::Mutex> lock(cb->_lock);
    if (!cb->_segError)
        cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                            code:MTLCommandBufferErrorInternal
                                        userInfo:@{NSLocalizedDescriptionKey : why}];
}

// The leasable signals no job carries from `at` (raw::signalJobRanges), with their events
static std::vector<size_t> carriedSignals(RMCommandBuffer *cb, size_t at, std::vector<TegraGM20BAbiEventUse> &signals,
                                          std::vector<std::shared_ptr<raw::SharedEventLease>> &leases)
{
    std::vector<raw::EventRangeView> views(at);
    for (size_t i = at; i < cb->_segs.size() && cb->_segs[i].event; i++)
        views.push_back(viewOf(cb, i));
    const std::vector<size_t> carried = raw::signalJobRanges(views, at);
    for (size_t i : carried) {
        const Cmd c = cb->_plan.legacy(cb->_segs[i].from);
        leases.push_back(sharedEventLease((__bridge id)c.obj));
        signals.push_back({leases.back()->handle(), c.c});
    }
    return carried;
}

// `waits` then `signals` in jobs of no pushes, ordered after the m2v work committed before them and checked against this
// channel's producers; the head range then retires with the last job
static RMEventJob submitJobs(RMCommandBuffer *cb, const std::vector<TegraGM20BAbiEventUse> &waits,
                             const std::vector<TegraGM20BAbiEventUse> &signals, const std::vector<size_t> &carried)
{
    RawState &r = *gRM;
    const size_t head = cb->_segNext;
    const bool ordering = r.gpuOrder && r.m2vSinceRaw;
    if (ordering) {
        uint32_t acquires = 0;
        const int kr = r.raw->orderAfterOthers(acquires);
        switch (raw::orderAnswer(kr)) {
        case raw::OrderAnswer::ordered:
            break;
        case raw::OrderAnswer::refused:
            orderRefused("event job after m2v", kr);
            return RMEventJob::Retry;
        case raw::OrderAnswer::noChannel:
            eventJobFailure(cb, kr);
            return RMEventJob::Retry;
        }
        RAW_DIAGNOSTIC(r.stats.acquires += acquires);
    }
    const uint64_t generation = r.raw->channelGeneration();
    if (validateNativeEventWaits(cb, generation) != raw::EventDependencyState::ready)
        return RMEventJob::Retry;
    uint32_t seqno = r.raw->submitted();
    for (const raw::EventJobSlice &slice : raw::eventJobSlices(waits.size(), signals.size(), TegraGM20BMaxJobEvents)) {
        int result = KERN_SUCCESS;
        const raw::JobEvents events{slice.waits ? waits.data() + slice.firstWait : nullptr,
                                    slice.signals ? signals.data() + slice.firstSignal : nullptr, slice.waits,
                                    slice.signals};
        if (!r.raw->eventJob(events, seqno, &result, generation)) {
            if (raw::eventJobAnswer(result) != raw::EventJobAnswer::reordered)
                eventJobFailure(cb, result, r.raw->setRefused());
            return RMEventJob::Retry;
        }
    }
    if (ordering)
        r.m2vSinceRaw = false;
    cb->_sharedSignalRanges = carried;
    publishSharedEvents(cb);
    cb->_seqno = seqno;
    cb->_channelGeneration = r.raw->channelGeneration();  // the job's: eventJob makes a channel the kernel killed again
    r.rawSinceM2v = true;
    cb->_rangeQuiet = false;
    retireRawRange(cb, cb->_segs[head].from, cb->_segs[head].to);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gSignalJobs.fetch_add(!signals.empty(), std::memory_order_relaxed);
    gWaitJobs.fetch_add(!waits.empty(), std::memory_order_relaxed);
    gPromiseWaits.fetch_add(waits.empty() && signals.empty(), std::memory_order_relaxed);
#endif
    return RMEventJob::Submitted;
}

RMEventJob submitEventJobs(RMCommandBuffer *cb)
{
    std::vector<TegraGM20BAbiEventUse> signals;
    std::vector<std::shared_ptr<raw::SharedEventLease>> leases;
    const std::vector<size_t> carried = carriedSignals(cb, cb->_segNext, signals, leases);
    if (carried.empty())
        return RMEventJob::None;
    return submitJobs(cb, {}, signals, carried);
}

RMEventJob submitWaitJob(RMCommandBuffer *cb)
{
    const size_t next = cb->_segNext + 1;
    if (next < cb->_segs.size() &&
        (cb->_segs[next].raw || (cb->_segs[next].event && cb->_plan.legacy(cb->_segs[next].from).op == OpWaitEvent)))
        return RMEventJob::None;
    std::vector<TegraGM20BAbiEventUse> waits, signals;
    for (const auto &wait : cb->_sharedEventWaits)
        waits.push_back({wait.lease->handle(), wait.value});
    std::vector<std::shared_ptr<raw::SharedEventLease>> leases;
    const std::vector<size_t> carried = carriedSignals(cb, next, signals, leases);
    if (waits.empty() && cb->_gpuEventWaits.empty())
        return RMEventJob::None;
    return submitJobs(cb, waits, signals, carried);
}
