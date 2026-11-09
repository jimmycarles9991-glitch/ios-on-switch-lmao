// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// ---- commit: order between the two drivers ----
// With the kernel's Order (TegraGM20BABI.h method 14), the two drivers' command buffers are ordered on the GPU: before a raw
// submission that follows m2v work, one Order(AfterTask) makes its first entry a semaphore acquire on what NVK's channels had
// submitted; before an m2v commit that follows raw work, one Order(BeforeTask) makes NVK's channels' next submissions start with
// an acquire on the raw channel's last seqno. Neither thread waits for the GPU. The one CPU handoff left: a raw command buffer is
// submitted once the m2v ones before it are scheduled (NVK's Exec made, so the AfterTask acquire covers them) - a wait for the
// m2v runtime's own submission of work the GPU is already running, not for that work. What the GPU order cannot cover is
// ordered on the CPU, each with a one-shot line naming it: a kernel without Order, an Order the kernel refuses, a shared event
// signalled outside this device's submitted work.

// What a raw command buffer waits for before it is submitted: GPU order, the m2v command buffers before it scheduled; CPU
// order, completed. m2vLock held.
static bool m2vClear()
{
    return gRM->gpuOrder ? gRM->m2vUnscheduled == 0 : gRM->m2vPending == 0;
}

// The kernel refused an Order: from here on the drivers are ordered on the CPU (one line).
void orderRefused(const char *what, int kr)
{
    once([NSString stringWithFormat:@"CPU-ordered from now on: the kernel refused Order (%s, 0x%x)", what, kr]);
    std::lock_guard<std::mutex> m(gRM->m2vLock);
    gRM->gpuOrder = false;
}

static void countDriver(int driver)
{
    RawState &r = *gRM;
    if (r.lastDriver && r.lastDriver != driver)
        r.stats.switches++;
    r.lastDriver = driver;
}

// Submits `cb` now: the caller holds gRM->submit and everything committed before it is submitted. false: not yet (a raw
// command buffer whose Order was refused just now waits for the m2v work before it to complete; the caller keeps it first).
// Diagnostics: each predicate `cb` hit, once (and once more when it was the only one)
static void countPreds(RMCommandBuffer *cb)
{
    const uint64_t m = cb->_preds;
    if (!m)
        return;
    const bool only = !(m & (m - 1));
    for (uint64_t left = m; left; left &= left - 1) {
        const int p = __builtin_ctzll(left);
        gPredCbs[p].fetch_add(1, std::memory_order_relaxed);
        if (only)
            gPredOnly[p].fetch_add(1, std::memory_order_relaxed);
    }
}

#if SWITCH_FRAME_TIMELINE
// The m2v replay's parts for the rates line: the wait for an m2v command buffer (its queue's slots), the twins made, and the
// replay's encode end to its commit's return (the Order call, a CPU-order wait, m2v's commit)
static void noteReplay(const switch_frame::Stamps &f, uint64_t returned)
{
    using namespace switch_frame;
    Stats &s = gRM->stats;
    const uint64_t perMs = ticksPerMs();
    auto us = [perMs](uint64_t from, uint64_t to) {
        return to > from ? (to - from) * 1000 / perMs : 0;
    };
    s.replaySlotUs += us(f.get(SlotStart), f.get(SlotEnd));
    s.replayTwinUs += f.get(Twins) * 1000 / perMs;
    s.replayCommitUs += us(f.get(ReplayEnd), returned);
    const uint64_t total = us(f.get(ReplayStart), returned);
    uint64_t seen = s.replayMaxUs.load(std::memory_order_relaxed);
    while (total > seen && !s.replayMaxUs.compare_exchange_weak(seen, total, std::memory_order_relaxed)) {
    }
}
#endif

static bool submitSegments(RMCommandBuffer *cb);

// A command buffer part of whose raw submission ran before the kernel refused the rest: replayed on m2v, that part would run
// twice. It completes with an error, as Metal's does when its GPU work fails.
static void finishPartial(RMCommandBuffer *cb)
{
    once(@"a command buffer whose raw submission the kernel refused after part of it ran completes with an error (not "
         @"replayed: that part would run twice)");
    finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:nil]);
}

static bool submitLocked(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    if (!cb->_segs.empty())
        return submitSegments(cb);
    const char *why = nullptr;
    if (cb->_reason == WhyRaw) {
        if (r.gpuOrder && r.m2vSinceRaw) {  // its first entry: an acquire on what NVK's channels submitted
            uint32_t waits = 0;
            const int kr = r.raw->orderAfterOthers(waits);
            if (kr == KERN_SUCCESS) {
                r.m2vSinceRaw = false;
                r.stats.acquires += waits;
            } else {
                orderRefused("raw after m2v", kr);
                std::lock_guard<std::mutex> m(r.m2vLock);
                if (r.m2vPending)
                    return false;
            }
        }
        const uint64_t began = nowUs();
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::EncodeStart);
        switch_frame::trace(switch_frame::kRawEncode, switch_frame::kStart, cb->_frame.seq);
#endif
        const bool encoded = encodeRaw(cb, 0, cb->_tape.size(), &why);
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::EncodeEnd);
        switch_frame::trace(switch_frame::kRawEncode, switch_frame::kEnd, cb->_frame.seq, encoded);
#endif
        if (encoded) {
            r.stats.encodeUs += nowUs() - began;
            r.stats.raw++;
            r.rawSinceM2v = true;
            countDriver(1);
            noteSignals(cb, r.rawSignals);
            {
                std::lock_guard<std::mutex> l(r.doneLock);
                r.inflight.push_back(cb);
            }
            r.doneChanged.notify_one();
            scheduled(cb);
            return true;
        }
        r.stats.aborted++;
        r.stats.abortUs += nowUs() - began;
        if (cb->_partial) {
            finishPartial(cb);
            return true;
        }
        cb->_reason = WhyEncode;
        cb->_why = [NSString stringWithUTF8String:why ?: "?"];
    }
    r.stats.m2v++;
    r.stats.reasons[cb->_reason]++;
    countPreds(cb);
    // one line per reason kind (no string per command buffer: the predicates' own lines carry the values)
    SWITCH_REFUSE_DETAIL("raw", "cb.m2v", cb->_reason, "command buffer on m2v (%s): %s [first predicate %s]",
                         kReasonNames[cb->_reason], cb->_why.UTF8String ?: "-", kPredNames[cb->_pred0]);
    cb->_m2v = cb->_signalsOnM2v = true;
    const uint64_t replayBegan = nowUs();
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::ReplayStart);
    switch_frame::trace(switch_frame::kReplay, switch_frame::kStart, cb->_frame.seq);
    tTwinTicks = 0;
#endif
    id<MTLCommandBuffer> m = r.fallback ? replay(cb, 0, cb->_tape.size(), false) : nil;
#if SWITCH_FRAME_TIMELINE
    cb->_frame.mark(switch_frame::ReplayEnd);
    cb->_frame.set(switch_frame::Twins, tTwinTicks);
    switch_frame::trace(switch_frame::kReplay, switch_frame::kEnd, cb->_frame.seq, tTwinTicks);
#endif
    if (!m) {
        finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorInternal
                                   userInfo:nil]);
        return true;
    }
    // after the raw work before it: NVK's next submissions start with an acquire on the raw channel (GPU order), or that work
    // completes first (CPU order: a kernel wait on the fence interrupt)
    if (r.gpuOrder && r.rawSinceM2v) {
        uint32_t waits = 0;
        const int kr = r.raw->orderOthersAfter(waits);
        if (kr == KERN_SUCCESS)
            r.stats.acquires += waits;
        else
            orderRefused("m2v after raw", kr);
    }
    if (!r.gpuOrder && !r.raw->done(r.raw->submitted())) {
        r.stats.cpuOrdered++;
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::OrderWaitStart);
        switch_frame::trace(switch_frame::kOrderWait, switch_frame::kStart, cb->_frame.seq);
#endif
        r.raw->wait(r.raw->submitted());
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::OrderWaitEnd);
        switch_frame::trace(switch_frame::kOrderWait, switch_frame::kEnd, cb->_frame.seq);
#endif
    }
    r.rawSinceM2v = false;
    r.m2vSinceRaw = true;
    countDriver(2);
    noteSignals(cb, r.m2vSignals);
    __block bool counted = false;  // m2vLock: it left m2vUnscheduled (scheduled, or completed without that)
    {
        std::lock_guard<std::mutex> l(r.m2vLock);
        r.m2vPending++;
        r.m2vUnscheduled++;
    }
    [m addScheduledHandler:^(id<MTLCommandBuffer>) {
        {
            std::lock_guard<std::mutex> l(gRM->m2vLock);
            if (!counted)
                gRM->m2vUnscheduled--;
            counted = true;
        }
        gRM->m2vChanged.notify_all();
        scheduled(cb);
    }];
    [m addCompletedHandler:^(id<MTLCommandBuffer> done) {
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::Fence);
        switch_frame::trace(switch_frame::kFence, switch_frame::kPoint, cb->_frame.seq);
#endif
        finish(cb, done.error);
        {
            std::lock_guard<std::mutex> l(gRM->m2vLock);
            if (!counted)
                gRM->m2vUnscheduled--;  // a refused commit never schedules
            counted = true;
            gRM->m2vPending--;
        }
        gRM->m2vChanged.notify_all();
    }];
    [m commit];
    r.stats.replayUs += nowUs() - replayBegan;
#if SWITCH_FRAME_TIMELINE
    noteReplay(cb->_frame, cb->_frame.mark(switch_frame::Returned));
    switch_frame::trace(switch_frame::kReplayCommitted, switch_frame::kPoint, cb->_frame.seq);
#endif
    return true;
}

// A command buffer some of whose encoders go to m2v and some do not, with no refusal of its own: its segments (each run of
// encoders of one driver), else none (it goes whole, as its reason says). m2v replays only the encoders it must: a replayed draw
// costs m2v's encode (~0.28 ms each on the device, 11-06), so a whole frame for one refused pass cost tens of ms.
// Its shared event waits before its first encoder are the first segment's (its range starts at 0: an m2v one encodes them, a raw
// one is submitted once they are covered, eventsReady/waitEvents), and its signals after its last encoder the last segment's (an
// m2v one encodes them, on m2v's timeline as a whole replay would; a raw one's are made as the command buffer completes, finish).
// A wait or signal between encoders keeps it whole.
static void planSegments(RMCommandBuffer *cb)
{
    cb->_segs.clear();
    cb->_segNext = 0;
    [cb closeEncoder];
    if (cb->_reason == WhyRaw || cb->_whole)
        return;
    bool raw = false, m2v = false, events = false, between = false, signalSeen = false;
    for (size_t at = 0; at < cb->_tape.size(); at++) {
        const Cmd &c = cb->_tape[at];
        if (c.op == OpInvocation && c.stage == 0xff) {
            cb->_segs.clear();
            return;
        }
        if (c.op == OpWaitEvent || c.op == OpSignalEvent) {
            events = true;
            between |= c.op == OpWaitEvent && !cb->_segs.empty();
            signalSeen |= c.op == OpSignalEvent;
            continue;
        }
        if (c.op != OpRender && c.op != OpBlit && c.op != OpCompute) {
            if (cb->_segs.empty())
                return;  // a command before the first encoder
            continue;
        }
        between |= signalSeen;
        const bool r = !c.index;
        (r ? raw : m2v) = true;
        if (!cb->_segs.empty())
            cb->_segs.back().to = uint32_t(at);
        if (!cb->_segs.empty() && cb->_segs.back().raw == r)
            continue;
        cb->_segs.push_back({uint32_t(cb->_segs.empty() ? 0 : at), uint32_t(cb->_tape.size()), r});
    }
    if (!cb->_segs.empty())
        cb->_segs.back().to = uint32_t(cb->_tape.size());
    if (raw && m2v && between)
        gRM->stats.eventWhole++;
    if (!raw || !m2v || between)
        cb->_segs.clear();
    else if (events)
        gRM->stats.eventSplits++;
}

// Whether what `cb` submits next is raw work (it needs the m2v work before it scheduled: m2vClear)
static bool nextRaw(RMCommandBuffer *cb)
{
    return cb->_segs.empty() ? cb->_reason == WhyRaw : cb->_segNext < cb->_segs.size() && cb->_segs[cb->_segNext].raw;
}

// Whether what `cb` submits next is raw work that carries its shared event waits: its first submission (planSegments)
static bool nextRawWaits(RMCommandBuffer *cb)
{
    return nextRaw(cb) && cb->_segNext == 0;
}

// submit lock held: a split command buffer's segments from the next one, in order, each as a command buffer of its driver would
// go (submitLocked), the scheduled and completed handlers with the last. false: the next is raw and the m2v work before it is not
// scheduled yet (it waits on the submit thread, as a raw command buffer after m2v work does).
static bool submitSegments(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    if (cb->_segNext == 0) {
        r.stats.splits++;
        countPreds(cb);
    }
    while (cb->_segNext < cb->_segs.size()) {
        RMSegment &s = cb->_segs[cb->_segNext];
        const bool last = cb->_segNext + 1 == cb->_segs.size();
        if (s.raw) {
            {
                std::lock_guard<std::mutex> m(r.m2vLock);
                if (!m2vClear())
                    return false;
            }
            if (r.gpuOrder && r.m2vSinceRaw) {
                uint32_t waits = 0;
                const int kr = r.raw->orderAfterOthers(waits);
                if (kr == KERN_SUCCESS) {
                    r.m2vSinceRaw = false;
                    r.stats.acquires += waits;
                } else {
                    orderRefused("raw after m2v", kr);
                    std::lock_guard<std::mutex> m(r.m2vLock);
                    if (r.m2vPending)
                        return false;
                }
            }
            const char *why = nullptr;
            const uint64_t began = nowUs();
            if (!encodeRaw(cb, s.from, s.to, &why)) {
                if (cb->_partial) {
                    finishPartial(cb);
                    return true;
                }
                s.raw = false;  // the kernel refused it: this segment on m2v
                continue;
            }
            r.stats.encodeUs += nowUs() - began;
            r.rawSinceM2v = true;
            countDriver(1);
            if (last) {
                noteSignals(cb, r.rawSignals);
                {
                    std::lock_guard<std::mutex> l(r.doneLock);
                    r.inflight.push_back(cb);
                }
                r.doneChanged.notify_one();
                scheduled(cb);
            }
        } else {
            r.stats.splitM2v++;
            const uint64_t began = nowUs();
#if SWITCH_FRAME_TIMELINE
            cb->_frame.mark(switch_frame::ReplayStart);
            tTwinTicks = 0;
#endif
            id<MTLCommandBuffer> m = r.fallback ? replay(cb, s.from, s.to, true) : nil;
#if SWITCH_FRAME_TIMELINE
            cb->_frame.mark(switch_frame::ReplayEnd);
            cb->_frame.set(switch_frame::Twins, tTwinTicks);
#endif
            if (!m) {
                cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                                    code:MTLCommandBufferErrorInternal
                                                userInfo:nil];
                if (last)
                    finish(cb, cb->_segError);
                cb->_segNext++;
                continue;
            }
            if (r.gpuOrder && r.rawSinceM2v) {
                uint32_t waits = 0;
                const int kr = r.raw->orderOthersAfter(waits);
                if (kr == KERN_SUCCESS)
                    r.stats.acquires += waits;
                else
                    orderRefused("m2v after raw", kr);
            }
            if (!r.gpuOrder && !r.raw->done(r.raw->submitted())) {
                r.stats.cpuOrdered++;
                r.raw->wait(r.raw->submitted());
            }
            r.rawSinceM2v = false;
            r.m2vSinceRaw = true;
            countDriver(2);
            if (last) {
                cb->_signalsOnM2v = true;
                noteSignals(cb, r.m2vSignals);
            }
            __block bool counted = false;
            {
                std::lock_guard<std::mutex> l(r.m2vLock);
                r.m2vPending++;
                r.m2vUnscheduled++;
            }
            [m addScheduledHandler:^(id<MTLCommandBuffer>) {
                {
                    std::lock_guard<std::mutex> l(gRM->m2vLock);
                    if (!counted)
                        gRM->m2vUnscheduled--;
                    counted = true;
                }
                gRM->m2vChanged.notify_all();
                if (last)
                    scheduled(cb);
            }];
            [m addCompletedHandler:^(id<MTLCommandBuffer> done) {
                if (done.error && !cb->_segError)
                    cb->_segError = done.error;
                if (last)
                    finish(cb, cb->_segError);
                {
                    std::lock_guard<std::mutex> l(gRM->m2vLock);
                    if (!counted)
                        gRM->m2vUnscheduled--;
                    counted = true;
                    gRM->m2vPending--;
                }
                gRM->m2vChanged.notify_all();
            }];
            [m commit];
            r.stats.replayUs += nowUs() - began;
#if SWITCH_FRAME_TIMELINE
            noteReplay(cb->_frame, cb->_frame.mark(switch_frame::Returned));
#endif
        }
        cb->_segNext++;
    }
    return true;
}

// A committed command buffer: submitted now when nothing committed before it waits and what it needs is there (raw: m2vClear,
// the shared events it waits for covered), else handed to the submit thread in order.
void commitBuffer(RMCommandBuffer *cb)
{
    raw::followers().committing();
    planSegments(cb);
    RawState &r = *gRM;
    r.committed.fetch_add(1);
    if (r.raw->purgedArenas())
        restoreArenas(cb);
    if (waitsForEvent(cb)) {
        std::lock_guard<std::mutex> h(gHoldersLock);
        gHolders.insert(cb);
    }
    std::unique_lock<std::mutex> l(r.submit);
    bool now = r.waiting.empty();
    if (now && nextRaw(cb)) {
        std::lock_guard<std::mutex> m(r.m2vLock);
        now = m2vClear();
        if (!now && !r.gpuOrder)
            r.stats.cpuOrdered++;  // it waits for the m2v work before it to complete
    }
    if (now && nextRawWaits(cb))
        now = eventsReady(cb);
    if (now && submitLocked(cb))
        return;
    r.stats.deferred++;
    r.waiting.push_back(cb);
    l.unlock();
    r.waitingChanged.notify_one();
}

// The submit thread: the command buffers that had to wait, in order, each once what it waits for is there.
void submitLoop()
{
    RawState &r = *gRM;
    for (;;) {
        RMCommandBuffer *cb;
        {
            std::unique_lock<std::mutex> l(r.submit);
            r.waitingChanged.wait(l, [&] {
                return !r.waiting.empty();
            });
            cb = r.waiting.front();
        }
        if (nextRaw(cb)) {
            {
                std::unique_lock<std::mutex> m(r.m2vLock);
                if (!r.m2vChanged.wait_for(m, kWaitLimit, [&] {
                        return m2vClear();
                    }))
                    waitTooLong("raw command buffer waiting for the m2v work before it, command buffer",
                                (__bridge void *)cb);
            }
            if (nextRawWaits(cb))
                waitEvents(cb);
        }
        std::lock_guard<std::mutex> l(r.submit);
        if (!submitLocked(cb))
            continue;  // CPU order from now on: it waits for the m2v work before it to complete
        r.waiting.pop_front();
    }
}
