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
// SwitchXUDC*.cpp implement the 18 pure-virtual hardware methods the in-cache reference backend (AppleUSBXDCI, a DWC3
// driver) implements, on top of XudcHardware. Full, high and SuperSpeed; bulk, interrupt and isochronous endpoints. Data
// moves between the controller and the functions' own buffers, scatter-gather, many requests in flight
// (SwitchXUDCData.cpp).
//
// Contract (symbolized macOS IOUSBDeviceFamily, checked against the 21H461 and 24B119 code):
// - setup packets go to controlRequest()->inputSetupPacket() + processSetupPacket();
// - io() requests reach ioGated() inside the family's command gate on our work loop; completion = status + actual
//   into the request, then completeEndpointIO(); a nonzero ioGated() return means "not queued";
// - EP0: the family sends IN data and the device's IN status as io(0x80), tells us OUT data stages (state 5/6) and
//   the host's OUT status (state 8) through handleControlRequestStateChange(); both end in inputDataPacket().
// All calls run on one IOWorkLoop: the family's command gate and the XUDC interrupt (GIC SPI 44 = INTID 76: Linux
// tegra210.dtsi, OLED DTB xudc@700d0000) share it, so nothing here needs a lock. The interrupt drains the event ring
// (Linux tegra_xudc_irq), or a drain the driver asks of it (SwitchXUDCData.cpp drainSoon); nothing else reads it. One
// timer runs, and only after forceReEnumeration: it ends the off time.
//
// Host mode (SwitchUSB.cpp) takes USB2 pad 0 from us with switchxudc_set_host_owns_port(true): we go off the bus as a
// cable pull would, keep the family's wish to be on it, and go back when the pad is returned.
#pragma once
#include "OriginalIOUSBDeviceFamily.h"
#include "XudcHardware.h"
#include "XudcTransfer.h"
#include "XudcIsoch.h"
#include "SwitchXUDCStats.h"
#include "UsbSmmuSpace.h"
#include "XudcIova.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <string.h>
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

using namespace xudc;

namespace xudc {
// _IOUSBDeviceIORequestTag fields (IOUSBDeviceController::io, completeEndpointIO; the same in every kind so far).
constexpr size_t kReqEndpoint = 0, kReqBuffer = 8, kReqStatus = 40, kReqIsoc = 48, kReqLength = 64, kReqActual = 72;
// An isochronous request's frame count and IOUSBDeviceIsocFrame list in place of length and actual (21H461's
// IOUSBDeviceController isochronous io; completeEndpointIO hands the list back to the client).
constexpr size_t kReqFrameCount = kReqLength, kReqFrames = kReqActual;

template <typename T> T get(const _IOUSBDeviceIORequestTag *r, size_t off)
{
    T value;
    // NOLINTNEXTLINE(bugprone-sizeof-expression): T may be a pointer; the field is that pointer
    memcpy(&value, reinterpret_cast<const uint8_t *>(r) + off, sizeof(value));
    return value;
}

template <typename T> void put(_IOUSBDeviceIORequestTag *r, size_t off, T value)
{
    memcpy(reinterpret_cast<uint8_t *>(r) + off, &value, sizeof(value));
}

constexpr uint32_t kEp0Depth = 16;
constexpr uint32_t kEp0Bytes = 0x1000;  // descriptors and control data
// Data endpoints (SwitchXUDCData.cpp): requests per endpoint, on the ring or waiting for room; TRBs one request may
// take (one less than the ring's room, so a serial TRB always fits beside the largest reserved tail) and the physically
// contiguous runs it may have (each run is at least one TRB, so the runs never bind before the ring does); the bounce
// pool (4 KiB pages, XudcTransfer.h); the serial path's buffer, one TRB at a time. 1021 TRBs: a contiguous request of up
// to 63 MiB, 4 KiB pages scattered up to 4 MiB, 2 KiB mbuf clusters up to 2 MiB.
constexpr uint32_t kQueueDepth = 64;
constexpr uint32_t kMaxPieces = kDataRingTrbs - 3;
constexpr uint32_t kMaxRuns = kMaxPieces;
using Tds = TdRing<kDataRingTrbs - 1, kQueueDepth>;
// Isochronous endpoints (SwitchXUDCIsoch.cpp): requests on the ring at once, frames per request.
constexpr uint32_t kIsocRequests = 16, kIsocFrames = 64;
using IsocTds = IsocRing<kDataRingTrbs - 1, kIsocRequests, kIsocFrames>;
static_assert(kIsocFrames <= kMaxPieces, "a request's frames plan into scratchPieces");
static_assert(kMaxPieces < kDataRingTrbs - 2, "a serial TRB fits beside the largest tail");
// Every event the controller can owe at once fits the event ring. A bulk or interrupt TD raises at most two (a short
// packet's before its last TRB, and that TRB's second event), kQueueDepth TDs are on a ring at most, and one retired TD
// may still owe its second; an isochronous frame raises at most one, on at most kMaxIsochEndpoints rings of
// kDataRingTrbs - 2 frames; a cancel's stale events stand in for its TDs' (nothing goes on a fenced ring); EP0 has one
// TD at a time; the rest is setup and port events, paced by the host through EP0 (256 left for those).
static_assert((kMaxDci - 2 - kMaxIsochEndpoints) * (2 * kQueueDepth + 1) + kMaxIsochEndpoints * (kDataRingTrbs - 2) +
                      (kRingTrbs - 1) + 256 <
                  2 * kEventSegmentTrbs,
              "the event ring holds every event the rings can owe");
constexpr uint32_t kPoolBytes = 512 * 1024, kPoolPages = kPoolBytes / kBouncePageBytes;
constexpr uint32_t kSerialBytes = 0x10000;
constexpr uint64_t kReach = 1ULL << 32;  // the XUDC's DMA reach as used here (XudcTransfer.h)
constexpr uint32_t kXudcIntid = 32 + 44;

inline uint32_t dciFor(int address)
{
    const auto bits = static_cast<uint32_t>(address);
    const uint32_t number = bits & 0xfu;
    return number ? number * 2 + ((bits & 0x80u) ? 1 : 0) : 0;
}
}  // namespace xudc

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchXUDC : public IOUSBDeviceController {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchXUDC)

        // A data endpoint's request (SwitchXUDCData.cpp): its memory, prepared from admission to completion, and its
        // window pages when it went into the SMMU's window (SwitchXUDCSmmu.cpp); the serial path's progress.
        struct Request : RequestDirect {
        _IOUSBDeviceIORequestTag *tag;
        IOMemoryDescriptor *md;
        uint32_t length, done;
        bool prepared;
        uint32_t iovaIndex, iovaPages;
    };

    // queue: kQueueDepth requests from head, the first tds->onRing() of them with TDs on the ring. queue and tds are
    // allocated at the first activation: SwitchXUDC itself is capped at 16 KiB. serial: the head request runs alone one
    // TRB at a time through `serial` (its one-TRB TD in tds, serialLength in flight).
    struct Endpoint {
        bool active, in, serialBusy, isoch;
        uint32_t mps, head, count, serialLength;
        uint64_t isocInterval;  // one service interval, absolute time
        Request *queue;
        Tds *tds;
        IsocTds *iso;
        DmaBuffer serial;
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
    IOFilterInterruptEventSource *interrupt;
    IOTimerEventSource *busReturn;  // forceReEnumeration's off time
    Endpoint endpoints[kMaxDci];
    _IOUSBDeviceIORequestTag *ep0Queue[kEp0Depth];
    uint32_t ep0Head, ep0Count;
    DmaBuffer ep0Buffer;
    DmaBuffer pool;  // bounce pages (XudcTransfer.h), below 4 GiB
    uint8_t poolUsed[kPoolPages];
    uint32_t poolFree, poolCursor;
    bool poolFreed;  // pages came back since the last kickWaiting (another endpoint may be waiting for them)
    XudcStats stats;
    // Scratch of admit() (work loop only, never nested: it calls out to no one before it is done), allocated with the
    // controller: SwitchXUDC itself is capped at 16 KiB.
    Run *scratchRuns;
    Piece *scratchPieces;
    Run *scratchIova;
    UsbSmmuSpace smmu;
#if DEVELOPMENT || DEBUG
    UsbSmmuSpace::Cost windowLogged;  // the window's cost at the last stats line
#endif
    uint8_t secondLogged, eventsLogged;  // one-shot lines: the first second event; events for no request, errors
    uint8_t starvedLogged;               // one-shot line: an isochronous ring ran dry
    bool draining;                       // serviceEvents is reading the event ring
    bool systemErrorLogged;              // one-shot line: the first ST.DSE
    Stage stage;
    _IOUSBDeviceIORequestTag *ep0Request;
    uint32_t ep0Trb, ep0Length;
    uint8_t setup[8];
    uint16_t sequence;
    int speed;
    bool onBus, suspended, hostPresent, reportedOnBus, hostOwnsPort, wantBus;
    bool returning;                           // off the bus for forceReEnumeration, on again when busReturn fires
    bool asleep, busAfterSleep, lostToSleep;  // systemSleep: between suspend and resume; resume could not power on
    IOPMDriverAssertionID idleHold;           // while a host is attached (setHostPresent; idleHoldCall only)
    thread_call_t idleHoldCall;
    volatile bool idleWanted;  // what setHostPresent last asked idleHoldCall for
    void setHostPresent(bool present);
    static void applyIdleHold(thread_call_param_t owner, thread_call_param_t);

    void serviceEvents();
    void attachInterrupt();
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void busReturned(OSObject *owner, IOTimerEventSource *sender);
    bool endReturn();
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

    // An IN request's last TRB, held by admit() until kick() knows whether another request follows it onto the ring
    // (SwitchXUDCData.cpp "Interrupts").
    struct HeldTrb {
        Hardware::Slot slot;
        uint64_t pa;
        uint32_t length, flags;
        bool held;
    };

    void writeHeld(uint32_t dci, const HeldTrb &trb, bool another);
    void smmuBegin();
    const Run *viewOf(const Request &r, uint32_t runCount, uint32_t *count);
    uint32_t bounceBytes(const Run *runs, uint32_t count, uint32_t length, uint32_t mps, bool out);
    bool remap(Request &r, uint32_t runCount);
    void releaseMemory(Request &r);
    void drainSoon();
    Admit admit(uint32_t dci, Request &r, HeldTrb *last);
    void startSerial(uint32_t dci);
    void serialPiece(uint32_t dci);
    void serialEvent(uint32_t dci, const Decision &d);
    void complete(uint32_t dci, IOReturn status, uint32_t actual, const Decision *event);
    void releaseTrbs(uint32_t dci, const Tds::Td &td, Request &r, bool copyBack, uint32_t actual);
    void lostEvent(uint32_t dci, const Event &e, uint32_t index, const Decision &d);
    IOReturn isocIo(uint32_t dci, _IOUSBDeviceIORequestTag *request);
    void kickIsoc(uint32_t dci);
    Admit admitIsoc(uint32_t dci, Request &r, HeldTrb *last);
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
        return (pool.va != nullptr) && pa >= pool.pa && pa < static_cast<uint64_t>(pool.pa) + pool.size;
    }

    void cancel(uint32_t dci, IOReturn status);
    void finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual);
    static bool copy(_IOUSBDeviceIORequestTag *request, uint32_t offset, uint8_t *bytes, uint32_t length,
                     bool toDevice);
    void setBus(bool on);

    // The stats line (SwitchXUDCStats.cpp): DEVELOPMENT and DEBUG kernels only.
#if DEVELOPMENT || DEBUG
    void statsBegin();
    void drained(uint64_t entered);
    void logStats(uint64_t now);
    void logWindow(uint64_t ns);
#else
    static void statsBegin()
    {}

    static void drained(uint64_t)
    {}
#endif

public:
    bool begin(IOService *provider, char *why, size_t whySize);
    static SwitchXUDC *running;
    IOReturn portOwner(bool host);
    void vbusChanged();
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
