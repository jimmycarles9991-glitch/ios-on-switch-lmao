// SPDX-License-Identifier: BSD-2-Clause-Patent
// System sleep (SC7) coordinator for the Switch OLED: see SwitchSuspend.h for the path and the order.
//
// Firmware (feasibility, switchroot ATF = CTCaer/switch-l4t-atf, the bl31.bin this boot chain uses):
//   * plat_supports_system_suspend() is true on T210B01 (soc/t210/plat_setup.c), so PSCI 1.0 SYSTEM_SUSPEND is
//     offered; it needs every other core OFF (lib/psci psci_system_suspend -> PSCI_E_DENIED otherwise).
//   * SC7 entry is the BPMP firmware's (tegra_bpmp_init sees its SIGN_OF_LIFE: hekate's l4t.c starts
//     bootloader/sys/l4t/bpmpfw_b01.bin with mtc_tbl_b01.bin on the COP; it refuses to launch L4T without them),
//     exit is the bootrom running hekate's warmboot (sc7exit_b01.bin, matched to the burnt fuses; hekate warns
//     "Failed to match warmboot with fuses! If you continue, sleep wont work!" at boot otherwise).
//   * ATF resumes the boot CPU at the SYSTEM_SUSPEND entry: physical address, MMU and caches off, at EL2 (BL33 was
//     entered in EL2, so SCR_EL3.HCE is set: lib/psci psci_get_ns_ep_info), x0 = context id, GIC distributor and
//     CPU interface re-initialized by ATF (tegra_gic_init), memory controller restored, CNTFRQ written.
// The PMC (always-on, non-secure on B01: hekate FLAGS_PMC_NON_SECURE) keeps its registers across SC7; the GIC,
// legacy interrupt controller, GPIO controller, pinmux and every clock/reset come back at their reset values.
//
// PMC as switchroot L4T's pmc.c sets it up for this DT (tegra20_pmc_init, tegra210_pmc_irq_set_wake/_set_type;
// OLED DTB pmc@7000e400: core-pwr-good-time 13100/7800 us, core-pwr-off-time 44160 us, core-pwr-req-active-high,
// sys-clock-req-active-high; max77620 nvidia,pmc-wakeup = wake 51 level low, pwr_int_n).
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <libkern/c++/OSSymbol.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../SwitchUSB/SwitchUSB.h"
#include "../TegraBPMP/TegraBPMP.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../diagnostics/switch/SwitchPowerTrail.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "SwitchSleepDevices.h"
#include "SwitchSleepSteps.h"
#include "SwitchSuspend.h"
#include "SwitchWake.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

// The SoC's hooks (strong: the kernel link takes no undefined weak references; every one below is in a linked
// driver). The devices' are in SwitchSleepDevices.cpp.
extern "C" {
int armcpu_system_sleep_enable(char *why, size_t size);
void arm_gic_suspend(void);
void arm_gic_resume(void);
void tegra_gpio_suspend(void);
void tegra_gpio_resume(void);
void tegra_i2c_suspend(void);
void tegra_i2c_resume(void);
void switch_rtc_sleep_enable(void);
int switch_rtc_resume(void);  // 1 when ALARM1 is what woke the console
void switch_pmic_sleep_report(char *line, size_t size);
bool switch_display_power_enabled(void);  // SwitchBrightness
}

namespace {
constexpr vm_offset_t kPmcPagePA = 0x7000E000, kApbMiscPA = 0x70000000, kCarPA = 0x60006000, kTmrPA = 0x60005000;

// TMR (Linux tegra_wdt.c): TMR5's preset (PTV) and present count (PCR bits 28:0), WDT0's config and status.
enum : uint32_t {
    TIMERUS_USEC_CFG = 0x014,
    TMR5_PTV = 0x060,
    TMR5_PCR = 0x064,
    WDT0_CFG = 0x100,
    WDT0_STATUS = 0x104,
    TMR_COUNT_MASK = 0x1fffffff,
};

// The Tegra RTC at the start of the PMC page (Linux rtc-tegra.c); its APB clock is CAR L4. A write waits for BUSY
// clear (the RTC posts its counters to the bus every eight 32 kHz ticks, at most ~490 us).
enum : uint32_t {
    RTC_BUSY = 0x004,
    RTC_SECONDS = 0x008,
    RTC_SHADOW_SECONDS = 0x00c,
    RTC_MILLI_SECONDS = 0x010,
    RTC_SECONDS_ALARM0 = 0x014,
    RTC_INTR_MASK = 0x028,
    RTC_INTR_STATUS = 0x02c,
    RTC_SEC_ALARM0 = 1u << 0
};
#if DEVELOPMENT || DEBUG
constexpr uint32_t kRtcBusyUS = 600;
// The Tegra RTC's ALARM0 (PMC wake 16, inside the always-on PMC) ends every SC7 this long after entry, whatever else
// does. A sleep the power key cannot end then shows whether SC7 exit works without the MAX77620.
constexpr uint32_t kBackstopSeconds = 180, kBackstopEvent = 16;
#endif

enum : uint32_t {
    CAR_RST_DEVICES_L = 0x004,
    CAR_CLK_OUT_ENB_L = 0x010,
    CAR_CLK_ENB_L_SET = 0x320,
    CAR_L_RTC = 1u << 4
};

constexpr uint32_t kPmc = 0x400;  // PMC registers within their page

enum : uint32_t {
    PMC_CNTRL = kPmc + 0x000,
    PMC_WAKE_MASK = kPmc + 0x00c,
    PMC_WAKE_LVL = kPmc + 0x010,
    PMC_WAKE_STATUS = kPmc + 0x014,
    PMC_SW_WAKE_STATUS = kPmc + 0x018,
    PMC_DPD_SAMPLE = kPmc + 0x020,
    PMC_DPD_ENABLE = kPmc + 0x024,
    PMC_COREPWRGOOD_TIMER = kPmc + 0x03c,
    PMC_CPUPWRGOOD_TIMER = kPmc + 0x0c8,
    PMC_CPUPWROFF_TIMER = kPmc + 0x0cc,
    PMC_COREPWROFF_TIMER = kPmc + 0x0e0,
    PMC_WAKE2_MASK = kPmc + 0x160,
    PMC_WAKE2_LVL = kPmc + 0x164,
    PMC_WAKE2_STATUS = kPmc + 0x168,
    PMC_SW_WAKE2_STATUS = kPmc + 0x16c,
};

enum : uint32_t {
    CNTRL_LATCH_WAKEUPS = 1u << 5,
    CNTRL_PWRREQ_POLARITY = 1u << 8,
    CNTRL_SYSCLK_POLARITY = 1u << 10,
    CNTRL_SYSCLK_OE = 1u << 11,
    CNTRL_SIDE_EFFECT_LP0 = 1u << 14,
    CNTRL_CPU_PWRREQ_OE = 1u << 16,
};

// The wake events (SwitchWakeSources.h switch_wake_pmc_events): 51 pwr_int_n (MAX77620 nIRQ: power key, RTC alarm),
// level low; 14 gpio_pk4 (BM92T36 USB-PD ALERT: plug, unplug, charger contract), level low (OLED DTB bm92t@18
// nvidia,pmc-wakeup), armed only while the line is high, since SwitchPD masks a line stuck low for the boot.
// DIV_ROUND_UP(13100 us * 8192 Hz), DIV_ROUND_UP(7800 us * 32768 Hz) & 0xff, DIV_ROUND_UP(44160 us * 32768 Hz).
constexpr uint32_t kCoreOsc = (13100u * 8192u + 999999u) / 1000000u, kCorePmu = (7800u * 32768u + 999999u) / 1000000u;
constexpr uint32_t kCorePowerGood = ((kCoreOsc << 8) & 0xff00u) | (kCorePmu & 0xffu);
constexpr uint32_t kCorePowerOff = (44160u * 32768u + 999999u) / 1000000u;
// Pinmux banks, OLED DTB pinmux@700008d4 reg: pad control 0x8d4 + 0x2a5, mux 0x3000 + 0x294 (APB_MISC offsets).
constexpr uint32_t kPadFirst = 0x8d4, kPadWords = 0x2a8 / 4, kMuxFirst = 0x3000, kMuxWords = 0x294 / 4;
// Platform action priorities (one service, so they only place it among other services' actions).
constexpr uint32_t kActionPriority = 1000;
// The SoC watchdog over the sleep and wake actions (debug kernels): longer than IOPMrootDomain's 35 s sleep/wake
// watchdog, so that a hung hook ends in its panic (whose stackshot names the hook) and this only catches what that misses.
constexpr unsigned kActionWatchSeconds = 60;

// ArmCpu panics on a halt unless SC7 is on (armcpu_system_sleep_enable), so whenever it is not, a CPU assertion
// keeps IOPMrootDomain from sleeping (checkSystemSleepAllowed: kPMCPUAssertion), idle and demand sleep alike.
IOPMDriverAssertionID switch_suspend_hold(IOService *owner, const char *why)
{
    IOPMrootDomain *root = IOService::getPMRootDomain();
    const IOPMDriverAssertionID id =
        root ? root->createPMAssertion(kIOPMDriverAssertionCPUBit, kIOPMDriverAssertionLevelOn, owner, "SwitchSuspend")
             : 0;
    IOLog("SwitchSuspend: system sleep held off%s: %s\n", id ? "" : " (NO assertion: a sleep would panic)", why);
    switch_power_journal("SwitchSuspend: system sleep held off: %s", why);
    return id;
}
}  // namespace

class SwitchSuspend : public IOService {
    OSDeclareDefaultStructors(SwitchSuspend) volatile uint8_t *pmc = nullptr, *apb = nullptr, *car = nullptr,
                                                              *tmr = nullptr;
    uint32_t usecConfig = 0, usecAfter = 0;  // TIMERUS_USEC_CFG at quiesce and as SC7 exit left it
    uint32_t pad[kPadWords] = {}, mux[kMuxWords] = {};
    uint32_t wakeStatus = 0, wake2Status = 0;
    uint64_t wakeArmed = 0;              // PMC wake events, bit n = event n
    bool btWake = false;                 // the Bluetooth controller was set up to wake the console for this sleep
    IOPMDriverAssertionID wakeHold = 0;  // after a radio wake, until its controller reconnects or the window ends
    thread_call_t wakeHoldBegin = nullptr, wakeHoldEnd = nullptr;
    static void beginWakeHold(thread_call_param_t owner, thread_call_param_t);
    static void endWakeHold(thread_call_param_t owner, thread_call_param_t);
    uint64_t quiesceNS = 0, activateNS = 0, suspendedAt = 0;
    uint64_t rtcBeforeMS = 0, rtcAfterMS = 0;  // the Tegra RTC at quiesce and at activate
    bool rtcUsable = false;
    switch_battery_gauge gaugeBefore = {};
    int gaugeBeforeErr = -1;
    unsigned sleeps = 0;
    IOPMDriverAssertionID hold = 0;

    uint32_t rd(uint32_t o) const
    {
        return *reinterpret_cast<volatile uint32_t *>(pmc + o);
    }

    uint64_t rtcMS() const
    {
        const uint32_t millis = rd(RTC_MILLI_SECONDS);
        return switch_sleep_rtc_ms(rd(RTC_SHADOW_SECONDS), millis);
    }

    bool rtcClockOn(bool log);
#if DEVELOPMENT || DEBUG
    void journalWatchdog();
    bool rtcWrite(uint32_t o, uint32_t v);
    bool armBackstop();
    void disarmBackstop();
#endif

    void wr(uint32_t o, uint32_t v)
    {
        *reinterpret_cast<volatile uint32_t *>(pmc + o) = v;
    }

    const char *checkPmc(char *why, size_t size);
    void sleepDevices();
    void quiesce();
    void activate();
    void wakeDevices();

public:
    using IOService::callPlatformFunction;
    bool start(IOService *provider) override;
    IOReturn callPlatformFunction(const OSSymbol *function, bool wait, void *p1, void *p2, void *p3, void *p4) override;
};

OSDefineMetaClassAndStructors(SwitchSuspend, IOService)

    // The RTC keeps time through SC7 by itself; only its register interface needs the APB clock, which nothing else here
    // turns off. In reset it reads nothing: the wake line then has no time asleep.
    bool SwitchSuspend::rtcClockOn(bool log)
{
    auto reg = [this](uint32_t o) {
        return reinterpret_cast<volatile uint32_t *>(car + o);
    };
    if (*reg(CAR_RST_DEVICES_L) & CAR_L_RTC) {
        if (log)
            IOLog("SwitchSuspend: the Tegra RTC is held in reset; no time asleep in the wake lines\n");
        return false;
    }
    if (!(*reg(CAR_CLK_OUT_ENB_L) & CAR_L_RTC)) {
        *reg(CAR_CLK_ENB_L_SET) = CAR_L_RTC;
        (void)*reg(CAR_CLK_OUT_ENB_L);
        IODelay(2);
    }
    return true;
}

#if DEVELOPMENT || DEBUG
// The SoC watchdog as the power trail just armed it (SwitchPanicSave.c: TMR5 periodic at 250 ms feeding WDT0, reset
// through the PMC), read back: whether the writes took and whether TMR5 counts.
void SwitchSuspend::journalWatchdog()
{
    if (!tmr)
        return;
    auto reg = [this](uint32_t o) {
        return *reinterpret_cast<volatile uint32_t *>(tmr + o);
    };
    const uint32_t count = reg(TMR5_PCR) & TMR_COUNT_MASK;
    IODelay(10);
    switch_power_journal("SwitchSuspend: watchdog: TMR5_PTV %#x, count %u then %u, WDT0_CFG %#x, WDT0_STATUS %#x",
                         reg(TMR5_PTV), count, reg(TMR5_PCR) & TMR_COUNT_MASK, reg(WDT0_CFG), reg(WDT0_STATUS));
}

bool SwitchSuspend::rtcWrite(uint32_t o, uint32_t v)
{
    if (!switch_mmio_wait32(pmc, RTC_BUSY, 1, 0, kRtcBusyUS))
        return false;
    wr(o, v);
    return true;
}

bool SwitchSuspend::armBackstop()
{
    if (!rtcUsable || !rtcClockOn(false))
        return false;
    return rtcWrite(RTC_SECONDS_ALARM0, rd(RTC_SECONDS) + kBackstopSeconds) &&
           rtcWrite(RTC_INTR_STATUS, RTC_SEC_ALARM0) && rtcWrite(RTC_INTR_MASK, rd(RTC_INTR_MASK) | RTC_SEC_ALARM0);
}

void SwitchSuspend::disarmBackstop()
{
    if (!(wakeArmed >> kBackstopEvent & 1u))
        return;
    if (!rtcWrite(RTC_INTR_MASK, rd(RTC_INTR_MASK) & ~RTC_SEC_ALARM0) || !rtcWrite(RTC_SECONDS_ALARM0, 0) ||
        !rtcWrite(RTC_INTR_STATUS, RTC_SEC_ALARM0))
        IOLog("SwitchSuspend: the Tegra RTC stayed busy; its backstop alarm may still be set\n");
}
#endif

// The power-request setup L4T's PMC probe makes (CPU and system-clock requests driven, core and system-clock requests
// active high, no LP0 side effect of a CPU power gate) must already be there: flipping a request's polarity or drive
// while it is live could drop a rail, so it is checked, never written. The core power-good/off timers only time an
// SC7 entry/exit and are written as L4T writes them.
const char *SwitchSuspend::checkPmc(char *why, size_t size)
{
    const uint32_t control = rd(PMC_CNTRL);
    const uint32_t wanted = CNTRL_CPU_PWRREQ_OE | CNTRL_SYSCLK_OE;
    const uint32_t unwanted = CNTRL_SIDE_EFFECT_LP0 | CNTRL_PWRREQ_POLARITY | CNTRL_SYSCLK_POLARITY;
    if ((control & wanted) != wanted || (control & unwanted)) {
        snprintf(why, size, "PMC_CNTRL %#x is not L4T's power-request setup (wants bits 16, 11 set, 14, 10, 8 clear)",
                 control);
        return why;
    }
    wr(PMC_COREPWRGOOD_TIMER, kCorePowerGood);
    wr(PMC_COREPWROFF_TIMER, kCorePowerOff);
    (void)rd(PMC_COREPWROFF_TIMER);
    return nullptr;
}

bool SwitchSuspend::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    pmc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPmcPagePA, 0x1000));
    apb = reinterpret_cast<volatile uint8_t *>(ml_io_map(kApbMiscPA, 0x4000));
    car = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCarPA, 0x1000));
    tmr = reinterpret_cast<volatile uint8_t *>(ml_io_map(kTmrPA, 0x1000));
    setName("SwitchSuspend");
#if DEVELOPMENT || DEBUG
    rtcUsable = pmc && car && rtcClockOn(true);  // the wake line's time asleep
#endif
    char why[224] = "";
    const char *blocker = nullptr;
    if (!pmc || !apb) {
        blocker = "the PMC/APB_MISC could not be mapped";
    } else if (!switch_display_power_enabled()) {
        blocker = "display power is off (switch-no-sleep)";
    } else if (!(blocker = checkPmc(why, sizeof(why))) && armcpu_system_sleep_enable(why, sizeof(why))) {
        blocker = why;
    }
    if (blocker) {
        hold = switch_suspend_hold(this, blocker);
        registerService();
        return true;
    }
    wakeHoldBegin = thread_call_allocate(&SwitchSuspend::beginWakeHold, this);
    wakeHoldEnd = thread_call_allocate(&SwitchSuspend::endWakeHold, this);
    switch_rtc_sleep_enable();
    // IOService::registerService() installs these as platform actions (iokit/Kernel/IOPlatformActions.cpp).
    setProperty("IOPlatformSleepAction", kActionPriority, 32);
    setProperty("IOPlatformQuiesceAction", kActionPriority, 32);
    setProperty("IOPlatformActiveAction", kActionPriority, 32);
    setProperty("IOPlatformWakeAction", kActionPriority, 32);
    registerService();
    IOLog("SwitchSuspend: started (PSCI SYSTEM_SUSPEND to SC7, wake on the power key and the MAX77620 RTC)\n");
    switch_power_journal("SwitchSuspend: system sleep on (SC7): PMC_CNTRL %#x, CPUPWRGOOD %#x CPUPWROFF %#x",
                         rd(PMC_CNTRL), rd(PMC_CPUPWRGOOD_TIMER), rd(PMC_CPUPWROFF_TIMER));
    return true;
}

IOReturn SwitchSuspend::callPlatformFunction(const OSSymbol *function, bool wait, void *p1, void *p2, void *p3,
                                             void *p4)
{
    if (function->isEqualTo("IOPlatformSleepAction")) {
        sleepDevices();
        return kIOReturnSuccess;
    }
    if (function->isEqualTo("IOPlatformQuiesceAction")) {
        quiesce();
        return kIOReturnSuccess;
    }
    if (function->isEqualTo("IOPlatformActiveAction")) {
        activate();
        return kIOReturnSuccess;
    }
    if (function->isEqualTo("IOPlatformWakeAction")) {
        wakeDevices();
        return kIOReturnSuccess;
    }
    return IOService::callPlatformFunction(function, wait, p1, p2, p3, p4);
}

// Thread context, before any core goes down. I2C last: from here to wakeDevices() every transfer waits.
void SwitchSuspend::sleepDevices()
{
    const uint64_t start = mach_absolute_time();
    switch_sleep_mark(SWITCH_STEP_SLEEP);
    switch_power_trail_watch(kActionWatchSeconds, "sleep action");
    switch_sleep_mark(SWITCH_STEP_SLEEP_WATCHDOG);
    switch_power_journal("SwitchSuspend: sleep %u: entering SC7", sleeps + 1);
#if DEVELOPMENT || DEBUG
    journalWatchdog();
#endif
    switch_power_journal_flush(true);
    btWake = switch_sleep_devices();
#if DEVELOPMENT || DEBUG
    gaugeBeforeErr = switch_battery_gauge_sample(&gaugeBefore);
#endif
    if (!gaugeBeforeErr) {
        switch_power_journal("SwitchSuspend: sleep gauge: AvgCurrent %d mA, Current %d mA, QH %u", gaugeBefore.avg_ma,
                             gaugeBefore.now_ma, gaugeBefore.qh);
    }
#if DEVELOPMENT || DEBUG
    {
        char pmic[160] = "";
        switch_pmic_sleep_report(pmic, sizeof(pmic));
        switch_power_journal("SwitchSuspend: sleep pmic: %s", pmic);
    }
#endif
    // Wake sources (SwitchWakeSources.h): the MAX77620's nIRQ (power key, RTC alarm), the USB-PD ALERT (USB-C
    // plug/unplug) and the gauge's ALRT (the battery's charge zone). Statuses cleared first, as
    // tegra210_pmc_irq_set_wake does.
    switch_sleep_mark(SWITCH_STEP_SLEEP_PMC_WAKE);
    wakeArmed =
        switch_wake_pmc_armed(tegra_gpio_read(TEGRA_GPIO_PD_ALERT) == 1, tegra_gpio_read(TEGRA_GPIO_GAUGE_ALERT) == 1,
                              btWake, false, switch_usb_remote_wake());
#if DEVELOPMENT || DEBUG
    if (armBackstop())
        wakeArmed |= UINT64_C(1) << kBackstopEvent;
#endif
    uint32_t mask[2], high[2], low[2];
    switch_wake_pmc_registers(wakeArmed, mask, high, low);
    // Linux pmc.c tegra_pmc_suspend: software statuses cleared, the wake latches pulsed, then the latched statuses
    // cleared (write one to clear) before the levels and masks.
    wr(PMC_SW_WAKE_STATUS, 0);
    wr(PMC_SW_WAKE2_STATUS, 0);
    wr(PMC_CNTRL, rd(PMC_CNTRL) | CNTRL_LATCH_WAKEUPS);
    IODelay(120);
    wr(PMC_CNTRL, rd(PMC_CNTRL) & ~CNTRL_LATCH_WAKEUPS);
    IODelay(120);
    wr(PMC_WAKE_STATUS, rd(PMC_WAKE_STATUS));
    wr(PMC_WAKE2_STATUS, rd(PMC_WAKE2_STATUS));
    wr(PMC_WAKE_LVL, (rd(PMC_WAKE_LVL) | high[0]) & ~low[0]);
    wr(PMC_WAKE_MASK, mask[0]);
    wr(PMC_WAKE2_LVL, (rd(PMC_WAKE2_LVL) | high[1]) & ~low[1]);
    wr(PMC_WAKE2_MASK, mask[1]);
    (void)rd(PMC_WAKE2_MASK);
    switch_power_journal("SwitchSuspend: sleep pmc wake: events armed %#llx (WAKE_MASK %#x WAKE2_MASK %#x)",
                         (unsigned long long)wakeArmed, mask[0], mask[1]);
    {
        char line[160] = "";
        switch_sleep_mark(SWITCH_STEP_SLEEP_BPMP);
        const uint64_t at = mach_absolute_time();
        const int err = tegra_bpmp_sc7_suspend(line, sizeof(line));
        switch_power_journal("SwitchSuspend: sleep bpmp: %llu us: %s%s", switch_sleep_ns_since(at) / 1000, line,
                             err ? " (SC7 may not be entered)" : "");
#if !(DEVELOPMENT || DEBUG)
        if (err)
            switch_power_event("SwitchSuspend: sleep %u: BPMP SC7 entry refused: %s (SC7 may not be entered)",
                               sleeps + 1, line);
#endif
    }
    switch_sleep_storage();
    const uint64_t us = switch_sleep_ns_since(start) / 1000;
    switch_power_journal("SwitchSuspend: sleep i2c (last; devices took %llu.%03llu ms)", us / 1000, us % 1000);
    switch_sleep_mark(SWITCH_STEP_SLEEP_I2C);
    tegra_i2c_suspend();
    suspendedAt = mach_absolute_time();
}

// Boot CPU, interrupts off, the other cores already off: the state ATF does not keep.
void SwitchSuspend::quiesce()
{
    const uint64_t start = mach_absolute_time();
    tegra_gpio_suspend();
    for (uint32_t i = 0; i < kPadWords; ++i)
        pad[i] = *reinterpret_cast<volatile uint32_t *>(apb + kPadFirst + 4 * i);
    for (uint32_t i = 0; i < kMuxWords; ++i)
        mux[i] = *reinterpret_cast<volatile uint32_t *>(apb + kMuxFirst + 4 * i);
    arm_gic_suspend();
    usecConfig = tmr ? *reinterpret_cast<volatile uint32_t *>(tmr + TIMERUS_USEC_CFG) : 0;
    rtcBeforeMS = rtcUsable ? rtcMS() : 0;
    quiesceNS = switch_sleep_ns_since(start);  // journaled at wake: nothing here may take a lock
    // What ATF's SYSTEM_SUSPEND and the PMC will act on (debug kernels; the power trail keeps it through a warm reset).
    switch_power_trail("bpmp channels %#010x", tegra_bpmp_semaphore());
    switch_power_trail("pmc msk %08x %08x st %08x %08x", rd(PMC_WAKE_MASK), rd(PMC_WAKE2_MASK), rd(PMC_WAKE_STATUS),
                       rd(PMC_WAKE2_STATUS));
    switch_power_trail("pmc lvl %08x %08x cntrl %08x", rd(PMC_WAKE_LVL), rd(PMC_WAKE2_LVL), rd(PMC_CNTRL));
}

// Boot CPU after SC7, interrupts still off: pads and GPIOs before any interrupt can be unmasked.
void SwitchSuspend::activate()
{
    const uint64_t start = mach_absolute_time();
    rtcAfterMS = rtcUsable && rtcClockOn(false) ? rtcMS() : rtcBeforeMS;
    // The microsecond timer's divider (the SoC watchdog counts in its ticks), as Linux tegra210_timer.c restores it.
    if (tmr && usecConfig) {
        usecAfter = *reinterpret_cast<volatile uint32_t *>(tmr + TIMERUS_USEC_CFG);
        *reinterpret_cast<volatile uint32_t *>(tmr + TIMERUS_USEC_CFG) = usecConfig;
    }
    for (uint32_t i = 0; i < kMuxWords; ++i)
        *reinterpret_cast<volatile uint32_t *>(apb + kMuxFirst + 4 * i) = mux[i];
    for (uint32_t i = 0; i < kPadWords; ++i)
        *reinterpret_cast<volatile uint32_t *>(apb + kPadFirst + 4 * i) = pad[i];
    __asm__ volatile("dsb sy" ::: "memory");
    tegra_gpio_resume();
    // The pads held their levels in deep power-down through SC7; released as Linux tegra_pmc_resume does, once the
    // pinmux and GPIO state they come back to is in place.
    wr(PMC_DPD_SAMPLE, 0);
    wr(PMC_DPD_ENABLE, 0);
    (void)rd(PMC_DPD_ENABLE);
    arm_gic_resume();
    wakeStatus = rd(PMC_WAKE_STATUS) | rd(PMC_SW_WAKE_STATUS);
    wake2Status = rd(PMC_WAKE2_STATUS) | rd(PMC_SW_WAKE2_STATUS);
    activateNS = switch_sleep_ns_since(start);
}

void SwitchSuspend::wakeDevices()
{
    const uint64_t start = mach_absolute_time();
    // The time between the last sleep step and the first wake step: SC7 and the firmware's warm boot, by this clock.
    const uint64_t asleepMS = suspendedAt ? switch_sleep_ns_since(suspendedAt) / 1000000 : 0;
    switch_sleep_trail(SWITCH_STEP_WAKE);
    switch_power_trail_watch(kActionWatchSeconds, "wake action");
#if DEVELOPMENT || DEBUG
    disarmBackstop();
#endif
    {
        switch_sleep_trail(SWITCH_STEP_WAKE_I2C);
        const uint64_t at = mach_absolute_time();
        tegra_i2c_resume();
        switch_power_journal("SwitchSuspend: wake i2c: %llu us", switch_sleep_ns_since(at) / 1000);
    }
    switch_battery_gauge gaugeAfter = {};
#if DEVELOPMENT || DEBUG
    const int gaugeAfterErr = switch_battery_gauge_sample(&gaugeAfter);
#else
    const int gaugeAfterErr = -1;
#endif
    const uint64_t sc7MS = rtcUsable ? switch_sleep_elapsed_ms(rtcBeforeMS, rtcAfterMS) : 0;
    switch_power_journal("SwitchSuspend: wake %u: asleep %llu.%03llu s (Tegra RTC, quiesce to activate); entry to wake "
                         "%llu ms by the timebase; quiesce %llu us, activate %llu us (interrupts off)",
                         sleeps + 1, sc7MS / 1000, sc7MS % 1000, asleepMS, quiesceNS / 1000, activateNS / 1000);
    switch_power_journal("SwitchSuspend: wake %u: TIMERUS_USEC_CFG %#x after SC7, %#x restored", sleeps + 1, usecAfter,
                         usecConfig);
    if (!gaugeBeforeErr && !gaugeAfterErr) {
        switch_power_journal(
            "SwitchSuspend: wake %u battery: %d mA average asleep (QH %u -> %u, %u uAh a count), "
            "AvgCurrent %d mA, Current %d mA at wake",
            sleeps + 1, switch_sleep_average_ma(gaugeBefore.qh, gaugeAfter.qh, SWITCH_BATTERY_QH_UAH, sc7MS),
            gaugeBefore.qh, gaugeAfter.qh, SWITCH_BATTERY_QH_UAH, gaugeAfter.avg_ma, gaugeAfter.now_ma);
    } else {
        switch_power_journal("SwitchSuspend: wake %u battery: gauge not read (I2C %d before, %d after)", sleeps + 1,
                             gaugeBeforeErr, gaugeAfterErr);
    }
    {
        char line[160] = "";
        switch_sleep_mark(SWITCH_STEP_WAKE_BPMP);
        const uint64_t at = mach_absolute_time();
        (void)tegra_bpmp_sc7_resume(line, sizeof(line));
        switch_power_journal("SwitchSuspend: wake bpmp: %llu us: %s", switch_sleep_ns_since(at) / 1000, line);
    }
    bool alarm = false;
    {
        switch_sleep_mark(SWITCH_STEP_WAKE_RTC);
        const uint64_t at = mach_absolute_time();
        alarm = switch_rtc_resume() == 1;
        switch_power_journal("SwitchSuspend: wake rtc: %llu us%s", switch_sleep_ns_since(at) / 1000,
                             alarm ? " (ALARM1)" : "");
    }
    {
        switch_sleep_mark(SWITCH_STEP_WAKE_SMMU);
        const uint64_t at = mach_absolute_time();
        const int lost = tegra_smmu_resume();  // before any translated client (host1x engines, display) runs
        switch_power_journal("SwitchSuspend: wake smmu: %llu us%s", switch_sleep_ns_since(at) / 1000,
                             lost ? " (a swgroup was not restored)" : "");
    }
    switch_wake_devices();
    ++sleeps;
    const uint32_t status[2] = {wakeStatus, wake2Status};
    const char *reason = alarm ? "rtc" : switch_wake_pmc_reason(status, wakeArmed);
    IOPMrootDomain *root = getPMRootDomain();
    if (root)
        root->claimSystemWakeEvent(this, kIOPMWakeEventSource, reason, nullptr);
    // A key press on a USB keyboard (or a click) woke the console through the PMC's sleepwalk; the press itself went
    // with the host controller's state in SC7, so the console's Home clicks for it, as a key press with the panel off
    // does on an iPad (the lock screen).
    if (!strcmp(reason, "usb"))
        switch_buttons_bt_click(SWITCH_BT_HOME);
    if (!strcmp(reason, "bt")) {
        // The page that woke the console is a controller reconnecting: keep the system up while it does, and let its
        // first report light the panel (SwitchWake).
        switch_wake_bt_slept();
        if (wakeHoldBegin)
            thread_call_enter(wakeHoldBegin);  // after IOCPUSleepKernel has let the root domain go
        uint64_t deadline;
        clock_interval_to_deadline(SWITCH_BT_RECONNECT_MS, kMillisecondScale, &deadline);
        if (wakeHoldEnd)
            thread_call_enter_delayed(wakeHoldEnd, deadline);
    }
    const uint64_t us = switch_sleep_ns_since(start) / 1000;
    switch_power_event("SwitchSuspend: wake %u: %s (PMC WAKE_STATUS %#x WAKE2_STATUS %#x), devices %llu.%03llu ms",
                       sleeps, reason, wakeStatus, wake2Status, us / 1000, us % 1000);
    switch_sleep_mark(SWITCH_STEP_AWAKE);
    switch_power_trail_watch(0, "awake");
    switch_power_journal_flush(false);
    if (!strcmp(reason, "unknown")) {
        IOLog("SwitchSuspend: wake %u without an armed PMC wake event (PMC WAKE_STATUS %#x WAKE2_STATUS %#x, armed "
              "%#llx)\n",
              sleeps, wakeStatus, wake2Status, (unsigned long long)wakeArmed);
    }
}

void SwitchSuspend::beginWakeHold(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<SwitchSuspend *>(owner);
    IOPMrootDomain *root = getPMRootDomain();
    if (!root || __atomic_load_n(&self->wakeHold, __ATOMIC_ACQUIRE))
        return;
    const IOPMDriverAssertionID id = root->createPMAssertion(kIOPMDriverAssertionCPUBit, kIOPMDriverAssertionLevelOn,
                                                             self, "SwitchSuspend Bluetooth wake");
    IOPMDriverAssertionID none = 0;
    if (id && !__atomic_compare_exchange_n(&self->wakeHold, &none, id, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        root->releasePMAssertion(id);
}

void SwitchSuspend::endWakeHold(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<SwitchSuspend *>(owner);
    IOPMrootDomain *root = getPMRootDomain();
    const IOPMDriverAssertionID id = __atomic_exchange_n(&self->wakeHold, 0, __ATOMIC_ACQ_REL);
    if (root && id)
        root->releasePMAssertion(id);
}

extern "C" int switch_suspend_start(void)
{
    static SwitchSuspend *suspend;
    if (suspend)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchSuspend *candidate = OSTypeAlloc(SwitchSuspend);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        OSSafeReleaseNULL(candidate);
        if (resources)
            (void)switch_suspend_hold(resources, "SwitchSuspend could not attach");
        return 0;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        (void)switch_suspend_hold(resources, "SwitchSuspend did not start");
        return 0;
    }
    suspend = candidate;  // the attachment keeps it
    candidate->release();
    return 0;
}
