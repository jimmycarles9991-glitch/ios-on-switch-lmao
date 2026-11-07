// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED touch screen: STMicro FingerTip S (FTM4CD60/FTM4CD50, chip id 0x3670) on I2C3 0x49, reported
// to iPadOS as a ten-finger touch screen through SwitchHIDDevice (SwitchHID.h).
//
// Hardware (switchroot tegra210b01-nx-common.dtsi fts@49; hekate bdk/input/touch.c; switchroot Linux ftm4.c):
//   * Power: hekate's hw_deinit runs touch_power_off (reset/VIO low, MAX77620 LDO6 off, I2C3 clock off), so the
//     panel is brought up from scratch as touch_power_on does: PJ7 (stm,vio-gpio) low, LDO6 2.9 V on (PMIC 0x3C on
//     I2C5), I2C3 up, PJ7 high, 10 ms.
//   * Init (touch_init, Linux fts_init): chip id at hardware register 0x0004, system reset (register 0x0028 = 0x80,
//     then the controller-ready event 0x10), finger sense mode (C3 01), sense on (93), clear the event stack (A1),
//     and the interrupt enable Linux sets and hekate (which polls) does not: register 0x002C = 0x48.
//   * Interrupt: INT on GPIO PX1, level low while the event FIFO holds events (DT IRQ_TYPE_LEVEL_LOW, IRQF_ONESHOT).
//     The action reads the event count (register 0x0023: byte 1 >> 1) and then the events (86, 8 bytes each) once, as
//     Linux fts_interrupt_handler does: two I2C transactions per interrupt (r17-touch; a third that re-read the count
//     to see the FIFO empty is gone). Events that arrive meanwhile keep INT low, so the pin fires again as soon as the
//     action returns and TegraGPIO unmasks it (enableVector clears the latch first, a still-low line latches again).
//   * Timing (r17-touch): the interrupt's filter (primary interrupt context) stamps the time INT reached the CPU and
//     raises the CPU governor; every HID report of that interrupt carries that time (switch_hid_post_at, strictly
//     increasing), the scan's own time as near as the CPU can know it, not the time the I2C reads ended. iOS's touch
//     resampling and velocity work from these stamps. Every 10 s with touches, one os_log line (never the console):
//     interrupts, reports, events, the FTM4's report interval while a contact is down, and filter->action, the count
//     read, the event read, the HID post and filter->post, mean/p99/max, with the bus's I2C interrupts per report.
//   * Contacts (r17-touch, after r8a: keyboard taps read as flicks down, the app switcher hard to reach, 4-finger
//     swipes slow to start): a palm (strength 500 or more, as Linux ftm4 and hekate) is not a contact; a contact's
//     reported point is SwitchTouchFilter.h's (r29-touch: what MultitouchHID does for Apple's screens); the edge band is
//     40 px. A second os_log line beside the stats one measures the touch-down drift, taps' raw and reported travel,
//     where edge swipes start, how far apart a 4-finger touch's contacts arrive, contact strength, palms and re-touches
//     (a contact back within 40 ms where it left).
//   * Events: byte 0 low nibble = type (3 enter, 4 leave, 5 motion), high nibble = contact 0..9; X = b1 << 4 | b3 >> 4,
//     Y = b2 << 4 | b3 & 0xf, already in the landscape frame hekate's GUI uses (stm,max_coords 1280 x 720); scaled
//     from the panel's active area (stm,max-real-coords 1264 x 704, stm,edge-offset 15) as fts_coordinates_factor.
// That landscape frame is the one the display shows iOS (NativeScanout.h and the rotate-3 console both put landscape
// pixel (x, y) at portrait row 1279 - x, column y, upright in handheld use, as hekate's GUI). Portrait-native
// (SwitchPortrait.h): iOS's screen is the portrait panel, so contacts are reported at that row and column (position).
//
// Failures panic with a one-line reason; there is no polled mode. A 10 s watchdog only checks that a latched INT
// reached the CPU (tegra_gpio_check_delivery).
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "../SwitchPower/SwitchPower.h"
#include "SwitchHID.h"
#include "SwitchTouch.h"
#include "SwitchTouchFilter.h"
#if DEVELOPMENT || DEBUG
#include "SwitchTouchTrace.h"
#endif

// SwitchBrightness (SwitchBacklight.cpp): false while the panel is dark (display power off). Weak, so
// the touch screen links and reports as before without it.
extern "C" __attribute__((weak)) bool switch_display_is_on(void)
{
    return true;
}

// SwitchBrightness (SwitchALS.cpp): LDO6 also feeds the ambient light sensor's I2C side, which waits for it. Weak, as
// above.
extern "C" __attribute__((weak)) void switch_als_supply(bool)
{}

// For NativeLegacyDisplay's touch->swap / touch->scan (below, after the driver).
static void switch_touch_note_report(uint64_t stamp);

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
// XNU (kernel/switch/diag/frames.c): each report, for the display's touch interaction windows and their latency;
// `changes` has kFramesBegan if a contact went down in it, kFramesEnded if one lifted (diag/latency.h's bits).
extern "C" void switch_frames_touch(uint64_t stamp, bool down, unsigned changes);
constexpr unsigned kFramesBegan = 1, kFramesEnded = 2;
#endif

namespace {
constexpr unsigned kBus = 3, kPmicBus = 5;
constexpr uint8_t kTouch = 0x49, kPmic = 0x3c;
// MAX77620 LDO6_CFG: output voltage 5:0 in 50 mV steps from 800 mV, power mode 7:6 (3 = normal); hekate
// max7762x_regulator_set_voltage / _enable (LDO6 = touch, 2.9 V).
constexpr uint8_t kLdo6Cfg = 0x2f, kLdoVoltMask = 0x3f, kLdoModeNormal = 0xc0, kLdo6Volt = (2900 - 800) / 50;

enum : uint8_t {  // FTM4 commands (hekate touch.h FTS4_CMD_*, Linux ftm4.h)
    CmdReadOneEvent = 0x85,
    CmdReadAllEvents = 0x86,
    CmdSenseOn = 0x93,
    CmdClearEvents = 0xa1,
    CmdHwReg = 0xb6,
    CmdSenseMode = 0xc3,
};

enum : uint8_t {
    EvEnter = 0x03,
    EvLeave = 0x04,
    EvMotion = 0x05,
    EvReady = 0x10
};

enum : uint16_t {
    RegChipId = 0x0004,
    RegEventCount = 0x0023,
    RegReset = 0x0028,
    RegInterrupt = 0x002c
};

constexpr uint8_t kFingerMode = 0x01, kResetValue = 0x80, kInterruptEnable = 0x48;
constexpr uint16_t kChipId = 0x3670;
constexpr unsigned kEventSize = 8, kFifoDepth = 32, kSlots = kSwitchTouchSlots;
constexpr unsigned kRealMaxX = 1264, kRealMaxY = 704, kEdgeOffset = 15;
// px: ~4.8 mm on the 155 x 87 mm panel (8.26 px/mm). A swipe in from the bezel is first seen several mm in (the FTM4
// needs the pad on the glass, and the finger travels a few mm per scan), so a 3 mm band missed most of them (r8a: 2 of
// 9 touches from the edge; the app switcher hard to reach). The geometry line measures where edge swipes start.
constexpr unsigned kEdgeBand = 40;
constexpr uint32_t kWatchdogMS = 10000;  // with as much leeway (switch_check_after_ms): no wakeup of its own
constexpr unsigned kEmptyLimit = 64;     // interrupts in a row that found the FIFO empty: INT is stuck low

// The touch-down drift is only measured: the largest raw distance from the first point over the first kDriftMs.
// Palm: contact strength z (Linux ftm4 / hekate: bytes 4-5 << 6 over the gain in byte 6) at kPalmStrength or more is a
// palm, as both reject it (L4T then lifts every contact; here only that one, so a gesture's fingers stay down).
constexpr uint64_t kDriftMs = 30;
constexpr unsigned kPalmStrength = 500;
// Geometry stats (the second stats line): taps are contacts down under kTapMs with under kTapTravel of raw travel (their
// raw and reported travel, and how many were reported moving at all); an edge swipe starts
// within kEdgeNear of an edge and goes kEdgeSwipe away from it within kEdgeSwipeMs; a re-touch is a contact back within
// kRetouchMs and kRetouchPx of where the same contact left (a dropout, or a double tap faster than a hand makes).
constexpr uint64_t kTapMs = 300, kEdgeSwipeMs = 200, kRetouchMs = 40;
#if DEVELOPMENT || DEBUG
constexpr uint64_t kTraceMs = 3000;  // contacts traced (SwitchTouchTrace.h): taps, flicks, swipes, a paused swipe
#endif
constexpr int kEdgeNear = 120, kEdgeSwipe = 100, kRetouchPx = 64, kTapTravel = 64;

int distance(int dx, int dy)
{
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    return ax > ay ? ax + ay / 2 : ay + ax / 2;  // within 12 % of the Euclidean length, no square root
}

// Linux ftm4 fts_event_handler_type_b / hekate _touch_process_contact_event: the contact's strength.
unsigned strength(const uint8_t *event)
{
    unsigned gain = event[6] & 0x3f;
    if (gain == 1 || gain == 0x3f)
        gain = 0x40;
    return (((unsigned)event[4] | (unsigned)event[5] << 8) << 6) / (gain + 0x40);
}

// The FTM4's report interval is measured between interrupts that both carried contact events with a contact down in
// between; a gap this long is a finger resting still (the FTM4 reports only changes), not a report period.
constexpr uint64_t kIntervalLimitUs = 50000;

uint64_t ticksToUs(uint64_t ticks)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / 1000;
}

// Microsecond spans for the stats line: count, sum, min, max and a log histogram for the p99 (exact below 16 us, then
// 8 steps per octave, so a quantile is within 12.5 %; the top bucket holds everything from 2^24 us).
struct Spread {
    static constexpr unsigned kBuckets = 16 + 8 * 20;
    uint32_t n = 0, hist[kBuckets] = {};
    uint64_t sum = 0, max = 0, min = UINT64_MAX;

    static unsigned bucket(uint64_t us)
    {
        if (us < 16)
            return (unsigned)us;
        const unsigned msb = 63u - (unsigned)__builtin_clzll(us);
        const unsigned b = 16 + (msb - 4) * 8 + (unsigned)((us >> (msb - 3)) & 7);
        return b < kBuckets ? b : kBuckets - 1;
    }

    static uint64_t lower(unsigned b)
    {
        if (b < 16)
            return b;
        return (8ull + (b - 16) % 8) << ((b - 16) / 8 + 1);
    }

    void add(uint64_t us)
    {
        ++n;
        sum += us;
        if (us > max)
            max = us;
        if (us < min)
            min = us;
        ++hist[bucket(us)];
    }

    uint64_t mean() const
    {
        return n ? sum / n : 0;
    }

    void reset()
    {
        n = 0;
        sum = max = 0;
        min = UINT64_MAX;
        for (uint32_t &h : hist)
            h = 0;
    }

    // The smallest bucket bound at or above `permille` of the values (never above the maximum).
    uint64_t quantile(unsigned permille) const
    {
        if (!n)
            return 0;
        const uint64_t want = ((uint64_t)n * permille + 999) / 1000;
        uint64_t seen = 0;
        for (unsigned b = 0; b < kBuckets; ++b) {
            seen += hist[b];
            if (seen >= want) {
                const uint64_t top = b + 1 < kBuckets ? lower(b + 1) - 1 : max;
                return top < max ? top : max;
            }
        }
        return max;
    }
};

int command(const uint8_t *bytes, size_t length)
{
    return tegra_i2c_transfer(kBus, kTouch, bytes, length, nullptr, 0);
}

int readRegister(uint16_t reg, uint8_t *buffer, size_t length)
{
    const uint8_t tx[3] = {CmdHwReg, (uint8_t)(reg >> 8), (uint8_t)reg};
    return tegra_i2c_transfer(kBus, kTouch, tx, sizeof(tx), buffer, length);
}

int writeRegister(uint16_t reg, uint8_t value)
{
    const uint8_t tx[4] = {CmdHwReg, (uint8_t)(reg >> 8), (uint8_t)reg, value};
    return command(tx, sizeof(tx));
}

int eventCount(unsigned *count)
{
    uint8_t b[2] = {};
    const int err = readRegister(RegEventCount, b, sizeof(b));
    *count = err ? 0 : b[1] >> 1;
    return err;
}

// Polls the event stack for one event type during bring-up only (hekate _touch_wait_event): INT is not enabled yet.
bool waitEvent(uint8_t type, unsigned timeoutMS)
{
    for (unsigned waited = 0; waited <= timeoutMS; ++waited) {
        unsigned count = 0;
        if (!eventCount(&count) && count) {
            const uint8_t cmd = CmdReadOneEvent;
            uint8_t event[kEventSize] = {};
            if (!tegra_i2c_transfer(kBus, kTouch, &cmd, 1, event, sizeof(event)) && event[0] == type)
                return true;
        }
        IOSleep(1);
    }
    return false;
}

uint16_t scale(unsigned raw, unsigned realMax, unsigned size)
{
    if (raw < kEdgeOffset)
        raw = kEdgeOffset;
    if (raw > realMax)
        raw = realMax;
    return (uint16_t)((raw - kEdgeOffset) * (size - 1) / (realMax - kEdgeOffset));
}
}  // namespace

class SwitchTouch : public IOService {
    OSDeclareDefaultStructors(SwitchTouch)

        struct Contact {
        bool down;
        uint16_t x, y;  // as reported
        bool palm;      // rejected until it leaves
        switch_touch_filter::Contact filter;
        int16_t startX, startY;  // raw first point
        uint64_t downAt;
        // Geometry stats: the largest raw distance from the first point (in the first kDriftMs, and over the contact),
        // whether it was reported moving, its edge swipe (the nearest edge and the first point's distance from it).
        uint16_t drift, rawTravel, reportedTravel;
        bool driftSeen, moved, edgeCandidate, edgeSwipeSeen;
        uint8_t nearEdge;
        uint16_t edgeDistance;
        // Where and when it last left (re-touch stat).
        uint64_t leftAt;
        int16_t leftX, leftY;
#if DEVELOPMENT || DEBUG
        SwitchTouchTrace trace;
#endif
    };

    // One stats window (file comment): counts, and the spans in microseconds.
    struct Stats {
        uint64_t start = 0;  // mach time the window began
        uint32_t irqs = 0, empty = 0, reports = 0, events = 0, maxEvents = 0;
        tegra_i2c_counters i2c = {};  // the bus's counters at the window's start
        Spread wake, countRead, eventRead, post, total, interval;
        // Geometry (pixels, ms): touch-down drift, taps' raw travel, edge swipes' first point, 4-finger arrival spread,
        // contact strength; taps reported moving, re-touches, palms.
        Spread settle, tapRaw, tapReported, edgeStart, arrival, strength;
        uint32_t tapsMoved = 0, retouches = 0, palms = 0;

        void reset()
        {
            start = 0;
            irqs = empty = reports = events = maxEvents = 0;
            tapsMoved = retouches = palms = 0;
            i2c = {};
            Spread *const spreads[] = {&wake,   &countRead, &eventRead,   &post,      &total,   &interval,
                                       &settle, &tapRaw,    &tapReported, &edgeStart, &arrival, &strength};
            for (Spread *spread : spreads)
                spread->reset();
        }
    };

    IOWorkLoop *loop = nullptr;
    IOFilterInterruptEventSource *irq = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOService *hid = nullptr;
    Contact contacts[kSlots] = {};
    uint32_t edge = 0;     // contacts that began within kEdgeBand of a panel edge
    uint32_t changed = 0;  // contacts that went down or up since the last report
    bool dirty = false, running = false, announced = false;
    bool screenOff = false;     // display power off (switch_touch_display_power): stays down until the screen lights
    bool timed = false;         // reports carry the interrupt's time (switch_hid_supports_timestamps)
    bool pathsChecked = false;  // I2C3's chained and DMA paths checked (once a boot, checkPaths)
    unsigned empty = 0;
    volatile uint64_t irqStamp = 0;  // the filter's: when INT last reached the CPU (mach time)
    uint64_t reportStamp = 0;        // the interrupt's time, for the reports flush() posts now
    uint64_t lastStamp = 0;          // the last report's time: each report's is later
    uint64_t lastContactIrq = 0;     // the last interrupt with contact events after which a contact stayed down
    uint64_t postTicks = 0;          // this interrupt's HID posts so far
    Stats stats;

    static bool filterInterrupt(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    void logStats();
    bool anyDown() const;
    void powerUp();
    void bringUp();
    bool checkPath(bool dma, const uint8_t *id, size_t size, char *why, size_t whySize);
    void checkPaths(const uint8_t *id, size_t size);
    void enableSensing();
    void powerDown();
    void handle(const uint8_t *event);
    void position(const uint8_t *event, int &x, int &y) const;
    void begin(Contact &contact, int x, int y, unsigned z, uint64_t now);
    void track(Contact &contact, int x, int y, unsigned z, uint64_t now);
    void noteDrift(Contact &contact);
    void ended(Contact &contact, uint64_t now);
    void report(Contact &contact, int x, int y);
    void flush();
    void releaseAll();

public:
    bool start(IOService *provider) override;
    void free() override;
    void suspend();
    void resume();
    void displayPower(bool on);
};

OSDefineMetaClassAndStructors(SwitchTouch, IOService)

    bool SwitchTouch::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    hid = switch_hid_create(kSwitchHIDTouch);
    timed = switch_hid_supports_timestamps(hid);
    if (!timed)
        IOLog("SwitchTouch: HID reports cannot carry the interrupt's time here (SwitchHID); stamped at the post "
              "instead\n");
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_TOUCH_INT, TEGRA_GPIO_LEVEL_LOW);
    if (!nub) {
        char state[256];
        tegra_gpio_describe(TEGRA_GPIO_TOUCH_INT, state, sizeof(state));
        panic("SwitchTouch: no interrupt nub for the touch INT line (PX1): %s", state);
    }
    loop = IOWorkLoop::workLoop();
    irq = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchTouch::interruptOccurred,
                                                                   &SwitchTouch::filterInterrupt, nub, 0);
    timer = IOTimerEventSource::timerEventSource(this, &SwitchTouch::timerFired);
    if (!loop || !irq || !timer || loop->addEventSource(irq) != kIOReturnSuccess ||
        loop->addEventSource(timer) != kIOReturnSuccess) {
        panic("SwitchTouch: work loop setup failed");
    }
    timer->setTimeoutMS(1);  // power the panel up on the work loop, off the caller's thread
    return true;
}

void SwitchTouch::free()
{
    if (loop) {
        if (irq)
            loop->removeEventSource(irq);
        if (timer)
            loop->removeEventSource(timer);
    }
    OSSafeReleaseNULL(irq);
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(loop);
    IOService::free();
}

// hekate touch_power_on.
void SwitchTouch::powerUp()
{
    if (tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 0))
        panic("SwitchTouch: cannot drive the touch reset line (PJ7)");
    IODelay(20);

    uint8_t ldo = 0;
    int err = tegra_i2c_init(kPmicBus);
    if (!err)
        err = tegra_i2c_read(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    if (!err) {
        ldo = (uint8_t)((ldo & ~kLdoVoltMask) | kLdo6Volt);
        err = tegra_i2c_write(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    }
    IOSleep(1);
    if (!err) {
        ldo |= kLdoModeNormal;
        err = tegra_i2c_write(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    }
    if (err)
        panic("SwitchTouch: MAX77620 LDO6 (touch 2.9 V, I2C5 0x3c reg 0x2f) could not be enabled: I2C error %d", err);
    IOSleep(1);

    err = tegra_i2c_init(kBus);
    if (err)
        panic("SwitchTouch: I2C3 init failed: %d", err);
    IOSleep(1);
    tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 1);
    IOSleep(10);
    switch_als_supply(true);  // LDO6 on for 12 ms or more: L4T's bh1730 waits 10 ms after its supplies
    waitEvent(EvReady, 20);   // as hekate: the reset below waits for it again
}

// hekate touch_init (chip id, system reset, sensing), three attempts.
void SwitchTouch::bringUp()
{
    int err = 0;
    uint8_t id[7] = {};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        err = readRegister(RegChipId, id, sizeof(id));
        if (err || (id[1] << 8 | id[2]) != kChipId)
            continue;
        checkPaths(id, sizeof(id));
        err = writeRegister(RegReset, kResetValue);
        IOSleep(10);
        if (err || !waitEvent(EvReady, 20))
            continue;
        enableSensing();
        if (!announced)
            IOLog("SwitchTouch: started (FTM4 firmware %02x%02x, config %02x/%02x)\n", id[3], id[4], id[5], id[6]);
        announced = true;
        return;
    }
    panic("SwitchTouch: FTM4 on I2C3 0x49 did not come up: I2C error %d, chip id %02x%02x (expected 3670), "
          "no controller-ready event after reset otherwise",
          err, id[1], id[2]);
}

// I2C3's never-booted paths (TegraI2C.h), once a boot, before any event read can use them: the chip ID block just read
// one packet per message, read again chained (one submission, one interrupt), then again with its read by DMA, must
// come back the same each time (bytes 1-6: the ID, firmware and config; byte 0 is the FTM4's dummy byte, which Linux
// ftm4 and hekate skip too). A failed check turns its path (and, for the chain, DMA with it) off for the boot with one
// line, and the bus keeps the path proven before r17-touch; nothing here panics.
bool SwitchTouch::checkPath(bool dma, const uint8_t *id, size_t size, char *why, size_t whySize)
{
    const uint8_t tx[3] = {CmdHwReg, (uint8_t)(RegChipId >> 8), (uint8_t)RegChipId};
    uint8_t again[8] = {};
    if (size > sizeof(again)) {
        snprintf(why, whySize, "chip ID block too long");
        return false;
    }
    const int err = dma ? tegra_i2c_transfer_dma(kBus, kTouch, tx, sizeof(tx), again, size, why, whySize)
                        : tegra_i2c_transfer_chained(kBus, kTouch, tx, sizeof(tx), again, size, why, whySize);
    if (err)
        return false;
    if (memcmp(again + 1, id + 1, size - 1) != 0) {
        snprintf(
            why, whySize,
            "chip ID block read back %02x%02x %02x%02x %02x%02x, one packet per message %02x%02x %02x%02x %02x%02x",
            again[1], again[2], again[3], again[4], again[5], again[6], id[1], id[2], id[3], id[4], id[5], id[6]);
        return false;
    }
    return true;
}

void SwitchTouch::checkPaths(const uint8_t *id, size_t size)
{
    if (pathsChecked)
        return;
    pathsChecked = true;
    char why[224] = "";
    const uint64_t began = mach_absolute_time();
    if (!checkPath(false, id, size, why, sizeof(why))) {
        IOLog("SwitchTouch: I2C3 chained reads off for this boot: %s; one packet per message, no DMA\n", why);
        return;
    }
    tegra_i2c_set_chained(kBus, true);
    const uint64_t chained = mach_absolute_time();
    if (!checkPath(true, id, size, why, sizeof(why))) {
        IOLog("SwitchTouch: I2C3 DMA off for this boot: %s; event reads by PIO\n", why);
        return;
    }
    tegra_i2c_set_dma(kBus, true);
    os_log(
        OS_LOG_DEFAULT, "SwitchTouch: I2C3 chained reads and DMA on: the chip ID block reads the same (%llu, %llu us)",
        (unsigned long long)ticksToUs(chained - began), (unsigned long long)ticksToUs(mach_absolute_time() - chained));
}

// Finger sense mode, sensing on, empty event stack, interrupt on (Linux fts_init; hekate touch_sense_enable).
void SwitchTouch::enableSensing()
{
    const uint8_t mode[2] = {CmdSenseMode, kFingerMode}, on = CmdSenseOn, clear = CmdClearEvents;
    int err = command(mode, sizeof(mode));
    IOSleep(10);
    if (!err)
        err = command(&on, 1);
    if (!err)
        err = command(&clear, 1);
    if (!err)
        err = writeRegister(RegInterrupt, kInterruptEnable);
    if (err)
        panic("SwitchTouch: FTM4 sense/interrupt enable failed: I2C error %d", err);
}

void SwitchTouch::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchTouch, owner);
    if (!self)
        return;
    if (!self->running) {
        self->powerUp();
        self->bringUp();
        self->running = true;
        self->irq->enable();
    } else {
        self->logStats();
        tegra_gpio_check_delivery(TEGRA_GPIO_TOUCH_INT, "SwitchTouch");
    }
    switch_check_after_ms(sender, kWatchdogMS);
}

// Primary interrupt context, PX1's vector (TegraGPIO::serviceBank): when INT reached the CPU, and the CPU governor to its
// top point now, so the work loop and the frame that answers this touch already run at full clock (SwitchPower.h: any
// context, a few stores). True: IOFilterInterruptEventSource masks the level pin and wakes the work loop, as the plain
// source did.
bool SwitchTouch::filterInterrupt(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchTouch *>(owner);
    self->irqStamp = mach_absolute_time();
    if (switch_display_is_on())
        switch_perf_hint(SWITCH_PERF_HINT_TOUCH);
    return true;
}

// Work loop, PX1 masked until return: the event count, then that many events (Linux fts_interrupt_handler), so the
// controller releases INT unless more came meanwhile (then the pin fires again once this returns). While the panel is
// dark the events are read and dropped (a finger on a dark screen must not reach SpringBoard), and contacts that were
// down when it went dark are lifted once.
void SwitchTouch::interruptOccurred(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchTouch, owner);
    if (!self)
        return;
    const uint64_t irqAt = self->irqStamp, began = mach_absolute_time();
    Stats &st = self->stats;
    if (!st.start) {  // a window begins with its first interrupt, before that interrupt's transfers
        st.start = began;
        tegra_i2c_get_counters(kBus, &st.i2c);
    }
    const bool shown = switch_display_is_on();
    unsigned count = 0;
    int err = eventCount(&count);
    const uint64_t counted = mach_absolute_time();
    uint8_t events[kFifoDepth * kEventSize];
    if (!err && count) {
        if (count > kFifoDepth)
            count = kFifoDepth;  // the rest keep INT low: the next interrupt takes them
        const uint8_t cmd = CmdReadAllEvents;
        err = tegra_i2c_transfer(kBus, kTouch, &cmd, 1, events, count * kEventSize);
    }
    if (err)
        panic("SwitchTouch: reading the FTM4 event FIFO failed: I2C error %d", err);
    const uint64_t read = mach_absolute_time();
    self->reportStamp = irqAt && irqAt <= began ? irqAt : began;
    self->postTicks = 0;
    const uint32_t reportsBefore = st.reports;
    bool contactEvents = false;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t type = events[i * kEventSize] & 0x0f;
        contactEvents |= type == EvEnter || type == EvMotion || type == EvLeave;
        if (shown || events[i * kEventSize] == EvReady)
            self->handle(events + i * kEventSize);
    }
    if (shown) {
        self->flush();
    } else {
        self->releaseAll();
    }
    self->reportStamp = 0;  // reports outside an interrupt (power-down's lift) are stamped when posted
    const uint64_t done = mach_absolute_time();
    if (count) {
        self->empty = 0;
    } else if (++self->empty > kEmptyLimit) {
        char state[256];
        tegra_gpio_describe(TEGRA_GPIO_TOUCH_INT, state, sizeof(state));
        panic("SwitchTouch: INT (PX1) fired %u times in a row with an empty event FIFO: %s", self->empty, state);
    }

    ++st.irqs;
    if (!count)
        ++st.empty;
    st.events += count;
    if (count > st.maxEvents)
        st.maxEvents = count;
    if (irqAt && irqAt <= began) {
        st.wake.add(ticksToUs(began - irqAt));
        if (st.reports != reportsBefore)
            st.total.add(ticksToUs(done - irqAt));
        // The FTM4's report period: from the last interrupt with contact events after which a contact stayed down.
        if (contactEvents && self->lastContactIrq && irqAt > self->lastContactIrq) {
            const uint64_t us = ticksToUs(irqAt - self->lastContactIrq);
            if (us < kIntervalLimitUs)
                st.interval.add(us);
        }
        if (contactEvents)
            self->lastContactIrq = self->anyDown() ? irqAt : 0;
    }
    st.countRead.add(ticksToUs(counted - began));
    if (count)
        st.eventRead.add(ticksToUs(read - counted));
    if (st.reports != reportsBefore)
        st.post.add(ticksToUs(self->postTicks));
}

bool SwitchTouch::anyDown() const
{
    for (const Contact &contact : contacts) {
        if (contact.down)
            return true;
    }
    return false;
}

// Work loop, from the watchdog (every 10 s): one os_log line for the window if it had an interrupt (file comment), then
// a new window. Formatted here and logged as one string (the kernel's os_log drops a message whose scalar arguments do
// not fit its 256-byte buffer; NativeLegacyDisplay noteStatsWindow).
void SwitchTouch::logStats()
{
    Stats &st = stats;
    if (!st.irqs)
        return;
    const uint64_t now = mach_absolute_time();
    const uint64_t ms = ticksToUs(now - st.start) / 1000;
    tegra_i2c_counters i2c = {};
    tegra_i2c_get_counters(kBus, &i2c);
    const uint32_t i2cIrqs = i2c.interrupts - st.i2c.interrupts, dma = i2c.dmaReads - st.i2c.dmaReads;
    char line[768];
    size_t at = 0;
    auto add = [&](const char *format, auto... values) {
        if (at < sizeof(line))
            at += (size_t)snprintf(line + at, sizeof(line) - at, format, values...);
    };
    auto span = [&](const char *name, const Spread &value) {
        add("%s %llu/%llu/%llu", name, (unsigned long long)value.mean(), (unsigned long long)value.quantile(990),
            (unsigned long long)value.max);
    };
    add("%llu ms: %u interrupts (%u empty), %u reports, %u events (max %u); ", (unsigned long long)ms, st.irqs,
        st.empty, st.reports, st.events, st.maxEvents);
    if (st.interval.n) {
        const uint64_t mean = st.interval.mean();
        span("report interval", st.interval);
        add(" us mean/p99/max (min %llu, %llu.%llu Hz, %u)", (unsigned long long)st.interval.min,
            (unsigned long long)(mean ? 1000000 / mean : 0), (unsigned long long)(mean ? 10000000 / mean % 10 : 0),
            st.interval.n);
    } else {
        add("%s", "report interval none");
    }
    span("; irq->action", st.wake);
    span(", count read", st.countRead);
    span(", event read", st.eventRead);
    span(", HID post", st.post);
    span(", irq->post", st.total);
    add(" us mean/p99/max; I2C%u %u interrupts (%u.%u per interrupt)%s, %u DMA reads%s; stamps %s", kBus, i2cIrqs,
        i2cIrqs / st.irqs, i2cIrqs * 10 / st.irqs % 10, i2c.chainOff ? " unchained" : "", dma,
        i2c.dmaOff ? " (DMA off)" : "", timed ? "irq" : "post");
    os_log(OS_LOG_DEFAULT, "SwitchTouch stats: %s", line);  // log only, never the boot console
    // The contact filter's view (file comment): what it took out of touches, and the numbers its constants answer to.
    at = 0;
    span("touch-down drift", st.settle);
    add(" px mean/p99/max (%u; first %llu ms, measured only)", st.settle.n, (unsigned long long)kDriftMs);
    span("; taps raw travel", st.tapRaw);
    add(" px (%u, reported moving %u", st.tapRaw.n, st.tapsMoved);
    span(", reported travel", st.tapReported);
    add(" px; hysteresis %d.%d px)", switch_touch_filter::kHysteresis / switch_touch_filter::kQ,
        switch_touch_filter::kHysteresis % switch_touch_filter::kQ * 10 / switch_touch_filter::kQ);
    span("; edge swipes start", st.edgeStart);
    add(" px from the edge (%u; band %u)", st.edgeStart.n, kEdgeBand);
    span("; 4-finger arrival", st.arrival);
    add(" ms (%u)", st.arrival.n);
    span("; strength", st.strength);
    add(" (palms %u); re-touches %u", st.palms, st.retouches);
    os_log(OS_LOG_DEFAULT, "SwitchTouch geometry: %s", line);  // log only, never the boot console
    st.reset();
}

void SwitchTouch::position(const uint8_t *event, int &x, int &y) const
{
    const uint16_t lx = scale((unsigned)event[1] << 4 | event[3] >> 4, kRealMaxX, kSwitchTouchWidth);
    const uint16_t ly = scale((unsigned)event[2] << 4 | (event[3] & 0xf), kRealMaxY, kSwitchTouchHeight);
    // Portrait-native (SwitchHID.h): the panel's frame, as the display maps landscape pixel (x, y) to row 1279 - x, column y.
    x = SWITCH_PORTRAIT_NATIVE ? ly : lx;
    y = SWITCH_PORTRAIT_NATIVE ? (int)(kSwitchTouchWidth - 1 - lx) : ly;
}

void SwitchTouch::report(Contact &contact, int x, int y)
{
    x = x < 0 ? 0 : x >= (int)kSwitchTouchReportWidth ? (int)kSwitchTouchReportWidth - 1 : x;
    y = y < 0 ? 0 : y >= (int)kSwitchTouchReportHeight ? (int)kSwitchTouchReportHeight - 1 : y;
    contact.x = (uint16_t)x;
    contact.y = (uint16_t)y;
}

// Touch-down at raw (x, y): reported there; its edge flag and edge-swipe candidacy; a re-touch.
void SwitchTouch::begin(Contact &contact, int x, int y, unsigned z, uint64_t now)
{
    if (contact.leftAt && now > contact.leftAt && ticksToUs(now - contact.leftAt) < kRetouchMs * 1000 &&
        distance(x - contact.leftX, y - contact.leftY) < kRetouchPx)
        ++stats.retouches;
    report(contact, x, y);
    contact.filter.begin(x, y, z, ticksToUs(now));
    contact.startX = (int16_t)x;
    contact.startY = (int16_t)y;
    contact.downAt = now;
    contact.drift = contact.rawTravel = contact.reportedTravel = 0;
#if DEVELOPMENT || DEBUG
    contact.trace.start(z);
#endif
    contact.driftSeen = contact.moved = contact.edgeSwipeSeen = false;
    // The nearest edge (0 left, 1 right, 2 top, 3 bottom of the report frame) and how far the first point is from it.
    const int d[4] = {x, (int)kSwitchTouchReportWidth - 1 - x, y, (int)kSwitchTouchReportHeight - 1 - y};
    unsigned nearest = 0;
    for (unsigned i = 1; i < 4; ++i)
        nearest = d[i] < d[nearest] ? i : nearest;
    contact.nearEdge = (uint8_t)nearest;
    contact.edgeDistance = (uint16_t)d[nearest];
    contact.edgeCandidate = d[nearest] < kEdgeNear;
}

// The touch-down drift, once a contact (after kDriftMs, or at its lift if sooner).
void SwitchTouch::noteDrift(Contact &contact)
{
    if (contact.driftSeen)
        return;
    contact.driftSeen = true;
    stats.settle.add(contact.drift);
}

// A motion to raw (x, y) at strength z: the contact's reported position (SwitchTouchFilter.h) and its stats.
void SwitchTouch::track(Contact &contact, int x, int y, unsigned z, uint64_t now)
{
    const int fromStart = distance(x - contact.startX, y - contact.startY);
    if (fromStart > contact.rawTravel)
        contact.rawTravel = (uint16_t)fromStart;
    const uint64_t ms = now > contact.downAt ? ticksToUs(now - contact.downAt) / 1000 : 0;
    if (ms < kDriftMs) {
        if (fromStart > contact.drift)
            contact.drift = (uint16_t)fromStart;
    } else {
        noteDrift(contact);
    }
    if (contact.edgeCandidate && !contact.edgeSwipeSeen && ms < kEdgeSwipeMs) {
        const int away[4] = {x - contact.startX, contact.startX - x, y - contact.startY, contact.startY - y};
        if (away[contact.nearEdge] >= kEdgeSwipe) {
            contact.edgeSwipeSeen = true;
            stats.edgeStart.add(contact.edgeDistance);
        }
    }
#if DEVELOPMENT || DEBUG
    contact.trace.add((unsigned)ms, x - contact.startX, y - contact.startY, z);
#endif
    if (!contact.filter.sample(x, y, z, ticksToUs(now)))
        return;
    const uint16_t wasX = contact.x, wasY = contact.y;
    report(contact, contact.filter.x.pixel(), contact.filter.y.pixel());
    if (contact.x == wasX && contact.y == wasY)
        return;
    contact.moved = true;
    const int reported = distance(contact.x - contact.startX, contact.y - contact.startY);
    if (reported > contact.reportedTravel)
        contact.reportedTravel = (uint16_t)reported;
}

// A contact lifted (or rejected): its drift, tap and re-touch stats.
void SwitchTouch::ended(Contact &contact, uint64_t now)
{
    noteDrift(contact);
    const uint64_t ms = contact.downAt && now >= contact.downAt ? ticksToUs(now - contact.downAt) / 1000 : UINT64_MAX;
    if (ms < kTapMs && contact.rawTravel < kTapTravel) {
        stats.tapRaw.add(contact.rawTravel);
        stats.tapReported.add(contact.reportedTravel);
        if (contact.moved)
            ++stats.tapsMoved;
    }
#if DEVELOPMENT || DEBUG
    if (ms < kTraceMs)
        contact.trace.log((unsigned)(&contact - contacts), contact.startX, contact.startY, (unsigned)ms,
                          contact.x - contact.startX, contact.y - contact.startY);
#endif
    contact.leftAt = now;
    contact.leftX = (int16_t)contact.x;
    contact.leftY = (int16_t)contact.y;
}

// One FIFO event. A contact that goes down and up within one batch gets each transition in its own report.
void SwitchTouch::handle(const uint8_t *event)
{
    if (event[0] == EvReady) {  // the controller reset itself (ESD): its sensing and interrupt are off again
        releaseAll();
        enableSensing();
        return;
    }
    const uint8_t type = event[0] & 0x0f, id = event[0] >> 4;
    if (id >= kSlots || (type != EvEnter && type != EvMotion && type != EvLeave))
        return;
    Contact &contact = contacts[id];
    const uint32_t bit = 1u << id;
    const uint64_t now = reportStamp ? reportStamp : mach_absolute_time();
    if (type == EvLeave) {  // its coordinates are noise (hekate skips them too)
        contact.palm = false;
        if (!contact.down)
            return;
        if (changed & bit)
            flush();
        contact.down = false;
        changed |= bit;
        ended(contact, now);
        dirty = true;
        return;
    }
    const unsigned z = strength(event);
    if (z >= kPalmStrength) {  // a palm: lifted once if it was a contact, ignored until it leaves
        if (!contact.palm)
            ++stats.palms;
        contact.palm = true;
        if (!contact.down)
            return;
        if (changed & bit)
            flush();
        contact.down = false;
        changed |= bit;
        ended(contact, now);
        dirty = true;
        return;
    }
    if (contact.palm)
        return;
    int x = 0, y = 0;
    position(event, x, y);
    if (!contact.down) {
        if (type == EvMotion)
            return;  // as Linux: motion of a contact that has left is stale
        if (changed & bit)
            flush();
        begin(contact, x, y, z, now);
        stats.strength.add(z);
        contact.down = true;
        changed |= bit;
        const bool atEdge = contact.x < kEdgeBand || contact.x >= kSwitchTouchReportWidth - kEdgeBand ||
                            contact.y < kEdgeBand || contact.y >= kSwitchTouchReportHeight - kEdgeBand;
        edge = atEdge ? edge | bit : edge & ~bit;
        // The fourth contact down: how long after the earliest of the four it came.
        unsigned down = 0;
        uint64_t earliest = now;
        for (const Contact &c : contacts) {
            if (!c.down)
                continue;
            ++down;
            if (c.downAt < earliest)
                earliest = c.downAt;
        }
        if (down == 4)
            stats.arrival.add(ticksToUs(now - earliest) / 1000);
        dirty = true;
        return;
    }
    track(contact, x, y, z, now);
    // Every scan of a contact is a report, moved or not: iOS assumes a digitizer that reports each frame while touched,
    // and IOHIDEventDriver dispatches an unchanged report of a down contact as a stationary touch. Without them
    // SpringBoard's touch history (averaged over the last 42 ms by the clock) never saw a pause, so the app switcher's
    // swipe and pause went home, and UIKit's velocity (the last motion over the time since) flung content after a
    // finger had stopped.
    dirty = true;
}

void SwitchTouch::flush()
{
    if (!dirty)
        return;
    SwitchTouchReport report = {};
    report.id = 1;
    for (unsigned i = 0; i < kSlots; ++i) {
        report.slot[i].flags = (uint8_t)(kSwitchTouchValid | (contacts[i].down ? kSwitchTouchTip : 0));
        report.slot[i].contact = (uint8_t)i;
        report.slot[i].x = contacts[i].x;
        report.slot[i].y = contacts[i].y;
    }
    switch_hid_set_edge_contacts(hid, edge);
    // The interrupt's time (interruptOccurred), or now outside one (power-down's lift); each report's later than the last
    // one's: IOHIDEventDriver takes an element whose time equals the report's as part of it (IOHIDElement processReport
    // stamps only changed values), so two reports with one time would repeat the first one's lifts.
    const uint64_t began = mach_absolute_time();
    uint64_t stamp = reportStamp ? reportStamp : began;
    if (stamp <= lastStamp)
        stamp = lastStamp + 1;
    lastStamp = stamp;
    // Dropped by IOHIDFamily until the event driver has opened the device.
    if (timed)
        switch_hid_post_at(hid, &report, sizeof(report), stamp);
    else
        switch_hid_post(hid, &report, sizeof(report));
    postTicks += mach_absolute_time() - began;
    ++stats.reports;
    if (reportStamp && switch_display_is_on()) {
        switch_touch_note_report(stamp);  // an interrupt's, on a lit screen
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
        bool down = false;
        unsigned changes = 0;
        for (unsigned i = 0; i < kSlots; ++i) {
            down |= contacts[i].down;
            if (changed & 1u << i)
                changes |= contacts[i].down ? kFramesBegan : kFramesEnded;
        }
        switch_frames_touch(stamp, down, changes);
#endif
    }
    changed = 0;
    dirty = false;
}

void SwitchTouch::releaseAll()
{
    for (Contact &contact : contacts) {
        contact.palm = false;
        if (contact.down) {
            contact.down = false;
            dirty = true;
        }
    }
    flush();
}

// Work loop: the panel powered off as hekate's touch_power_off (reset low, LDO6 off) with every contact lifted, the
// watchdog stopped. It comes back up from scratch by the same timer path as at start (touch_power_on, touch_init).
void SwitchTouch::powerDown()
{
    timer->cancelTimeout();
    if (!running)
        return;
    irq->disable();
    releaseAll();
    running = false;
    lastContactIrq = 0;
    switch_als_supply(false);  // first: the sensor powers down while it still has LDO6
    tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 0);
    uint8_t ldo = 0;
    if (!tegra_i2c_read(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1)) {
        ldo = (uint8_t)(ldo & ~kLdoModeNormal);
        (void)tegra_i2c_write(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    }
}

// System sleep (SwitchSuspend's sleep and wake actions, thread context, I2C up). After SC7 the panel stays down while
// the screen is dark (a wake that did not light it, e.g. the RTC alarm); the next display power-on brings it up.
void SwitchTouch::suspend()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<SwitchTouch *>(owner)->powerDown();
            return kIOReturnSuccess;
        },
        this);
}

void SwitchTouch::resume()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchTouch *>(owner);
            if (!self->screenOff)
                self->timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        },
        this);
}

// Display power (SwitchBacklight's work loop, thread context): down with the screen, as Linux ftm4 fts_suspend powers it
// off; back up when the screen lights, on this work loop (about 60 ms, while the panel takes about 0.5 s to wake).
void SwitchTouch::displayPower(bool on)
{
    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchTouch *>(owner);
            self->screenOff = arg == nullptr;
            if (self->screenOff)
                self->powerDown();
            else if (!self->running)
                self->timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        },
        this, on ? this : nullptr);
}

namespace {
SwitchTouch *touch;
// The display's touch->scan stat (NativeLegacyDisplay notePresent, r17-touch): the interrupt time of the oldest report
// posted that no swap has followed since. Set by the touch work loop only while 0, taken by the display's gate.
volatile uint64_t gUnansweredReport;
}

// A report older than the display's idle gap (kGapUs, 250 ms) when the next swap comes was a touch the screen had no
// reason to answer (nothing on it moves): taken and dropped, so it does not stand for a frame's input age.
extern "C" uint64_t switch_touch_take_report(uint64_t before)
{
    uint64_t stamp = __atomic_load_n(&gUnansweredReport, __ATOMIC_ACQUIRE);
    if (!stamp || stamp >= before)
        return 0;
    if (!__atomic_compare_exchange_n(&gUnansweredReport, &stamp, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
    uint64_t stale = 0;
    nanoseconds_to_absolutetime(250000000ull, &stale);
    return before - stamp < stale ? stamp : 0;
}

static void switch_touch_note_report(uint64_t stamp)
{
    uint64_t none = 0;
    __atomic_compare_exchange_n(&gUnansweredReport, &none, stamp, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

extern "C" void switch_touch_suspend(void)
{
    if (touch)
        touch->suspend();
}

extern "C" void switch_touch_resume(void)
{
    if (touch)
        touch->resume();
}

extern "C" void switch_touch_display_power(bool on)
{
    if (touch)
        touch->displayPower(on);
}

extern "C" int switch_touch_start(void)
{
    if (touch)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchTouch *candidate = OSTypeAlloc(SwitchTouch);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources) ||
        !candidate->start(resources)) {
        panic("SwitchTouch: attach or start failed");
    }
    touch = candidate;  // the attachment keeps it
    candidate->release();
    return 0;
}
