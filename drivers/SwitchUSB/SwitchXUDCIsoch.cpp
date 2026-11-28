// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC (SwitchXUDC.h), isochronous endpoints (XudcIsoch.h): AppleUSBMike's USB audio and the USB Audio 2 / IDAM
// functions. Each frame of a request is one Isoch TRB, sent or taken in the first service interval the controller can
// give it (Start Isoch ASAP: the client's frame numbers are its own bookkeeping). Requests complete in order, at their
// last frame's event or a later request's; every frame then gets its status, the bytes it moved and the time of its
// interval. Nothing halts: a frame the host never serviced fails alone, and an empty ring when the host asks costs that
// interval only.
#include "SwitchXUDC.h"

IOReturn SwitchXUDC::isocIo(uint32_t dci, _IOUSBDeviceIORequestTag *request)
{
    Endpoint &ep = endpoints[dci];
    const uint64_t count = get<uint64_t>(request, kReqFrameCount);
    const IsocFrame *frames = get<IsocFrame *>(request, kReqFrames);
    if (!ep.isoch || !get<uint8_t>(request, kReqIsoc) || !frames || !count)
        return kIOReturnBadArgument;
    if (count > kIsocFrames) {
        switch_refusal("usb-device", "isochronous-frames",
                       "isochronous request of %llu frames on endpoint %#x refused: at most %u per request", count,
                       static_cast<unsigned>(get<int32_t>(request, kReqEndpoint)), kIsocFrames);
        return kIOReturnNoResources;
    }
    if (ep.count == kQueueDepth)
        return kIOReturnNoResources;
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
        total += frames[i].requestCount;
    Request &r = ep.queue[(ep.head + ep.count++) % kQueueDepth];
    r = {{}, request, get<IOMemoryDescriptor *>(request, kReqBuffer), total, 0, false, 0, 0};
    kickIsoc(dci);
    return kIOReturnSuccess;
}

// As kick(): an IN request's last frame is held and blocks its interrupt when another request follows it
// (SwitchXUDCData.cpp "Interrupts").
void SwitchXUDC::kickIsoc(uint32_t dci)
{
    const Endpoint &ep = endpoints[dci];
    bool queued = false;
    bool waiting = false;
    HeldTrb held = {};
    while (ep.active && !ep.iso->fenced() && !(settlingDcis & (1u << dci)) && ep.iso->onRing() < ep.count &&
           ep.iso->onRing() < kIsocRequests) {
        HeldTrb last = {};
        const Admit a = admitIsoc(dci, ep.queue[(ep.head + ep.iso->onRing()) % kQueueDepth], &last);
        if (a == Admit::Queued) {
            writeHeld(dci, held, last.held);
            held = last;
            queued = true;
            continue;
        }
        if (a == Admit::Wait || ep.iso->onRing()) {
            waiting = a == Admit::Wait;
            break;
        }
        failHeadIsoc(dci, kIOReturnDMAError);
    }
    writeHeld(dci, held, false);
    if (queued)
        hw.doorbell(dci);
    if (waiting)
        drainSoon();
}

SwitchXUDC::Admit SwitchXUDC::admitIsoc(uint32_t dci, Request &r, HeldTrb *last)
{
    const Endpoint &ep = endpoints[dci];
    const bool out = !ep.in;
    const IsocFrame *frames = get<IsocFrame *>(r.tag, kReqFrames);
    const uint32_t count = static_cast<uint32_t>(get<uint64_t>(r.tag, kReqFrameCount));
    if (count > ep.iso->room())
        return Admit::Wait;
    uint32_t runCount = 0;
    uint32_t covered = 0;
    if (r.length && (!r.md || !runsOf(r, &runCount, &covered) || covered < r.length))
        return Admit::Failed;
    Piece *const pieces = scratchPieces;
    uint16_t lengths[kIsocFrames];
    uint32_t bounces = 0;
    uint32_t bouncedBytes = 0;
    uint32_t viewCount = 0;
    const Run *runs = viewOf(r, runCount, &viewCount);
    if (!runs)
        return Admit::Failed;
    const auto planFrames = [&] {
        bounces = bouncedBytes = 0;
        for (uint32_t i = 0, offset = 0; i < count; offset += lengths[i++]) {
            lengths[i] = frames[i].requestCount;
            if (!planFrame(runs, viewCount, offset, lengths[i], out, kReach, &pieces[i]))
                return false;
            bounces += static_cast<uint32_t>(pieces[i].bounce);
            bouncedBytes += pieces[i].bounce ? static_cast<uint32_t>(lengths[i]) : 0u;
        }
        return true;
    };
    if (!planFrames())
        return Admit::Failed;
    if (bouncedBytes >= kRemapWorthBytes && !r.iovaPages && smmu.attached() && remapHelps(runs, viewCount) &&
        remap(r, runCount)) {
        runs = viewOf(r, runCount, &viewCount);
        if (!planFrames())
            return Admit::Failed;
    }
    if (bounces > poolFree)
        return bounces > (pool.va ? kPoolPages : 0) ? Admit::Failed : Admit::Wait;
    for (uint32_t i = 0; i < count; ++i) {
        Piece &piece = pieces[i];
        if (!piece.bounce)
            continue;
        const uint32_t page = static_cast<uint32_t>(poolTake());
        uint8_t *va = pool.va + static_cast<size_t>(page) * kBouncePageBytes;
        piece.pa = pool.pa + page * kBouncePageBytes;
        if (!out)
            requestRead(r, r.md, piece.offset, va, piece.length);
        switch_dcache_clean_invalidate(va, piece.length);
    }
    if (r.length)
        requestSync(r, r.md, 0, r.length, out);
    uint32_t first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const bool lastFrame = i + 1 == count;
        const uint32_t flags = trbType(TRB_ISOCH) | TRB_SIA | (out ? TRB_ISP : 0) | (lastFrame ? TRB_IOC : 0);
        uint32_t index = 0;
        if (lastFrame && !out) {
            *last = {hw.reserveData(dci, flags), pieces[i].pa, pieces[i].length, flags, true};
            index = last->slot.index;
        } else {
            index = hw.pushData(dci, pieces[i].pa, pieces[i].length, 0, flags);
        }
        if (!i)
            first = index;
    }
    ep.iso->push(static_cast<uint16_t>(first), lengths, static_cast<uint16_t>(count));
    return Admit::Queued;
}

void SwitchXUDC::isocEvent(uint32_t dci, const Event &e, uint32_t index)
{
    const Endpoint &ep = endpoints[dci];
    switch (ep.iso->event(index, e.code(), e.residue())) {
    case IsocVerdict::Recorded:
        break;
    case IsocVerdict::Stale:
        stats.stale();
        return;
    case IsocVerdict::Starved:
        if (!starvedLogged) {
            starvedLogged = 1;
            IOLog("SwitchUSB: isochronous DCI %u: the host asked for a frame with none queued (code %u); the function "
                  "queues its frames late\n",
                  dci, e.code());
        }
        return;
    case IsocVerdict::Unexplained:
        stats.unexplained();
        oddEvent(dci, e, index, "for no isochronous frame on the ring");
        return;
    }
    const uint64_t now = mach_absolute_time();
    while (ep.iso->complete())
        completeIsoc(dci, now);
    kickIsoc(dci);
}

// The head request, its frames all known: bounce pages copied out (OUT) and freed, every frame's status, length and
// time written into the client's list. The request leaves the queue before the completion, which may queue its next.
void SwitchXUDC::completeIsoc(uint32_t dci, uint64_t now)
{
    Endpoint &ep = endpoints[dci];
    Request r = ep.queue[ep.head];
    IsocFrame *frames = get<IsocFrame *>(r.tag, kReqFrames);
    const IsocTds::Request &q = ep.iso->request(0);
    if (!ep.in && r.length)
        requestSync(r, r.md, 0, r.length, true);
    IOReturn status = kIOReturnSuccess;
    uint32_t moved = 0;
    for (uint32_t i = 0, offset = 0; i < q.frames; offset += q.frame[i++].length) {
        const IsocTds::Frame &f = q.frame[i];
        const uint64_t pa = hw.trbBuffer(dci, (q.first + i) % (kDataRingTrbs - 1));
        if (f.length && inPool(pa)) {
            uint8_t *va = pool.va + (pa - pool.pa);
            if (!ep.in && f.actual) {
                switch_dcache_clean_invalidate(va, f.actual);
                requestWrite(r, r.md, offset, va, f.actual);
            }
            poolGive(pa);
        }
        const bool done = f.state == IsocTds::kDone;
        frames[i].status = done ? kIOReturnSuccess : kIsocFrameError;
        frames[i].actualCount = f.actual;
        moved += f.actual;
        if (!done)
            status = kIsocFrameError;
    }
    isocStamp(frames, q.frames, now, ep.isocInterval);
    ep.iso->pop();
    releaseMemory(r);
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    stats.completed(ep.in, moved, status != kIOReturnSuccess);
    put<int32_t>(r.tag, kReqStatus, status);
    completeEndpointIO(r.tag);
}

// The head request was never on the ring (it could not be prepared or planned).
void SwitchXUDC::failHeadIsoc(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    Request r = ep.queue[ep.head];
    releaseMemory(r);
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    stats.failed(ep.in);
    finishIsoc(r.tag, status);
}

void SwitchXUDC::finishIsoc(_IOUSBDeviceIORequestTag *request, IOReturn status)
{
    IsocFrame *frames = get<IsocFrame *>(request, kReqFrames);
    const uint64_t count = get<uint64_t>(request, kReqFrameCount);
    for (uint64_t i = 0; i < count; ++i) {
        frames[i].status = status;
        frames[i].actualCount = 0;
    }
    put<int32_t>(request, kReqStatus, status);
    completeEndpointIO(request);
}

// As cancel(): everything off the ring and out of the queue before any completion runs, the ring fenced until the
// event ring holds nothing for it.
void SwitchXUDC::cancelIsoc(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    DciOp op(this, 1u << dci);
    if (ep.iso->busy())
        hw.dropQueued(dci);
    for (uint32_t k = 0; k < ep.iso->onRing(); ++k) {
        const IsocTds::Request &q = ep.iso->request(k);
        for (uint32_t i = 0; i < q.frames; ++i) {
            const uint64_t pa = hw.trbBuffer(dci, (q.first + i) % (kDataRingTrbs - 1));
            if (q.frame[i].length && inPool(pa))
                poolGive(pa);
        }
    }

    _IOUSBDeviceIORequestTag *pending[kQueueDepth];
    const uint32_t count = ep.count;
    for (uint32_t i = 0; i < count; ++i) {
        Request &r = ep.queue[(ep.head + i) % kQueueDepth];
        releaseMemory(r);
        pending[i] = r.tag;
    }
    ep.head = ep.count = 0;
    if (ep.iso->cancel() && !draining && !hw.eventPending())
        ep.iso->liftFence();
    op.end();
    drainSoon();
    for (uint32_t i = 0; i < count; ++i)
        finishIsoc(pending[i], status);
    kickWaiting();
}
