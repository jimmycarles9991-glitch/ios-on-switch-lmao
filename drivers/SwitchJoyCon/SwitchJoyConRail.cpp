// SPDX-License-Identifier: BSD-2-Clause-Patent
// Joy-Cons on the Switch rails: UART-B (right) and UART-C (left), presented to iPadOS as Joy-Con HID devices.
//
// Each rail runs on its own IOWorkLoop (shared with its SwitchJoyConDevice; SwitchJoyConButtons shares the right
// rail's loop) so Left and Right never head-of-line block each other and input delivery takes zero cross-loop hops.
// Primary interrupt context (filter) and the rail workloop communicate via a lock-free SPSC RX byte ring and an
// explicit half-duplex link state machine (SwitchJoyConLink.h), with zero IOSimpleLocks and zero LL/SC atomics on
// steady-state paths. Output reports from user space enter a 16-slot lock-free SPSC command ring and latest-wins
// rumble seqlock and wake the side's workloop immediately.
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/clock.h>
#include <libkern/c++/OSData.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraAPBDMA/TegraAPBDMA.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "SwitchJoyCon.h"
#include "SwitchJoyConHID.h"
#include "SwitchJoyConCadence.h"
#include "SwitchJoyConForward.h"
#include "SwitchJoyConLink.h"
#include "SwitchJoyConPairingSlot.h"
#include "SwitchJoyConProbe.h"
#include "SwitchJoyConRailPacket.h"
#include "SwitchJoyConRxRing.h"
#include "SwitchJoyConSession.h"
#include "SwitchJoyConStick.h"
#include "SwitchJoyConTouch.h"
#include "SwitchJoyConUse.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
constexpr vm_offset_t kCarPA = 0x60006000, kGpioPA = 0x6000D000, kApbMiscPA = 0x70000000, kPinmuxPA = 0x70003000,
                      kUartPA = 0x70006000, kPmcPA = 0x7000E000;

enum : uint32_t {
    RBR_THR = 0x00,
    IER = 0x04,
    IIR_FCR = 0x08,
    LCR = 0x0c,
    MCR = 0x10,
    LSR = 0x14,
    MSR = 0x18,
    SPR = 0x1c,
    IRDA_CSR = 0x20
};

enum : uint32_t {
    IER_RDI = 0x01,
    IER_THRI = 0x02,
    IER_RLSI = 0x04,
    IER_RTOIE = 0x10,
    IER_EORD = 0x20,
    IER_RX = IER_RDI | IER_RLSI | IER_RTOIE,
    IER_RX_DMA = IER_RLSI | IER_RTOIE | IER_EORD,
};

enum : uint32_t {
    FCR_FIFO = 0x01,
    FCR_RX_CLR = 0x02,
    FCR_TX_CLR = 0x04,
    FCR_DMA = 0x08,
    FCR_TX_TRIG_16 = 0x00,
    FCR_RX_TRIG_1 = 0x00,
    FCR_RX_TRIG_8 = 0x80
};

constexpr uint32_t kDmaRingBytes = 16384;
constexpr uint32_t kDmaSettleUs = 4;

enum : uint32_t {
    LCR_8N1 = 0x03,
    LCR_2STOP = 0x04,
    LCR_DLAB = 0x80,
    MCR_RTS_EN = 0x40
};

enum : uint32_t {
    LSR_DR = 0x01,
    LSR_ERRORS = 0x1e,
    LSR_BI = 0x10,
    LSR_THRE = 0x20,
    LSR_TEMT = 0x40,
    LSR_TX_FULL = 0x100
};

constexpr uint32_t kTxBurst = 16;
constexpr uint32_t kTxFifoDepth = 32;

enum : uint32_t {
    CSR_INVERT_TXD = 0x02,
    CSR_INVERT_RTS = 0x08
};

constexpr uint32_t kUartDivEnable = 1u << 24, kDiv1M = 49 /* 25.5 */, kDiv3M = 15 /* 8.5 */;

enum : uint32_t {
    GPIO_CNF = 0x00,
    GPIO_OE = 0x10,
    GPIO_OUT = 0x20,
    GPIO_IN = 0x30,
    GPIO_MSK_CNF = 0x80,
    GPIO_MSK_OE = 0x90
};

enum : uint32_t {
    PIN_PULL_DOWN = 1u << 2,
    PIN_TRISTATE = 1u << 4,
    PIN_INPUT = 1u << 6,
    PIN_DRIVE_4X = 3u << 13,
    PIN_KEEP_FUNCTION = 3u
};

enum : uint32_t {
    RST_DEVICES_L = 0x004,
    RST_DEVICES_H = 0x008,
    CLK_OUT_ENB_L = 0x010,
    CLK_OUT_ENB_H = 0x014
};

enum : uint32_t {
    PMC_NO_IOPOWER = 0x444,
    PMC_PWR_DET_VAL = 0x4e4,
    PMC_IO_DPD_STATUS = 0x5bc
};

constexpr unsigned kPadUartDpdBit = 14;
constexpr uint32_t kPadUartPower = 1u << 2, kPadUartDpd = 1u << kPadUartDpdBit;
constexpr uint32_t PP_PINMUX_GLOBAL = 0x40, kClampTristatedInputs = 1u << 0;
constexpr unsigned kPin5V = TEGRA_GPIO(A, 5);

struct SideConfig {
    char uart, letter;
    bool left;
    uint32_t uartOffset, intid;
    uint32_t rstSet, rstClr, enbSet, enbClr, rstState, enbState, clockBit, clockSource;
    unsigned detectPin, txPin, chargePin;
    uint32_t muxTx, muxRx, muxRts, muxCts, rtsPull, muxDetect, muxCharge, chargeMux;
};

const SideConfig kSides[2] = {
    {'B',
     'R',
     false,
     0x040,
     69,
     0x300,
     0x304,
     0x320,
     0x324,
     RST_DEVICES_L,
     CLK_OUT_ENB_L,
     1u << 7,
     0x17c,
     TEGRA_GPIO(H, 6),
     TEGRA_GPIO(G, 0),
     TEGRA_GPIO(K, 3),
     0xf4,
     0xf8,
     0xfc,
     0x100,
     PIN_PULL_DOWN,
     0x250,
     0x260,
     PIN_DRIVE_4X | PIN_PULL_DOWN | 2},
    {'C',
     'L',
     true,
     0x200,
     78,
     0x308,
     0x30c,
     0x328,
     0x32c,
     RST_DEVICES_H,
     CLK_OUT_ENB_H,
     1u << 23,
     0x1a0,
     TEGRA_GPIO(E, 6),
     TEGRA_GPIO(D, 1),
     TEGRA_GPIO(CC, 3),
     0x104,
     0x108,
     0x10c,
     0x110,
     0,
     0x248,
     0x1a4,
     PIN_PULL_DOWN | 1},
};

enum : uint8_t {
    kCmdExtSend = 0x91,
    kCmdExtRet = 0x92,
    kCmdInitRet = 0x94,
    kCmdHandshake = 0xa5
};

enum : uint8_t {
    kInitMac = 0x01,
    kInitHidConnect = 0x10,
    kInitHidDisconnect = 0x11,
    kInitHidRate = 0x12,
    kInitBaud = 0x20
};

enum : uint8_t {
    kReportSubcommand = 0x01,
    kReportRumble = 0x10,
    kReportReply = 0x21,
    kReportFull = 0x30,
    kReportNfcIr = 0x31,
    kRequestInput = 0x1f
};

enum : uint8_t {
    kSubPairing = 0x01,
    kSubDeviceInfo = 0x02,
    kSubReportMode = 0x03,
    kSubHciState = 0x06,
    kSubShipment = 0x08,
    kSubSpiRead = 0x10,
    kSubImu = 0x40
};

enum : uint8_t {
    kModeSimpleHid = 0x3f
};

constexpr size_t kSharedButtonsByte = 4;
constexpr uint8_t kHomeBit = 0x10, kCaptureBit = 0x20;

constexpr size_t kHeader = SWITCH_JOYCON_RAIL_HEADER, kMaxOutput = SWITCH_JOYCON_MAX_OUTPUT,
                 kMaxTx = kHeader + kMaxOutput, kMaxRx = kHeader + 0x200;
static_assert(SWITCH_JOYCON_SUBCOMMAND_REPORT <= kMaxOutput &&
              SWITCH_JOYCON_PAIRING_REQUEST <= SWITCH_JOYCON_SUBCOMMAND_DATA);
constexpr size_t kMinPacket = 7;
constexpr size_t kStandardFull = 49;
constexpr uint8_t kReplyMagic[3] = {0x19, 0x81, 0x03};

constexpr uint32_t kSettleMS = 64, kHandshakeMS = 100, kCommandMS = 250, kRetryMS = 100, kAttempts = 30;
constexpr uint32_t kSubcommandMS = 1000;
constexpr uint32_t kDeliveryCheckMS = 50;
constexpr uint32_t kQuietLeewayMS = 2;
constexpr uint32_t kSilentMS = 1500, kQuietMS = 500;
constexpr uint8_t kCalibrateMode = SWITCH_JOYCON_STICK_READS;
constexpr uint32_t kRumbleKeepAliveMS = 50;
constexpr uint32_t kResendMS = 5, kDisconnectSends = 40, kFastCommandMS = 1000, kJoyConTimeoutMS = 2000,
                   kFastAttempts = 3;
constexpr uint32_t kSlotBase = 0x2000, kSlotStride = 0x26, kSlots = 0x1000 / kSlotStride;
#if DEVELOPMENT || DEBUG
constexpr bool kReportSlot = true;
#else
constexpr bool kReportSlot = false;
#endif
constexpr uint32_t kJoyConClass = 0x000508;

alignas(8) uint64_t gLastTouchMs;

bool bluetoothDisabled()
{
    unsigned off = 0;
    return (PE_parse_boot_argn("switch-no-bt", &off, sizeof(off)) != 0) && (off != 0u);
}

unsigned flag(uint32_t value, uint32_t mask)
{
    return (value & mask) ? 1 : 0;
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchJoyConPairing : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConPairing)
};

OSDefineMetaClassAndStructors(SwitchJoyConPairing, IOService)

    // NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
    class SwitchJoyConRail : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConRail)

        enum Phase : uint8_t {
        kSettling,
        kWaiting,
        kBackoff,
        kHandshake,
        kPairing,
        kCalibrating,
        kRunning,
        kEnding,
        kSuspended
    };

    enum Step : uint8_t {
        kWake,
        kInfo,
        kBaud,
        kDisconnect,
        kConnect,
        kRate
    };

    enum PairStep : uint8_t {
        kReadSlot,
        kReadSlotTail,
        kDeviceInfo,
        kShipment,
        kSendHost,
        kGetKey,
        kSave
    };

    enum DeliveryAction : uint8_t {
        kDeliveryNone = 0,
        kDeliverySend,
        kDeliveryHandshake,
        kDeliveryPairing,
        kDeliveryCalibrating,
        kDeliveryEnding,
        kDeliverySilence
    };

    struct Side {
        SwitchJoyConRail *owner;
        const SideConfig *cfg;
        volatile uint8_t *uart;
        IOWorkLoop *loop;
        IOFilterInterruptEventSource *irq;
        IOInterruptEventSource *detect;
        IOTimerEventSource *timer;
        IOTimerEventSource *poll;
        IOInterruptEventSource *wake;
        IOLock *outProdLock;  // cold producer serialization on setReport / sessionEnded only; never taken on hot path
        bool irqEnabled;

        // Lock-free SPSC RX ring and half-duplex link state (SwitchJoyConLink.h).
        switch_joycon_spsc_rx rxRing;
        uint32_t linkState;
        uint64_t rxTime;
        uint32_t interrupts, lineErrors, fastErrors, breaks;
        bool stuck, fast;

        // Framing state owned exclusively by primary IRQ filter (reset only with `irq` disabled).
        uint32_t frameLength, frameTotal;
        uint8_t frameSizeLow;
        bool packetDone;

        // TX buffer owned by filter while linkState == TX_BUSY, else by `loop`.
        uint8_t tx[kMaxTx];
        uint32_t txLength, txPosition;
        uint32_t ier;

        // RX APB DMA state (owned by filter while dmaOn && irqEnabled; mutated on `loop` only with `irq` disabled).
        tegra_apbdma_channel *dma;
        tegra_apbdma_buffer dmaRing;
        bool dmaOn, dmaOff, dmaChecked, dmaOverrun, dmaTail, dmaWaited;
        const char *dmaFailed;
        uint32_t dmaTaken, dmaLapsAtStart, dmaTakes, dmaLate, dmaTails, dmaTickAt;
        uint64_t dmaWords;

        // Per-side workloop state.
        Phase phase;
        uint8_t step, attempts, fastFailures, sends, slot, counter;
        uint32_t errorsSeen, epoch;
        uint64_t lastReply;
        uint64_t pollNext;
        uint32_t pollInterval;
        uint32_t reports, unanswered, silentMs;
        uint32_t errorsAtReply, breaksAtReply;
        uint32_t replies, dropped, silences, recoveries, republished;
        uint32_t requests, unsolicited, forwarded, suppressed;
        uint8_t packet[kMaxRx];
        size_t packetLength;
        bool oversizeLogged;

        // Non-blocking 50 ms lost-interrupt re-check state.
        DeliveryAction deliveryAction;
        uint32_t deliveryInterrupts;
        const char *deliveryWhy;

        // Attach diagnostics for the give-up line.
        uint8_t furthest;
        uint8_t heard[12], stepHeard[16];
        uint32_t heardCount, interruptsAtAttach, errorsAtAttach, stepHeardCount;
        uint32_t fastSent, fastHeardCount, fastErrorsAtAttach, lateCount;
        uint32_t errorsAtRun, overflowsAtRun;

        uint8_t mac[6];
        uint8_t key[16];
        uint8_t slotBytes[SWITCH_JOYCON_SLOT_BYTES];
        bool slotInUse, slotCorrupt;
        bool charging, chargeKnown;
        bool pairPending, paired;
        bool suspended, displayOff;

        IOService *hid;
        uint32_t hidGeneration;
        SwitchJoyConPairing *record;

        // Republish mailbox (written under outProdLock, polled atomically on `loop`).
        uint32_t republishPending;
        uint32_t republishGen;
        int32_t servedPid, openerPid;
        switch_joycon_republish_guard guard;

        // Lock-free output command ring (16 slots) and latest-wins rumble seqlock.
        switch_joycon_cmd_ring cmdRing;
        switch_joycon_rumble_slot rumbleSlot;

        // Forward/use/cadence state owned 100% by `loop`.
        switch_joycon_forward forward;
        switch_joycon_use use;
        switch_joycon_stick stick;
        uint8_t stickMac[6];
        bool stickKnown;
        switch_joycon_cadence cadence;
        bool sysButtonDown;

        // Cold-publish input replay buffer.
        uint8_t latestFull[kStandardFull];
        size_t latestFullLen;
        bool replayPending;

        bool imuOff;
        uint8_t ownReply;
        bool rumbleSent;
        uint64_t rumbleAt;
        uint32_t busyHold;  // relaxed cross-side flag: IMU or active rumble keeps iOS 27 gamepad alive
        uint32_t busyHoldCached;
#if DEVELOPMENT || DEBUG
        switch_joycon_probe probe;
#endif
    };

    volatile uint8_t *car = nullptr, *gpio = nullptr, *apbMisc = nullptr, *pinmux = nullptr, *uarts = nullptr,
                     *pmc = nullptr;
    Side sides[2] = {};
    bool pairing = false, nfc = false;
    uint32_t republishOff = 0;
    bool hostKnown = false;
    uint8_t host[6] = {};
    // Cross-side iOS 27 gamepad presence state (coordinated atomically without cross-workloop locks).
    uint32_t gamepads = 0;
    alignas(8) uint64_t lastUse = 0;
    uint32_t gamepadsPublished = 0, gamepadsIdled = 0;

    static uint32_t rd(const Side &s, uint32_t reg)
    {
        return switch_mmio_read32(s.uart, reg);
    }

    static void wr(Side &s, uint32_t reg, uint32_t value)
    {
        switch_mmio_write32(s.uart, reg, value);
    }

    void carWrite(uint32_t reg, uint32_t value)
    {
        switch_mmio_write32(car, reg, value);
    }

    uint32_t carRead(uint32_t reg) const
    {
        return switch_mmio_read32(car, reg);
    }

    void pinWrite(uint32_t reg, uint32_t value)
    {
        switch_mmio_write32(pinmux, reg, value);
    }

    uint32_t pinRead(uint32_t reg) const
    {
        return switch_mmio_read32(pinmux, reg);
    }

    uint32_t pmcRead(uint32_t reg) const
    {
        return switch_mmio_read32(pmc, reg);
    }

    void gpioMask(unsigned pin, uint32_t reg, bool on);
    unsigned gpioBit(unsigned pin, uint32_t reg) const;
    static void setIrqEnabled(Side &s, bool on);

    static bool uartFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void uartAction(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void detectAction(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void timerAction(OSObject *owner, IOTimerEventSource *sender);
    static void pollAction(OSObject *owner, IOTimerEventSource *sender);
    static void wakeAction(OSObject *owner, IOInterruptEventSource *sender, int count);
    static IOReturn output(void *context, uint32_t generation, const uint8_t *report, size_t length);
    static void sessionEnded(void *context, uint32_t generation, int32_t servedPid, int32_t openerPid);
    Side *sideFor(const OSObject *source);
    Side &otherSide(const Side &s);
    static bool filter(Side &s);
    static void fillTx(Side &s);
    static bool drainRx(Side &s);
    static void countLineStatus(Side &s, uint32_t lsr);
    static void frameByte(Side &s, uint8_t byte);
    static bool takeDma(Side &s);
    static void takeTail(Side &s);
    static void startDma(Side &s);
    static void stopDma(Side &s);
    static void finishDmaFailure(Side &s, const char *why);

    void setUp(Side &s, const SideConfig *cfg);
    void preparePads(const char *when);
    void initHardware(Side &s);
    static void quiesce(Side &s);
    void wake(Side &s);
    void configureUart(Side &s, bool fast);
    static void send(Side &s, const uint8_t *bytes, size_t length);
    static bool txIdle(const Side &s);
    static void sendCommand(Side &s, uint8_t subcmd, const uint8_t *payload = nullptr, uint16_t length = 0);
    static void sendReport(Side &s, const uint8_t *report, size_t length);
    static void sendOutput(Side &s, uint8_t *report, size_t length);
    static void sendSubcommand(Side &s, uint8_t id, const uint8_t *data, size_t length);
    void enterDetect(Side &s);
    void attachJoyCon(Side &s);
    void startAttempt(Side &s);
    void failAttempt(Side &s, const char *why);
    void finishFailAttempt(Side &s, const char *why);
    static void sendStep(Side &s);
    void startPairing(Side &s);
    static void endSession(Side &s);
    void sendPairStep(Side &s);
    void finishPairing(Side &s, bool paired, const char *how);
    static void calibrate(Side &s);
    static void sendCalibrationStep(Side &s);
    static const char *calibrationStepName(uint8_t step);
    void calibrationReply(Side &s, const uint8_t *report, size_t length);
    void run(Side &s);
    static IOService *createHid(Side &s);
    static void dropGamepad(Side &s);
    static void dropHid(Side &s);
    static void updateBusyHold(Side &s);
    bool anyBusyHold() const;
    bool gamepadsLive(Side &s, uint64_t now);
    void noteUse(Side &s, uint64_t now);
    void idleGamepads(Side &s, bool byTouch);
    void syncHid(Side &s);
    void republish(Side &s);
    void handleWake(Side &s);
    static bool dispatchQueuedCommand(Side &s, uint64_t now);
    static void armPoll(Side &s, bool resetPhase);
    void tick(Side &s);
    void finishSilence(Side &s);
    static bool popOutputCommand(Side &s, uint8_t report[kMaxOutput], size_t *length);
    static bool ownReplyTaken(Side &s, const uint8_t *report, size_t length);
    static bool probing(const Side &s);
#if DEVELOPMENT || DEBUG
    static bool probeTick(Side &s);
    static bool probePacket(Side &s, const uint8_t *packet, size_t length, uint64_t now);
#endif
    static void rumbleAfterReply(Side &s);
    void detach(Side &s);
    static void pause(Side &s);
    void received(Side &s, const uint8_t *packet, size_t length, uint64_t rxTime);
    void handshakeReply(Side &s, uint8_t cmd, uint8_t echo, const uint8_t *payload, size_t length);
    void pairingReply(Side &s, const uint8_t *report, size_t length);
    void pairingSlotReply(Side &s, const uint8_t *data, bool ack, size_t length);
    void runningReport(Side &s, const uint8_t *report, size_t length, uint64_t rxTime);
    static void updateSystemButton(Side &s, const uint8_t *report, size_t length, uint64_t rxTime);
    static void setCharging(Side &s, bool on);
    void publishPairing(Side &s, const char *source);
    static bool deliveryPending(const Side &s, uint32_t *lsrOut);
    static bool startDeliveryCheck(Side &s, const char *why, DeliveryAction action);
    void completeDeliveryCheck(Side &s);
    void describe(const Side &s, char *buffer, size_t size) const;

public:
    bool start(IOService *provider) override;
    void suspend();
    void resume();
    void displayPower(bool on);
    void bluetoothAddress(const uint8_t address[6]);
    void statsLine(char *buffer, size_t size) const;
#if DEVELOPMENT || DEBUG
    int probeStart();
    void probeState(char *buffer, size_t size);
#endif
};

OSDefineMetaClassAndStructors(SwitchJoyConRail, IOService)

    void SwitchJoyConRail::gpioMask(unsigned pin, uint32_t reg, bool on)
{
    const uint32_t bit = 1u << (pin & 7);
    const uint32_t at = (pin >> 5) * 0x100 + ((pin >> 3) & 3) * 4 + reg;
    switch_mmio_write32(gpio, at, (bit << 8) | (on ? bit : 0));
    (void)switch_mmio_read32(gpio, at);
}

unsigned SwitchJoyConRail::gpioBit(unsigned pin, uint32_t reg) const
{
    const uint32_t at = (pin >> 5) * 0x100 + ((pin >> 3) & 3) * 4 + reg;
    return (switch_mmio_read32(gpio, at) >> (pin & 7)) & 1;
}

void SwitchJoyConRail::setIrqEnabled(Side &s, bool on)
{
    if (s.irqEnabled == on)
        return;
    if (on) {
        s.irq->enable();
    } else {
        s.irq->disable();
    }
    s.irqEnabled = on;
}

SwitchJoyConRail::Side *SwitchJoyConRail::sideFor(const OSObject *source)
{
    for (Side &s : sides) {
        if (source == s.irq || source == s.detect || source == s.timer || source == s.poll || source == s.wake)
            return &s;
    }
    return nullptr;
}

SwitchJoyConRail::Side &SwitchJoyConRail::otherSide(const Side &s)
{
    return (&s == &sides[0]) ? sides[1] : sides[0];
}

// Primary interrupt filter returns false and signals explicitly so the level line stays unmasked at 3 Mbaud.
bool SwitchJoyConRail::uartFilter(OSObject *owner, IOFilterInterruptEventSource *sender)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (s && SwitchJoyConRail::filter(*s))
        sender->signalInterrupt();
    return false;
}

bool SwitchJoyConRail::filter(Side &s)
{
    if (s.ier == 0)
        return false;
    bool got = false;
    bool idle = false;
    bool dmaRx = false;
    __atomic_store_n(&s.interrupts, s.interrupts + 1u, __ATOMIC_RELAXED);
    const uint32_t errorsBefore = s.lineErrors;
    const uint32_t overflowsBefore = __atomic_load_n(&s.rxRing.overflows, __ATOMIC_RELAXED);
    for (unsigned rounds = 0;; ++rounds) {
        const uint32_t iir = rd(s, IIR_FCR);
        if (iir & 1u)
            break;
        if (rounds == 64) {
            s.ier = 0;
            wr(s, IER, 0);
            __atomic_store_n(&s.stuck, true, __ATOMIC_RELEASE);
            return true;
        }
        switch ((iir >> 1) & 7u) {
        case 0:
            (void)rd(s, MSR);
            break;
        case 1:
            fillTx(s);
            break;
        default:
            if (s.dmaOn) {
                if (!dmaRx) {
                    // Tegra UART erratum: pulse RDI before clearing RX sources to acknowledge EORD.
                    wr(s, IER, s.ier | IER_RDI);
                    wr(s, IER, s.ier & ~(IER_RX | IER_EORD));
                }
                dmaRx = true;
                if (((iir >> 1) & 7u) == 3u)
                    countLineStatus(s, rd(s, LSR));
                break;
            }
            idle |= ((iir >> 1) & 7u) == 6u;
            got |= drainRx(s);
            break;
        }
    }
    if (dmaRx) {
        got |= takeDma(s);
        idle = !s.dmaTail;
        if (s.dmaOn && !__atomic_load_n(&s.dmaFailed, __ATOMIC_RELAXED))
            wr(s, IER, s.dmaTail ? s.ier & ~IER_RTOIE : s.ier);
    }
    const bool failed =
        __atomic_load_n(&s.dmaFailed, __ATOMIC_RELAXED) != nullptr || __atomic_load_n(&s.dmaOverrun, __ATOMIC_RELAXED);
    if (got && !failed && s.phase == kRunning && !s.packetDone && !idle && s.lineErrors == errorsBefore &&
        __atomic_load_n(&s.rxRing.overflows, __ATOMIC_RELAXED) == overflowsBefore) {
        return false;
    }
    if (got) {
        if (s.packetDone)
            switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_RX_DONE);
        s.packetDone = false;
    }
    return got;
}

void SwitchJoyConRail::frameByte(Side &s, uint8_t byte)
{
    if (s.frameLength < 3 && byte != kReplyMagic[s.frameLength]) {
        s.frameLength = byte == kReplyMagic[0] ? 1 : 0;
        return;
    }
    if (s.frameLength == 3)
        s.frameSizeLow = byte;
    if (s.frameLength == 4)
        s.frameTotal = 5 + (s.frameSizeLow | static_cast<uint32_t>(byte) << 8);
    if (++s.frameLength < 5)
        return;
    if (s.frameTotal < kMinPacket || s.frameTotal > kMaxRx) {
        s.frameLength = 0;
        return;
    }
    if (s.frameLength < s.frameTotal)
        return;
    s.frameLength = 0;
    s.packetDone = true;
}

void SwitchJoyConRail::fillTx(Side &s)
{
    if (s.txPosition < s.txLength && (rd(s, LSR) & LSR_THRE)) {
        const uint32_t end = s.txLength - s.txPosition > kTxBurst ? s.txPosition + kTxBurst : s.txLength;
        while (s.txPosition < end)
            wr(s, RBR_THR, s.tx[s.txPosition++]);
    }
    while (s.txPosition < s.txLength && !(rd(s, LSR) & LSR_TX_FULL))
        wr(s, RBR_THR, s.tx[s.txPosition++]);
    if (s.txPosition == s.txLength) {
        if (s.ier & IER_THRI) {
            s.ier &= ~IER_THRI;
            wr(s, IER, s.ier);
        }
        switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_WAIT_RX);
    }
}

void SwitchJoyConRail::countLineStatus(Side &s, uint32_t lsr)
{
    if (!(lsr & LSR_ERRORS))
        return;
    __atomic_store_n(&s.lineErrors, s.lineErrors + 1u, __ATOMIC_RELAXED);
    if (s.fast)
        __atomic_store_n(&s.fastErrors, s.fastErrors + 1u, __ATOMIC_RELAXED);
    if (lsr & LSR_BI)
        __atomic_store_n(&s.breaks, s.breaks + 1u, __ATOMIC_RELAXED);
    s.frameLength = 0;
}

bool SwitchJoyConRail::drainRx(Side &s)
{
    uint32_t head = s.rxRing.head;
    const uint32_t tail = switch_joycon_spsc_rx_peek_tail(&s.rxRing);
    bool got = false;
    for (;;) {
        const uint32_t lsr = rd(s, LSR);
        countLineStatus(s, lsr);
        if (!(lsr & LSR_DR))
            break;
        const auto byte = static_cast<uint8_t>(rd(s, RBR_THR));
        if (!switch_joycon_spsc_rx_stage(&s.rxRing, &head, tail, byte)) {
            s.frameLength = 0;
            continue;
        }
        frameByte(s, byte);
        got = true;
    }
    if (got) {
        __atomic_store_n(&s.rxTime, mach_absolute_time(), __ATOMIC_RELAXED);
        switch_joycon_spsc_rx_commit_head(&s.rxRing, head);
    }
    return got;
}

// Primary IRQ filter (or workloop takeTail with `irq` disabled): takes DMA words into SPSC RX ring.
// Never calls IODelay; on first-reply magic failure masks UART RX/DMA and defers stop/settle to uartAction.
bool SwitchJoyConRail::takeDma(Side &s)
{
    const uint32_t words = s.dmaRing.size / 4;
    const switch_joycon_rx_take take =
        switch_joycon_rx_ring_take(words, s.dmaTaken, tegra_apbdma_moved(s.dma, nullptr),
                                   tegra_apbdma_interrupts(s.dma) - s.dmaLapsAtStart, s.dmaWords);
    if (take.overrun) {
        __atomic_store_n(&s.dmaOverrun, true, __ATOMIC_RELEASE);
        return true;
    }
    const auto *ring = reinterpret_cast<const uint32_t *>(s.dmaRing.va);
    if (take.count) {
        for (uint32_t done = 0; done < take.count;) {
            const uint32_t start = (take.from + done) % words;
            const uint32_t run = take.count - done < words - start ? take.count - done : words - start;
            switch_dcache_clean_invalidate_lines(s.dmaRing.va + static_cast<size_t>(start) * 4,
                                                 static_cast<size_t>(run) * 4);
            done += run;
        }
        switch_dcache_sync();
    }
    if (take.count && !s.dmaChecked) {
        s.dmaChecked = true;
        uint32_t i = 0;
        while (i < take.count && !(ring[i] & 0xffu))
            ++i;
        const bool magic = take.count - i >= 3 && (ring[i] & 0xffu) == kReplyMagic[0] &&
                           (ring[i + 1] & 0xffu) == kReplyMagic[1] && (ring[i + 2] & 0xffu) == kReplyMagic[2];
        if (take.from != 0 || !magic) {
            wr(s, IIR_FCR, FCR_FIFO | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
            s.ier &= ~(IER_RX | IER_EORD);
            wr(s, IER, s.ier);
            __atomic_store_n(&s.dmaFailed, "the first reply by DMA did not start with the reply magic",
                             __ATOMIC_RELEASE);
            return true;
        }
    }
    uint32_t head = s.rxRing.head;
    const uint32_t tail = switch_joycon_spsc_rx_peek_tail(&s.rxRing);
    for (uint32_t i = 0; i < take.count; ++i) {
        const auto byte = static_cast<uint8_t>(ring[(take.from + i) % words]);
        if (!switch_joycon_spsc_rx_stage(&s.rxRing, &head, tail, byte)) {
            s.frameLength = 0;
            continue;
        }
        frameByte(s, byte);
    }
    if (take.count) {
        __atomic_store_n(&s.rxTime, mach_absolute_time(), __ATOMIC_RELAXED);
        switch_joycon_spsc_rx_commit_head(&s.rxRing, head);
        __atomic_store_n(&s.dmaTakes, s.dmaTakes + 1u, __ATOMIC_RELAXED);
    }
    s.dmaTaken = (take.from + take.count) % words;
    s.dmaWords += take.count;
    const bool behind = (rd(s, LSR) & LSR_DR) != 0u;
    if (behind)
        __atomic_store_n(&s.dmaLate, s.dmaLate + 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s.dmaTail, behind || s.frameLength != 0, __ATOMIC_RELEASE);
    return take.count != 0;
}

// Steady-state fast path does one acquire load of dmaTail and returns immediately.
// Stuck-DMA check runs only on the silence path (s.unanswered > 0).
void SwitchJoyConRail::takeTail(Side &s)
{
    if (!s.dmaOn)
        return;
    const bool tail = __atomic_load_n(&s.dmaTail, __ATOMIC_ACQUIRE);
    const bool checkStuck = s.unanswered > 0;
    if (!tail && !checkStuck) {
        s.dmaWaited = false;
        return;
    }
    bool got = false;
    const char *stuckWhy = nullptr;
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    if (s.dmaOn && s.dmaTail) {
        got = takeDma(s);
        if (got)
            __atomic_store_n(&s.dmaTails, s.dmaTails + 1u, __ATOMIC_RELAXED);
        if (s.dmaOn && !s.dmaTail && !__atomic_load_n(&s.dmaFailed, __ATOMIC_RELAXED))
            wr(s, IER, s.ier);
    }
    if (s.dmaOn && checkStuck && !__atomic_load_n(&s.dmaFailed, __ATOMIC_RELAXED)) {
        const uint32_t at = (tegra_apbdma_moved(s.dma, nullptr) / 4) % (s.dmaRing.size / 4);
        const bool waiting = (rd(s, LSR) & LSR_DR) != 0u;
        if (waiting && s.dmaWaited && at == s.dmaTickAt)
            stuckWhy = "bytes waited in the FIFO for a whole request interval with the channel not moving";
        s.dmaWaited = waiting;
        s.dmaTickAt = at;
    }
    if (stuckWhy) {
        finishDmaFailure(s, stuckWhy);
        got = true;
    }
    if (wasEnabled)
        setIrqEnabled(s, true);
    if (got)
        s.irq->signalInterrupt();
}

void SwitchJoyConRail::startDma(Side &s)
{
    if (!s.dma || s.dmaOff || s.dmaOn)
        return;
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    (void)drainRx(s);
    s.dmaTaken = 0;
    s.dmaWords = 0;
    s.dmaChecked = false;
    s.dmaTail = false;
    s.dmaWaited = false;
    s.dmaLapsAtStart = tegra_apbdma_interrupts(s.dma);
    switch_dcache_clean_invalidate(s.dmaRing.va, s.dmaRing.size);
    const unsigned request = s.cfg->uart == 'B' ? TEGRA_APBDMA_REQ_UARTB : TEGRA_APBDMA_REQ_UARTC;
    tegra_apbdma_start_rx_words_ring(s.dma, static_cast<uint32_t>(kUartPA + s.cfg->uartOffset + RBR_THR), request,
                                     s.dmaRing.pa, s.dmaRing.size);
    wr(s, IIR_FCR, FCR_FIFO | FCR_DMA | FCR_RX_TRIG_1 | FCR_TX_TRIG_16);
    s.ier = (s.ier & IER_THRI) | IER_RX_DMA;
    wr(s, IER, s.ier);
    s.dmaOn = true;
    if (wasEnabled)
        setIrqEnabled(s, true);
}

// Masks UART DMA requests and settles outside any spinlock or IRQ-disabled critical section before stopping APB DMA.
void SwitchJoyConRail::stopDma(Side &s)
{
    if (!s.dmaOn)
        return;
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    wr(s, IIR_FCR, FCR_FIFO | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
    (void)rd(s, SPR);
    delay_for_interval_with_leeway(kDmaSettleUs, 4, NSEC_PER_USEC);
    tegra_apbdma_stop(s.dma);
    s.dmaOn = false;
    s.dmaTail = false;
    s.ier = (s.ier & IER_THRI) | IER_RX;
    wr(s, IER, s.ier);
    if (wasEnabled)
        setIrqEnabled(s, true);
}

// Workloop only (`irq` may already be disabled): stops DMA permanently for this boot, drains leftover PIO bytes, logs.
void SwitchJoyConRail::finishDmaFailure(Side &s, const char *why)
{
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    if (s.dmaOn) {
        wr(s, IIR_FCR, FCR_FIFO | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
        (void)rd(s, SPR);
        delay_for_interval_with_leeway(kDmaSettleUs, 4, NSEC_PER_USEC);
        tegra_apbdma_stop(s.dma);
        s.dmaOn = false;
        s.dmaTail = false;
    }
    s.dmaOff = true;
    (void)drainRx(s);
    s.ier = (s.ier & IER_THRI) | IER_RX;
    wr(s, IER, s.ier);
    if (wasEnabled)
        setIrqEnabled(s, true);
    char dma[320];
    tegra_apbdma_describe(s.dma, dma, sizeof(dma));
    IOLog("SwitchJoyCon: UART-%c RX DMA off for this boot (%s): RX by interrupt per FIFO trigger as before; %s\n",
          s.cfg->uart, why, dma);
}

void SwitchJoyConRail::uartAction(OSObject *owner, IOInterruptEventSource *sender, int)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (!s)
        return;
    if (__atomic_load_n(&s->stuck, __ATOMIC_ACQUIRE)) {
        char gic[160];
        switch_interrupt_describe(s->cfg->intid, gic, sizeof(gic));
        panic("SwitchJoyCon: UART-%c interrupt stuck (INTID %u): IIR %#x LSR %#x, %s", s->cfg->uart, s->cfg->intid,
              rd(*s, IIR_FCR), rd(*s, LSR), gic);
    }
    if (__atomic_load_n(&s->dmaOverrun, __ATOMIC_ACQUIRE)) {
        char dma[320];
        tegra_apbdma_describe(s->dma, dma, sizeof(dma));
        panic("SwitchJoyCon: UART-%c RX DMA overran its ring (data lost): taken %llu words, at word %u, %s",
              s->cfg->uart, s->dmaWords, s->dmaTaken, dma);
    }
    // Relaxed load guard avoids unconditional LL/SC exchange on every steady-state packet.
    if (__atomic_load_n(&s->dmaFailed, __ATOMIC_RELAXED) != nullptr) {
        if (const char *why = __atomic_exchange_n(&s->dmaFailed, nullptr, __ATOMIC_ACQ_REL))
            finishDmaFailure(*s, why);
    }
    const uint32_t errors = __atomic_load_n(&s->lineErrors, __ATOMIC_RELAXED);
    if (errors != s->errorsSeen) {
        s->errorsSeen = errors;
        s->packetLength = 0;
    }
    for (;;) {
        const uint32_t head = switch_joycon_spsc_rx_peek_head(&s->rxRing);
        uint32_t tail = s->rxRing.tail;
        if (tail == head)
            break;
        const uint64_t rxTime = __atomic_load_n(&s->rxTime, __ATOMIC_RELAXED);
        const uint32_t epoch = s->epoch;
        while (tail != head && s->epoch == epoch) {
            const uint8_t byte = s->rxRing.ring[tail];
            tail = (tail + 1u) & SWITCH_JOYCON_RX_RING_MASK;
            if (s->heardCount < sizeof(s->heard))
                s->heard[s->heardCount] = byte;
            ++s->heardCount;
            if (s->fast)
                ++s->fastHeardCount;
            if (s->phase == kBackoff)
                ++s->lateCount;
            if (s->phase == kHandshake) {
                if (s->stepHeardCount < sizeof(s->stepHeard))
                    s->stepHeard[s->stepHeardCount] = byte;
                ++s->stepHeardCount;
            }
            if (s->packetLength < 3 && byte != kReplyMagic[s->packetLength]) {
                s->packetLength = byte == 0x19 ? 1 : 0;
                if (s->packetLength)
                    s->packet[0] = byte;
                continue;
            }
            s->packet[s->packetLength++] = byte;
            if (s->packetLength < 5)
                continue;
            const size_t total = 5 + (s->packet[3] | static_cast<size_t>(s->packet[4]) << 8);
            if (total < kMinPacket || total > sizeof(s->packet)) {
                ++s->dropped;
                if (total > sizeof(s->packet) && !s->oversizeLogged) {
                    s->oversizeLogged = true;
                    IOLog("SwitchJoyCon: Joy-Con (%c) rail packet header gives %zu bytes (cmd %#x), dropped\n",
                          s->cfg->letter, total, s->packet[5]);
                }
                s->packetLength = 0;
                continue;
            }
            if (s->packetLength < total)
                continue;
            s->packetLength = 0;
            switch_joycon_spsc_rx_commit_tail(&s->rxRing, tail);
            self->received(*s, s->packet, total, rxTime);
        }
        if (s->epoch == epoch)
            switch_joycon_spsc_rx_commit_tail(&s->rxRing, tail);
    }
}

void SwitchJoyConRail::received(Side &s, const uint8_t *packet, size_t length, uint64_t rxTime)
{
    if (s.deliveryAction != kDeliveryNone) {
        s.deliveryAction = kDeliveryNone;
        if (s.phase == kRunning)
            s.timer->cancelTimeout();
    }
    const uint64_t now = switch_now_ms();
    if (s.phase == kRunning && s.unanswered == 0 && !s.rumbleSent && !probing(s))
        ++s.unsolicited;
    if (s.phase == kRunning && s.silentMs > kQuietMS) {
        IOLog("SwitchJoyCon: Joy-Con (%c) on the rail answered again after %u requests unanswered over %llu ms (line "
              "errors %u, breaks %u in between)\n",
              s.cfg->letter, s.unanswered - 1, now - s.lastReply,
              __atomic_load_n(&s.lineErrors, __ATOMIC_RELAXED) - s.errorsAtReply,
              __atomic_load_n(&s.breaks, __ATOMIC_RELAXED) - s.breaksAtReply);
        ++s.recoveries;
    }
    s.lastReply = now;
    s.unanswered = 0;
    s.silentMs = 0;
    s.errorsAtReply = __atomic_load_n(&s.lineErrors, __ATOMIC_RELAXED);
    s.breaksAtReply = __atomic_load_n(&s.breaks, __ATOMIC_RELAXED);
    const uint8_t cmd = packet[5];
    switch (s.phase) {
    case kHandshake:
        handshakeReply(s, cmd, packet[6], packet + kHeader, length > kHeader ? length - kHeader : 0);
        break;
    case kPairing:
        if (cmd == kCmdExtRet && length > kHeader)
            pairingReply(s, packet + kHeader, length - kHeader);
        break;
    case kCalibrating:
        if (cmd == kCmdExtRet && length > kHeader)
            calibrationReply(s, packet + kHeader, length - kHeader);
        break;
    case kRunning:
#if DEVELOPMENT || DEBUG
        if (probePacket(s, packet, length, now))
            break;
#endif
        if (cmd == kCmdExtRet && length > kHeader)
            runningReport(s, packet + kHeader, length - kHeader, rxTime);
        break;
    case kEnding:
        if (cmd == kCmdInitRet && packet[6] == kInitHidDisconnect)
            detach(s);
        break;
    default:
        break;
    }
}

void SwitchJoyConRail::send(Side &s, const uint8_t *bytes, size_t length)
{
    if (!txIdle(s)) {
        (void)startDeliveryCheck(s, "TX still busy", kDeliverySend);
        return;
    }
    // Fast path for steady-state running packets that fit the 32-byte TX FIFO (all 1F polls and 0x10 rumbles):
    // writes RBR_THR directly without touching IER, disabling IRQ, or taking any lock.
    if (s.phase == kRunning && length <= kTxFifoDepth && (rd(s, LSR) & LSR_THRE)) {
        const size_t burst = length > kTxBurst ? kTxBurst : length;
        size_t pos = 0;
        while (pos < burst)
            wr(s, RBR_THR, bytes[pos++]);
        while (pos < length && !(rd(s, LSR) & LSR_TX_FULL))
            wr(s, RBR_THR, bytes[pos++]);
        if (pos == length) {
            s.txLength = static_cast<uint32_t>(length);
            s.txPosition = static_cast<uint32_t>(length);
            switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_WAIT_RX);
            return;
        }
        // Unlikely fallback if FIFO filled early: hand the tail to THRI cleanly below.
        const bool wasEnabled = s.irqEnabled;
        setIrqEnabled(s, false);
        memcpy(s.tx, bytes, length);
        s.txLength = static_cast<uint32_t>(length);
        s.txPosition = static_cast<uint32_t>(pos);
        switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_TX_BUSY);
        s.ier |= IER_THRI;
        wr(s, IER, s.ier);
        if (wasEnabled)
            setIrqEnabled(s, true);
        return;
    }
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    memcpy(s.tx, bytes, length);
    s.txLength = static_cast<uint32_t>(length);
    s.txPosition = 0;
    fillTx(s);
    if (s.txPosition < s.txLength || s.phase != kRunning) {
        switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_TX_BUSY);
        s.ier |= IER_THRI;
        wr(s, IER, s.ier);
    } else {
        switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_WAIT_RX);
    }
    if (wasEnabled)
        setIrqEnabled(s, true);
}

bool SwitchJoyConRail::txIdle(const Side &s)
{
    return switch_joycon_link_tx_idle(&s.linkState);
}

void SwitchJoyConRail::sendCommand(Side &s, uint8_t subcmd, const uint8_t *payload, uint16_t length)
{
    uint8_t packet[kHeader + 8];
    send(s, packet, switch_joycon_rail_packet(packet, kCmdExtSend, subcmd, payload, length));
}

void SwitchJoyConRail::sendReport(Side &s, const uint8_t *report, size_t length)
{
    uint8_t packet[kHeader + kMaxOutput];
    send(s, packet, switch_joycon_rail_packet(packet, kCmdExtRet, 0, report, static_cast<uint16_t>(length)));
}

void SwitchJoyConRail::sendOutput(Side &s, uint8_t *report, size_t length)
{
    report[1] = static_cast<uint8_t>(s.counter++ & 0xfu);
    sendReport(s, report, length);
}

void SwitchJoyConRail::sendSubcommand(Side &s, uint8_t id, const uint8_t *data, size_t length)
{
    uint8_t report[SWITCH_JOYCON_SUBCOMMAND_REPORT];
    switch_joycon_subcommand_report(report, s.counter++, id, data, length);
    sendReport(s, report, sizeof(report));
}

// setReport cold path (user client thread): pushes into lock-free ring/seqlock and immediately wakes the side loop.
IOReturn SwitchJoyConRail::output(void *context, uint32_t generation, const uint8_t *report, size_t length)
{
    Side &s = *static_cast<Side *>(context);
    if (length < 2 || length > kMaxOutput)
        return kIOReturnBadArgument;
    IOReturn result = kIOReturnSuccess;
    IOLockLock(s.outProdLock);
    if (generation != __atomic_load_n(&s.hidGeneration, __ATOMIC_ACQUIRE)) {
        result = kIOReturnOffline;
    } else if (report[0] == kReportRumble) {
        switch_joycon_rumble_slot_write(&s.rumbleSlot, generation, report, length);
    } else {
        uint8_t buf[kMaxOutput];
        memcpy(buf, report, length);
        // Input report mode 0x3F silences a rail Joy-Con: ask for 0x30 instead.
        if (buf[0] == kReportSubcommand && length > 11 && buf[10] == kSubReportMode && buf[11] == kModeSimpleHid)
            buf[11] = kReportFull;
        if (!switch_joycon_cmd_ring_push(&s.cmdRing, generation, buf, length))
            result = kIOReturnNoSpace;
    }
    IOLockUnlock(s.outProdLock);
    if (result == kIOReturnSuccess)
        s.wake->interruptOccurred(nullptr, nullptr, 0);
    return result;
}

void SwitchJoyConRail::sessionEnded(void *context, uint32_t generation, int32_t servedPid, int32_t openerPid)
{
    Side &s = *static_cast<Side *>(context);
    IOLockLock(s.outProdLock);
    if (generation == __atomic_load_n(&s.hidGeneration, __ATOMIC_ACQUIRE)) {
        s.republishGen = generation;
        s.servedPid = servedPid;
        s.openerPid = openerPid;
        __atomic_store_n(&s.republishPending, 1u, __ATOMIC_RELEASE);
    }
    IOLockUnlock(s.outProdLock);
    s.wake->interruptOccurred(nullptr, nullptr, 0);
}

void SwitchJoyConRail::configureUart(Side &s, bool fast)
{
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    s.ier = 0;
    wr(s, IER, 0);
    (void)rd(s, IER);
    carWrite(s.cfg->clockSource, kUartDivEnable | (fast ? kDiv3M : kDiv1M));
    wr(s, LCR, LCR_DLAB | LCR_8N1);
    wr(s, RBR_THR, 1);
    wr(s, IER, 0);
    wr(s, LCR, (fast ? LCR_2STOP : 0) | LCR_8N1);
    (void)rd(s, SPR);
    wr(s, IIR_FCR, FCR_FIFO);
    (void)rd(s, SPR);

    delay_for_interval_with_leeway(20, 10, NSEC_PER_USEC);  // >= 3 UART clocks after enabling FIFO (T210 erratum)
    wr(s, MCR, 0);
    delay_for_interval_with_leeway(96, 20, NSEC_PER_USEC);
    wr(s, IIR_FCR, FCR_FIFO | FCR_RX_CLR | FCR_TX_CLR | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
    wr(s, IRDA_CSR, CSR_INVERT_TXD | CSR_INVERT_RTS);
    wr(s, MCR, MCR_RTS_EN);
    IODelay(3);

    while (rd(s, LSR) & LSR_DR)
        (void)rd(s, RBR_THR);
    switch_joycon_spsc_rx_drop(&s.rxRing);
    s.epoch++;
    s.frameLength = 0;
    s.packetDone = false;
    s.txLength = s.txPosition = 0;
    s.fast = fast;
    switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_IDLE);
    s.ier = IER_RX;
    wr(s, IER, s.ier);
    s.packetLength = 0;
    if (wasEnabled)
        setIrqEnabled(s, true);
}

void SwitchJoyConRail::enterDetect(Side &s)
{
    s.poll->cancelTimeout();
    s.pollInterval = 0;
    s.pollNext = 0;
    s.deliveryAction = kDeliveryNone;
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    stopDma(s);
    s.ier = 0;
    wr(s, IER, 0);
    s.txLength = s.txPosition = 0;
    switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_IDLE);
    if (wasEnabled)
        setIrqEnabled(s, true);
    gpioMask(s.cfg->txPin, GPIO_MSK_OE, false);
    gpioMask(s.cfg->txPin, GPIO_MSK_CNF, true);
    setCharging(s, true);
    s.phase = kSettling;
    updateBusyHold(s);
    s.timer->setTimeoutMS(kSettleMS);
}

void SwitchJoyConRail::detectAction(OSObject *owner, IOInterruptEventSource *sender, int)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (s && s->phase == kWaiting && tegra_gpio_read(s->cfg->detectPin) == 0)
        self->attachJoyCon(*s);
}

void SwitchJoyConRail::attachJoyCon(Side &s)
{
    s.detect->disable();
    s.timer->cancelTimeout();
    s.deliveryAction = kDeliveryNone;
    s.attempts = s.fastFailures = 0;
    s.oversizeLogged = false;
    s.furthest = 0;
    s.heardCount = s.fastHeardCount = s.fastSent = s.lateCount = 0;
    s.interruptsAtAttach = __atomic_load_n(&s.interrupts, __ATOMIC_RELAXED);
    s.errorsAtAttach = __atomic_load_n(&s.lineErrors, __ATOMIC_RELAXED);
    s.fastErrorsAtAttach = __atomic_load_n(&s.fastErrors, __ATOMIC_RELAXED);
    s.paired = false;
    startAttempt(s);
}

void SwitchJoyConRail::startAttempt(Side &s)
{
    configureUart(s, false);
    gpioMask(s.cfg->txPin, GPIO_MSK_CNF, false);
    s.phase = kHandshake;
    s.step = kWake;
    s.sends = 0;
    s.stepHeardCount = 0;
    sendStep(s);
}

void SwitchJoyConRail::sendStep(Side &s)
{
    static const uint8_t wake[] = {0xa1, 0xa2,          0xa3, 0xa4, 0x19, 0x01, 0x03, 0x07,
                                   0x00, kCmdHandshake, 0x02, 0x01, 0x7e, 0x00, 0x00, 0x00};
    static const uint8_t baud[] = {0xc0, 0xc6, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x00};  // 3,000,000
    static const uint8_t rate[] = {0x0a, 0x00, 0x00, 0x00};                          // 10 ms (100 Hz)
    uint32_t timeout = kCommandMS;
    if (s.step > s.furthest)
        s.furthest = s.step;
    ++s.sends;
    if (s.fast)
        ++s.fastSent;
    switch (s.step) {
    case kWake:
        send(s, wake, sizeof(wake));
        timeout = kHandshakeMS;
        break;
    case kInfo:
        sendCommand(s, kInitMac);
        break;
    case kBaud:
        sendCommand(s, kInitBaud, baud, sizeof(baud));
        break;
    case kDisconnect:
        sendCommand(s, kInitHidDisconnect);
        timeout = kResendMS;
        break;
    case kConnect:
        sendCommand(s, kInitHidConnect);
        timeout = kFastCommandMS;
        break;
    case kRate:
        sendCommand(s, kInitHidRate, rate, sizeof(rate));
        timeout = kFastCommandMS;
        break;
    default:
        break;
    }
    s.timer->setTimeoutMS(timeout);
}

void SwitchJoyConRail::handshakeReply(Side &s, uint8_t cmd, uint8_t echo, const uint8_t *payload, size_t length)
{
    static const uint8_t expected[] = {0, kInitMac, kInitBaud, kInitHidDisconnect, kInitHidConnect, kInitHidRate};
    if (s.step == kWake ? cmd != kCmdHandshake : (cmd != kCmdInitRet || echo != expected[s.step]))
        return;
    if (s.step == kInfo) {
        if (length < 7)
            return;
        for (unsigned i = 0; i < 6; ++i)
            s.mac[i] = payload[6 - i];
    }
    if (s.step == kBaud)
        configureUart(s, true);
    if (s.step != kRate) {
        ++s.step;
        s.sends = 0;
        s.stepHeardCount = 0;
        sendStep(s);
        return;
    }
    s.timer->cancelTimeout();
    s.pairPending = pairing;
    if (pairing && hostKnown) {
        startPairing(s);
    } else {
        calibrate(s);
    }
}

void SwitchJoyConRail::failAttempt(Side &s, const char *why)
{
    if (startDeliveryCheck(s, why, kDeliveryHandshake))
        return;
    finishFailAttempt(s, why);
}

void SwitchJoyConRail::finishFailAttempt(Side &s, const char *why)
{
    if (++s.attempts >= kAttempts || (s.fast && ++s.fastFailures >= kFastAttempts)) {
        char state[640];
        describe(s, state, sizeof(state));
        IOLog("SwitchJoyCon: Joy-Con (%c) on the rail does not answer (%s): %s\n", s.cfg->letter, why, state);
        enterDetect(s);
        return;
    }
    s.phase = kBackoff;
    s.timer->setTimeoutMS(s.fast ? kJoyConTimeoutMS : kRetryMS);
}

void SwitchJoyConRail::startPairing(Side &s)
{
    s.pairPending = false;
    s.phase = kPairing;
    s.step = kReportSlot ? kReadSlot : kDeviceInfo;
    s.slot = 0;
    s.slotInUse = s.slotCorrupt = false;
    sendPairStep(s);
}

void SwitchJoyConRail::endSession(Side &s)
{
    s.poll->cancelTimeout();
    s.pollInterval = 0;
    s.pollNext = 0;
    dropHid(s);
    stopDma(s);
    s.phase = kEnding;
    updateBusyHold(s);
    sendCommand(s, kInitHidDisconnect);
    s.timer->setTimeoutMS(kFastCommandMS);
}

void SwitchJoyConRail::sendPairStep(Side &s)
{
    switch (s.step) {
    case kReadSlot:
    case kReadSlotTail: {
        const bool tail = s.step == kReadSlotTail;
        const uint32_t address = kSlotBase + s.slot * kSlotStride + (tail ? SWITCH_JOYCON_SLOT_HEAD : 0);
        const uint8_t read[5] = {static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8),
                                 static_cast<uint8_t>(address >> 16), static_cast<uint8_t>(address >> 24),
                                 static_cast<uint8_t>(tail ? SWITCH_JOYCON_SLOT_TAIL : SWITCH_JOYCON_SLOT_HEAD)};
        sendSubcommand(s, kSubSpiRead, read, sizeof(read));
        break;
    }
    case kDeviceInfo:
        sendSubcommand(s, kSubDeviceInfo, nullptr, 0);
        break;
    case kShipment: {
        const uint8_t off = 0;
        sendSubcommand(s, kSubShipment, &off, 1);
        break;
    }
    case kSendHost:
    case kGetKey:
    case kSave: {
        static const uint8_t types[] = {SWITCH_JOYCON_PAIR_HOST, SWITCH_JOYCON_PAIR_KEY, SWITCH_JOYCON_PAIR_SAVE};
        uint8_t request[SWITCH_JOYCON_PAIRING_REQUEST];
        const size_t length = switch_joycon_pairing_request(request, types[s.step - kSendHost], host);
        sendSubcommand(s, kSubPairing, request, length);
        break;
    }
    default:
        break;
    }
    s.timer->setTimeoutMS(kSubcommandMS);
}

void SwitchJoyConRail::pairingSlotReply(Side &s, const uint8_t *data, bool ack, size_t length)
{
    const bool tail = s.step == kReadSlotTail;
    const uint32_t size = tail ? SWITCH_JOYCON_SLOT_TAIL : SWITCH_JOYCON_SLOT_HEAD;
    if (!ack || length < 15 + 5 + size) {
        finishPairing(s, false, "pairing slot read refused");
        return;
    }
    const uint32_t address = data[0] | static_cast<uint32_t>(data[1]) << 8 | static_cast<uint32_t>(data[2]) << 16 |
                             static_cast<uint32_t>(data[3]) << 24;
    if (address != kSlotBase + s.slot * kSlotStride + (tail ? SWITCH_JOYCON_SLOT_HEAD : 0))
        return;
    memcpy(s.slotBytes + (tail ? SWITCH_JOYCON_SLOT_HEAD : 0), data + 5, size);
    if (!tail) {
        switch (switch_joycon_slot_head_kind(s.slotBytes)) {
        case SWITCH_JOYCON_SLOT_EARLIER:
            if (++s.slot < kSlots) {
                sendPairStep(s);
                return;
            }
            break;
        case SWITCH_JOYCON_SLOT_IN_USE:
            s.step = kReadSlotTail;
            sendPairStep(s);
            return;
        case SWITCH_JOYCON_SLOT_END:
            break;
        }
        s.slotInUse = false;
    } else {
        s.slotInUse = switch_joycon_slot_valid(s.slotBytes);
        s.slotCorrupt = !s.slotInUse;
    }
    s.step = kDeviceInfo;
    sendPairStep(s);
}

void SwitchJoyConRail::pairingReply(Side &s, const uint8_t *report, size_t length)
{
    if (length < 16 || report[0] != kReportReply)
        return;
    const uint8_t id = report[14];
    const uint8_t *data = report + 15;
    const bool ack = (report[13] & 0x80) != 0;
    if ((s.step == kReadSlot || s.step == kReadSlotTail) && id == kSubSpiRead) {
        pairingSlotReply(s, data, ack, length);
        return;
    }
    if ((s.step == kDeviceInfo && id == kSubDeviceInfo) || (s.step == kShipment && id == kSubShipment)) {
        if (!ack) {
            finishPairing(s, false, s.step == kDeviceInfo ? "device info refused" : "shipment state refused");
            return;
        }
        ++s.step;
        sendPairStep(s);
        return;
    }
    if (id != kSubPairing || !ack || (s.step != kSave && data[0] != (s.step == kSendHost ? 0x01 : 0x02)))
        return;
    if (s.step == kSendHost) {
        s.step = kGetKey;
        sendPairStep(s);
        return;
    }
    if (s.step == kGetKey) {
        if (length < 15 + 1 + 16) {
            finishPairing(s, false, "short pairing key reply");
            return;
        }
        for (unsigned i = 0; i < 16; ++i)
            s.key[i] = data[1 + 15 - i] ^ 0xaa;
        s.step = kSave;
        sendPairStep(s);
        return;
    }
    finishPairing(s, true, "rail");
}

void SwitchJoyConRail::finishPairing(Side &s, bool paired, const char *how)
{
    s.timer->cancelTimeout();
    s.paired = paired;
#if DEVELOPMENT || DEBUG
    char found[96] = "before: no slot in use";
    if (s.slotCorrupt)
        snprintf(found, sizeof(found), "before: slot %u in use fails its checksum", s.slot);
    if (s.slotInUse) {
        const uint8_t *h = s.slotBytes + 4;
        snprintf(found, sizeof(found),
                 "before: slot %u in use names %02x:%02x:%02x:%02x:%02x:%02x, host capability 0x%02x", s.slot, h[0],
                 h[1], h[2], h[3], h[4], h[5], s.slotBytes[SWITCH_JOYCON_SLOT_CAPS]);
    }
#endif
    if (paired) {
        publishPairing(s, how);
#if DEVELOPMENT || DEBUG
        os_log(OS_LOG_DEFAULT,
               "SwitchJoyCon: Joy-Con (%c) %02x:%02x:%02x:%02x:%02x:%02x paired to %02x:%02x:%02x:%02x:%02x:%02x over "
               "the rail (%s)",
               s.cfg->letter, s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5], host[0], host[1], host[2],
               host[3], host[4], host[5], kReportSlot ? found : "");
#endif
    } else {
        IOLog("SwitchJoyCon: Joy-Con (%c) not paired for Bluetooth: %s\n", s.cfg->letter, how);
    }
    calibrate(s);
}

void SwitchJoyConRail::calibrate(Side &s)
{
    s.phase = kCalibrating;
    s.step = kCalibrateMode;
    if (!s.stickKnown || memcmp(s.stickMac, s.mac, sizeof(s.mac)) != 0) {
        switch_joycon_stick_defaults(&s.stick, s.cfg->left);
        s.step = 0;
    }
    sendCalibrationStep(s);
}

void SwitchJoyConRail::sendCalibrationStep(Side &s)
{
    if (s.step < SWITCH_JOYCON_STICK_READS) {
        uint8_t read[5];
        switch_joycon_stick_request(&s.stick, s.step, read);
        sendSubcommand(s, kSubSpiRead, read, sizeof(read));
    } else if (s.step == kCalibrateMode) {
        const uint8_t mode = kReportFull;
        sendSubcommand(s, kSubReportMode, &mode, 1);
    } else {
        const uint8_t off = 0;
        sendSubcommand(s, kSubImu, &off, 1);
    }
    s.timer->setTimeoutMS(kSubcommandMS);
}

const char *SwitchJoyConRail::calibrationStepName(uint8_t step)
{
    if (step < SWITCH_JOYCON_STICK_READS)
        return "stick calibration read";
    return step == kCalibrateMode ? "input report mode 0x30" : "IMU off";
}

void SwitchJoyConRail::calibrationReply(Side &s, const uint8_t *report, size_t length)
{
    if (length < 16 || report[0] != kReportReply)
        return;
    const bool ack = (report[13] & 0x80) != 0;
    if (s.step < SWITCH_JOYCON_STICK_READS) {
        if (report[14] != kSubSpiRead ||
            (ack && !switch_joycon_stick_reply(&s.stick, s.step, report + 15, length - 15)))
            return;
        ++s.step;
        sendCalibrationStep(s);
        return;
    }
    if (report[14] != (s.step == kCalibrateMode ? kSubReportMode : kSubImu))
        return;
    if (s.step == kCalibrateMode) {
        ++s.step;
        sendCalibrationStep(s);
        return;
    }
    s.timer->cancelTimeout();
    if (!s.stickKnown || memcmp(s.stickMac, s.mac, sizeof(s.mac)) != 0) {
        memcpy(s.stickMac, s.mac, sizeof(s.mac));
        s.stickKnown = true;
#if DEVELOPMENT || DEBUG
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: Joy-Con (%c) stick centre %u,%u (%s calibration), dead zone %u (%s)",
               s.cfg->letter, s.stick.centre[0], s.stick.centre[1], s.stick.user ? "user" : "factory",
               s.stick.dead_zone, s.stick.params ? "its own" : "default");
#endif
    }
    run(s);
}

void SwitchJoyConRail::publishPairing(Side &s, const char *source)
{
    if (s.record) {
        s.record->terminate();
        OSSafeReleaseNULL(s.record);
    }
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x", s.mac[0], s.mac[1], s.mac[2], s.mac[3],
             s.mac[4], s.mac[5]);
    auto *record = OSTypeAlloc(SwitchJoyConPairing);
    OSData *mac = OSData::withBytes(s.mac, 6);
    OSData *key = OSData::withBytes(s.key, 16);
    OSData *hostAddress = OSData::withBytes(host, 6);
    const bool ok =
        (record != nullptr) && (mac != nullptr) && (key != nullptr) && (hostAddress != nullptr) && record->init();
    if (ok) {
        record->setProperty("Address", mac);
        record->setProperty("AddressString", address);
        record->setProperty("LinkKey", key);
        record->setProperty("HostAddress", hostAddress);
        record->setProperty("Name", s.cfg->left ? "Joy-Con (L)" : "Joy-Con (R)");
        record->setProperty("ClassOfDevice", static_cast<unsigned long long>(kJoyConClass), 32);
        record->setProperty("VendorID", 0x057eULL, 32);
        record->setProperty("ProductID",
                            static_cast<unsigned long long>(s.cfg->left ? kSwitchJoyConLeftPID : kSwitchJoyConRightPID),
                            32);
        record->setProperty("Source", source);
    }
    OSSafeReleaseNULL(mac);
    OSSafeReleaseNULL(key);
    OSSafeReleaseNULL(hostAddress);
    if (!ok || !record->attach(this)) {
        OSSafeReleaseNULL(record);
        IOLog("SwitchJoyCon: Joy-Con (%c) pairing record not published\n", s.cfg->letter);
        return;
    }
    record->registerService();
    s.record = record;
}

void SwitchJoyConRail::run(Side &s)
{
    switch_joycon_cmd_ring_drop(&s.cmdRing);
    switch_joycon_rumble_slot_drop(&s.rumbleSlot);
    s.rumbleSent = false;
    s.phase = kRunning;
    s.lastReply = switch_now_ms();
    s.unanswered = 0;
    s.silentMs = 0;
    s.imuOff = false;
    s.ownReply = 0;
    s.latestFullLen = 0;
    s.replayPending = false;
    s.sysButtonDown = false;
    switch_joycon_cadence_reset(&s.cadence, s.lastReply);
    s.errorsAtReply = __atomic_load_n(&s.lineErrors, __ATOMIC_RELAXED);
    s.breaksAtReply = __atomic_load_n(&s.breaks, __ATOMIC_RELAXED);
    s.reports = 0;
    s.errorsAtRun = s.errorsAtReply;
    s.overflowsAtRun = __atomic_load_n(&s.rxRing.overflows, __ATOMIC_RELAXED);
    switch_joycon_use_reset(&s.use);
    s.hid = gamepadsLive(s, s.lastReply) ? createHid(s) : nullptr;
    updateBusyHold(s);
    startDma(s);  // before the first request so DMA takes its reply
    tick(s);
    armPoll(s, true);
}

IOService *SwitchJoyConRail::createHid(Side &s)
{
    const uint32_t gen = s.hidGeneration + 1u;
    __atomic_store_n(&s.hidGeneration, gen, __ATOMIC_RELEASE);
    switch_joycon_forward_reset(&s.forward);
    s.replayPending = true;
    updateBusyHold(s);
    return switch_joycon_hid_create(s.loop, s.cfg->left, s.mac, gen, &SwitchJoyConRail::output,
                                    &SwitchJoyConRail::sessionEnded, &s);
}

void SwitchJoyConRail::dropGamepad(Side &s)
{
    if (s.hid)
        switch_joycon_hid_destroy(s.hid);
    s.hid = nullptr;
    // Advancing generation invalidates any racing in-flight output/sessionEnded callback from the old device.
    __atomic_store_n(&s.hidGeneration, s.hidGeneration + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&s.republishPending, 0u, __ATOMIC_RELEASE);
    switch_joycon_cmd_ring_drop(&s.cmdRing);
    switch_joycon_rumble_slot_drop(&s.rumbleSlot);
    s.imuOff |= s.forward.imu;
    s.forward.imu = false;
    s.replayPending = false;
    updateBusyHold(s);
}

void SwitchJoyConRail::dropHid(Side &s)
{
    dropGamepad(s);
    if (s.sysButtonDown) {
        s.sysButtonDown = false;
        switch_joycon_hid_buttons_release(s.cfg->left);
    }
}

void SwitchJoyConRail::updateBusyHold(Side &s)
{
    const bool rumble = switch_joycon_rumble_moving(s.cfg->left, s.rumbleSlot.cached, s.rumbleSlot.cached_len);
    const uint32_t busy = (s.phase == kRunning && (s.forward.imu || rumble)) ? 1u : 0u;
    if (busy != s.busyHoldCached) {
        s.busyHoldCached = busy;
        __atomic_store_n(&s.busyHold, busy, __ATOMIC_RELAXED);
    }
}

bool SwitchJoyConRail::anyBusyHold() const
{
    return (__atomic_load_n(&sides[0].busyHold, __ATOMIC_RELAXED) |
            __atomic_load_n(&sides[1].busyHold, __ATOMIC_RELAXED)) != 0u;
}

// iOS 27 only: UIKitCore 24B119 enables UIFocusSystem while any GD/Gamepad IOHIDDevice exists; 17 publishes at attach.
bool SwitchJoyConRail::gamepadsLive(Side &s, uint64_t now)
{
#if XNU_12377_121_6
    if (!__atomic_load_n(&gamepads, __ATOMIC_ACQUIRE))
        return false;
    const uint64_t used = __atomic_load_n(&lastUse, __ATOMIC_RELAXED);
    const uint64_t touched = __atomic_load_n(&gLastTouchMs, __ATOMIC_RELAXED);
    if (switch_joycon_use_touch_preempt(used, touched, anyBusyHold(), now)) {
        idleGamepads(s, true);
        return false;
    }
    if (switch_joycon_use_idle(used, now)) {
        idleGamepads(s, false);
        return false;
    }
    return true;
#else
    (void)s;
    (void)now;
    return true;
#endif
}

void SwitchJoyConRail::noteUse(Side &s, uint64_t now)
{
    __atomic_store_n(&lastUse, now, __ATOMIC_RELAXED);
    if (__atomic_load_n(&gamepads, __ATOMIC_ACQUIRE))
        return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gamepads, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&gamepadsPublished, __atomic_load_n(&gamepadsPublished, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED);
    if (s.phase == kRunning && !s.hid)
        s.hid = createHid(s);
    (void)switch_joycon_cadence_wake(&s.cadence, true, now);
    otherSide(s).wake->interruptOccurred(nullptr, nullptr, 0);
#if DEVELOPMENT || DEBUG
    os_log(OS_LOG_DEFAULT, "SwitchJoyCon: rail Joy-Cons in use: game controllers published");
#endif
}

void SwitchJoyConRail::idleGamepads(Side &s, bool byTouch)
{
    uint32_t expected = 1u;
    if (!__atomic_compare_exchange_n(&gamepads, &expected, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&gamepadsIdled, __atomic_load_n(&gamepadsIdled, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED);
    dropGamepad(s);
    switch_joycon_use_reset(&s.use);
    otherSide(s).wake->interruptOccurred(nullptr, nullptr, 0);
#if DEVELOPMENT || DEBUG
    if (byTouch) {
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: rail Joy-Cons preempted by touch after %u ms idle: disconnected",
               SWITCH_JOYCON_USE_TOUCH_GRACE_MS);
    } else {
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: rail Joy-Cons idle for %u s: game controllers disconnected",
               SWITCH_JOYCON_USE_IDLE_MS / 1000u);
    }
#else
    (void)byTouch;
#endif
}

void SwitchJoyConRail::syncHid(Side &s)
{
#if XNU_12377_121_6
    const bool live = __atomic_load_n(&gamepads, __ATOMIC_ACQUIRE) != 0u;
    if (live && s.phase == kRunning && !s.hid) {
        s.hid = createHid(s);
    } else if (!live && s.hid) {
        dropGamepad(s);
        switch_joycon_use_reset(&s.use);
    }
#else
    (void)s;
#endif
}

void SwitchJoyConRail::republish(Side &s)
{
    if (!__atomic_load_n(&s.republishPending, __ATOMIC_ACQUIRE))
        return;
    IOLockLock(s.outProdLock);
    const uint32_t gen = s.republishGen;
    const int32_t served = s.servedPid;
    const int32_t opener = s.openerPid;
    __atomic_store_n(&s.republishPending, 0u, __ATOMIC_RELEASE);
    IOLockUnlock(s.outProdLock);
    if (gen != s.hidGeneration || __atomic_load_n(&republishOff, __ATOMIC_RELAXED) || s.phase != kRunning || !s.hid)
        return;
    if (!switch_joycon_republish_allowed(&s.guard, served, switch_now_ms())) {
        __atomic_store_n(&republishOff, 1u, __ATOMIC_RELAXED);
        IOLog("SwitchJoyCon: Joy-Con (%c): backboardd[%d] ended a second HID session within %u ms; not publishing "
              "Joy-Con HID devices again this boot\n",
              s.cfg->letter, served, static_cast<unsigned>(kSwitchJoyConRepublishGapMS));
        return;
    }
    dropHid(s);
    ++s.republished;
    s.rumbleSent = false;
    s.hid = createHid(s);
#if DEVELOPMENT || DEBUG
    if (opener) {
        os_log(OS_LOG_DEFAULT,
               "SwitchJoyCon: Joy-Con (%c) HID device published again: backboardd[%d] opened the one "
               "backboardd[%d] served",
               s.cfg->letter, opener, served);
    } else {
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: Joy-Con (%c) HID device published again: backboardd[%d] closed it",
               s.cfg->letter, served);
    }
#else
    (void)opener;
#endif
}

void SwitchJoyConRail::wakeAction(OSObject *owner, IOInterruptEventSource *sender, int)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    if (Side *s = self->sideFor(sender))
        self->handleWake(*s);
}

bool SwitchJoyConRail::popOutputCommand(Side &s, uint8_t report[kMaxOutput], size_t *length)
{
    if (!switch_joycon_cmd_ring_pop(&s.cmdRing, s.hidGeneration, report, length))
        return false;
    switch_joycon_forward_output(&s.forward, report, *length);
    updateBusyHold(s);
#if DEVELOPMENT || DEBUG
    if (report[0] == kReportSubcommand && *length > 11 && report[10] == kSubImu) {
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: Joy-Con (%c) HID client turns its IMU %s", s.cfg->letter,
               report[11] ? "on" : "off");
    }
#endif
    return true;
}

bool SwitchJoyConRail::dispatchQueuedCommand(Side &s, uint64_t now)
{
    if (s.phase != kRunning || probing(s) || s.deliveryAction != kDeliveryNone || s.unanswered != 0 || !txIdle(s))
        return false;
    uint8_t report[kMaxOutput];
    size_t length = 0;
    if (!popOutputCommand(s, report, &length))
        return false;
    (void)switch_joycon_cadence_wake(&s.cadence, s.hid != nullptr, now);
    s.cadence.sent_at = now;
    ++s.unanswered;
    ++s.requests;
    s.silentMs += SWITCH_JOYCON_CADENCE_ACTIVE_MS;
    s.rumbleSent = false;
    s.ownReply = 0;
    sendOutput(s, report, length);
    armPoll(s, true);
    return true;
}

// Handles cross-side HID sync, backboardd session republish, and immediate subcommand/rumble wakeups.
void SwitchJoyConRail::handleWake(Side &s)
{
    syncHid(s);
    republish(s);
    if (s.phase != kRunning || probing(s) || s.deliveryAction != kDeliveryNone)
        return;
    const uint64_t now = switch_now_ms();
    if (switch_joycon_rumble_slot_poll(&s.rumbleSlot, s.hidGeneration))
        updateBusyHold(s);
    const bool hasCmd = !switch_joycon_cmd_ring_empty(&s.cmdRing);
    const bool hasRumble = s.rumbleSlot.fresh;
    if (!hasCmd && !hasRumble)
        return;
    const bool woke = switch_joycon_cadence_wake(&s.cadence, s.hid != nullptr, now);
    if (hasCmd && dispatchQueuedCommand(s, now))
        return;
    if (hasRumble && s.unanswered == 0 && !s.rumbleSent && txIdle(s))
        rumbleAfterReply(s);
    if (woke || s.pollInterval > SWITCH_JOYCON_CADENCE_ACTIVE_MS)
        armPoll(s, true);
}

// Phase-locked per-side poll timer via wakeAtTime: active 10 ms (0 leeway), quiet 30 ms (2 ms leeway, exact phase).
void SwitchJoyConRail::armPoll(Side &s, bool resetPhase)
{
    if (s.phase != kRunning) {
        s.poll->cancelTimeout();
        s.pollInterval = 0;
        s.pollNext = 0;
        return;
    }
    const uint32_t interval = probing(s) ? SWITCH_JOYCON_CADENCE_ACTIVE_MS : switch_joycon_cadence_interval(&s.cadence);
    const uint64_t step = switch_abs_from_ms(interval);
    const uint64_t now = mach_absolute_time();
    if (resetPhase || s.pollInterval != interval || s.pollNext == 0) {
        s.pollNext = now + step;
    } else {
        s.pollNext += step;
        if (s.pollNext <= now)
            s.pollNext = now + step;
    }
    s.pollInterval = interval;
    if (interval == SWITCH_JOYCON_CADENCE_QUIET_MS) {
        s.poll->wakeAtTime(kIOTimeOptionsWithLeeway, s.pollNext, switch_abs_from_ms(kQuietLeewayMS));
    } else {
        s.poll->wakeAtTime(s.pollNext);
    }
}

void SwitchJoyConRail::pollAction(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (!s || s->phase != kRunning)
        return;
    self->tick(*s);
    (void)self->gamepadsLive(*s, switch_now_ms());
    armPoll(*s, false);
}

void SwitchJoyConRail::finishSilence(Side &s)
{
    const char *rxDma = s.dma && !s.dmaOff ? "idle" : "off";
    if (s.dmaOn)
        rxDma = "on";
    const uint32_t errs = __atomic_load_n(&s.lineErrors, __ATOMIC_RELAXED);
    const uint32_t brks = __atomic_load_n(&s.breaks, __ATOMIC_RELAXED);
    const uint32_t ovf = __atomic_load_n(&s.rxRing.overflows, __ATOMIC_RELAXED);
    IOLog("SwitchJoyCon: Joy-Con (%c) %02x:%02x:%02x:%02x:%02x:%02x (%s) on the rail stopped answering after %u "
          "input reports: %u requests unanswered over %llu ms (line errors %u, breaks %u since its last reply; line "
          "errors %u, ring overflows %u since the handshake; RX DMA %s: %u takes, %u found it behind, %u tails at "
          "the tick)\n",
          s.cfg->letter, s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5],
          s.paired ? "paired as its session opened" : "not paired", s.reports, s.unanswered,
          switch_now_ms() - s.lastReply, errs - s.errorsAtReply, brks - s.breaksAtReply, errs - s.errorsAtRun,
          ovf - s.overflowsAtRun, rxDma, __atomic_load_n(&s.dmaTakes, __ATOMIC_RELAXED),
          __atomic_load_n(&s.dmaLate, __ATOMIC_RELAXED), __atomic_load_n(&s.dmaTails, __ATOMIC_RELAXED));
    ++s.silences;
    detach(s);
}

void SwitchJoyConRail::tick(Side &s)
{
    takeTail(s);
    if (s.phase != kRunning || s.deliveryAction != kDeliveryNone)
        return;
#if DEVELOPMENT || DEBUG
    if (probeTick(s))
        return;
#endif
    if (s.silentMs >= kSilentMS) {
        if (startDeliveryCheck(s, "no reply to the requests of a whole silence", kDeliverySilence))
            return;
        finishSilence(s);
        return;
    }
    if (s.pairPending && hostKnown && s.unanswered == 0) {
        endSession(s);
        return;
    }
    if (switch_joycon_rumble_slot_poll(&s.rumbleSlot, s.hidGeneration))
        updateBusyHold(s);
    const bool busy = !switch_joycon_cmd_ring_empty(&s.cmdRing) || s.rumbleSlot.fresh || s.forward.imu ||
                      switch_joycon_rumble_moving(s.cfg->left, s.rumbleSlot.cached, s.rumbleSlot.cached_len);
    if (!switch_joycon_cadence_due(&s.cadence, busy || s.imuOff, switch_now_ms()))
        return;
    ++s.unanswered;
    ++s.requests;
    s.silentMs += switch_joycon_cadence_interval(&s.cadence);
    s.rumbleSent = false;
    s.ownReply = 0;
    if (s.imuOff) {
        const uint8_t off = 0;
        s.imuOff = false;
        s.ownReply = kSubImu;
        sendSubcommand(s, kSubImu, &off, 1);
        return;
    }
    uint8_t report[kMaxOutput];
    size_t length = 0;
    if (popOutputCommand(s, report, &length)) {
        sendOutput(s, report, length);
    } else {
        report[0] = kRequestInput;
        sendReport(s, report, 1);
    }
}

void SwitchJoyConRail::rumbleAfterReply(Side &s)
{
    if (s.rumbleSent || !txIdle(s))
        return;
    if (switch_joycon_rumble_slot_poll(&s.rumbleSlot, s.hidGeneration))
        updateBusyHold(s);
    if (s.rumbleSlot.cached_len < 10)
        return;
    const uint64_t now = switch_now_ms();
    const bool moving = switch_joycon_rumble_moving(s.cfg->left, s.rumbleSlot.cached, s.rumbleSlot.cached_len);
    if (!s.rumbleSlot.fresh && (!moving || now - s.rumbleAt < kRumbleKeepAliveMS))
        return;
    uint8_t report[kMaxOutput];
    const size_t length = s.rumbleSlot.cached_len;
    memcpy(report, s.rumbleSlot.cached, length);
    s.rumbleSlot.fresh = false;
    s.rumbleSent = true;
    s.rumbleAt = now;
    ++s.requests;
    sendOutput(s, report, length);
}

void SwitchJoyConRail::updateSystemButton(Side &s, const uint8_t *report, size_t length, uint64_t rxTime)
{
    if (length <= kSharedButtonsByte ||
        (report[0] != kReportReply && report[0] != kReportFull && report[0] != kReportNfcIr))
        return;
    const bool down = (report[kSharedButtonsByte] & (s.cfg->left ? kCaptureBit : kHomeBit)) != 0u;
    if (down == s.sysButtonDown)
        return;
    s.sysButtonDown = down;
    switch_joycon_hid_buttons_edge(s.cfg->left, down, rxTime);
}

void SwitchJoyConRail::runningReport(Side &s, const uint8_t *report, size_t length, uint64_t rxTime)
{
    if ((report[0] == kReportReply || report[0] == kReportFull || report[0] == kReportNfcIr) && length > 2) {
        const uint8_t level = report[2] >> 5;
        if (level == 4) {
            setCharging(s, false);
        } else if (level < 3) {
            setCharging(s, true);
        }
    }
    ++s.reports;
    ++s.replies;
    rumbleAfterReply(s);
    updateSystemButton(s, report, length, rxTime);

    if (report[0] == kReportFull && length >= 12) {
        const size_t copy = length < kStandardFull ? length : kStandardFull;
        memcpy(s.latestFull, report, copy);
        if (copy < kStandardFull)
            memset(s.latestFull + copy, 0, kStandardFull - copy);
        s.latestFullLen = kStandardFull;
    } else if (report[0] == kReportNfcIr && length >= kStandardFull) {
        memcpy(s.latestFull, report, kStandardFull);
        s.latestFull[0] = kReportFull;
        s.latestFullLen = kStandardFull;
    }

    const uint64_t now = switch_now_ms();
    if (switch_joycon_cadence_observe(&s.cadence, &s.stick, report, length, s.hid != nullptr, now) &&
        s.pollInterval > SWITCH_JOYCON_CADENCE_ACTIVE_MS) {
        armPoll(s, true);
    }
#if XNU_12377_121_6
    const bool published = __atomic_load_n(&gamepads, __ATOMIC_ACQUIRE) != 0u;
    if (switch_joycon_use_observe(&s.use, &s.stick, report, length, published))
        noteUse(s, now);
#endif
    if (s.hid && !ownReplyTaken(s, report, length)) {
        if (switch_joycon_forward_report(&s.forward, &s.stick, report, length)) {
            ++s.forwarded;
            switch_joycon_hid_input(s.hid, report, length, rxTime);
        } else {
            ++s.suppressed;
        }
        // Cold-publish input replay: once JoyConHIDServicePlugin completes 0x03 report mode, deliver live state.
        if (s.replayPending && s.latestFullLen && report[0] == kReportReply && length >= 15 &&
            report[14] == kSubReportMode) {
            s.replayPending = false;
            switch_joycon_forward_reprime(&s.forward);
            if (switch_joycon_forward_report(&s.forward, &s.stick, s.latestFull, s.latestFullLen)) {
                ++s.forwarded;
                switch_joycon_hid_input(s.hid, s.latestFull, s.latestFullLen, rxTime);
            }
        }
        if (nfc && report[0] == kReportNfcIr && length >= kStandardFull) {
            uint8_t standard[kStandardFull];
            memcpy(standard, report, sizeof(standard));
            standard[0] = kReportFull;
            switch_joycon_hid_input(s.hid, standard, sizeof(standard), rxTime);
        }
    }
    if (!s.rumbleSent && !switch_joycon_cmd_ring_empty(&s.cmdRing))
        (void)dispatchQueuedCommand(s, now);
}

bool SwitchJoyConRail::ownReplyTaken(Side &s, const uint8_t *report, size_t length)
{
    if (!s.ownReply || report[0] != kReportReply || length < 15 || report[14] != s.ownReply)
        return false;
    s.ownReply = 0;
    return true;
}

bool SwitchJoyConRail::probing(const Side &s)
{
#if DEVELOPMENT || DEBUG
    return s.probe.active;
#else
    (void)s;
    return false;
#endif
}

#if DEVELOPMENT || DEBUG
bool SwitchJoyConRail::probeTick(Side &s)
{
    const switch_joycon_probe_stage *stage = nullptr;
    const unsigned at = s.probe.stage;
    const uint8_t action = switch_joycon_probe_tick(&s.probe, switch_now_ms(), &stage);
    if (s.probe.stage != at) {
        char line[320];
        (void)switch_joycon_probe_format(&s.probe, at, line, sizeof(line));
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: probe Joy-Con (%c) %s", s.cfg->letter, line);
    }
    switch (action) {
    case SWITCH_JOYCON_PROBE_OFF:
        return false;
    case SWITCH_JOYCON_PROBE_REQUEST: {
        const uint8_t request = kRequestInput;
        sendReport(s, &request, 1);
        break;
    }
    case SWITCH_JOYCON_PROBE_INIT: {
        const uint8_t rate[4] = {stage->argument, 0, 0, 0};
        sendCommand(s, stage->command, stage->command == kInitHidRate ? rate : nullptr,
                    stage->command == kInitHidRate ? sizeof(rate) : 0);
        break;
    }
    case SWITCH_JOYCON_PROBE_MODE:
        sendSubcommand(s, stage->command, &stage->argument, 1);
        break;
    default:
        break;
    }
    return true;
}

bool SwitchJoyConRail::probePacket(Side &s, const uint8_t *packet, size_t length, uint64_t now)
{
    if (!s.probe.active)
        return false;
    switch_joycon_probe_packet(&s.probe, packet, length, now);
    return true;
}
#endif

void SwitchJoyConRail::detach(Side &s)
{
    s.poll->cancelTimeout();
    s.pollInterval = 0;
    s.pollNext = 0;
    s.timer->cancelTimeout();
    s.deliveryAction = kDeliveryNone;
    dropHid(s);
    enterDetect(s);
}

void SwitchJoyConRail::pause(Side &s)
{
    if (s.phase == kWaiting)
        tegra_gpio_check_delivery(s.cfg->detectPin, "SwitchJoyCon");
    s.poll->cancelTimeout();
    s.pollInterval = 0;
    s.pollNext = 0;
    s.timer->cancelTimeout();
    s.deliveryAction = kDeliveryNone;
    s.detect->disable();
    const bool wasEnabled = s.irqEnabled;
    setIrqEnabled(s, false);
    stopDma(s);
    s.ier = 0;
    wr(s, IER, 0);
    s.txLength = s.txPosition = 0;
    switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_IDLE);
    if (wasEnabled)
        setIrqEnabled(s, true);
    dropHid(s);
    s.phase = kSuspended;
    updateBusyHold(s);
}

void SwitchJoyConRail::setCharging(Side &s, bool on)
{
    if (s.chargeKnown && s.charging == on)
        return;
    if (tegra_gpio_output(s.cfg->chargePin, on ? 1 : 0))
        panic("SwitchJoyCon: charge-enable GPIO %u refused", s.cfg->chargePin);
    s.charging = on;
    s.chargeKnown = true;
}

void SwitchJoyConRail::timerAction(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (!s)
        return;
    if (s->deliveryAction != kDeliveryNone) {
        self->completeDeliveryCheck(*s);
        return;
    }
    switch (s->phase) {
    case kSettling:
        s->phase = kWaiting;
        s->detect->enable();
        if (tegra_gpio_read(s->cfg->detectPin) == 0)
            self->attachJoyCon(*s);
        break;
    case kWaiting:
        break;
    case kBackoff:
        self->startAttempt(*s);
        break;
    case kHandshake:
        if (s->step == kDisconnect && s->sends < kDisconnectSends) {
            SwitchJoyConRail::sendStep(*s);
        } else {
            self->failAttempt(*s, "handshake step timed out");
        }
        break;
    case kPairing:
        if (!startDeliveryCheck(*s, "pairing step timed out", kDeliveryPairing))
            self->finishPairing(*s, false, "no reply");
        break;
    case kCalibrating:
        if (startDeliveryCheck(*s, "calibration step timed out", kDeliveryCalibrating))
            break;
        IOLog("SwitchJoyCon: Joy-Con (%c) did not answer its %s; default stick calibration for this session\n",
              s->cfg->letter, calibrationStepName(s->step));
        self->run(*s);
        s->imuOff = true;
        break;
    case kEnding:
        if (startDeliveryCheck(*s, "no answer to 91/11", kDeliveryEnding))
            break;
        IOLog("SwitchJoyCon: Joy-Con (%c) did not answer the end of its session (91/11); handshaking it again\n",
              s->cfg->letter);
        self->detach(*s);
        break;
    case kRunning:
    case kSuspended:
        break;
    }
}

bool SwitchJoyConRail::deliveryPending(const Side &s, uint32_t *lsrOut)
{
    const uint32_t lsr = rd(s, LSR);
    if (lsrOut)
        *lsrOut = lsr;
    const bool ring = s.dmaOn && !__atomic_load_n(&s.dmaTail, __ATOMIC_ACQUIRE) &&
                      (tegra_apbdma_moved(s.dma, nullptr) / 4) % (s.dmaRing.size / 4) != s.dmaTaken;
    const bool rx = (((lsr & LSR_DR) != 0u) && ((s.ier & (IER_RDI | IER_RTOIE)) != 0u)) || ring;
    const bool tx = ((lsr & LSR_THRE) != 0u) && ((s.ier & IER_THRI) != 0u);
    return rx || tx;
}

// Non-blocking 50 ms re-check: returns true if a lost-interrupt candidate was deferred to s.timer.
bool SwitchJoyConRail::startDeliveryCheck(Side &s, const char *why, DeliveryAction action)
{
    if (!deliveryPending(s, nullptr))
        return false;
    s.deliveryAction = action;
    s.deliveryWhy = why;
    s.deliveryInterrupts = __atomic_load_n(&s.interrupts, __ATOMIC_RELAXED);
    s.timer->setTimeoutMS(kDeliveryCheckMS);
    return true;
}

void SwitchJoyConRail::completeDeliveryCheck(Side &s)
{
    const DeliveryAction action = s.deliveryAction;
    const char *why = s.deliveryWhy;
    s.deliveryAction = kDeliveryNone;
    uint32_t lsr = 0;
    if (__atomic_load_n(&s.interrupts, __ATOMIC_RELAXED) == s.deliveryInterrupts && deliveryPending(s, &lsr)) {
        char gic[160];
        switch_interrupt_describe(s.cfg->intid, gic, sizeof(gic));
        panic(
            "SwitchJoyCon: UART-%c interrupt lost (INTID %u, %s): LSR %#x IER %#x MCR %#x tx %u/%u dma %u ring %u/%u, "
            "%s",
            s.cfg->uart, s.cfg->intid, why, lsr, rd(s, IER), rd(s, MCR), s.txPosition, s.txLength, s.dmaOn,
            s.dmaOn ? tegra_apbdma_moved(s.dma, nullptr) / 4 : 0, s.dmaTaken, gic);
    }
    switch (action) {
    case kDeliveryHandshake:
        if (s.phase == kHandshake)
            finishFailAttempt(s, why);
        break;
    case kDeliveryPairing:
        if (s.phase == kPairing)
            finishPairing(s, false, "no reply");
        break;
    case kDeliveryCalibrating:
        if (s.phase == kCalibrating) {
            IOLog("SwitchJoyCon: Joy-Con (%c) did not answer its %s; default stick calibration for this session\n",
                  s.cfg->letter, calibrationStepName(s.step));
            run(s);
            s.imuOff = true;
        }
        break;
    case kDeliveryEnding:
        if (s.phase == kEnding) {
            IOLog("SwitchJoyCon: Joy-Con (%c) did not answer the end of its session (91/11); handshaking it again\n",
                  s.cfg->letter);
            detach(s);
        }
        break;
    case kDeliverySilence:
        if (s.phase == kRunning && s.silentMs >= kSilentMS)
            finishSilence(s);
        break;
    default:
        break;
    }
}

void SwitchJoyConRail::describe(const Side &s, char *buffer, size_t size) const
{
    const SideConfig *cfg = s.cfg;
    const auto hex = [](char (&text)[sizeof(Side::stepHeard) * 3 + 8], const uint8_t *bytes, uint32_t count,
                        unsigned room) {
        strlcpy(text, "none", sizeof(text));
        const unsigned shown = count < room ? count : room;
        size_t at = 0;
        for (unsigned i = 0; i < shown; ++i)
            at += static_cast<size_t>(snprintf(text + at, sizeof(text) - at, "%s%02x", i ? " " : "", bytes[i]));
        if (count > shown)
            strlcat(text, " ...", sizeof(text));
    };
    char heard[sizeof(s.stepHeard) * 3 + 8];
    char stepHeard[sizeof(s.stepHeard) * 3 + 8];
    hex(heard, s.heard, s.heardCount, sizeof(s.heard));
    hex(stepHeard, s.stepHeard, s.stepHeardCount, sizeof(s.stepHeard));
    unsigned in[4];
    unsigned cnf[4];
    uint32_t mux[4];
    const uint32_t muxReg[4] = {cfg->muxTx, cfg->muxRx, cfg->muxRts, cfg->muxCts};
    for (unsigned i = 0; i < 4; ++i) {
        in[i] = gpioBit(cfg->txPin + i, GPIO_IN);
        cnf[i] = gpioBit(cfg->txPin + i, GPIO_CNF);
        mux[i] = pinRead(muxReg[i]);
    }
    const auto driven = [this](unsigned pin) {
        return gpioBit(pin, GPIO_CNF) & gpioBit(pin, GPIO_OE) & gpioBit(pin, GPIO_OUT);
    };
    snprintf(
        buffer, size,
        "step %u rx %u [%s] irq %u err %u; 3M sent %u rx %u err %u; late %u; then rx %u [%s]; UART-%c LSR %#x IER %#x "
        "MCR %#x MSR %#x LCR %#x CSR %#x clk %#x on %u rst %u; pads in %u%u%u%u cnf %u%u%u%u mux %#x %#x %#x %#x "
        "det %u chg %u 5V %u; PMC dpd %u noio %u 3v3 %u clamp %u",
        s.furthest, s.heardCount, heard, __atomic_load_n(&s.interrupts, __ATOMIC_RELAXED) - s.interruptsAtAttach,
        __atomic_load_n(&s.lineErrors, __ATOMIC_RELAXED) - s.errorsAtAttach, s.fastSent, s.fastHeardCount,
        __atomic_load_n(&s.fastErrors, __ATOMIC_RELAXED) - s.fastErrorsAtAttach, s.lateCount, s.stepHeardCount,
        stepHeard, cfg->uart, rd(s, LSR), rd(s, IER), rd(s, MCR), rd(s, MSR), rd(s, LCR), rd(s, IRDA_CSR),
        carRead(cfg->clockSource), flag(carRead(cfg->enbState), cfg->clockBit),
        flag(carRead(cfg->rstState), cfg->clockBit), in[0], in[1], in[2], in[3], cnf[0], cnf[1], cnf[2], cnf[3], mux[0],
        mux[1], mux[2], mux[3], gpioBit(cfg->detectPin, GPIO_IN), driven(cfg->chargePin), driven(kPin5V),
        flag(pmcRead(PMC_IO_DPD_STATUS), kPadUartDpd), flag(pmcRead(PMC_NO_IOPOWER), kPadUartPower),
        flag(pmcRead(PMC_PWR_DET_VAL), kPadUartPower),
        switch_mmio_read32(apbMisc, PP_PINMUX_GLOBAL) & kClampTristatedInputs);
}

void SwitchJoyConRail::preparePads(const char *when)
{
    char fixed[96] = "";
    if (pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd) {
        (void)switch_pmc_io_pad_on(kPadUartDpdBit);
        strlcat(fixed,
                pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd ? " deep power-down stuck on," : " deep power-down off,",
                sizeof(fixed));
    }
    if (pmcRead(PMC_NO_IOPOWER) & kPadUartPower) {
        switch_pmc_rmw(PMC_NO_IOPOWER - 0x400, kPadUartPower, 0);
        (void)pmcRead(PMC_NO_IOPOWER);
        strlcat(fixed, " IO power on,", sizeof(fixed));
    }
    if (switch_mmio_read32(apbMisc, PP_PINMUX_GLOBAL) & kClampTristatedInputs) {
        switch_mmio_write32_flush(apbMisc, PP_PINMUX_GLOBAL,
                                  switch_mmio_read32(apbMisc, PP_PINMUX_GLOBAL) & ~kClampTristatedInputs);
        strlcat(fixed, " tristate input clamp off,", sizeof(fixed));
    }
    if (!fixed[0])
        return;
    fixed[strlen(fixed) - 1] = 0;
    IOLog("SwitchJoyCon: rail pads were not usable at %s, fixed:%s\n", when, fixed);
}

void SwitchJoyConRail::initHardware(Side &s)
{
    const SideConfig *cfg = s.cfg;
    pinWrite(cfg->muxTx, PIN_INPUT);
    pinWrite(cfg->muxRx, PIN_INPUT | PIN_TRISTATE);
    pinWrite(cfg->muxRts, cfg->rtsPull | PIN_INPUT);
    pinWrite(cfg->muxCts, PIN_INPUT | PIN_TRISTATE);
    pinWrite(cfg->muxDetect, (pinRead(cfg->muxDetect) & PIN_KEEP_FUNCTION) | PIN_INPUT | PIN_TRISTATE);
    pinWrite(cfg->muxCharge, cfg->chargeMux);
    (void)pinRead(cfg->muxCharge);
    for (unsigned pad = 1; pad < 4; ++pad)
        gpioMask(cfg->txPin + pad, GPIO_MSK_CNF, false);

    carWrite(cfg->rstSet, cfg->clockBit);
    carWrite(cfg->enbClr, cfg->clockBit);
    carWrite(cfg->clockSource, kUartDivEnable | kDiv1M);
    carWrite(cfg->enbSet, cfg->clockBit);
    IODelay(2);
    carWrite(cfg->rstClr, cfg->clockBit);
    wr(s, IER, 0);
    wr(s, IRDA_CSR, CSR_INVERT_TXD | CSR_INVERT_RTS);
}

void SwitchJoyConRail::setUp(Side &s, const SideConfig *cfg)
{
    s.owner = this;
    s.cfg = cfg;
    s.uart = uarts + cfg->uartOffset;
    s.loop = IOWorkLoop::workLoop();
    s.outProdLock = IOLockAlloc();
    if (!s.loop || !s.outProdLock)
        panic("SwitchJoyCon: no memory for UART-%c workloop or lock", cfg->uart);
    switch_joycon_spsc_rx_init(&s.rxRing);
    switch_joycon_cmd_ring_init(&s.cmdRing);
    switch_joycon_rumble_slot_init(&s.rumbleSlot);
    switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_IDLE);
    initHardware(s);

    IOService *uartNub = switch_interrupt_nub(cfg->intid, false);
    s.irq = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchJoyConRail::uartAction,
                                                                     &SwitchJoyConRail::uartFilter, uartNub, 0);
    if (!s.irq || s.loop->addEventSource(s.irq) != kIOReturnSuccess)
        panic("SwitchJoyCon: cannot attach the UART-%c interrupt (INTID %u)", cfg->uart, cfg->intid);

    IOService *detectNub = tegra_gpio_interrupt_nub(cfg->detectPin, TEGRA_GPIO_EDGE_FALLING);
    if (!detectNub) {
        char state[256];
        tegra_gpio_describe(cfg->detectPin, state, sizeof(state));
        panic("SwitchJoyCon: no interrupt nub for the Joy-Con (%c) detect pin: %s", cfg->letter, state);
    }
    s.detect = IOInterruptEventSource::interruptEventSource(this, &SwitchJoyConRail::detectAction, detectNub, 0);
    s.timer = IOTimerEventSource::timerEventSource(this, &SwitchJoyConRail::timerAction);
    s.poll = IOTimerEventSource::timerEventSource(this, &SwitchJoyConRail::pollAction);
    s.wake = IOInterruptEventSource::interruptEventSource(this, &SwitchJoyConRail::wakeAction);
    if (!s.detect || !s.timer || !s.poll || !s.wake || s.loop->addEventSource(s.detect) != kIOReturnSuccess ||
        s.loop->addEventSource(s.timer) != kIOReturnSuccess || s.loop->addEventSource(s.poll) != kIOReturnSuccess ||
        s.loop->addEventSource(s.wake) != kIOReturnSuccess) {
        panic("SwitchJoyCon: cannot attach Joy-Con (%c) event sources (GPIO %u)", cfg->letter, cfg->detectPin);
    }
    setIrqEnabled(s, true);

    char why[128] = "no DMA-able ring";
    if (tegra_apbdma_alloc(kDmaRingBytes, &s.dmaRing)) {
        s.dma = tegra_apbdma_open(cfg->uart == 'B' ? TEGRA_APBDMA_CH_UARTB_RX : TEGRA_APBDMA_CH_UARTC_RX, s.loop,
                                  nullptr, nullptr, why, sizeof(why));
    }
    if (!s.dma)
        IOLog("SwitchJoyCon: UART-%c RX DMA off (%s): RX by interrupt per FIFO trigger\n", cfg->uart, why);
}

// Work loop (sleepable SC7 suspend): sends HCI sleep (0x06 0x00) and waits non-spinning up to 1 ms for TEMT.
void SwitchJoyConRail::quiesce(Side &s)
{
    if (s.phase == kRunning) {
        const uint8_t sleep = 0;
        sendSubcommand(s, kSubHciState, &sleep, 1);
        for (unsigned waited = 0; waited < 10 && (!txIdle(s) || !(rd(s, LSR) & LSR_TEMT)); ++waited)
            delay_for_interval_with_leeway(100, 50, NSEC_PER_USEC);
    }
    s.poll->cancelTimeout();
    s.pollInterval = 0;
    s.pollNext = 0;
    s.timer->cancelTimeout();
    s.deliveryAction = kDeliveryNone;
    s.detect->disable();
    setIrqEnabled(s, false);
    stopDma(s);
    s.ier = 0;
    wr(s, IER, 0);
    s.txLength = s.txPosition = 0;
    switch_joycon_link_store(&s.linkState, SWITCH_JOYCON_LINK_IDLE);
    dropHid(s);
    setCharging(s, false);
    s.chargeKnown = false;
    s.phase = kSuspended;
    updateBusyHold(s);
}

void SwitchJoyConRail::wake(Side &s)
{
    initHardware(s);
    setIrqEnabled(s, true);
    if (!s.displayOff)
        enterDetect(s);
}

void SwitchJoyConRail::suspend()
{
    for (Side &s : sides) {
        s.loop->runAction(
            [](OSObject *, void *arg, void *, void *, void *) -> IOReturn {
                auto *side = static_cast<Side *>(arg);
                if (side->suspended)
                    return kIOReturnSuccess;
                SwitchJoyConRail::quiesce(*side);
                side->suspended = true;
                return kIOReturnSuccess;
            },
            this, &s);
    }
}

void SwitchJoyConRail::resume()
{
    preparePads("resume");
    for (Side &s : sides) {
        s.loop->runAction(
            [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
                auto *side = static_cast<Side *>(arg);
                if (!side->suspended)
                    return kIOReturnSuccess;
                side->suspended = false;
                static_cast<SwitchJoyConRail *>(owner)->wake(*side);
                return kIOReturnSuccess;
            },
            this, &s);
    }
}

void SwitchJoyConRail::displayPower(bool on)
{
    for (Side &s : sides) {
        s.loop->runAction(
            [](OSObject *owner, void *sideArg, void *onArg, void *, void *) -> IOReturn {
                auto *self = static_cast<SwitchJoyConRail *>(owner);
                auto *side = static_cast<Side *>(sideArg);
                const bool powerOn = onArg != nullptr;
                if (side->displayOff == !powerOn)
                    return kIOReturnSuccess;
                side->displayOff = !powerOn;
                if (side->suspended)
                    return kIOReturnSuccess;
                if (powerOn) {
                    self->enterDetect(*side);
                } else {
                    SwitchJoyConRail::pause(*side);
                }
                return kIOReturnSuccess;
            },
            this, &s, on ? this : nullptr);
    }
}

void SwitchJoyConRail::bluetoothAddress(const uint8_t address[6])
{
    const bool changed = !hostKnown || memcmp(host, address, 6) != 0;
    memcpy(host, address, 6);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&hostKnown, true, __ATOMIC_RELEASE);
    for (Side &s : sides) {
        s.loop->runAction(
            [](OSObject *owner, void *sideArg, void *changedArg, void *, void *) -> IOReturn {
                const auto *self = static_cast<const SwitchJoyConRail *>(owner);
                Side &s = *static_cast<Side *>(sideArg);
                const bool addrChanged = changedArg != nullptr;
                s.pairPending |= self->pairing && addrChanged;
                return kIOReturnSuccess;
            },
            this, &s, changed ? this : nullptr);
    }
}

bool SwitchJoyConRail::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    car = switch_mmio_map(kCarPA, 0x1000);
    gpio = switch_mmio_map(kGpioPA, 0x1000);
    apbMisc = switch_mmio_map(kApbMiscPA, 0x1000);
    pinmux = switch_mmio_map(kPinmuxPA, 0x1000);
    uarts = switch_mmio_map(kUartPA, 0x1000);
    pmc = switch_mmio_map(kPmcPA, 0x1000);
    if (!car || !gpio || !apbMisc || !pinmux || !uarts || !pmc)
        panic("SwitchJoyCon: cannot map CAR/GPIO/APB_MISC/pinmux/UART/PMC");

    pairing = !bluetoothDisabled();
    preparePads("start");
    for (unsigned i = 0; i < 2; ++i)
        setUp(sides[i], &kSides[i]);
    switch_joycon_hid_buttons_init(sides[0].loop);
    for (Side &s : sides) {
        s.loop->runAction(
            [](OSObject *owner, void *side, void *, void *, void *) -> IOReturn {
                static_cast<SwitchJoyConRail *>(owner)->enterDetect(*static_cast<Side *>(side));
                return kIOReturnSuccess;
            },
            this, &s);
    }
    IOLog("SwitchJoyCon: rail driver started%s\n", pairing ? " (Bluetooth pairing on attach)" : "");
    return true;
}

void SwitchJoyConRail::statsLine(char *buffer, size_t size) const
{
    size_t used = 0;
    buffer[0] = 0;
    const uint32_t pub = __atomic_load_n(&gamepadsPublished, __ATOMIC_RELAXED);
    const uint32_t idl = __atomic_load_n(&gamepadsIdled, __ATOMIC_RELAXED);
    for (const Side &s : sides) {
        const int n = snprintf(
            buffer + used, size - used,
            "%s%c running=%d dma=%s replies=%u late=%u tails=%u errors=%u breaks=%u overflows=%u "
            "dropped=%u silences=%u recoveries=%u republished=%u gamepad=%d published=%u idled=%u "
            "requests=%u interrupts=%u unsolicited=%u forwarded=%u suppressed=%u quiet=%d "
            "deadzone=%u calibrated=%d",
            used ? "; " : "", s.cfg->letter, __atomic_load_n(&s.phase, __ATOMIC_RELAXED) == kRunning ? 1 : 0,
            __atomic_load_n(&s.dmaOn, __ATOMIC_RELAXED) ? "on" : "off", __atomic_load_n(&s.replies, __ATOMIC_RELAXED),
            __atomic_load_n(&s.dmaLate, __ATOMIC_RELAXED), __atomic_load_n(&s.dmaTails, __ATOMIC_RELAXED),
            __atomic_load_n(&s.lineErrors, __ATOMIC_RELAXED), __atomic_load_n(&s.breaks, __ATOMIC_RELAXED),
            __atomic_load_n(&s.rxRing.overflows, __ATOMIC_RELAXED), __atomic_load_n(&s.dropped, __ATOMIC_RELAXED),
            __atomic_load_n(&s.silences, __ATOMIC_RELAXED), __atomic_load_n(&s.recoveries, __ATOMIC_RELAXED),
            __atomic_load_n(&s.republished, __ATOMIC_RELAXED),
            __atomic_load_n(&s.hid, __ATOMIC_RELAXED) != nullptr ? 1 : 0, pub, idl,
            __atomic_load_n(&s.requests, __ATOMIC_RELAXED), __atomic_load_n(&s.interrupts, __ATOMIC_RELAXED),
            __atomic_load_n(&s.unsolicited, __ATOMIC_RELAXED), __atomic_load_n(&s.forwarded, __ATOMIC_RELAXED),
            __atomic_load_n(&s.suppressed, __ATOMIC_RELAXED),
            __atomic_load_n(&s.cadence.quiet, __ATOMIC_RELAXED) ? 1 : 0,
            __atomic_load_n(&s.stick.dead_zone, __ATOMIC_RELAXED),
            __atomic_load_n(&s.stickKnown, __ATOMIC_RELAXED) ? 1 : 0);
        if (n < 0 || static_cast<size_t>(n) >= size - used)
            break;
        used += static_cast<size_t>(n);
    }
}

#if DEVELOPMENT || DEBUG
int SwitchJoyConRail::probeStart()
{
    bool busy = false;
    for (Side &s : sides) {
        s.loop->runAction(
            [](OSObject *, void *sideArg, void *busyArg, void *, void *) -> IOReturn {
                if (static_cast<Side *>(sideArg)->probe.active)
                    *static_cast<bool *>(busyArg) = true;
                return kIOReturnSuccess;
            },
            this, &s, &busy);
    }
    if (busy)
        return EBUSY;
    int result = ENODEV;
    for (Side &s : sides) {
        s.loop->runAction(
            [](OSObject *, void *sideArg, void *outArg, void *, void *) -> IOReturn {
                auto *side = static_cast<Side *>(sideArg);
                if (side->phase == kRunning) {
                    switch_joycon_probe_start(&side->probe);
                    armPoll(*side, true);
                    *static_cast<int *>(outArg) = 0;
                }
                return kIOReturnSuccess;
            },
            this, &s, &result);
    }
    return result;
}

void SwitchJoyConRail::probeState(char *buffer, size_t size)
{
    switch_joycon_probe copies[2] = {};
    for (unsigned i = 0; i < 2; ++i) {
        sides[i].loop->runAction(
            [](OSObject *, void *sideArg, void *dstArg, void *, void *) -> IOReturn {
                *static_cast<switch_joycon_probe *>(dstArg) = static_cast<Side *>(sideArg)->probe;
                return kIOReturnSuccess;
            },
            this, &sides[i], &copies[i]);
    }
    const switch_joycon_probe *probes[2] = {&copies[0], &copies[1]};
    const char letters[2] = {sides[0].cfg->letter, sides[1].cfg->letter};
    switch_joycon_probe_state(probes, letters, 2, buffer, size);
}
#endif

static SwitchJoyConRail *gRail;

#if DEVELOPMENT || DEBUG
static int statsSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    char line[1024] = "";
    if (const SwitchJoyConRail *rail = __atomic_load_n(&gRail, __ATOMIC_ACQUIRE))
        rail->statsLine(line, sizeof(line));
    return SYSCTL_OUT(req, line, strlen(line) + 1);
}

static int probeSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    constexpr size_t kSize = 8192;
    SwitchJoyConRail *rail = __atomic_load_n(&gRail, __ATOMIC_ACQUIRE);
    if (!rail)
        return ENODEV;
    if (req->newptr) {
        if (const int error = rail->probeStart())
            return error;
    }
    auto *line = static_cast<char *>(IOMallocData(kSize));
    if (!line)
        return ENOMEM;
    rail->probeState(line, kSize);
    const int error = SYSCTL_OUT(req, line, strlen(line) + 1);
    IOFreeData(line, kSize);
    return error;
}

static void registerStats()
{
    static struct sysctl_oid probe;
    if (!probe.oid_name) {
        switch_sysctl_oid(&probe, &sysctl__debug_children, "switch_joycon_probe",
                          static_cast<int>(CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED), probeSysctl,
                          "A", "rail Joy-Con protocol probe: write to start, read for its state and results");
        sysctl_register_oid(&probe);
    }
    static struct sysctl_oid oid;
    if (oid.oid_name)
        return;
    switch_sysctl_oid(
        &oid, &sysctl__kern_children, "switch_joycon", static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED),
        statsSysctl, "A",
        "each rail: running, RX DMA, and since boot its replies, late and tail takes, "
        "line errors, breaks, overflows, dropped packets, silences, recoveries, republished devices, "
        "requests, UART interrupts, unsolicited packets, reports forwarded and held back; its cadence and "
        "stick dead zone");
    sysctl_register_oid(&oid);
}
#endif

extern "C" void switch_joycon_note_touch_ms(uint64_t now_ms)
{
    __atomic_store_n(&gLastTouchMs, now_ms, __ATOMIC_RELAXED);
}

extern "C" int switch_joycon_start(void)
{
    static bool started;
    if (started)
        return 0;
    started = true;
    switch_joycon_hid_select_personalities();
    IOService *resources = IOService::getResourceService();
    switch_joycon_hid_prepare();
    auto *driver = OSTypeAlloc(SwitchJoyConRail);
    if (!resources || !driver || !driver->init() || !driver->attach(resources) || !driver->start(resources))
        panic("SwitchJoyCon: rail driver attach or start failed");
    __atomic_store_n(&gRail, driver, __ATOMIC_RELEASE);
    driver->release();
#if DEVELOPMENT || DEBUG
    registerStats();
#endif
    return 0;
}

extern "C" void switch_joycon_suspend(void)
{
    if (gRail)
        gRail->suspend();
}

extern "C" void switch_joycon_resume(void)
{
    if (gRail)
        gRail->resume();
}

extern "C" void switch_joycon_display_power(bool on)
{
    if (gRail)
        gRail->displayPower(on);
}

extern "C" void switch_joycon_bluetooth_address(const uint8_t address[6])
{
    if (gRail)
        gRail->bluetoothAddress(address);
}
