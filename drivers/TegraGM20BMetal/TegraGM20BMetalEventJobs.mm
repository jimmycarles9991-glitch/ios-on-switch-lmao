// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalEventJobs.h"
#import "TegraGM20BMetalInternal.h"
#include "event_job.h"

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

static gm20b::EventRangeView viewOf(SwitchMetalCommandBuffer *cb, size_t at)
{
    const SwitchMetalSegment &range = cb->_segs[at];
    gm20b::EventRangeView view;
    view.event = range.event;
    view.carried = range.gpuSignal;
    if (range.event) {
        const Cmd c = cb->_plan.legacy(range.from);
        view.signal = c.op == OpSignalEvent;
        view.leasable = view.signal && sharedEventLease((__bridge id)c.obj) != nullptr;
    }
    return view;
}

bool signalOrderedOnGpu(SwitchMetalCommandBuffer *cb, const SwitchMetalSegment &range)
{
    if (!range.event || range.gpuSignal)
        return false;
    const Cmd c = cb->_plan.legacy(range.from);
    return c.op == OpSignalEvent && sharedEventLease((__bridge id)c.obj);
}

// `setRefused`: the job's resource set refusal (gm20b::Device::setRefused), empty for none
static void eventJobFailure(SwitchMetalCommandBuffer *cb, int result, const std::string &setRefused = {})
{
    NSString *why =
        setRefused.empty()
            ? [NSString
                  stringWithFormat:@"The kernel refused an event job (0x%x): a producer failed or is lost", result]
            : [NSString stringWithUTF8String:setRefused.c_str()];
    std::lock_guard<gm20b::UnfairMutex> lock(cb->_lock);
    if (!cb->_segError)
        cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                            code:MTLCommandBufferErrorInternal
                                        userInfo:@{NSLocalizedDescriptionKey : why}];
}

// The leasable signals no job carries from `at` (gm20b::signalJobRanges), with their events
static std::vector<size_t> carriedSignals(SwitchMetalCommandBuffer *cb, size_t at,
                                          std::vector<TegraGM20BAbiEventUse> &signals,
                                          std::vector<std::shared_ptr<gm20b::SharedEventLease>> &leases)
{
    std::vector<gm20b::EventRangeView> views(at);
    for (size_t i = at; i < cb->_segs.size() && cb->_segs[i].event; i++)
        views.push_back(viewOf(cb, i));
    const std::vector<size_t> carried = gm20b::signalJobRanges(views, at);
    for (size_t i : carried) {
        const Cmd c = cb->_plan.legacy(cb->_segs[i].from);
        leases.push_back(sharedEventLease((__bridge id)c.obj));
        signals.push_back({leases.back()->handle(), c.c});
    }
    return carried;
}

// `waits` then `signals` in jobs of no pushes, checked against this channel's producers; the head range then retires with
// the last job
static SwitchMetalEventJob submitJobs(SwitchMetalCommandBuffer *cb, const std::vector<TegraGM20BAbiEventUse> &waits,
                                      const std::vector<TegraGM20BAbiEventUse> &signals,
                                      const std::vector<size_t> &carried)
{
    GM20BState &r = *gRM;
    const size_t head = cb->_segNext;
    const uint64_t generation = r.raw->channelGeneration();
    if (validateNativeEventWaits(cb, generation) != gm20b::EventDependencyState::ready)
        return SwitchMetalEventJob::Retry;
    uint32_t seqno = r.raw->submitted();
    for (const gm20b::EventJobSlice &slice :
         gm20b::eventJobSlices(waits.size(), signals.size(), TegraGM20BMaxJobEvents)) {
        int result = KERN_SUCCESS;
        const gm20b::JobEvents events{slice.waits ? waits.data() + slice.firstWait : nullptr,
                                      slice.signals ? signals.data() + slice.firstSignal : nullptr, slice.waits,
                                      slice.signals};
        if (!r.raw->eventJob(events, seqno, &result, generation)) {
            if (gm20b::eventJobAnswer(result) != gm20b::EventJobAnswer::reordered)
                eventJobFailure(cb, result, r.raw->setRefused());
            return SwitchMetalEventJob::Retry;
        }
    }
    cb->_sharedSignalRanges = carried;
    publishSharedEvents(cb);
    cb->_seqno = seqno;
    cb->_channelGeneration = r.raw->channelGeneration();  // the job's: eventJob makes a channel the kernel killed again
    cb->_rangeQuiet = false;
    retireRawRange(cb, cb->_segs[head].from, cb->_segs[head].to);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gSignalJobs.fetch_add(!signals.empty(), std::memory_order_relaxed);
    gWaitJobs.fetch_add(!waits.empty(), std::memory_order_relaxed);
    gPromiseWaits.fetch_add(waits.empty() && signals.empty(), std::memory_order_relaxed);
#endif
    return SwitchMetalEventJob::Submitted;
}

SwitchMetalEventJob submitEventJobs(SwitchMetalCommandBuffer *cb)
{
    std::vector<TegraGM20BAbiEventUse> signals;
    std::vector<std::shared_ptr<gm20b::SharedEventLease>> leases;
    const std::vector<size_t> carried = carriedSignals(cb, cb->_segNext, signals, leases);
    if (carried.empty())
        return SwitchMetalEventJob::None;
    return submitJobs(cb, {}, signals, carried);
}

SwitchMetalEventJob submitWaitJob(SwitchMetalCommandBuffer *cb)
{
    const size_t next = cb->_segNext + 1;
    if (next < cb->_segs.size() && (!cb->_segs[next].event || cb->_plan.legacy(cb->_segs[next].from).op == OpWaitEvent))
        return SwitchMetalEventJob::None;
    std::vector<TegraGM20BAbiEventUse> waits, signals;
    for (const auto &wait : cb->_sharedEventWaits)
        waits.push_back({wait.lease->handle(), wait.value});
    std::vector<std::shared_ptr<gm20b::SharedEventLease>> leases;
    const std::vector<size_t> carried = carriedSignals(cb, next, signals, leases);
    if (waits.empty() && cb->_gpuEventWaits.empty())
        return SwitchMetalEventJob::None;
    return submitJobs(cb, waits, signals, carried);
}
