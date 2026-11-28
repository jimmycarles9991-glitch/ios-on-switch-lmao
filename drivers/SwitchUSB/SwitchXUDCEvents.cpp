// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC (SwitchXUDC.h): the interrupt, the event drain, port events and EP0.
#include "SwitchXUDC.h"

namespace {
// IOUSBDeviceControlRequest states that need the controller (see OriginalIOUSBDeviceFamily.h).
enum : int {
    kStateOutData = 5,
    kStateOutDataLong = 6,
    kStateInStatus = 7,
    kStateOutStatus = 8
};
}  // namespace

// Primary interrupt context: acknowledge ST.IP so the level line drops, then let the work loop drain the ring.
bool SwitchXUDC::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchXUDC *>(owner);
    const uint64_t entered = XudcStats::clock();
    if (!self->hw.takeInterrupt())
        return false;
    self->stats.filtered(entered);
    return true;
}

void SwitchXUDC::interruptOccurred(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<SwitchXUDC *>(owner)->serviceEvents();
}

// The whole event ring, as Linux tegra_xudc_process_event_ring drains it (at most one ring's worth per pass, so a pass
// ends). The ring is sized never to fill (XudcTransfer.h kEventSegmentTrbs); should the controller still report it
// full (completion code 21 on a transfer event or, as xHCI does, a host controller event; a setup event's third word is
// its sequence number, not looked at), events were lost, and that panics with one line.
void SwitchXUDC::serviceEvents()
{
    const uint64_t entered = XudcStats::clock();
    if (hw.systemError.seen && !systemErrorLogged) {
        systemErrorLogged = true;
        IOLog("SwitchUSB: XUDC system error (ST.DSE) on the FPCI wrapper: IPFS interrupt code %u, signature %#x (%s, "
              "address bits 31:2 %#x)\n",
              hw.systemError.code & 0x1fu, hw.systemError.signature,
              (hw.systemError.signature & 1u) != 0u ? "read" : "write", hw.systemError.signature & ~3u);
    }
    Event e;
    bool emptied = false;
    draining = true;
    for (uint32_t handled = 0; handled < 2 * kEventSegmentTrbs; ++handled) {
        if (settlingDcis && !hw.nextTransferEventReady(settlingDcis))
            break;
        if (!hw.nextEvent(e)) {
            emptied = true;
            break;
        }
        stats.event();
        if (e.type() != TRB_SETUP_EVENT && e.type() != TRB_PORT_EVENT && e.code() == COMP_EVENT_RING_FULL) {
            panic("SwitchUSB: XUDC event ring full, events were lost (event type %u, DCI %u, TRB %#x, d %08x %08x %08x "
                  "%08x)",
                  e.type(), e.dci(), e.trb(), e.d[0], e.d[1], e.d[2], e.d[3]);
        }
        switch (e.type()) {
        case TRB_TRANSFER_EVENT:
            transferEvent(e);
            break;
        case TRB_PORT_EVENT:
            portEvent();
            break;
        case TRB_SETUP_EVENT:
            setupEvent(e);
            break;
        default:
            break;
        }
    }
    draining = false;
    // Always: the ERDP write also clears EHB, without which no further interrupt is raised. Events left over (a full
    // ring's worth read) raise a new one.
    hw.finishEvents();
    // Emptied: every event the controller wrote before now has been read, so whatever a cancel left in the event ring
    // is gone: the fenced endpoints take requests again (all lifted first: a kick may cancel another endpoint, whose
    // fence this drain does not cover).
    if (emptied) {
        uint32_t lifted = 0;
        for (uint32_t dci = 2; dci < kMaxDci; ++dci) {
            if (settlingDcis & (1u << dci))
                continue;
            const Endpoint &ep = endpoints[dci];
            if (ep.tds && ep.tds->fenced()) {
                ep.tds->liftFence();
            } else if (ep.iso && ep.iso->fenced()) {
                ep.iso->liftFence();
            } else {
                continue;
            }
            lifted |= 1u << dci;
        }
        for (uint32_t dci = 2; dci < kMaxDci; ++dci) {
            if (lifted & (1u << dci) && endpoints[dci].queue)
                kick(dci);
        }
    }
    kickWaiting();
    drained(entered);
}

// Port: reset, connect, suspend (Linux tegra_xudc port handling); the family hears reset, connection done, suspend
// and resume. A host that goes away without a disconnect (the cable pulled with VBUS overridden on) shows as a
// suspend; its next reset starts over.
void SwitchXUDC::portEvent()
{
    PortChange change;
    {
        const DciOp op(this, UINT32_MAX);
        change = hw.portChange();
    }
    if (change.disconnected && speed != kSpeedNone) {
        ep0Cancel(kIOReturnAborted);
        speed = kSpeedNone;
        handleUSBReset();  // the family drops the configuration (deactivateEndpoint for every pipe)
    }
    if (change.disconnected && hostPresent) {
        setHostPresent(false);
        IOLog("SwitchUSB: host disconnected\n");
    }
    if ((change.reset || change.connected) && onBus && change.speed != kSpeedNone) {
        ep0Cancel(kIOReturnAborted);
        stage = Stage::Idle;
        suspended = false;
        handleUSBReset();
        speed = change.speed;
        handleUSBConnectionDone();
        if (!hostPresent) {
            setHostPresent(true);
            IOLog("SwitchUSB: host connected (%s speed)\n", speed == kSpeedSuper  ? "super"
                                                            : speed == kSpeedHigh ? "high"
                                                                                  : "full");
        }
    }
    if (change.suspended && !suspended && speed != kSpeedNone) {
        suspended = true;
        handleUSBSuspend();
        if (hostPresent) {
            setHostPresent(false);
            IOLog("SwitchUSB: bus suspended (host asleep or cable out)\n");
        }
    } else if (change.resumed && suspended) {
        suspended = false;
        setHostPresent(true);
        handleUSBResume();
    }
}

// EP0.
void SwitchXUDC::setupEvent(const Event &e)
{
    memcpy(setup, e.d, sizeof(setup));
    sequence = e.setupSequence();
    IOUSBDeviceControlRequest *request = controlRequest();
    {
        const DciOp op(this, 1u);
        hw.setHalt(0, false);
        if (sequence == 0xfffe || sequence == 0xffff || !request) {  // Tegra210 invalid sequence (tegra_xudc.c)
            hw.setHalt(0, true);
            return;
        }
        // A new setup packet ends the previous control transfer: what it left on the ring is dropped. Our own OUT
        // stages end here; a family request still pending is aborted by the family itself (premature setup ->
        // abortEndpoint -> handleAbortEndpoint -> ep0Cancel).
        if (stage != Stage::Idle)
            hw.dropQueued(0);
    }
    if (stage == Stage::DataOut || stage == Stage::StatusOut)
        stage = Stage::Idle;
    request->inputSetupPacket(setup);
    request->processSetupPacket();
}

void SwitchXUDC::handleControlRequestStateChange(int, int newState)
{
    const IOUSBDeviceControlRequest *request = controlRequest();
    if (!request || stage != Stage::Idle)
        return;
    const DmaBuffer &buffer = ep0Buffer;
    if (newState == kStateOutData || newState == kStateOutDataLong) {
        ep0Length = request->setupLength() < buffer.size ? request->setupLength() : buffer.size;
        switch_dcache_clean_invalidate(buffer.va, ep0Length ? ep0Length : 1);
        stage = Stage::DataOut;
        ep0Trb = hw.push(0, buffer.pa, ep0Length, trbType(TRB_DATA) | TRB_ISP | TRB_IOC);
        hw.doorbell(0, sequence);
    } else if (newState == kStateOutStatus) {
        stage = Stage::StatusOut;
        ep0Trb = hw.push(0, 0, 0, trbType(TRB_STATUS) | TRB_IOC);
        hw.doorbell(0, sequence);
    }
}

// EP0 IN requests queue: a data stage that is a multiple of the packet size and shorter than wLength is followed at
// once by a zero-length one (IOUSBDeviceControlRequest::sendControlInDataGated, zlpCompletion).
IOReturn SwitchXUDC::ep0Io(_IOUSBDeviceIORequestTag *request, bool in)
{
    if (!in || !controlRequest())
        return kIOReturnUnsupported;  // OUT data arrives through inputDataPacket
    if (get<uint64_t>(request, kReqLength) > ep0Buffer.size || ep0Count == kEp0Depth)
        return kIOReturnNoResources;
    ep0Queue[(ep0Head + ep0Count++) % kEp0Depth] = request;
    ep0Kick();
    return kIOReturnSuccess;
}

// A zero-length IN while the family waits for the device's status (state 7) is the status stage; anything else is
// a data stage (including the ZLP).
void SwitchXUDC::ep0Kick()
{
    const IOUSBDeviceControlRequest *control = controlRequest();
    while (stage == Stage::Idle && ep0Count && control) {
        _IOUSBDeviceIORequestTag *request = ep0Queue[ep0Head];
        ep0Head = (ep0Head + 1) % kEp0Depth;
        ep0Count--;
        const uint32_t length = static_cast<uint32_t>(get<uint64_t>(request, kReqLength));
        if (!length && control->state() == kStateInStatus) {
            stage = Stage::StatusIn;
            ep0Length = 0;
            ep0Trb = hw.push(0, 0, 0, trbType(TRB_STATUS) | TRB_IOC | TRB_DIR_IN);
        } else if (!copy(request, 0, ep0Buffer.va, length, true)) {
            finish(request, kIOReturnDMAError, 0);
            continue;
        } else {
            switch_dcache_clean_invalidate(ep0Buffer.va, length ? length : 1);
            stage = Stage::DataIn;
            ep0Length = length;
            ep0Trb = hw.push(0, ep0Buffer.pa, length, trbType(TRB_DATA) | TRB_ISP | TRB_IOC | TRB_DIR_IN);
        }
        ep0Request = request;
        hw.doorbell(0, sequence);
    }
}

void SwitchXUDC::ep0Complete(const Event &e)
{
    if (stage == Stage::Idle || e.trb() != ep0Trb)
        return;  // a superseded control transfer
    const bool ok = e.code() == COMP_SUCCESS || e.code() == COMP_SHORT_PACKET;
    const uint32_t actual = ok && e.residue() <= ep0Length ? ep0Length - e.residue() : 0;
    const Stage done = stage;
    stage = Stage::Idle;
    _IOUSBDeviceIORequestTag *request = ep0Request;
    ep0Request = nullptr;
    IOUSBDeviceControlRequest *control = controlRequest();
    const DmaBuffer &buffer = ep0Buffer;
    switch (done) {
    case Stage::DataIn:
        finish(request, ok ? kIOReturnSuccess : kIOReturnAborted, actual);
        break;  // -> state 8
    case Stage::StatusIn:
        finish(request, ok ? kIOReturnSuccess : kIOReturnAborted, 0);
        break;  // -> state 1
    case Stage::DataOut:
        if (ok && control) {
            switch_dcache_clean_invalidate(buffer.va, actual ? actual : 1);
            control->inputDataPacket(buffer.va, actual);
        }
        break;
    case Stage::StatusOut:
        if (ok && control)
            control->inputDataPacket(buffer.va, 0);
        break;
    case Stage::Idle:
        break;
    }
    ep0Kick();
}

void SwitchXUDC::ep0Cancel(IOReturn status)
{
    {
        const DciOp op(this, 1u);
        if (stage != Stage::Idle)
            hw.dropQueued(0);
        stage = Stage::Idle;
    }
    _IOUSBDeviceIORequestTag *pending[kEp0Depth + 1];
    uint32_t count = 0;
    if (ep0Request)
        pending[count++] = ep0Request;
    ep0Request = nullptr;
    for (uint32_t i = 0; i < ep0Count; ++i)
        pending[count++] = ep0Queue[(ep0Head + i) % kEp0Depth];
    ep0Head = ep0Count = 0;
    for (uint32_t i = 0; i < count; ++i)
        finish(pending[i], status, 0);
}

bool SwitchXUDC::copy(_IOUSBDeviceIORequestTag *request, uint32_t offset, uint8_t *bytes, uint32_t length,
                      bool toDevice)
{
    if (!length)
        return true;
    IOMemoryDescriptor *md = get<IOMemoryDescriptor *>(request, kReqBuffer);
    if (!md || md->prepare() != kIOReturnSuccess)
        return false;
    const IOByteCount done = toDevice ? md->readBytes(offset, bytes, length) : md->writeBytes(offset, bytes, length);
    md->complete();
    return done == length;
}
