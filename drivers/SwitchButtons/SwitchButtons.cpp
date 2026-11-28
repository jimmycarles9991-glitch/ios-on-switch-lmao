// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED buttons for iPadOS, through SwitchHIDDevice's consumer-control device (../SwitchTouch/SwitchHID.h):
// Power (0x0C/0x30, lock/sleep, Siri and the power-off slider on long presses), Volume Increment (0xE9) and Volume
// Decrement (0xEA), the usage pairs iOS's button handling takes (AppleM68Buttons; ios-sweet SweetHIDButtons).
//
// Hardware:
//   * Volume up/down: GPIO PX6/PX7, active low with pull-ups, both edges, 16 ms debounce (OLED DTB gpio-keys,
//     debounce-interval 0x10; hekate bdk/utils/btn.c reads them the same way).
//   * Power: not a GPIO on this board (gpio-keys "power" PX5 is disabled, "Not connected"). It is the MAX77620's EN0
//     input: ONOFFSTAT (0x15) bit 2 is set while pressed (hekate btn_read), and its edges raise ONOFFIRQ (0x0B)
//     EN0_F/EN0_R, clear on read, under the ONOFF bit of IRQTOP (0x05). The PMIC's nIRQ (active low) reaches the GIC
//     as SPI 86 = INTID 118 (tegra210b01-nx-pmic.dtsi max77620@3c "PMU_EXT") through the PMC, which inverts it when
//     PMC_CNTRL (0x7000E400) bit 17 INTR_POLARITY is set; the OLED DT asks for that (nvidia,invert-interrupt) and
//     Linux pmc.c tegra20_pmc_setup_irq_polarity sets it at boot, so this driver does the same. Every other PMIC
//     interrupt source stays masked (IRQTOPM 0x0D, ONOFFIRQM 0x12); the global mask GLBLM (INTENLBT 0x0E bit 0) is
//     cleared as Linux max77620_irq_global_unmask does. Register map: hekate bdk/power/max77620.h.
// A press and release that both happen before the work loop runs still give one press: two volume edges with the
// level unchanged, or both EN0 edges latched, are reported as a press followed by a release.
// A volume key whose interrupt cannot be had panics with a one-line reason; there is no polled mode. A 10 s watchdog
// checks that latched volume edges reached the CPU (tegra_gpio_check_delivery). A PMIC that does not answer on I2C5, or
// whose interrupt storms, is one log line and the power key off for the boot; the volume keys keep working.
// Waking (../SwitchSuspend/SwitchWake.h): a power-key press with the panel off is a wake source (journal line, wake
// guarantee); the press itself goes to SpringBoard as always. Menu (0x0C/0x40, J120's Home button) and Snapshot
// (0x0C/0x65, the screenshot key) are this device's fourth and fifth usages: switch_buttons_bt_click() clicks them
// (press, then release, each its own report) for a Bluetooth Nintendo controller's Home and Capture
// (../SwitchSuspend/SwitchWakeSources.h), from a software event source on this work loop, so any context may ask.
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../SwitchTouch/SwitchHID.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "../SwitchPower/SwitchPower.h"
#include "../SwitchSuspend/SwitchWake.h"
#include "SwitchButtons.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"

#if SWITCH_RECORDER
// XNU kernel/switch/recorder (--//kernel:recorder): the owner saw a hitch; freeze the ring into a dump.
extern "C" __attribute__((weak)) void switch_recorder_mark(void)
{}
#endif

namespace {
constexpr unsigned kPmicBus = 5;
constexpr uint8_t kPmic = 0x3c;

enum : uint8_t {
    IrqTop = 0x05,
    IrqTopMask = 0x0d,
    IntEnLbt = 0x0e,
    OnOffIrq = 0x0b,
    OnOffIrqMask = 0x12,
    OnOffStat = 0x15
};

constexpr unsigned kTopOnOff = 1u << 1, kTopRtc = 1u << 3, kGlobalMask = 1u << 0, kEn0Falling = 1u << 2,
                   kEn0Rising = 1u << 3, kEn0Pressed = 1u << 2;
constexpr uint32_t kPmicIntid = 118;  // GIC SPI 86
constexpr uint32_t kPmcCntrl = 0;
constexpr uint32_t kPmcIntrPolarity = 1u << 17;
#if DEVELOPMENT || DEBUG
constexpr uint32_t kWatchdogMS = 10000;  // with as much leeway (switch_check_after_ms): no wakeup of its own
#endif
constexpr unsigned kPmicBurst = 20;      // PMIC interrupts per second that mean the line is stuck
constexpr uint32_t kBtClickHoldMS = 50;  // synthetic press duration for SpringBoard recognizers
constexpr uint32_t kBtClickGapMS = 50;   // release gap between queued back-to-back clicks
constexpr uint8_t kBtClickMaxQueue = 4;

struct BtClickMap {
    uint8_t bt, button;
};

constexpr BtClickMap kBtClicks[] = {
    {SWITCH_BT_HOME, kSwitchButtonHome},
    {SWITCH_BT_CAPTURE, kSwitchButtonSnapshot},
};
constexpr unsigned kBtClickCount = sizeof(kBtClicks) / sizeof(kBtClicks[0]);

enum class BtClickPhase : uint8_t {
    Idle,
    Holding,
    Gap,
};

struct BtClickSlot {
    BtClickPhase phase = BtClickPhase::Idle;
    uint8_t queued = 0;
    uint64_t deadline = 0;
};

struct Volume {
    unsigned gpio;
    uint8_t button;
    const char *name;
};

constexpr Volume kVolume[] = {
    {TEGRA_GPIO_VOLUME_UP, kSwitchButtonVolumeUp, "volume up (PX6)"},
    {TEGRA_GPIO_VOLUME_DOWN, kSwitchButtonVolumeDown, "volume down (PX7)"},
};
constexpr unsigned kVolumeCount = sizeof(kVolume) / sizeof(kVolume[0]);

int pmicRead(uint8_t reg, uint8_t *value)
{
    return tegra_i2c_read(kPmicBus, kPmic, reg, value, 1);
}

int pmicWrite(uint8_t reg, uint8_t value)
{
    return tegra_i2c_write(kPmicBus, kPmic, reg, &value, 1);
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchButtons : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchButtons)

        IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *volume[kVolumeCount] = {};
    IOInterruptEventSource *pmic = nullptr;
    IOInterruptEventSource *home = nullptr;  // software source (no provider): switch_buttons_bt_click
    IOTimerEventSource *btTimer = nullptr;   // synthetic hold/gap timer for Bluetooth clicks
    uint8_t clicks = 0;                      // SWITCH_BT_HOME | SWITCH_BT_CAPTURE asked for (atomics)
    BtClickSlot btSlots[kBtClickCount] = {};
#if DEVELOPMENT || DEBUG
    IOTimerEventSource *watchdog = nullptr;
#endif
    IOService *hid = nullptr;
    uint8_t buttons = 0;
    unsigned pmicCount = 0;
    uint64_t pmicWindow = 0;

    static void volumeChanged(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void pmicInterrupt(OSObject *owner, IOInterruptEventSource *sender, int count);
#if DEVELOPMENT || DEBUG
    static void watchdogFired(OSObject *owner, IOTimerEventSource *sender);
#endif
    static void btClicks(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void btTimerFired(OSObject *owner, IOTimerEventSource *sender);
    void advanceBtClicks();
    bool setUpPmic();
    void powerKeyOff();
    void change(uint8_t button, bool pressed, bool pulse);
    void post();

public:
    bool start(IOService *provider) override;
    void free() override;

    void click(uint8_t which)
    {
        __atomic_fetch_or(&clicks, which, __ATOMIC_ACQ_REL);
        home->interruptOccurred(nullptr, nullptr, 0);
    }
};

static SwitchButtons *volatile gButtons;  // set once started

OSDefineMetaClassAndStructors(SwitchButtons, IOService)

    bool SwitchButtons::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    loop = IOWorkLoop::workLoop();
    if (!loop)
        panic("SwitchButtons: work loop setup failed");
#if DEVELOPMENT || DEBUG
    watchdog = IOTimerEventSource::timerEventSource(this, &SwitchButtons::watchdogFired);
    if (!watchdog || loop->addEventSource(watchdog) != kIOReturnSuccess)
        panic("SwitchButtons: work loop setup failed");
#endif

    for (unsigned i = 0; i < kVolumeCount; ++i) {
        IOService *nub = tegra_gpio_interrupt_nub(kVolume[i].gpio, TEGRA_GPIO_EDGE_BOTH);
        if (!nub) {
            char state[256];
            tegra_gpio_describe(kVolume[i].gpio, state, sizeof(state));
            panic("SwitchButtons: no interrupt nub for %s: %s", kVolume[i].name, state);
        }
        tegra_gpio_set_debounce(kVolume[i].gpio, 16);
        volume[i] = IOInterruptEventSource::interruptEventSource(this, &SwitchButtons::volumeChanged, nub, 0);
        if (!volume[i] || loop->addEventSource(volume[i]) != kIOReturnSuccess) {
            panic("SwitchButtons: cannot attach %s", kVolume[i].name);
        }
        if (tegra_gpio_read(kVolume[i].gpio) == 0)
            buttons |= kVolume[i].button;
    }

    if (setUpPmic()) {
        pmic = IOInterruptEventSource::interruptEventSource(this, &SwitchButtons::pmicInterrupt,
                                                            switch_interrupt_nub(kPmicIntid, false), 0);
        if (!pmic || loop->addEventSource(pmic) != kIOReturnSuccess)
            panic("SwitchButtons: cannot attach the MAX77620 interrupt (INTID 118)");
    }

    home = IOInterruptEventSource::interruptEventSource(this, &SwitchButtons::btClicks);
    btTimer = IOTimerEventSource::timerEventSource(this, &SwitchButtons::btTimerFired);
    if (!home || !btTimer || loop->addEventSource(home) != kIOReturnSuccess ||
        loop->addEventSource(btTimer) != kIOReturnSuccess) {
        panic("SwitchButtons: click source setup failed");
    }

    hid = switch_hid_create(kSwitchHIDButtons, loop);

    for (auto *source : volume)
        source->enable();
    if (pmic)
        pmic->enable();
    home->enable();
#if DEVELOPMENT || DEBUG
    switch_check_after_ms(watchdog, kWatchdogMS);
    IOLog("SwitchButtons: started\n");
#endif
    return true;
}

void SwitchButtons::free()
{
    if (loop) {
        for (auto *source : volume) {
            if (source)
                loop->removeEventSource(source);
        }
        if (pmic)
            loop->removeEventSource(pmic);
        if (home)
            loop->removeEventSource(home);
        if (btTimer) {
            btTimer->cancelTimeout();
            loop->removeEventSource(btTimer);
        }
#if DEVELOPMENT || DEBUG
        if (watchdog)
            loop->removeEventSource(watchdog);
#endif
    }
    for (auto *&source : volume)
        OSSafeReleaseNULL(source);
    OSSafeReleaseNULL(pmic);
    OSSafeReleaseNULL(home);
    OSSafeReleaseNULL(btTimer);
#if DEVELOPMENT || DEBUG
    OSSafeReleaseNULL(watchdog);
#endif
    OSSafeReleaseNULL(loop);
    IOService::free();
}

// PMC polarity first (the GIC line is still disabled), then the PMIC: stale EN0 edges read away, only EN0's edges
// unmasked, global mask off.
bool SwitchButtons::setUpPmic()
{
    switch_pmc_rmw(kPmcCntrl, 0, kPmcIntrPolarity);
    const uint32_t control = switch_pmc_read(kPmcCntrl);
    if (!(control & kPmcIntrPolarity)) {
        panic("SwitchButtons: PMC_CNTRL %#x: INTR_POLARITY (bit 17) does not set, so the MAX77620's active-low nIRQ "
              "would "
              "reach GIC SPI 86 inverted",
              control);
    }

    uint8_t stale = 0;
    uint8_t global = 0;
    uint8_t state = 0;
    int err = tegra_i2c_init(kPmicBus);
    if (!err)
        err = pmicRead(OnOffIrq, &stale);
    if (!err)
        err = pmicWrite(OnOffIrqMask, static_cast<uint8_t>(~(kEn0Falling | kEn0Rising)));
    if (!err)
        err = pmicWrite(IrqTopMask, static_cast<uint8_t>(~kTopOnOff));
    if (!err)
        err = pmicRead(IntEnLbt, &global);
    if (!err && (global & kGlobalMask))
        err = pmicWrite(IntEnLbt, static_cast<uint8_t>(global & ~kGlobalMask));
    if (!err)
        err = pmicRead(OnOffStat, &state);
    if (err) {
        IOLog("SwitchButtons: power key off: MAX77620 (I2C5 0x3c) interrupt setup failed: I2C error %d\n", err);
        return false;
    }
    if (state & kEn0Pressed)
        buttons |= kSwitchButtonPower;
    return true;
}

// Work loop: the PMIC's interrupt stays masked for the rest of the boot (nIRQ may stay low: nothing reads it again).
void SwitchButtons::powerKeyOff()
{
    pmic->disable();
    change(kSwitchButtonPower, false, false);
}

void SwitchButtons::post()
{
    const SwitchButtonsReport report = {1, buttons};
    switch_hid_post(hid, &report, sizeof(report));  // dropped by IOHIDFamily until the event driver has opened it
}

void SwitchButtons::change(uint8_t button, bool pressed, bool pulse)
{
    const bool was = (buttons & button) != 0;
    if (pressed == was && !pulse)
        return;
    if (pressed == was) {  // a whole press (or release) happened before we got here: report both halves
        buttons ^= button;
        post();
    }
    buttons = static_cast<uint8_t>(pressed ? buttons | button : buttons & ~static_cast<unsigned>(button));
    post();
}

// Work loop. `count` is the number of edges since the action last ran.
void SwitchButtons::volumeChanged(OSObject *owner, IOInterruptEventSource *sender, int count)
{
    auto *self = OSDynamicCast(SwitchButtons, owner);
    if (!self)
        return;
    for (unsigned i = 0; i < kVolumeCount; ++i) {
        if (self->volume[i] == sender)
            self->change(kVolume[i].button, tegra_gpio_read(kVolume[i].gpio) == 0, count >= 2);
    }
}

// Work loop, INTID 118 masked until return: reading ONOFFIRQ releases nIRQ.
void SwitchButtons::pmicInterrupt(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchButtons, owner);
    if (!self)
        return;
    uint8_t top = 0;
    uint8_t edges = 0;
    uint8_t state = 0;
    int err = pmicRead(IrqTop, &top);
    if (!err && (top & kTopOnOff))
        err = pmicRead(OnOffIrq, &edges);
    // The RTC alarm shares nIRQ; it is unmasked only around system sleep (SwitchPMIC's wake alarm).
    if (!err && (top & kTopRtc))
        switch_rtc_interrupt();
    if (!err)
        err = pmicRead(OnOffStat, &state);
    if (err) {
        IOLog("SwitchButtons: power key off: reading the MAX77620 interrupt status failed: I2C error %d\n", err);
        self->powerKeyOff();
        return;
    }

    const uint64_t now = mach_absolute_time();
    uint64_t elapsedNS = 0;
    absolutetime_to_nanoseconds(now - self->pmicWindow, &elapsedNS);
    if (elapsedNS > 1000000000ULL) {
        self->pmicWindow = now;
        self->pmicCount = 0;
    }
    if (++self->pmicCount > kPmicBurst) {
        char gic[160];
        switch_interrupt_describe(kPmicIntid, gic, sizeof(gic));
        IOLog("SwitchButtons: power key off: MAX77620 interrupt (INTID 118) fired %u times in %llu ms: IRQTOP %#x "
              "ONOFFIRQ %#x ONOFFSTAT %#x, %s\n",
              self->pmicCount, elapsedNS / 1000000, top, edges, state, gic);
        self->powerKeyOff();
        return;
    }
    const bool wasPressed = (self->buttons & kSwitchButtonPower) != 0;
    self->change(kSwitchButtonPower, (state & kEn0Pressed) != 0,
                 (edges & (kEn0Falling | kEn0Rising)) == (kEn0Falling | kEn0Rising));
    // A press (down, or a whole press latched before this ran) with the panel off: a wake source.
    if ((!wasPressed && (state & kEn0Pressed)) || (edges & (kEn0Falling | kEn0Rising)) == (kEn0Falling | kEn0Rising))
        switch_wake_event(SWITCH_WAKE_POWER_KEY);
}

void SwitchButtons::advanceBtClicks()
{
    const uint64_t now = mach_absolute_time();
    uint64_t holdAbs = 0;
    uint64_t gapAbs = 0;
    clock_interval_to_absolutetime_interval(kBtClickHoldMS, kMillisecondScale, &holdAbs);
    clock_interval_to_absolutetime_interval(kBtClickGapMS, kMillisecondScale, &gapAbs);
    bool changed = false;
    uint64_t nextDeadline = 0;
    for (unsigned i = 0; i < kBtClickCount; ++i) {
        BtClickSlot &slot = btSlots[i];
        const uint8_t bit = kBtClicks[i].button;
        if (slot.phase == BtClickPhase::Holding && now >= slot.deadline) {
            buttons &= static_cast<uint8_t>(~bit);
            changed = true;
            if (slot.queued) {
                slot.phase = BtClickPhase::Gap;
                slot.deadline = now + gapAbs;
            } else {
                slot.phase = BtClickPhase::Idle;
                slot.deadline = 0;
            }
        } else if (slot.phase == BtClickPhase::Gap && now >= slot.deadline) {
            slot.phase = BtClickPhase::Idle;
            slot.deadline = 0;
        }
        if (slot.phase == BtClickPhase::Idle && slot.queued) {
            --slot.queued;
            buttons |= bit;
            changed = true;
            slot.phase = BtClickPhase::Holding;
            slot.deadline = now + holdAbs;
        }
        if (slot.phase != BtClickPhase::Idle && (!nextDeadline || slot.deadline < nextDeadline))
            nextDeadline = slot.deadline;
    }
    if (changed)
        post();
    if (nextDeadline)
        btTimer->wakeAtTime(nextDeadline);
}

void SwitchButtons::btTimerFired(OSObject *owner, IOTimerEventSource *)
{
    if (auto *self = OSDynamicCast(SwitchButtons, owner))
        self->advanceBtClicks();
}

// Work loop: press Menu (J120's Home button) and/or Snapshot and hold for kBtClickHoldMS via btTimer so SpringBoard's
// press-duration recognizers see a real press without blocking the work loop.
void SwitchButtons::btClicks(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchButtons, owner);
    if (!self)
        return;
    const uint8_t which = __atomic_exchange_n(&self->clicks, static_cast<uint8_t>(0), __ATOMIC_ACQ_REL);
#if SWITCH_RECORDER
    // A Bluetooth controller's Capture.
    if (which & SWITCH_BT_CAPTURE)
        switch_recorder_mark();
#endif
    for (unsigned i = 0; i < kBtClickCount; ++i) {
        if ((which & kBtClicks[i].bt) && self->btSlots[i].queued < kBtClickMaxQueue)
            ++self->btSlots[i].queued;
    }
    self->advanceBtClicks();
}

#if DEVELOPMENT || DEBUG
void SwitchButtons::watchdogFired(OSObject *owner, IOTimerEventSource *sender)
{
    for (const Volume &v : kVolume)
        tegra_gpio_check_delivery(v.gpio, "SwitchButtons");
    switch_check_after_ms(sender, kWatchdogMS);
}
#endif

extern "C" int switch_buttons_start(void)
{
    static SwitchButtons *buttons;
    if (buttons)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchButtons *candidate = OSTypeAlloc(SwitchButtons);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources) ||
        !candidate->start(resources)) {
        panic("SwitchButtons: attach or start failed");
    }
    buttons = candidate;  // the attachment keeps it
    __atomic_store_n(&gButtons, candidate, __ATOMIC_RELEASE);
    candidate->release();
    return 0;
}

extern "C" void switch_buttons_bt_click(uint8_t buttons)
{
    if (SwitchButtons *driver = __atomic_load_n(&gButtons, __ATOMIC_ACQUIRE))
        driver->click(buttons);
}
