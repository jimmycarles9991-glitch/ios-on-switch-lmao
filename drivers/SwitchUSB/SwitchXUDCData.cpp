// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC (SwitchXUDC.h), data endpoints. The controller moves data straight between the bus and the functions' own
// memory: a request (AppleUSBDeviceMux's are kernel virtual ranges over its mbufs,
// IOMemoryDescriptor::withAddressRanges) reaches its memory without prepare() when it is zone memory or a buffer wired
// for its life, else is prepared once (XudcRequestMemory.h); its physical runs become chained TRBs (XudcTransfer.h plan:
// whole packets per TRB, IOC on the last, ISP on OUT), and up to kQueueDepth requests sit on a kDataRingTrbs ring at
// once, so the controller goes from one to the next without the CPU. Each completion (the request's own event,
// interrupt moderation coalescing bursts) finishes its request and lets the next ones on (completion-driven: ioGated
// and the event drain both call kick). Requests complete only in order, each from its own event (XudcTransfer.h
// "Completion accounting": a TD ended by a short packet keeps its TRBs reserved until the controller is past it, a
// cancel fences the ring until its stale events are read, an event for a later request than the head is a lost event
// and panics). Packets that cannot go direct (XudcTransfer.h: across a break, a 64 KiB boundary or 4 GiB, an OUT
// cache-line edge) go through 4 KiB pages of the bounce pool, copied in before an IN and out after an OUT; a request
// with more TRBs than the ring holds (or more bounced packets than the pool has pages) takes the serial path: alone on
// the ring, one TRB at a time through the endpoint's 64 KiB buffer. Cache: the controller does not snoop, so an IN's memory is cleaned before the doorbell
// and an OUT's invalidated before it and again over what arrived (XudcRequestMemory.h requestSync). Everything runs on
// the work loop.
#include "SwitchXUDC.h"

int SwitchXUDC::poolTake()
{
    if (!poolFree)
        return -1;
    for (uint32_t n = 0; n < kPoolPages; ++n) {
        const uint32_t page = (poolCursor + n) % kPoolPages;
        if (poolUsed[page])
            continue;
        poolUsed[page] = 1;
        --poolFree;
        poolCursor = (page + 1) % kPoolPages;
        return static_cast<int>(page);
    }
    return -1;
}

void SwitchXUDC::poolGive(uint64_t pa)
{
    const uint32_t page = static_cast<uint32_t>((pa - pool.pa) / kBouncePageBytes);
    if (page < kPoolPages && poolUsed[page]) {
        poolUsed[page] = 0;
        ++poolFree;
        poolFreed = true;
    }
}

// The pool is shared: pages one endpoint's completions free may be what another's head request waits for (Admit::Wait
// with nothing of its own in flight, so no completion of its own would ever kick it). After an event drain or a cancel
// gave pages back, every data endpoint with requests off the ring tries again.
void SwitchXUDC::kickWaiting()
{
    if (!poolFreed || !onBus)
        return;  // off the bus (setBus cancelling every endpoint): nothing to start
    poolFreed = false;
    for (uint32_t dci = 2; dci < kMaxDci; ++dci) {
        const Endpoint &ep = endpoints[dci];
        if (ep.active && ep.queue && !ep.serialBusy && ep.tds->onRing() < ep.count)
            kick(dci);
    }
}

IOReturn SwitchXUDC::ioGated(_IOUSBDeviceIORequestTag *request)
{
    const int address = get<int32_t>(request, kReqEndpoint);
    const uint32_t waitDci = dciFor(address);
    if (waitDci < kMaxDci && __builtin_expect((settlingDcis & (1u << waitDci)) != 0u, 0))
        waitForSettling(1u << waitDci);
    if (!onBus)
        return kIOReturnNotReady;
    if (!(static_cast<uint32_t>(address) & 0x7fu))
        return ep0Io(request, (static_cast<uint32_t>(address) & 0x80u) != 0);
    const uint32_t dci = waitDci;
    if (dci >= kMaxDci || !endpoints[dci].active)
        return kIOReturnNotReady;
    Endpoint &ep = endpoints[dci];
    if (get<uint8_t>(request, kReqIsoc) || ep.isoch)
        return isocIo(dci, request);
    if (get<uint64_t>(request, kReqLength) > UINT32_MAX || ep.count == kQueueDepth)
        return kIOReturnNoResources;
    const uint64_t entered = XudcStats::clock();
    Request &r = ep.queue[(ep.head + ep.count++) % kQueueDepth];
    r = {{},
         request,
         get<IOMemoryDescriptor *>(request, kReqBuffer),
         static_cast<uint32_t>(get<uint64_t>(request, kReqLength)),
         0,
         false,
         0,
         0};
    kick(dci);
    stats.busy(entered);
    return kIOReturnSuccess;
}

// Interrupts. An IN request's last TRB carries IOC and, when another request follows it onto the ring in the same
// kick, BEI (xHCI 1.2c 4.17.5, the Normal TRB's bit 9): its transfer event is written but raises no interrupt. The
// ring's last request never has BEI, and the controller writes events in TD order, so its interrupt brings the drain
// that reads every event before it: a deep queue of writes costs one interrupt. The TRB is held (Hardware::reserveData)
// until the next request is planned, so BEI is never set on a TRB the controller may already have read. OUT requests
// never get it: a read ends where the host's message does, at a short packet the device must see at once (the host
// waits for its reply), and BEI blocks a short packet's event too. Errors raise their interrupt whatever BEI says.
// Where the last request's event may not come, a drain is asked for (drainSoon): a cancel or a halt stops the
// endpoint before it, and a request waiting for pool pages may be waiting on completions other endpoints' blocked
// events hold.
void SwitchXUDC::writeHeld(uint32_t dci, const HeldTrb &trb, bool another)
{
    if (trb.held)
        hw.fillData(dci, trb.slot, trb.pa, trb.length, 0, trb.flags | (another ? TRB_BEI : 0));
}

void SwitchXUDC::drainSoon()
{
    if (!draining && !asleep && interrupt && interrupt->isEnabled() && hw.eventPending())
        interrupt->signalInterrupt();
}

// Requests in order onto the ring while it and the pool have room; one doorbell for the lot. A request that cannot be
// prepared fails when it reaches the head (the ones before it complete first, in order); one for the serial path
// starts once the ring has drained. A fenced ring takes nothing (its fence lifts after the next full event drain).
// NOLINTNEXTLINE(misc-no-recursion): each round completes the head request, so it goes at most kQueueDepth deep
void SwitchXUDC::kick(uint32_t dci)
{
    const Endpoint &ep = endpoints[dci];
    if (ep.isoch) {
        kickIsoc(dci);
        return;
    }
    bool queued = false;
    bool waiting = false;
    HeldTrb held = {};
    while (ep.active && !ep.serialBusy && !ep.tds->fenced() && !(settlingDcis & (1u << dci)) &&
           ep.tds->onRing() < ep.count && !hw.halted(dci)) {
        Request &r = ep.queue[(ep.head + ep.tds->onRing()) % kQueueDepth];
        HeldTrb last = {};
        const Admit a = admit(dci, r, &last);
        if (a == Admit::Queued) {
            writeHeld(dci, held, last.held);
            held = last;
            queued = true;
            continue;
        }
        if (a == Admit::Wait || ep.tds->onRing()) {
            waiting = a == Admit::Wait;
            break;  // room comes with completions; the rest waits for the head
        }
        if (a == Admit::Serial) {
            startSerial(dci);
            break;
        }
        complete(dci, kIOReturnDMAError, 0, nullptr);  // Failed, at the head (not on the ring)
    }
    writeHeld(dci, held, false);
    if (queued)
        hw.doorbell(dci);
    if (waiting)
        drainSoon();
}

// The request's physical runs into scratchRuns, prepare()d first unless its memory needs none (XudcRequestMemory.h).
// False if it cannot be prepared or has a hole; *covered falls short of its length when it has more runs than
// kMaxRuns.
bool SwitchXUDC::runsOf(Request &r, uint32_t *runCount, uint32_t *covered)
{
    if (!r.prepared && !requestDirect(r, scratchRuns, kMaxRuns)) {
        if (r.md->prepare() != kIOReturnSuccess)
            return false;
        r.prepared = true;
    }
    Run *const runs = scratchRuns;
    uint32_t n = directRunCount(r);
    uint32_t offset = directOffset(r);
    while (offset < r.length && n < kMaxRuns) {
        IOByteCount segment = 0;
        const addr64_t pa = r.md->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
        if (!pa || !segment)
            return false;
        const uint32_t take = segment < r.length - offset ? static_cast<uint32_t>(segment) : r.length - offset;
        if (n && runs[n - 1].pa + runs[n - 1].length == pa) {
            runs[n - 1].length += take;
        } else {
            runs[n++] = {pa, take};
        }
        offset += take;
    }
    *runCount = n;
    *covered = offset;
    return true;
}

// One request onto the ring (see the section comment). Queued, or why not; an IN request's last TRB in *last.
SwitchXUDC::Admit SwitchXUDC::admit(uint32_t dci, Request &r, HeldTrb *last)
{
    const Endpoint &ep = endpoints[dci];
    const bool out = !ep.in;
    if (!r.md)
        return r.length ? Admit::Failed : Admit::Serial;
    uint32_t runCount = 0;
    uint32_t covered = 0;
    if (!runsOf(r, &runCount, &covered))
        return Admit::Failed;
    if (covered < r.length)
        return Admit::Serial;
    uint32_t count = 0;
    const Run *runs = viewOf(r, runCount, &count);
    if (!runs)
        return Admit::Failed;
    if (!r.iovaPages && smmu.attached() && remapHelps(runs, count) &&
        bounceBytes(runs, count, r.length, ep.mps, out) >= kRemapWorthBytes && remap(r, runCount))
        runs = viewOf(r, runCount, &count);
    const uint32_t room = ep.tds->room();
    Piece *const pieces = scratchPieces;
    XudcStats::Why why = {};
    const uint32_t n =
        plan(runs, count, r.length, ep.mps, out, kReach, pieces, room < kMaxPieces ? room : kMaxPieces, &why);
    if (!n) {
        NoPlanStats ignored;
        const bool fits = plan(runs, count, r.length, ep.mps, out, kReach, pieces, kMaxPieces, &ignored) != 0u;
        // Room comes with completions; with nothing on the ring only a reserved tail can hold it, until the
        // controller's next event, so this one goes serial (one TRB, which always fits beside a tail: kMaxPieces).
        return fits && ep.tds->onRing() ? Admit::Wait : Admit::Serial;
    }
    uint32_t bounces = 0;
    for (uint32_t i = 0; i < n; ++i)
        bounces += static_cast<uint32_t>(pieces[i].bounce);
    if (bounces > poolFree)
        return bounces > (pool.va ? kPoolPages : 0) ? Admit::Serial : Admit::Wait;
    for (uint32_t i = 0; i < n; ++i) {
        Piece &piece = pieces[i];
        if (!piece.bounce)
            continue;
        const uint32_t page = static_cast<uint32_t>(poolTake());
        uint8_t *va = pool.va + static_cast<size_t>(page) * kBouncePageBytes;
        piece.pa = pool.pa + page * kBouncePageBytes;
        if (!out)
            requestRead(r, r.md, piece.offset, va, piece.length);
        switch_dcache_clean_invalidate(va, piece.length ? piece.length : 1);
    }
    if (r.length)
        requestSync(r, r.md, 0, r.length, out);
    const uint32_t base = trbType(TRB_NORMAL) | (out ? TRB_ISP : 0);
    uint32_t first = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const bool lastTrb = i + 1 == n;
        const uint32_t flags = base | (lastTrb ? TRB_IOC : TRB_CHAIN);
        uint32_t index = 0;
        if (lastTrb && !out) {
            *last = {hw.reserveData(dci, flags), pieces[i].pa, pieces[i].length, flags, true};
            index = last->slot.index;
        } else {
            index = hw.pushData(dci, pieces[i].pa, pieces[i].length, n - 1 - i, flags);
        }
        if (!i)
            first = index;
    }
    ep.tds->push({static_cast<uint16_t>(first), static_cast<uint16_t>(n), r.length});
    stats.queued(!out, n, why);
    return Admit::Queued;
}

// The memory of an on-ring request's TD (`td`) given back: what arrived in the client's memory invalidated first (OUT:
// a line the CPU fetched meanwhile is stale, and a bounced packet's bytes written into it would carry it back over the
// data), then bounce pages copied out (OUT, up to `actual`) and freed. The TRB slots themselves are the TdRing's.
void SwitchXUDC::releaseTrbs(uint32_t dci, const Tds::Td &td, Request &r, bool copyBack, uint32_t actual)
{
    if (copyBack && actual)
        requestSync(r, r.md, 0, actual, true);
    uint32_t index = td.first;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < td.trbs; ++i, index = hw.nextIndex(dci, index)) {
        const uint64_t pa = hw.trbBuffer(dci, index);
        const uint32_t length = hw.trbLength(dci, index);
        if (inPool(pa)) {
            uint8_t *va = pool.va + (pa - pool.pa);
            if (copyBack && offset < actual) {
                const uint32_t got = actual - offset < length ? actual - offset : length;
                switch_dcache_clean_invalidate(va, got);
                requestWrite(r, r.md, offset, va, got);
            }
            poolGive(pa);
        }
        offset += length;
    }
}

// Only the head ever completes. `event` is its own (its TD given back and retired); without one it was not on the ring.
// The request leaves the queue before finish(): the function may queue its next transfer from there.
// NOLINTNEXTLINE(misc-no-recursion): each round completes the head request, so it goes at most kQueueDepth deep
void SwitchXUDC::complete(uint32_t dci, IOReturn status, uint32_t actual, const Decision *event)
{
    Endpoint &ep = endpoints[dci];
    if (!ep.count)
        return;
    Request r = ep.queue[ep.head];
    if (event) {
        releaseTrbs(dci, ep.tds->td(0), r, !ep.in && status == kIOReturnSuccess, actual);
        ep.tds->apply(*event);
    }
    releaseMemory(r);
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    stats.completed(ep.in, actual, status != kIOReturnSuccess);
    finish(r.tag, status, actual);
    kick(dci);
}

// The serial path (the section comment): the head request alone, one TRB at a time through ep.serial.
// NOLINTNEXTLINE(misc-no-recursion): each round completes the head request, so it goes at most kQueueDepth deep
void SwitchXUDC::startSerial(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    Request &r = ep.queue[ep.head];
    if (!ep.serial.va && !ep.serial.allocate(kSerialBytes)) {
        complete(dci, kIOReturnNoMemory, 0, nullptr);
        return;
    }
    if (r.md && !r.prepared) {
        if (r.md->prepare() != kIOReturnSuccess) {
            complete(dci, kIOReturnDMAError, 0, nullptr);
            return;
        }
        r.prepared = true;
    }
    r.done = 0;
    ep.serialBusy = true;
    stats.serial(ep.in);
    serialPiece(dci);
}

// NOLINTNEXTLINE(misc-no-recursion): each round completes the head request, so it goes at most kQueueDepth deep
void SwitchXUDC::serialPiece(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    const Request &r = ep.queue[ep.head];
    const uint32_t left = r.length - r.done;
    const uint32_t piece = left < ep.serial.size ? left : ep.serial.size;
    if (ep.in && piece && r.md->readBytes(r.done, ep.serial.va, piece) != piece) {
        ep.serialBusy = false;
        complete(dci, kIOReturnDMAError, 0, nullptr);
        return;
    }
    switch_dcache_clean_invalidate(ep.serial.va, piece ? piece : 1);
    ep.serialLength = piece;
    const uint32_t index = hw.pushData(dci, ep.serial.pa, piece, 0, trbType(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    ep.tds->push({static_cast<uint16_t>(index), 1, piece});
    hw.doorbell(dci);
}

namespace {
IOReturn statusOf(Outcome outcome)
{
    if (outcome == Outcome::Done)
        return kIOReturnSuccess;
    return outcome == Outcome::Stall ? kIOReturnNotResponding : kIOReturnIOError;
}
}  // namespace

void SwitchXUDC::serialEvent(uint32_t dci, const Decision &d)
{
    Endpoint &ep = endpoints[dci];
    ep.tds->apply(d);
    Request &r = ep.queue[ep.head];
    const uint32_t actual = d.outcome == Outcome::Done && d.actual <= ep.serialLength ? d.actual : 0;
    IOReturn status = statusOf(d.outcome);
    if (status == kIOReturnSuccess && !ep.in && actual) {
        switch_dcache_clean_invalidate(ep.serial.va, actual);
        if (r.md->writeBytes(r.done, ep.serial.va, actual) != actual)
            status = kIOReturnDMAError;
    }
    r.done += actual;
    if (status == kIOReturnSuccess && r.done < r.length && actual == ep.serialLength) {
        serialPiece(dci);
        return;
    }
    ep.serialBusy = false;
    const uint32_t moved = r.done;
    stats.serialMoved(ep.in, moved);
    complete(dci, status, moved, nullptr);
}

// What each event means: XudcTransfer.h "Completion accounting".
void SwitchXUDC::transferEvent(const Event &e)
{
    const uint32_t dci = e.dci();
    if (dci == 0) {
        ep0Complete(e);
        return;
    }
    if (dci >= kMaxDci)
        return;
    const Endpoint &ep = endpoints[dci];
    uint32_t index = 0;
    if (!hw.trbIndex(dci, e.trb(), &index))
        index = Tds::kNoTrb;
    if (!ep.tds) {  // never configured
        stats.unexplained();
        oddEvent(dci, e, index, "for an endpoint never configured");
        return;
    }
    if (ep.isoch) {
        isocEvent(dci, e, index);
        return;
    }
    const Decision d = ep.tds->decide(index, e.code(), e.residue(), ep.in, [this, dci](uint32_t t) {
        return hw.trbLength(dci, t);
    });
    switch (d.verdict) {
    case Verdict::Head:
        break;
    case Verdict::SecondEvent:
        stats.second();
        if (!secondLogged) {
            secondLogged = 1;
            IOLog("SwitchUSB: an OUT TD ended by a short packet before its last TRB gets a second event on that TRB "
                  "(DCI %u, code %u, residue %u): absorbed, its TRBs reused after it\n",
                  dci, e.code(), e.residue());
        }
        ep.tds->apply(d);  // the tail's TRBs are free: room for what waits
        kick(dci);
        return;
    case Verdict::Stale:
        stats.stale();
        return;
    case Verdict::Lost:
        lostEvent(dci, e, index, d);
        return;
    case Verdict::Unexplained:
        stats.unexplained();
        oddEvent(dci, e, index, "for no request on the ring");
        return;
    }
    if (d.outcome == Outcome::Error || d.outcome == Outcome::Stall)
        oddEvent(dci, e, index, "with an error");
    if (d.outcome == Outcome::Error && e.code() == COMP_BABBLE)
        hw.haltAfterBabble(dci);
    if (d.outcome == Outcome::Failed)
        oddEvent(dci, e, index, "with a residue larger than its TRB");
    if (ep.serialBusy) {
        serialEvent(dci, d);
        return;
    }
    if (d.armTail)
        stats.shortEarly();
    complete(dci, statusOf(d.outcome), d.outcome == Outcome::Done ? d.actual : 0, &d);
}

void SwitchXUDC::lostEvent(uint32_t dci, const Event &e, uint32_t index, const Decision &d)
{
    const Endpoint &ep = endpoints[dci];
    const Tds::Td &head = ep.tds->td(0);
    const Tds::Td &owner = ep.tds->td(d.k);
    panic("SwitchUSB: XUDC transfer event lost on DCI %u (EP%u %s): an event on TRB %u (%#x, code %u, residue %u) is "
          "for request %u of %u on the ring (TRBs %u..%u), but the head (TRBs %u..%u, %u bytes) had none",
          dci, dci / 2, ep.in ? "IN" : "OUT", index, e.trb(), e.code(), e.residue(), d.k, ep.tds->onRing(),
          (uint32_t)owner.first, Tds::at(owner.first, owner.trbs - 1u), (uint32_t)head.first,
          Tds::at(head.first, head.trbs - 1u), head.bytes);
}

// Events that lose nothing but should not happen.
void SwitchXUDC::oddEvent(uint32_t dci, const Event &e, uint32_t index, const char *what)
{
    if (eventsLogged >= 8)
        return;
    ++eventsLogged;
    const Tds *tds = endpoints[dci].tds;
    const uint32_t onRing = tds ? tds->onRing() : 0;
    IOLog("SwitchUSB: XUDC transfer event %s on DCI %u: TRB %#x (index %d), code %u, residue %u; %u requests on the "
          "ring from TRB %u, tail %u TRBs\n",
          what, dci, e.trb(), static_cast<int>(index), e.code(), e.residue(), onRing,
          onRing ? static_cast<uint32_t>(tds->td(0).first) : 0, tds ? tds->tailTrbs() : 0);
}

// Requests are taken off the queue before completion: completions re-enter ioGated(). The events the controller wrote
// for what was on the ring may still be in the event ring: the endpoint is fenced (XudcTransfer.h TdRing::cancel) until
// an event drain has read them, at once if none is there and no drain is under way (the controller wrote them before
// dropQueued's pause completed). Their interrupts may be blocked (BEI): the drain is asked for.
void SwitchXUDC::cancel(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    if (!ep.queue || !ep.tds)
        return;
    if (ep.isoch) {
        cancelIsoc(dci, status);
        return;
    }
    DciOp op(this, 1u << dci);
    if (ep.tds->busy())
        hw.dropQueued(dci);

    _IOUSBDeviceIORequestTag *pending[kQueueDepth];
    const uint32_t count = ep.count;
    for (uint32_t i = 0; i < count; ++i) {
        Request &r = ep.queue[(ep.head + i) % kQueueDepth];
        if (!ep.serialBusy && i < ep.tds->onRing())
            releaseTrbs(dci, ep.tds->td(i), r, false, 0);
        releaseMemory(r);
        pending[i] = r.tag;
    }
    ep.head = ep.count = 0;
    ep.serialBusy = false;
    if (ep.tds->cancel() && !draining && !hw.eventPending())
        ep.tds->liftFence();
    op.end();
    drainSoon();
    for (uint32_t i = 0; i < count; ++i)
        finish(pending[i], status, 0);
    kickWaiting();
}

void SwitchXUDC::finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual)
{
    put<int32_t>(request, kReqStatus, status);
    put<uint64_t>(request, kReqActual, actual);  // the family reads 32 bits; isochronous: finishIsoc
    completeEndpointIO(request);
}
