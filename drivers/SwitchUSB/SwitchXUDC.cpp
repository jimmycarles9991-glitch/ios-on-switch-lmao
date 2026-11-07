// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC: the Tegra X1 XUSB device controller as a backend of Apple's original IOUSBDeviceController
// (com.apple.iokit.IOUSBDeviceFamily, the selected IPSW kind's: kits/SwitchABI).
//
// Apple's code does everything above the controller, as on an iPad:
//   * the family (prelinked) builds descriptors, runs the EP0 request state machine (IOUSBDeviceControlRequest) and
//     publishes one IOUSBDeviceInterface per function;
//   * configd's USBDeviceConfiguration plug-in reads the controller's ConfigurationType (the provider's
//     configuration-string, /arm-io/usb-device in the ADT) and sets the description from
//     /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist, with the UDID as serial number;
//   * the functions' drivers register: AppleUSBDeviceMux (prelinked) for AppleUSBMux (usbmuxd, lockdownd, Xcode,
//     Finder, atc sync), ptpd (launched by launchd on the PTP interface) for photos;
//   * once every function has registered, the family's startUSBStack asks getConnectionSpeed(): anything but -1 means
//     a cable, so it calls handleUSBCableConnect(), which calls goOnBus(true). Without a cable detector (the BM92T36
//     is not ours yet, see SwitchUSB.h) the port always reports a cable: the pull-up goes on once the functions are
//     ready, and a host plugged in later simply resets the bus.
// This file implements the 18 pure-virtual hardware methods the in-cache reference backend (AppleUSBXDCI, a DWC3
// driver) implements, on top of XudcHardware. USB2 high/full speed; bulk, interrupt and isochronous endpoints. Data
// moves between the controller and the functions' own buffers, scatter-gather, many requests in flight (section "Data
// endpoints").
//
// Contract (symbolized macOS IOUSBDeviceFamily, checked against the 21H461 and 24B119 code):
// - setup packets go to controlRequest()->inputSetupPacket() + processSetupPacket();
// - io() requests reach ioGated() inside the family's command gate on our work loop; completion = status + actual
//   into the request, then completeEndpointIO(); a nonzero ioGated() return means "not queued";
// - EP0: the family sends IN data and the device's IN status as io(0x80), tells us OUT data stages (state 5/6) and
//   the host's OUT status (state 8) through handleControlRequestStateChange(); both end in inputDataPacket().
// All calls run on one IOWorkLoop: the family's command gate and the XUDC interrupt (GIC SPI 44 = INTID 76: Linux
// tegra210.dtsi, OLED DTB xudc@700d0000) share it, so nothing here needs a lock. The interrupt drains the event ring
// (Linux tegra_xudc_irq); nothing else does. No timer runs in steady state. The one lost-interrupt check is armed only
// on positive evidence that an event is due: the PD controller reports a cable from a host (switchxudc_host_expected,
// SwitchUSB.cpp), and the host's bus reset must follow. 2 s later ST.IP is looked at once; pending with no interrupt
// taken (and still so 50 ms later) means the interrupt does not reach the CPU, and that panics with one line. No cable,
// or a charger that never resets the bus, leaves ST.IP clear: nothing happens.
//
// Host mode (SwitchUSB.cpp) takes USB2 pad 0 from us with switchxudc_set_host_owns_port(true): we go off the bus as a
// cable pull would, keep the family's wish to be on it, and go back when the pad is returned.
#include "OriginalIOUSBDeviceFamily.h"
#include "XudcHardware.h"
#include "XudcTransfer.h"
#include "XudcIsoch.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>
#include <os/log.h>
#include <string.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "SwitchUSB.h"

using namespace xudc;

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void switchxudc_static_init(void) __asm__("__GLOBAL__sub_I_SwitchXUDC.cpp");
}

namespace {
// _IOUSBDeviceIORequestTag fields (IOUSBDeviceController::io, completeEndpointIO; the same in every kind so far).
constexpr size_t kReqEndpoint = 0, kReqBuffer = 8, kReqStatus = 40, kReqIsoc = 48, kReqLength = 64, kReqActual = 72;
// An isochronous request's frame count and IOUSBDeviceIsocFrame list in place of length and actual (21H461's
// IOUSBDeviceController isochronous io; completeEndpointIO hands the list back to the client).
constexpr size_t kReqFrameCount = kReqLength, kReqFrames = kReqActual;

template <typename T> T get(const _IOUSBDeviceIORequestTag *r, size_t off)
{
    T value;
    memcpy(&value, reinterpret_cast<const uint8_t *>(r) + off, sizeof(value));
    return value;
}

template <typename T> void put(_IOUSBDeviceIORequestTag *r, size_t off, T value)
{
    memcpy(reinterpret_cast<uint8_t *>(r) + off, &value, sizeof(value));
}

// IOUSBDeviceControlRequest states that need the controller (see OriginalIOUSBDeviceFamily.h).
enum : int {
    kStateOutData = 5,
    kStateOutDataLong = 6,
    kStateInStatus = 7,
    kStateOutStatus = 8
};

// USB transfer types as passed to activateEndpoint.
enum : int {
    kTypeIsochronous = 1,
    kTypeBulk = 2,
    kTypeInterrupt = 3
};

constexpr int kSpeedFull = 1, kSpeedHigh = 2, kSpeedNone = -1;

constexpr uint32_t kEp0Depth = 16;
constexpr uint32_t kEp0Bytes = 0x1000;  // descriptors and control data
// Data endpoints (section "Data endpoints"): requests per endpoint, on the ring or waiting for room; the physically
// contiguous runs one request may have (more: the serial path); TRBs one request may take (one less than the ring's
// room, so a serial TRB always fits beside the largest reserved tail); the bounce pool (4 KiB pages, XudcTransfer.h);
// the serial path's buffer, one TRB at a time.
constexpr uint32_t kQueueDepth = 64;
constexpr uint32_t kMaxRuns = 128;
constexpr uint32_t kMaxPieces = kDataRingTrbs - 3;
using Tds = TdRing<kDataRingTrbs - 1, kQueueDepth>;
// Isochronous endpoints (section "Isochronous endpoints"): requests on the ring at once, frames per request.
constexpr uint32_t kIsocRequests = 16, kIsocFrames = 64;
using IsocTds = IsocRing<kDataRingTrbs - 1, kIsocRequests, kIsocFrames>;
static_assert(kIsocFrames <= kMaxPieces, "a request's frames plan into scratchPieces");
static_assert(kMaxPieces < kDataRingTrbs - 2, "a serial TRB fits beside the largest tail");
constexpr uint32_t kPoolBytes = 512 * 1024, kPoolPages = kPoolBytes / kBouncePageBytes;
constexpr uint32_t kSerialBytes = 0x10000;
constexpr uint64_t kReach = 1ull << 32;  // the XUDC's DMA reach as used here (XudcTransfer.h)
constexpr uint32_t kStatsMs = 2000;      // the stats line: at most this often, only after data moved
constexpr uint32_t kXudcIntid = 32 + 44;
constexpr uint32_t kDeliveryCheckMS = 2000, kDeliveryConfirmMS = 50;

uint32_t dciFor(int address)
{
    uint32_t number = address & 0xf;
    return number ? number * 2 + ((address & 0x80) ? 1 : 0) : 0;
}

uint32_t log2Floor(uint32_t v)
{
    uint32_t n = 0;
    while (v > 1) {
        v >>= 1;
        ++n;
    }
    return n;
}
}  // namespace

class SwitchXUDC : public IOUSBDeviceController {
    OSDeclareDefaultStructors(SwitchXUDC)

        // A data endpoint's request (section "Data endpoints"): its memory, prepared from admission to completion; the
        // serial path's progress.
        struct Request : RequestDirect {
        _IOUSBDeviceIORequestTag *tag;
        IOMemoryDescriptor *md;
        uint32_t length, done;
        bool prepared;
    };

    // queue: kQueueDepth requests from head, the first tds->onRing() of them with TDs on the ring. queue and tds are
    // allocated at the first activation: SwitchXUDC itself is capped at 16 KiB. serial: the head request runs alone one
    // TRB at a time through `serial` (its one-TRB TD in tds, serialLength in flight).
    struct Endpoint {
        bool active, in, serialBusy, isoch, bulk;
        uint32_t mps, head, count, serialLength;
        uint64_t isocInterval;  // one service interval, absolute time
        Request *queue;
        Tds *tds;
        IsocTds *iso;
        DmaBuffer serial;
    };

    // Bytes, requests, failed requests and bounced bytes per direction (0 OUT, 1 IN), requests by the serial path, why
    // packets were bounced (XudcTransfer.h PlanStats), interrupts, events, time in the filter, the event drain and
    // queueing; transfer events by kind (XudcTransfer.h Verdict): TDs ended at a short packet before their last TRB,
    // their second events absorbed, stale events after a cancel, events for no request.
    struct Stats {
        uint64_t bytes[2], requests[2], failed[2], bounced[2], serial[2];
        PlanStats why;
        uint32_t events, shortEarly, second, stale, unexplained;
        uint64_t ticks;
    };
    enum class Stage : uint8_t {
        Idle,
        DataIn,
        StatusIn,
        DataOut,
        StatusOut
    };

    Hardware hw;
    IOWorkLoop *loop;
    IOTimerEventSource *delivery;
    IOFilterInterruptEventSource *interrupt;
    Endpoint endpoints[kMaxDci];
    _IOUSBDeviceIORequestTag *ep0Queue[kEp0Depth];
    uint32_t ep0Head, ep0Count;
    DmaBuffer ep0Buffer;
    DmaBuffer pool;  // bounce pages (XudcTransfer.h), below 4 GiB
    uint8_t poolUsed[kPoolPages];
    uint32_t poolFree, poolCursor;
    bool poolFreed;   // pages came back since the last kickWaiting (another endpoint may be waiting for them)
    uint8_t residue;  // OUT residue reading (XudcTransfer.h kResidue*), learned once per boot
    Stats st, stLogged;
    uint64_t statsAt, statsWindow;
    // Scratch of admit() and transferEvent() (work loop only, never nested: neither calls out before it is done).
    Run scratchRuns[kMaxRuns];
    Piece scratchPieces[kMaxPieces];
    uint32_t interruptsLogged;
    uint32_t mostTrbs[2];                // the most TRBs one request took per direction since the last stats line
    uint8_t secondLogged, eventsLogged;  // one-shot lines: the first second event; events for no request, errors
    uint8_t starvedLogged;               // one-shot line: an isochronous ring ran dry
    bool draining;                       // serviceEvents is reading the event ring
    volatile uint64_t filterTicks;
    Stage stage;
    _IOUSBDeviceIORequestTag *ep0Request;
    uint32_t ep0Trb, ep0Length;
    uint8_t setup[8];
    uint16_t sequence;
    int speed;
    bool onBus, suspended, hostPresent, reportedOnBus, hostOwnsPort, wantBus;
    bool asleep, busAfterSleep, lostToSleep;  // systemSleep: between suspend and resume; resume could not power on
    IOPMDriverAssertionID idleHold;           // while a host is attached (setHostPresent; idleHoldCall only)
    thread_call_t idleHoldCall;
    volatile bool idleWanted;  // what setHostPresent last asked idleHoldCall for
    void setHostPresent(bool present);
    static void applyIdleHold(thread_call_param_t owner, thread_call_param_t);
    uint8_t deliveryStage;
    volatile uint32_t interrupts;  // ST.IP acknowledged by the filter
    uint32_t interruptsAtCheck;

    void serviceEvents();
    void attachInterrupt();
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);
    void armDeliveryCheck(uint32_t ms, uint8_t nextStage);
    void deliveryCheck(IOTimerEventSource *);
    void portEvent();
    void setupEvent(const Event &e);
    void transferEvent(const Event &e);
    void ep0Complete(const Event &e);
    IOReturn ep0Io(_IOUSBDeviceIORequestTag *request, bool in);
    void ep0Kick();
    void ep0Cancel(IOReturn status);
    enum class Admit : uint8_t {
        Queued,
        Wait,
        Serial,
        Failed
    };
    void kick(uint32_t dci);
    void kickWaiting();
    Admit admit(uint32_t dci, Request &r);
    void startSerial(uint32_t dci);
    void serialPiece(uint32_t dci);
    void serialEvent(uint32_t dci, const Decision &d);
    void complete(uint32_t dci, IOReturn status, uint32_t actual, const Decision *event);
    void releaseTrbs(uint32_t dci, const Tds::Td &td, Request &r, bool copyBack, uint32_t actual);
    void lostEvent(uint32_t dci, const Event &e, uint32_t index, const Decision &d);
    IOReturn isocIo(uint32_t dci, _IOUSBDeviceIORequestTag *request);
    void kickIsoc(uint32_t dci);
    Admit admitIsoc(uint32_t dci, Request &r);
    void isocEvent(uint32_t dci, const Event &e, uint32_t index);
    void completeIsoc(uint32_t dci, uint64_t now);
    void failHeadIsoc(uint32_t dci, IOReturn status);
    void finishIsoc(_IOUSBDeviceIORequestTag *request, IOReturn status);
    void cancelIsoc(uint32_t dci, IOReturn status);
    bool runsOf(Request &r, uint32_t *runCount, uint32_t *covered);
    void oddEvent(uint32_t dci, const Event &e, uint32_t index, const char *what);
    int poolTake();
    void poolGive(uint64_t pa);

    bool inPool(uint64_t pa) const
    {
        return pool.va && pa >= pool.pa && pa < (uint64_t)pool.pa + pool.size;
    }

    void logStats(uint64_t now);
    void cancel(uint32_t dci, IOReturn status);
    void finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual);
    bool copy(_IOUSBDeviceIORequestTag *request, uint32_t offset, uint8_t *bytes, uint32_t length, bool toDevice);
    void setBus(bool on);

public:
    bool begin(IOService *provider, char *why, size_t whySize);
    static SwitchXUDC *running;
    IOReturn hostExpected();
    IOReturn portOwner(bool host);
    IOReturn systemSleep(bool entering);
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    void softDisconnect(uint32_t reason) override;
    void goOnBus(bool on) override;

    bool connectedWithAccessoryCable() override
    {
        return false;
    }

    IOReturn handleStallEndpoint(int address, bool stall) override;
    IOReturn handleAbortEndpoint(int address) override;
    IOReturn handleSetAddressPreStatusPhase(int address) override;

    IOReturn handleSetAddressPostStatusPhase(int) override
    {
        return kIOReturnSuccess;
    }

    int getConnectionSpeed() override
    {
        return speed == kSpeedNone ? kSpeedHigh : speed;
    }  // always a cable

    IOReturn provideEndpointIDsForConfiguration(OSArray *interfaces, uint8_t *ids) override;

    IOReturn prepareConfiguration(OSArray *) override
    {
        return kIOReturnSuccess;
    }

    IOReturn activateEndpoint(int address, int type, int maxPacketSize, int interval, int property) override;
    IOReturn deactivateEndpoint(int address) override;
    IOBufferMemoryDescriptor *createIOBuffer(IOOptionBits options, uint64_t capacity) override;

    bool isEndpointStalled(int address) override
    {
        return hw.halted(dciFor(address));
    }

    IOReturn resetEndpointDataToggle(int address) override;
    void forceReEnumeration() override;

    IOReturn doRemoteWakeGated() override
    {
        switch_refusal("usb-device", "remote-wake", "remote wake of the USB host refused: not implemented on the XUDC");
        return kIOReturnUnsupported;
    }

    void handleControlRequestStateChange(int oldState, int newState) override;
    IOReturn ioGated(_IOUSBDeviceIORequestTag *request) override;

    IOReturn getBufferOptions(uint32_t &options) override
    {
        options = 0;
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(SwitchXUDC, IOUSBDeviceController) SwitchXUDC *SwitchXUDC::running;

// ---------------------------------------------------------------------------
// Lifecycle.
// Everything that can fail without side effects on the family happens here, before start().
bool SwitchXUDC::begin(IOService *provider, char *why, size_t whySize)
{
    stage = Stage::Idle;
    speed = kSpeedNone;
    if (!hw.map()) {
        snprintf(why, whySize, "MMIO map failed");
        return false;
    }
    loop = IOWorkLoop::workLoop();
    idleHoldCall = thread_call_allocate_with_options(&SwitchXUDC::applyIdleHold, this, THREAD_CALL_PRIORITY_KERNEL,
                                                     THREAD_CALL_OPTIONS_ONCE);
    delivery = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SwitchXUDC::deliveryCheck));
    if (!loop || !delivery || !ep0Buffer.allocate(kEp0Bytes)) {
        snprintf(why, whySize, "out of memory");
        return false;
    }
    // The bounce pool, taken at start-up while a contiguous run below 4 GiB is easy to find (as SwitchUSB's host pool).
    // Without it every packet that cannot go direct takes the serial path.
    if (pool.allocate(kPoolBytes))
        poolFree = kPoolPages;
    else
        IOLog("SwitchUSB: no %u KiB bounce pool below 4 GiB: requests that need one go one TRB at a time\n",
              kPoolBytes / 1024);
    nanoseconds_to_absolutetime((uint64_t)kStatsMs * 1000000ULL, &statsWindow);
    return hw.powerOn(why, whySize);
}

bool SwitchXUDC::start(IOService *provider)
{
    // The family builds its command gate, pool and timers on getWorkLoop().
    if (!IOUSBDeviceController::start(provider))
        return false;
    if (loop->addEventSource(delivery) != kIOReturnSuccess)
        return false;
    attachInterrupt();  // sleeps until the GIC is up; panics if the interrupt cannot be had
    readyForHost();     // control request, USB device description defaults, registerService()
    return true;
}

void SwitchXUDC::stop(IOService *provider)
{
    if (delivery)
        delivery->cancelTimeout();
    if (interrupt)
        interrupt->disable();
    setBus(false);
    if (delivery && loop)
        loop->removeEventSource(delivery);
    if (interrupt && loop)
        loop->removeEventSource(interrupt);
    IOUSBDeviceController::stop(provider);
}

void SwitchXUDC::free()
{
    OSSafeReleaseNULL(delivery);
    OSSafeReleaseNULL(interrupt);
    for (Endpoint &ep : endpoints) {
        ep.serial.release();
        if (ep.queue)
            IODelete(ep.queue, Request, kQueueDepth);
        if (ep.tds)
            IODelete(ep.tds, Tds, 1);
        ep.queue = nullptr;
        ep.tds = nullptr;
    }
    ep0Buffer.release();
    pool.release();
    OSSafeReleaseNULL(loop);
    IOUSBDeviceController::free();
}

// ---------------------------------------------------------------------------
// Interrupt (work loop context: the family's gate is held).
void SwitchXUDC::attachInterrupt()
{
    IOService *nub = switch_interrupt_nub(kXudcIntid, false);
    interrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXUDC::interruptOccurred,
                                                                         &SwitchXUDC::interruptFilter, nub, 0);
    if (!interrupt || loop->addEventSource(interrupt) != kIOReturnSuccess) {
        panic("SwitchUSB: cannot attach the XUDC interrupt (INTID %u)", kXudcIntid);
    }
    interrupt->enable();
}

// Primary interrupt context: acknowledge ST.IP so the level line drops, then let the work loop drain the ring.
bool SwitchXUDC::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchXUDC *>(owner);
    const uint64_t entered = mach_absolute_time();
    if (!self->hw.takeInterrupt())
        return false;
    __atomic_fetch_add(&self->interrupts, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&self->filterTicks, mach_absolute_time() - entered, __ATOMIC_RELAXED);
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
    const uint64_t entered = mach_absolute_time();
    Event e;
    bool emptied = false;
    draining = true;
    for (uint32_t handled = 0; handled < 2 * kEventSegmentTrbs; ++handled) {
        if (!hw.nextEvent(e)) {
            emptied = true;
            break;
        }
        ++st.events;
        if (e.type() != TRB_SETUP_EVENT && e.type() != TRB_PORT_EVENT && e.code() == COMP_EVENT_RING_FULL)
            panic("SwitchUSB: XUDC event ring full, events were lost (event type %u, DCI %u, TRB %#x, d %08x %08x %08x "
                  "%08x)",
                  e.type(), e.dci(), e.trb(), e.d[0], e.d[1], e.d[2], e.d[3]);
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
            Endpoint &ep = endpoints[dci];
            if (ep.tds && ep.tds->fenced())
                ep.tds->liftFence();
            else if (ep.iso && ep.iso->fenced())
                ep.iso->liftFence();
            else
                continue;
            lifted |= 1u << dci;
        }
        for (uint32_t dci = 2; dci < kMaxDci; ++dci)
            if (lifted & (1u << dci) && endpoints[dci].queue)
                kick(dci);
    }
    kickWaiting();
    const uint64_t now = mach_absolute_time();
    st.ticks += now - entered;
    if (now - statsAt >= statsWindow)
        logStats(now);
}

// Work loop: one os_log line per kStatsMs at most, only after data moved (never the console): MB/s, requests and what
// was bounced per direction, interrupts and events per second, and the driver's own CPU time (the interrupt filter, the
// event drain and the queueing in ioGated, which includes prepare() and the bounce copies) as a share of one core. A
// second line (the kernel's os_log encodes at most about 236 bytes of arguments: one line would be dropped whole): how
// transfer events matched requests (XudcTransfer.h Verdict), failed requests and the most TRBs one request took.
void SwitchXUDC::logStats(uint64_t now)
{
    const uint32_t irqs = __atomic_load_n(&interrupts, __ATOMIC_RELAXED);
    st.ticks += __atomic_exchange_n(&filterTicks, 0, __ATOMIC_RELAXED);
    const Stats &o = stLogged;
    if (st.bytes[0] == o.bytes[0] && st.bytes[1] == o.bytes[1]) {
        statsAt = now;
        stLogged = st;
        interruptsLogged = irqs;
        mostTrbs[0] = mostTrbs[1] = 0;
        return;
    }
    uint64_t ns = 0, busy = 0;
    absolutetime_to_nanoseconds(now - statsAt, &ns);
    absolutetime_to_nanoseconds(st.ticks - o.ticks, &busy);
    if (!ns)
        ns = 1;
    auto rate = [ns](uint64_t bytes) {
        return bytes * 100000ULL / ns;
    };  // hundredths of a MB/s (10^6 bytes)
    const uint64_t in = rate(st.bytes[1] - o.bytes[1]), out = rate(st.bytes[0] - o.bytes[0]);
    const uint64_t cpu = busy * 10000ULL / ns;  // hundredths of a percent of one core
    static const char *const kResidueName[] = {"unknown", "per TRB", "per TD", "broken"};
    os_log(OS_LOG_DEFAULT,
           "SwitchXUDC: stats %llu ms: IN %llu.%02llu MB/s (%llu requests, %llu KiB bounced, %llu serial), OUT "
           "%llu.%02llu MB/s (%llu requests, %llu KiB bounced, %llu serial); %llu interrupts/s, %llu events/s; CPU "
           "%llu.%02llu%% of one core (filter, events, queueing); bounced packets: %u breaks, %u 64K, %u above 4G, %u "
           "cache-line edges; bounce pages free %u/%u; OUT residue %s",
           ns / 1000000, in / 100, in % 100, st.requests[1] - o.requests[1], (st.bounced[1] - o.bounced[1]) / 1024,
           st.serial[1] - o.serial[1], out / 100, out % 100, st.requests[0] - o.requests[0],
           (st.bounced[0] - o.bounced[0]) / 1024, st.serial[0] - o.serial[0],
           (uint64_t)(irqs - interruptsLogged) * 1000000000ULL / ns,
           (uint64_t)(st.events - o.events) * 1000000000ULL / ns, cpu / 100, cpu % 100,
           st.why.discontinuity - o.why.discontinuity, st.why.boundary - o.why.boundary, st.why.high - o.why.high,
           st.why.edge - o.why.edge, poolFree, pool.va ? kPoolPages : 0, kResidueName[residue & 3]);
    os_log(OS_LOG_DEFAULT,
           "SwitchXUDC: transfer events %llu ms: %u TDs ended short before their last TRB, %u second events absorbed, "
           "%u stale, %u for no request; failed requests IN %u OUT %u; most TRBs per request IN %u OUT %u",
           ns / 1000000, st.shortEarly - o.shortEarly, st.second - o.second, st.stale - o.stale,
           st.unexplained - o.unexplained, (uint32_t)(st.failed[1] - o.failed[1]),
           (uint32_t)(st.failed[0] - o.failed[0]), mostTrbs[1], mostTrbs[0]);
    stLogged = st;
    interruptsLogged = irqs;
    mostTrbs[0] = mostTrbs[1] = 0;
    statsAt = now;
}

// One deferred look at ST.IP: pending with no interrupt taken since the check was armed, and still so 50 ms later,
// means the XUDC interrupt does not reach the CPU. Nothing pending (no host, or interrupts working): done.
void SwitchXUDC::armDeliveryCheck(uint32_t ms, uint8_t nextStage)
{
    deliveryStage = nextStage;
    interruptsAtCheck = __atomic_load_n(&interrupts, __ATOMIC_RELAXED);
    delivery->setTimeoutMS(ms);
}

void SwitchXUDC::deliveryCheck(IOTimerEventSource *)
{
    const bool lost = hw.interruptPending() && __atomic_load_n(&interrupts, __ATOMIC_RELAXED) == interruptsAtCheck;
    if (!lost) {
        deliveryStage = 0;
        return;
    }
    if (deliveryStage == 1) {
        armDeliveryCheck(kDeliveryConfirmMS, 2);
        return;
    }
    char state[192], gic[192];
    hw.describeInterrupt(state, sizeof(state));
    switch_interrupt_describe(kXudcIntid, gic, sizeof(gic));
    panic("SwitchUSB: XUDC interrupt lost (INTID %u): %s, %s", kXudcIntid, state, gic);
}

// ---------------------------------------------------------------------------
// Port: reset, connect, suspend (Linux tegra_xudc port handling); the family hears reset, connection done, suspend
// and resume. A host that goes away without a disconnect (the cable pulled with VBUS overridden on) shows as a
// suspend; its next reset starts over.
void SwitchXUDC::portEvent()
{
    PortChange change = hw.portChange();
    if (change.disconnected && speed != kSpeedNone) {
        ep0Cancel(kIOReturnAborted);
        speed = kSpeedNone;
        handleUSBReset();  // the family drops the configuration (deactivateEndpoint for every pipe)
    }
    if (change.disconnected && hostPresent) {
        setHostPresent(false);
        IOLog("SwitchUSB: host disconnected\n");
    }
    if (change.reset && onBus) {
        ep0Cancel(kIOReturnAborted);
        stage = Stage::Idle;
        suspended = false;
        handleUSBReset();
        speed = change.fullSpeed ? kSpeedFull : kSpeedHigh;
        handleUSBConnectionDone();
        if (!hostPresent) {
            setHostPresent(true);
            IOLog("SwitchUSB: host connected (%s speed)\n", speed == kSpeedHigh ? "high" : "full");
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

// ---------------------------------------------------------------------------
// EP0.
void SwitchXUDC::setupEvent(const Event &e)
{
    memcpy(setup, e.d, sizeof(setup));
    sequence = e.setupSequence();
    hw.setHalt(0, false);
    IOUSBDeviceControlRequest *request = controlRequest();
    if (sequence == 0xfffe || sequence == 0xffff || !request) {  // Tegra210 invalid sequence (tegra_xudc.c)
        hw.setHalt(0, true);
        return;
    }
    // A new setup packet ends the previous control transfer: what it left on the ring is dropped. Our own OUT stages
    // end here; a family request still pending is aborted by the family itself (premature setup -> abortEndpoint ->
    // handleAbortEndpoint -> ep0Cancel).
    if (stage != Stage::Idle)
        hw.dropQueued(0);
    if (stage == Stage::DataOut || stage == Stage::StatusOut)
        stage = Stage::Idle;
    request->inputSetupPacket(setup);
    request->processSetupPacket();
}

void SwitchXUDC::handleControlRequestStateChange(int, int newState)
{
    IOUSBDeviceControlRequest *request = controlRequest();
    if (!request || stage != Stage::Idle)
        return;
    DmaBuffer &buffer = ep0Buffer;
    if (newState == kStateOutData || newState == kStateOutDataLong) {
        ep0Length = request->setupLength() < buffer.size ? request->setupLength() : buffer.size;
        cacheFlush(buffer.va, ep0Length ? ep0Length : 1);
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
    IOUSBDeviceControlRequest *control = controlRequest();
    while (stage == Stage::Idle && ep0Count && control) {
        _IOUSBDeviceIORequestTag *request = ep0Queue[ep0Head];
        ep0Head = (ep0Head + 1) % kEp0Depth;
        ep0Count--;
        uint32_t length = (uint32_t)get<uint64_t>(request, kReqLength);
        if (!length && control->state() == kStateInStatus) {
            stage = Stage::StatusIn;
            ep0Length = 0;
            ep0Trb = hw.push(0, 0, 0, trbType(TRB_STATUS) | TRB_IOC | TRB_DIR_IN);
        } else if (!copy(request, 0, ep0Buffer.va, length, true)) {
            finish(request, kIOReturnDMAError, 0);
            continue;
        } else {
            cacheFlush(ep0Buffer.va, length ? length : 1);
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
    bool ok = e.code() == COMP_SUCCESS || e.code() == COMP_SHORT_PACKET;
    uint32_t actual = ok && e.residue() <= ep0Length ? ep0Length - e.residue() : 0;
    Stage done = stage;
    stage = Stage::Idle;
    _IOUSBDeviceIORequestTag *request = ep0Request;
    ep0Request = nullptr;
    IOUSBDeviceControlRequest *control = controlRequest();
    DmaBuffer &buffer = ep0Buffer;
    switch (done) {
    case Stage::DataIn:
        finish(request, ok ? kIOReturnSuccess : kIOReturnAborted, actual);
        break;  // -> state 8
    case Stage::StatusIn:
        finish(request, ok ? kIOReturnSuccess : kIOReturnAborted, 0);
        break;  // -> state 1
    case Stage::DataOut:
        if (ok && control) {
            cacheFlush(buffer.va, actual ? actual : 1);
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
    if (stage != Stage::Idle)
        hw.dropQueued(0);
    stage = Stage::Idle;
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

// ---------------------------------------------------------------------------
// Data endpoints. The controller moves data straight between the bus and the functions' own memory: a
// request (AppleUSBDeviceMux's are kernel virtual ranges over its mbufs, IOMemoryDescriptor::withAddressRanges) is
// prepared once, its physical runs become chained TRBs (XudcTransfer.h plan: whole packets per TRB, IOC on the last,
// ISP on OUT), and up to kQueueDepth requests sit on a kDataRingTrbs ring at once, so the controller goes from one to
// the next without the CPU. Each completion (the request's own event, interrupt moderation coalescing bursts) finishes
// its request and lets the next ones on (completion-driven: ioGated and the event drain both call kick). Requests
// complete only in order, each from its own event (XudcTransfer.h "Completion accounting": a TD ended by a short packet
// keeps its TRBs reserved until the controller is past it, a cancel fences the ring until its stale events are read, an
// event for a later request than the head is a lost event and panics). Packets that
// cannot go direct (XudcTransfer.h: across a break, a 64 KiB boundary or 4 GiB, an OUT cache-line edge) go through
// 4 KiB pages of the bounce pool, copied in before an IN and out after an OUT; a request with more runs than kMaxRuns
// or more TRBs than the ring holds, an OUT one too small to probe the residue, or every multi-TRB OUT one once the
// residue proved broken, takes the serial path: alone on the ring, one TRB at a time through the endpoint's 64 KiB
// buffer. Cache: the controller does not snoop, so an IN's memory is cleaned before
// the doorbell and an OUT's cleaned and invalidated before it and invalidated again over what arrived
// (IOMemoryDescriptor::performOperation, as SwitchUSB's host mapper does). Everything runs on the work loop.

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
        return (int)page;
    }
    return -1;
}

void SwitchXUDC::poolGive(uint64_t pa)
{
    const uint32_t page = (uint32_t)((pa - pool.pa) / kBouncePageBytes);
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
    if (!onBus)
        return kIOReturnNotReady;
    int address = get<int32_t>(request, kReqEndpoint);
    if (!(address & 0x7f))
        return ep0Io(request, address & 0x80);
    uint32_t dci = dciFor(address);
    if (dci >= kMaxDci || !endpoints[dci].active)
        return kIOReturnNotReady;
    Endpoint &ep = endpoints[dci];
    if (get<uint8_t>(request, kReqIsoc) || ep.isoch)
        return isocIo(dci, request);
    if (get<uint64_t>(request, kReqLength) > UINT32_MAX || ep.count == kQueueDepth)
        return kIOReturnNoResources;
    const uint64_t entered = mach_absolute_time();
    Request &r = ep.queue[(ep.head + ep.count++) % kQueueDepth];
    r = {{}, request, get<IOMemoryDescriptor *>(request, kReqBuffer), (uint32_t)get<uint64_t>(request, kReqLength),
         0,  false};
    kick(dci);
    st.ticks += mach_absolute_time() - entered;
    return kIOReturnSuccess;
}

// Requests in order onto the ring while it and the pool have room; one doorbell for the lot. A request that cannot be
// prepared fails when it reaches the head (the ones before it complete first, in order); one for the serial path
// starts once the ring has drained. A fenced ring takes nothing (its fence lifts after the next full event drain).
void SwitchXUDC::kick(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    if (ep.isoch)
        return kickIsoc(dci);
    bool queued = false;
    while (ep.active && !ep.serialBusy && !ep.tds->fenced() && ep.tds->onRing() < ep.count && !hw.halted(dci)) {
        Request &r = ep.queue[(ep.head + ep.tds->onRing()) % kQueueDepth];
        const Admit a = admit(dci, r);
        if (a == Admit::Queued) {
            queued = true;
            continue;
        }
        if (a == Admit::Wait || ep.tds->onRing())
            break;  // room comes with completions; the rest waits for the head
        if (a == Admit::Serial) {
            startSerial(dci);
            break;
        }
        complete(dci, kIOReturnDMAError, 0, nullptr);  // Failed, at the head (not on the ring)
    }
    if (queued)
        hw.doorbell(dci);
}

// The request's physical runs into scratchRuns, prepare()d first unless it goes direct. False if it cannot be prepared
// or has a hole; *covered falls short of its length when it has more runs than kMaxRuns.
bool SwitchXUDC::runsOf(Request &r, uint32_t *runCount, uint32_t *covered)
{
    if (!r.prepared && !requestDirect(r, scratchRuns, kMaxRuns)) {
        if (r.md->prepare() != kIOReturnSuccess)
            return false;
        r.prepared = true;
    }
    Run *const runs = scratchRuns;
    uint32_t n = directRunCount(r), offset = directOffset(r);
    while (offset < r.length && n < kMaxRuns) {
        IOByteCount segment = 0;
        const addr64_t pa = r.md->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
        if (!pa || !segment)
            return false;
        const uint32_t take = segment < r.length - offset ? (uint32_t)segment : r.length - offset;
        if (n && runs[n - 1].pa + runs[n - 1].length == pa)
            runs[n - 1].length += take;
        else
            runs[n++] = {pa, take};
        offset += take;
    }
    *runCount = n;
    *covered = offset;
    return true;
}

// One request onto the ring (see the section comment). Queued, or why not.
SwitchXUDC::Admit SwitchXUDC::admit(uint32_t dci, Request &r)
{
    Endpoint &ep = endpoints[dci];
    const bool out = !ep.in;
    if (!r.md)
        return r.length ? Admit::Failed : Admit::Serial;
    Run *const runs = scratchRuns;
    uint32_t runCount = 0, covered = 0;
    if (!runsOf(r, &runCount, &covered))
        return Admit::Failed;
    if (covered < r.length)
        return Admit::Serial;
    const bool probe = out && residue == kResidueUnknown && r.length >= 2 * ep.mps;
    const uint32_t room = ep.tds->room();
    Piece *const pieces = scratchPieces;
    PlanStats why = {};
    const uint32_t n =
        plan(runs, runCount, r.length, ep.mps, out, probe, kReach, pieces, room < kMaxPieces ? room : kMaxPieces, &why);
    if (!n) {
        PlanStats ignored = {};
        const bool fits = plan(runs, runCount, r.length, ep.mps, out, probe, kReach, pieces, kMaxPieces, &ignored);
        // Room comes with completions; with nothing on the ring only a reserved tail can hold it, until the
        // controller's next event, so this one goes serial (one TRB, which always fits beside a tail: kMaxPieces).
        return fits && ep.tds->onRing() ? Admit::Wait : Admit::Serial;
    }
    if (out && n > 1 && (residue == kResidueBroken || (residue == kResidueUnknown && !probe)))
        return Admit::Serial;
    uint32_t bounces = 0;
    for (uint32_t i = 0; i < n; ++i)
        bounces += pieces[i].bounce;
    if (bounces > poolFree)
        return bounces > (pool.va ? kPoolPages : 0) ? Admit::Serial : Admit::Wait;
    for (uint32_t i = 0; i < n; ++i) {
        Piece &piece = pieces[i];
        if (!piece.bounce)
            continue;
        const uint32_t page = (uint32_t)poolTake();
        uint8_t *va = pool.va + page * kBouncePageBytes;
        piece.pa = pool.pa + page * kBouncePageBytes;
        if (!out)
            requestRead(r, r.md, piece.offset, va, piece.length);
        cacheFlush(va, piece.length ? piece.length : 1);
    }
    if (r.length)
        requestSync(r, r.md, 0, r.length, out);
    const uint32_t base = trbType(TRB_NORMAL) | (out ? TRB_ISP : 0);
    uint32_t first = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const bool last = i + 1 == n;
        const uint32_t flags = base | (last ? TRB_IOC : TRB_CHAIN) | (probe && i == 0 && !last ? TRB_IOC : 0);
        const uint32_t index = hw.pushData(dci, pieces[i].pa, pieces[i].length, n - 1 - i, flags);
        if (!i)
            first = index;
    }
    ep.tds->push({(uint16_t)first, (uint16_t)n, r.length, probe && n > 1});
    const uint32_t d = out ? 0 : 1;
    if (n > mostTrbs[d])
        mostTrbs[d] = n;
    st.bounced[d] += why.bounceBytes;
    st.why.discontinuity += why.discontinuity;
    st.why.boundary += why.boundary;
    st.why.high += why.high;
    st.why.edge += why.edge;
    return Admit::Queued;
}

// The memory of an on-ring request's TD (`td`) given back: what arrived in the client's memory invalidated first (OUT:
// a line the CPU fetched meanwhile is stale, and a bounced packet's bytes written into it would carry it back over the
// data), then bounce pages copied out (OUT, up to `actual`) and freed. The TRB slots themselves are the TdRing's.
void SwitchXUDC::releaseTrbs(uint32_t dci, const Tds::Td &td, Request &r, bool copyBack, uint32_t actual)
{
    if (copyBack && actual)
        requestSync(r, r.md, 0, actual, true);
    uint32_t index = td.first, offset = 0;
    for (uint32_t i = 0; i < td.trbs; ++i, index = hw.nextIndex(dci, index)) {
        const uint64_t pa = hw.trbBuffer(dci, index);
        const uint32_t length = hw.trbLength(dci, index);
        if (inPool(pa)) {
            uint8_t *va = pool.va + (pa - pool.pa);
            if (copyBack && offset < actual) {
                const uint32_t got = actual - offset < length ? actual - offset : length;
                cacheFlush(va, got);
                requestWrite(r, r.md, offset, va, got);
            }
            poolGive(pa);
        }
        offset += length;
    }
}

// Only the head ever completes. `event` is its own (its TD given back and retired); without one it was not on the ring.
// The request leaves the queue before finish(): the function may queue its next transfer from there.
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
    if (r.prepared)
        r.md->complete();
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    const uint32_t d = ep.in ? 1 : 0;
    st.bytes[d] += actual;
    ++st.requests[d];
    if (status != kIOReturnSuccess)
        ++st.failed[d];
    finish(r.tag, status, actual);
    kick(dci);
}

// The serial path (the section comment): the head request alone, one TRB at a time through ep.serial.
void SwitchXUDC::startSerial(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    Request &r = ep.queue[ep.head];
    if (!ep.serial.va && !ep.serial.allocate(kSerialBytes))
        return complete(dci, kIOReturnNoMemory, 0, nullptr);
    if (r.md && !r.prepared) {
        if (r.md->prepare() != kIOReturnSuccess)
            return complete(dci, kIOReturnDMAError, 0, nullptr);
        r.prepared = true;
    }
    r.done = 0;
    ep.serialBusy = true;
    ++st.serial[ep.in ? 1 : 0];
    serialPiece(dci);
}

void SwitchXUDC::serialPiece(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    Request &r = ep.queue[ep.head];
    const uint32_t left = r.length - r.done, piece = left < ep.serial.size ? left : ep.serial.size;
    if (ep.in && piece && r.md->readBytes(r.done, ep.serial.va, piece) != piece) {
        ep.serialBusy = false;
        return complete(dci, kIOReturnDMAError, 0, nullptr);
    }
    cacheFlush(ep.serial.va, piece ? piece : 1);
    ep.serialLength = piece;
    const uint32_t index = hw.pushData(dci, ep.serial.pa, piece, 0, trbType(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    ep.tds->push({(uint16_t)index, 1, piece, false});
    hw.doorbell(dci);
}

namespace {
IOReturn statusOf(Outcome outcome)
{
    return outcome == Outcome::Done    ? kIOReturnSuccess
           : outcome == Outcome::Stall ? kIOReturnNotResponding
                                       : kIOReturnIOError;
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
        cacheFlush(ep.serial.va, actual);
        if (r.md->writeBytes(r.done, ep.serial.va, actual) != actual)
            status = kIOReturnDMAError;
    }
    r.done += actual;
    if (status == kIOReturnSuccess && r.done < r.length && actual == ep.serialLength)
        return serialPiece(dci);
    ep.serialBusy = false;
    const uint32_t moved = r.done;
    st.bounced[ep.in ? 1 : 0] += moved;
    complete(dci, status, moved, nullptr);
}

// What each event means: XudcTransfer.h "Completion accounting".
void SwitchXUDC::transferEvent(const Event &e)
{
    uint32_t dci = e.dci();
    if (dci == 0)
        return ep0Complete(e);
    if (dci >= kMaxDci)
        return;
    Endpoint &ep = endpoints[dci];
    uint32_t index;
    if (!hw.trbIndex(dci, e.trb(), &index))
        index = Tds::kNoTrb;
    if (!ep.tds) {  // never configured
        ++st.unexplained;
        return oddEvent(dci, e, index, "for an endpoint never configured");
    }
    if (ep.isoch)
        return isocEvent(dci, e, index);
    const Decision d = ep.tds->decide(index, e.code(), e.residue(), ep.in, residue, [this, dci](uint32_t t) {
        return hw.trbLength(dci, t);
    });
    switch (d.verdict) {
    case Verdict::Head:
        break;
    case Verdict::SecondEvent:
        ++st.second;
        if (!secondLogged) {
            secondLogged = 1;
            IOLog("SwitchUSB: an OUT TD ended by a short packet before its last TRB gets a second event on that TRB "
                  "(DCI %u, code %u, residue %u): absorbed, its TRBs reused after it\n",
                  dci, e.code(), e.residue());
        }
        ep.tds->apply(d);  // the tail's TRBs are free: room for what waits
        return kick(dci);
    case Verdict::Stale:
        ++st.stale;
        return;
    case Verdict::Lost:
        return lostEvent(dci, e, index, d);
    case Verdict::Unexplained:
        ++st.unexplained;
        return oddEvent(dci, e, index, "for no request on the ring");
    }
    if (d.learned != kResidueUnknown && d.learned != residue && residue != kResidueBroken) {
        residue = d.learned;
        IOLog("SwitchUSB: OUT transfer events report the residue %s%s\n",
              residue == kResiduePerTd    ? "of the whole TD (as Linux tegra-xudc reads it)"
              : residue == kResiduePerTrb ? "of the event's TRB (as xHCI hosts do)"
                                          : "as neither the TD's nor the TRB's",
              residue == kResidueBroken ? ": multi-TRB OUT requests go one TRB at a time from now" : "");
    }
    if (d.outcome == Outcome::Error || d.outcome == Outcome::Stall)
        oddEvent(dci, e, index, "with an error");
    if (ep.serialBusy)
        return serialEvent(dci, d);
    if (d.outcome == Outcome::Pending) {  // the probe's packet: the rest of the TD is coming
        ep.tds->apply(d);
        return kick(dci);  // a tail it released makes room
    }
    if (d.armTail)
        ++st.shortEarly;
    complete(dci, statusOf(d.outcome), d.outcome == Outcome::Done ? d.actual : 0, &d);
}

void SwitchXUDC::lostEvent(uint32_t dci, const Event &e, uint32_t index, const Decision &d)
{
    const Endpoint &ep = endpoints[dci];
    const Tds::Td &head = ep.tds->td(0), &owner = ep.tds->td(d.k);
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
          what, dci, e.trb(), (int)index, e.code(), e.residue(), onRing, onRing ? (uint32_t)tds->td(0).first : 0,
          tds ? tds->tailTrbs() : 0);
}

// Requests are taken off the queue before completion: completions re-enter ioGated(). The events the controller wrote
// for what was on the ring may still be in the event ring: the endpoint is fenced (XudcTransfer.h TdRing::cancel) until
// an event drain has read them, at once if none is there and no drain is under way (the controller wrote them before
// dropQueued's pause completed).
void SwitchXUDC::cancel(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    if (!ep.queue || !ep.tds)
        return;
    if (ep.isoch)
        return cancelIsoc(dci, status);
    if (ep.tds->busy())
        hw.dropQueued(dci);

    struct {
        _IOUSBDeviceIORequestTag *tag;
        IOMemoryDescriptor *prepared;
    } pending[kQueueDepth];

    const uint32_t count = ep.count;
    for (uint32_t i = 0; i < count; ++i) {
        Request &r = ep.queue[(ep.head + i) % kQueueDepth];
        if (!ep.serialBusy && i < ep.tds->onRing())
            releaseTrbs(dci, ep.tds->td(i), r, false, 0);
        pending[i] = {r.tag, r.prepared ? r.md : nullptr};
    }
    ep.head = ep.count = 0;
    ep.serialBusy = false;
    if (ep.tds->cancel() && !draining && !hw.eventPending())
        ep.tds->liftFence();
    for (uint32_t i = 0; i < count; ++i) {
        if (pending[i].prepared)
            pending[i].prepared->complete();
        finish(pending[i].tag, status, 0);
    }
    kickWaiting();
}

void SwitchXUDC::finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual)
{
    put<int32_t>(request, kReqStatus, status);
    put<uint64_t>(request, kReqActual, actual);  // the family reads 32 bits; isochronous: finishIsoc
    completeEndpointIO(request);
}

// ---------------------------------------------------------------------------
// Isochronous endpoints (XudcIsoch.h): AppleUSBMike's USB audio and the USB Audio 2 / IDAM functions. Each frame of a
// request is one Isoch TRB, sent or taken in the first service interval the controller can give it (Start Isoch ASAP:
// the client's frame numbers are its own bookkeeping). Requests complete in order, at their last frame's event or a
// later request's; every frame then gets its status, the bytes it moved and the time of its interval. Nothing halts: a
// frame the host never serviced fails alone, and an empty ring when the host asks costs that interval only.
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
                       (unsigned)get<int32_t>(request, kReqEndpoint), kIsocFrames);
        return kIOReturnNoResources;
    }
    if (ep.count == kQueueDepth)
        return kIOReturnNoResources;
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
        total += frames[i].requestCount;
    Request &r = ep.queue[(ep.head + ep.count++) % kQueueDepth];
    r = {{}, request, get<IOMemoryDescriptor *>(request, kReqBuffer), total, 0, false};
    kickIsoc(dci);
    return kIOReturnSuccess;
}

void SwitchXUDC::kickIsoc(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    bool queued = false;
    while (ep.active && !ep.iso->fenced() && ep.iso->onRing() < ep.count && ep.iso->onRing() < kIsocRequests) {
        const Admit a = admitIsoc(dci, ep.queue[(ep.head + ep.iso->onRing()) % kQueueDepth]);
        if (a == Admit::Queued) {
            queued = true;
            continue;
        }
        if (a == Admit::Wait || ep.iso->onRing())
            break;
        failHeadIsoc(dci, kIOReturnDMAError);
    }
    if (queued)
        hw.doorbell(dci);
}

SwitchXUDC::Admit SwitchXUDC::admitIsoc(uint32_t dci, Request &r)
{
    Endpoint &ep = endpoints[dci];
    const bool out = !ep.in;
    const IsocFrame *frames = get<IsocFrame *>(r.tag, kReqFrames);
    const uint32_t count = (uint32_t)get<uint64_t>(r.tag, kReqFrameCount);
    if (count > ep.iso->room())
        return Admit::Wait;
    uint32_t runCount = 0, covered = 0;
    if (r.length && (!r.md || !runsOf(r, &runCount, &covered) || covered < r.length))
        return Admit::Failed;
    Piece *const pieces = scratchPieces;
    uint16_t lengths[kIsocFrames];
    uint32_t bounces = 0;
    for (uint32_t i = 0, offset = 0; i < count; offset += lengths[i++]) {
        lengths[i] = frames[i].requestCount;
        if (!planFrame(scratchRuns, runCount, offset, lengths[i], out, kReach, &pieces[i]))
            return Admit::Failed;
        bounces += pieces[i].bounce;
    }
    if (bounces > poolFree)
        return bounces > (pool.va ? kPoolPages : 0) ? Admit::Failed : Admit::Wait;
    for (uint32_t i = 0; i < count; ++i) {
        Piece &piece = pieces[i];
        if (!piece.bounce)
            continue;
        const uint32_t page = (uint32_t)poolTake();
        uint8_t *va = pool.va + page * kBouncePageBytes;
        piece.pa = pool.pa + page * kBouncePageBytes;
        if (!out)
            requestRead(r, r.md, piece.offset, va, piece.length);
        cacheFlush(va, piece.length);
    }
    if (r.length)
        requestSync(r, r.md, 0, r.length, out);
    uint32_t first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t flags = trbType(TRB_ISOCH) | TRB_SIA | (out ? TRB_ISP : 0) | (i + 1 == count ? TRB_IOC : 0);
        const uint32_t index = hw.pushData(dci, pieces[i].pa, pieces[i].length, 0, flags);
        if (!i)
            first = index;
    }
    ep.iso->push((uint16_t)first, lengths, (uint16_t)count);
    return Admit::Queued;
}

void SwitchXUDC::isocEvent(uint32_t dci, const Event &e, uint32_t index)
{
    Endpoint &ep = endpoints[dci];
    switch (ep.iso->event(index, e.code(), e.residue())) {
    case IsocVerdict::Recorded:
        break;
    case IsocVerdict::Stale:
        ++st.stale;
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
        ++st.unexplained;
        return oddEvent(dci, e, index, "for no isochronous frame on the ring");
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
                cacheFlush(va, f.actual);
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
    if (r.prepared)
        r.md->complete();
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    const uint32_t d = ep.in ? 1 : 0;
    st.bytes[d] += moved;
    ++st.requests[d];
    if (status != kIOReturnSuccess)
        ++st.failed[d];
    put<int32_t>(r.tag, kReqStatus, status);
    completeEndpointIO(r.tag);
}

// The head request was never on the ring (it could not be prepared or planned).
void SwitchXUDC::failHeadIsoc(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    Request r = ep.queue[ep.head];
    if (r.prepared)
        r.md->complete();
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    ++st.failed[ep.in ? 1 : 0];
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

    struct {
        _IOUSBDeviceIORequestTag *tag;
        IOMemoryDescriptor *prepared;
    } pending[kQueueDepth];

    const uint32_t count = ep.count;
    for (uint32_t i = 0; i < count; ++i) {
        const Request &r = ep.queue[(ep.head + i) % kQueueDepth];
        pending[i] = {r.tag, r.prepared ? r.md : nullptr};
    }
    ep.head = ep.count = 0;
    if (ep.iso->cancel() && !draining && !hw.eventPending())
        ep.iso->liftFence();
    for (uint32_t i = 0; i < count; ++i) {
        if (pending[i].prepared)
            pending[i].prepared->complete();
        finishIsoc(pending[i].tag, status);
    }
    kickWaiting();
}

bool SwitchXUDC::copy(_IOUSBDeviceIORequestTag *request, uint32_t offset, uint8_t *bytes, uint32_t length,
                      bool toDevice)
{
    if (!length)
        return true;
    IOMemoryDescriptor *md = get<IOMemoryDescriptor *>(request, kReqBuffer);
    if (!md || md->prepare() != kIOReturnSuccess)
        return false;
    IOByteCount done = toDevice ? md->readBytes(offset, bytes, length) : md->writeBytes(offset, bytes, length);
    md->complete();
    return done == length;
}

// ---------------------------------------------------------------------------
// Pure-virtual hardware methods.
void SwitchXUDC::goOnBus(bool on)
{
    if (hostOwnsPort) {  // the host has the pad: remember the wish
        wantBus = on;
        return;
    }
    setBus(on);
}

// The PD controller saw a cable from a host (SwitchUSB.cpp): its bus reset is due, so the interrupt can be checked.
IOReturn SwitchXUDC::hostExpected()
{
    if (onBus && !hostPresent && !deliveryStage)
        armDeliveryCheck(kDeliveryCheckMS, 1);
    return kIOReturnSuccess;
}

// The pad changes hands. To the host: off the bus as a cable pull would be, the family's wish kept. Back: on the bus
// again if the family wants it.
IOReturn SwitchXUDC::portOwner(bool host)
{
    if (host == hostOwnsPort)
        return kIOReturnSuccess;
    if (host) {
        wantBus = onBus;
        setBus(false);
        hostOwnsPort = true;
    } else {
        hostOwnsPort = false;
        if (wantBus)
            setBus(true);
    }
    return kIOReturnSuccess;
}

extern "C" void switchxudc_host_expected(void)
{
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller)
        controller->getWorkLoop()->runAction(
            [](OSObject *owner, void *, void *, void *, void *) {
                return static_cast<SwitchXUDC *>(owner)->hostExpected();
            },
            controller);
}

extern "C" void switchxudc_set_host_owns_port(bool host)
{
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller)
        controller->getWorkLoop()->runAction(
            [](OSObject *owner, void *host, void *, void *, void *) {
                return static_cast<SwitchXUDC *>(owner)->portOwner(host != nullptr);
            },
            controller, host ? controller : nullptr);
}

// SC7 resets the controller, its partitions, PLLU/UTMIPLL and the pad block, and with them the event ring and every
// context: off the bus first, as a cable pull (the family gets every request back), with the interrupt and the
// delivery check off; after it the whole powerOn again. Its event ring starts empty, so nothing a cancel left can come
// back and every fence lifts at once.
IOReturn SwitchXUDC::systemSleep(bool entering)
{
    if (entering) {
        if (asleep)
            return kIOReturnSuccess;
        asleep = true;
        delivery->cancelTimeout();
        deliveryStage = 0;
        busAfterSleep = hostOwnsPort ? wantBus : onBus;
        if (!hostOwnsPort)
            setBus(false);
        interrupt->disable();
        return kIOReturnSuccess;
    }
    if (!asleep)
        return kIOReturnSuccess;
    asleep = false;
    char why[160] = "";
    if (!hw.powerOn(why, sizeof(why))) {
        lostToSleep = true;
        IOLog("SwitchUSB: device mode off after sleep: %s\n", why);
        return kIOReturnSuccess;
    }
    for (uint32_t dci = 2; dci < kMaxDci; ++dci)
        if (endpoints[dci].tds)
            endpoints[dci].tds->liftFence();
    interrupt->enable();
    if (hostOwnsPort)
        wantBus = busAfterSleep;
    else if (busAfterSleep)
        setBus(true);
    return kIOReturnSuccess;
}

void switchxudc_sleep(bool entering)
{
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller)
        controller->getWorkLoop()->runAction(
            [](OSObject *owner, void *entering, void *, void *, void *) {
                return static_cast<SwitchXUDC *>(owner)->systemSleep(entering != nullptr);
            },
            controller, entering ? controller : nullptr);
}

void SwitchXUDC::setBus(bool on)
{
    if (on == onBus)
        return;
    if (on && lostToSleep)
        return;
    if (on) {
        ep0Cancel(kIOReturnAborted);
        hw.initEndpoint(0, EP_CONTROL, 64, 0);
        (void)hw.reloadEndpoint(0);
        hw.connect();
        onBus = true;
        if (!reportedOnBus) {
            reportedOnBus = true;
            IOLog("SwitchUSB: functions registered, pull-up on (waiting for a host)\n");
        }
        return;
    }
    onBus = false;
    hw.disconnect();
    ep0Cancel(kIOReturnAborted);
    for (uint32_t dci = 2; dci < kMaxDci; ++dci)
        cancel(dci, kIOReturnAborted);
    speed = kSpeedNone;
    suspended = false;
    setHostPresent(false);
}

// An iPad's powerd holds PreventUserIdleSystemSleep while its port's IOPortTransportStateUSB shows a device-role data
// connection (Active, DataRole 1) on AC power. Nothing here publishes that object (IOPortFamily's port controller
// makes it, and its init is not ours to call), so the attached host holds the kernel's idle-sleep assertion instead:
// the same effect on IOPMrootDomain, and a host always powers VBUS. Demand sleep is untouched.
// The assertion changes on a thread call: hostPresent changes in the event drain, on the work loop, where the root
// domain's assertion lock and PM queue are not to be waited on.
void SwitchXUDC::setHostPresent(bool present)
{
    hostPresent = present;
    if (idleWanted == present)
        return;
    idleWanted = present;
    if (idleHoldCall)
        thread_call_enter(idleHoldCall);
}

void SwitchXUDC::applyIdleHold(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<SwitchXUDC *>(owner);
    IOPMrootDomain *root = getPMRootDomain();
    const bool want = self->idleWanted;
    if (!root || want == (self->idleHold != 0))
        return;
    if (want) {
        self->idleHold = root->createPMAssertion(kIOPMDriverAssertionPreventSystemIdleSleepBit,
                                                 kIOPMDriverAssertionLevelOn, self, "SwitchUSB host attached");
    } else {
        root->releasePMAssertion(self->idleHold);
        self->idleHold = 0;
    }
}

// AppleUSBXDCI::softDisconnect clears DCTL.RUN_STOP and nothing more: the family puts us back with goOnBus(true).
void SwitchXUDC::softDisconnect(uint32_t)
{
    goOnBus(false);
}

// The family's own off-and-on (userland GoOffAndOnBus). 100 ms off so the host sees the detach.
void SwitchXUDC::forceReEnumeration()
{
    if (hostOwnsPort)
        return;
    setBus(false);
    IOSleep(100);
    setBus(true);
}

IOReturn SwitchXUDC::handleStallEndpoint(int address, bool stall)
{
    uint32_t dci = dciFor(address);
    if (dci >= kMaxDci)
        return kIOReturnBadArgument;
    if (!dci) {
        if (stall)
            ep0Cancel(kIOReturnAborted);
        hw.setHalt(0, stall);
    } else if (endpoints[dci].isoch) {
        switch_refusal("usb-device", "isochronous-halt",
                       "halt of isochronous endpoint %#x refused: isochronous "
                       "endpoints have no halt (USB 2.0 9.4.5)",
                       (unsigned)address);
        return kIOReturnUnsupported;
    } else if (stall) {
        hw.setHalt(dci, true);
    } else {
        hw.resetSequence(dci);
        kick(dci);
    }
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::handleAbortEndpoint(int address)
{
    uint32_t dci = dciFor(address);
    if (dci >= kMaxDci)
        return kIOReturnBadArgument;
    if (!dci)
        ep0Cancel(kIOReturnAborted);
    else
        cancel(dci, kIOReturnAborted);
    return kIOReturnSuccess;
}

// SET_ADDRESS takes effect before the status stage (hekate, Linux tegra_xudc_ep0_set_address).
IOReturn SwitchXUDC::handleSetAddressPreStatusPhase(int address)
{
    hw.setAddress(address & 0x7f);
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::resetEndpointDataToggle(int address)
{
    uint32_t dci = dciFor(address);
    if (!dci || dci >= kMaxDci || endpoints[dci].isoch)
        return kIOReturnSuccess;
    hw.resetSequence(dci);
    kick(dci);
    return kIOReturnSuccess;
}

// AppleUSBXDCI::provideEndpointIDsForConfiguration: per function interface, each alternate setting numbers its pipes
// from the next free IN/OUT endpoint; the widest alternate setting decides how many numbers the interface uses.
IOReturn SwitchXUDC::provideEndpointIDsForConfiguration(OSArray *interfaces, uint8_t *ids)
{
    if (!interfaces || !ids)
        return kIOReturnBadArgument;
    uint32_t nextIn = 1, nextOut = 1, written = 0;
    for (unsigned i = 0; i < interfaces->getCount(); ++i) {
        auto *interface = reinterpret_cast<IOUSBDeviceInterface *>(interfaces->getObject(i));
        if (!interface)
            continue;
        uint32_t widestIn = 0, widestOut = 0;
        int alternates = interface->getAlternateSettingCount();
        for (int alternate = 0; alternate < alternates; ++alternate) {
            uint32_t in = 0, out = 0;
            int pipes = interface->getPipeCountForAlternateSetting((uint8_t)alternate);
            for (int p = 0; p < pipes; ++p) {
                IOUSBDevicePipe *pipe = interface->getPipeForAlternateSetting(p, (uint8_t)alternate);
                int direction = pipe ? pipe->getDirection() : -1;
                if (direction == 1) {
                    if (nextIn + in > 15)
                        return kIOReturnNoResources;
                    ids[written++] = (uint8_t)(0x80 | (nextIn + in++));
                } else if (direction == 0) {
                    if (nextOut + out > 15)
                        return kIOReturnNoResources;
                    ids[written++] = (uint8_t)(nextOut + out++);
                }
            }
            if (in > widestIn)
                widestIn = in;
            if (out > widestOut)
                widestOut = out;
        }
        nextIn += widestIn;
        nextOut += widestOut;
    }
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::activateEndpoint(int address, int type, int maxPacketSize, int interval, int)
{
    uint32_t dci = dciFor(address);
    if (!dci)
        return kIOReturnSuccess;  // EP0 is always ours
    if (dci >= kMaxDci || maxPacketSize <= 0)
        return kIOReturnBadArgument;
    bool in = address & 0x80;
    const bool isoch = type == kTypeIsochronous;
    uint32_t kind, period = 0, packet = (uint32_t)maxPacketSize, burst = 0;
    if (type == kTypeBulk) {
        kind = in ? EP_BULK_IN : EP_BULK_OUT;
    } else if (type == kTypeInterrupt) {
        kind = in ? EP_INTR_IN : EP_INTR_OUT;
        uint32_t value = interval < 1 ? 1 : (uint32_t)interval;
        period = speed == kSpeedFull ? log2Floor(value * 8) : (value > 16 ? 16 : value) - 1;  // 2^n x 125 us
    } else if (isoch) {
        uint32_t others = 0;
        for (uint32_t d = 2; d < kMaxDci; ++d)
            others += d != dci && endpoints[d].active && endpoints[d].isoch;
        if (others >= kMaxIsochEndpoints) {
            switch_refusal("usb-device", "isochronous-endpoints",
                           "isochronous endpoint %#x refused: the XUDC runs at most %u at once", (unsigned)address,
                           kMaxIsochEndpoints);
            return kIOReturnNoResources;
        }
        kind = in ? EP_ISOCH_IN : EP_ISOCH_OUT;
        period = interval < 1 ? 1 : interval > 16 ? 16 : (uint32_t)interval;  // bInterval as Linux tegra-xudc sets it
        packet = (uint32_t)maxPacketSize & 0x7ff;
        const uint32_t extra = ((uint32_t)maxPacketSize >> 11) & 3;  // high-bandwidth: transactions per microframe - 1
        burst = speed == kSpeedFull ? 0 : extra > 2 ? 2 : extra;
    } else {
        switch_refusal_k(switch_refusal_key_with(switch_refusal_key("usb-device", "endpoint-type"), (uint64_t)type),
                         "usb-device", "endpoint-type", "endpoint %#x of type %u refused: not a data endpoint type",
                         (unsigned)address, (unsigned)type);
        return kIOReturnUnsupported;
    }
    if (!packet)
        return kIOReturnBadArgument;
    Endpoint &ep = endpoints[dci];
    if (ep.queue && ep.tds && (ep.count || ep.tds->busy()))
        cancel(dci, kIOReturnAborted);  // activated without a deactivation
    if (!ep.queue)
        ep.queue = IONewZero(Request, kQueueDepth);
    if (!ep.tds)
        ep.tds = IONewZero(Tds, 1);  // zeroed: empty, unfenced
    if (isoch && !ep.iso)
        ep.iso = IONewZero(IsocTds, 1);
    if (!ep.queue || !ep.tds || (isoch && !ep.iso))
        return kIOReturnNoMemory;
    ep.head = ep.count = 0;
    ep.serialBusy = false;
    ep.in = in;
    ep.isoch = isoch;
    ep.bulk = type == kTypeBulk;
    ep.mps = packet;
    if (isoch)
        nanoseconds_to_absolutetime(isocIntervalNs(speed == kSpeedFull, period), &ep.isocInterval);
    // Linux tegra-xudc pauses the bulk endpoints while an isochronous one is set up, so that it gets its bandwidth.
    uint32_t paused = 0;
    for (uint32_t d = 2; isoch && d < kMaxDci; ++d) {
        if (d != dci && endpoints[d].active && endpoints[d].bulk) {
            hw.setPause(d, true);
            paused |= 1u << d;
        }
    }
    const bool ready = hw.initEndpoint(dci, kind, packet, period, burst);
    const bool reloaded = ready && hw.reloadEndpoint(dci);
    for (uint32_t d = 2; d < kMaxDci; ++d)
        if (paused & (1u << d))
            hw.setPause(d, false);
    if (!ready) {
        IOLog("SwitchUSB: endpoint %#x (DCI %u) not activated: no memory for its transfer ring\n", address, dci);
        return kIOReturnNoMemory;
    }
    if (!reloaded) {
        IOLog("SwitchUSB: endpoint %#x (DCI %u) not activated: EP_RELOAD did not clear\n", address, dci);
        return kIOReturnTimeout;
    }
    ep.active = true;
    hw.run();  // hekate starts the controller at SET_CONFIGURATION
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::deactivateEndpoint(int address)
{
    uint32_t dci = dciFor(address);
    if (!dci || dci >= kMaxDci)
        return kIOReturnSuccess;
    endpoints[dci].active = false;
    cancel(dci, kIOReturnAborted);
    hw.disableEndpoint(dci);
    return kIOReturnSuccess;
}

// AppleUSBXDCI::createIOBuffer gives pageable memory its DART maps. Here the controller reads and writes the buffer
// itself (section "Data endpoints"): up to 64 KiB, wired and one physically contiguous run below 4 GiB, so a request on
// it is one TRB, never bounced, and prepare() has nothing to wire; larger (or should that fail) pageable as before, the
// planner taking its pages wherever they are (arm64 ignores a below-4-GiB mask without contiguity: KMA_LOMEM is x86's).
IOBufferMemoryDescriptor *SwitchXUDC::createIOBuffer(IOOptionBits options, uint64_t capacity)
{
    if (capacity && capacity <= kTrbMaxBytes) {
        const IOOptionBits wired = (options & ~(IOOptionBits)kIOMemoryPageable) | kIOMemoryPhysicallyContiguous;
        if (IOBufferMemoryDescriptor *b =
                IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, wired, capacity, 0x00000000fffff000ull))
            return b;
    }
    return IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options | kIOMemoryPageable, capacity,
                                                            ~(uint64_t)PAGE_MASK);
}

// ---------------------------------------------------------------------------
// Registration: SwitchXUDC's metaclass needs the loaded original family, so its static initializer is staged
// (Drivers.cmake switch_late_object) and runs here.
namespace {
const OSMetaClass *lookupClass(const char *text)
{
    const OSSymbol *name = OSSymbol::withCString(text);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}
}  // namespace

bool switchxudc_start(IOService *provider, char *why, size_t whySize)
{
    if (OSKext::loadKextWithIdentifier("com.apple.iokit.IOUSBDeviceFamily", false) != KERN_SUCCESS) {
        snprintf(why, whySize, "IOUSBDeviceFamily did not load (is it prelinked?)");
        return false;
    }
    if (OSKext::loadKextWithIdentifier("com.apple.driver.AppleUSBDeviceMux", false) != KERN_SUCCESS) {
        snprintf(why, whySize, "AppleUSBDeviceMux did not load (is it prelinked?)");
        return false;
    }
    const OSMetaClass *parent = lookupClass("IOUSBDeviceController");
    if (parent != &IOUSBDeviceController::gMetaClass || parent->getClassSize() != sizeof(IOUSBDeviceController)) {
        snprintf(why, whySize, "the loaded IOUSBDeviceController is not the " SWITCH_ABI_KIND " one");
        return false;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        snprintf(why, whySize, "preModLoad failed");
        return false;
    }
    switchxudc_static_init();
    if (OSMetaClass::postModLoad(handle) != KERN_SUCCESS || lookupClass("SwitchXUDC") != &SwitchXUDC::gMetaClass ||
        SwitchXUDC::gMetaClass.getSuperClass() != parent) {
        snprintf(why, whySize, "SwitchXUDC class registration failed");
        return false;
    }
    auto *controller = OSTypeAlloc(SwitchXUDC);
    if (!controller || !controller->init()) {
        OSSafeReleaseNULL(controller);
        snprintf(why, whySize, "out of memory");
        return false;
    }
    if (!controller->begin(provider, why, whySize)) {
        controller->release();
        return false;
    }
    if (!controller->attach(provider)) {
        controller->release();
        snprintf(why, whySize, "attach to %s failed", provider->getName());
        return false;
    }
    if (!controller->start(provider)) {
        controller->detach(provider);
        controller->release();
        snprintf(why, whySize, "IOUSBDeviceController::start failed");
        return false;
    }
    __atomic_store_n(&SwitchXUDC::running, controller, __ATOMIC_RELEASE);
    controller->release();  // the registry holds it
    return true;
}
