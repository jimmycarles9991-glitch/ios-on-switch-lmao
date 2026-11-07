// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD: the BM92T36 USB-PD controller's one owner and the port's power role; see SwitchPD.h. The decisions are
// SwitchPDPolicy.h; this file does what they say.
//
// Registers, bits and the source bring-up: CTCaer switch-l4t-kernel-4.9 drivers/misc/bm92txx.c (bm92t_probe,
// bm92t_extcon_cable_set_init_state, bm92t_enable_ocp, bm92t_set_source_mode, bm92t_event_handler's OTG branch).
// Board (OLED DTB bm92t@18; L4T nx-platform tegra210-nx-power-fixed.dtsi, tegra210b01-fric-power-tree.dtsi): ALERT
// PK4 level low; VCONN enable PK5 active high; v_vdd5v3 ("VBUS power", the controller's vbus-source-supply, 5.3 V,
// 10 ms ramp) enable PX3 active high, switched onto the connector by the controller's SPDSRC; the BQ24193 boost
// (vbus-supply usb0_vbus) is turned on as Linux does, but on Mariko boards it is not what powers the port.
// The USB-PD request (SwitchPD.h): bm92txx.c bm92t_event_handler's INIT_STATE/NEW_PDO/PS_RDY_SENT steps and
// bm92t_power_work, bm92t_check_pdo/bm92t_send_rdo through SwitchBattery/SwitchChargePolicy.h; its msleep()s are
// one-shot timers here; DR swap, VDMs and the dock's modes are SwitchPDAltMode.cpp's (SwitchPDAltModeHook.h).
#include "SwitchPD.h"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <os/log.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../SwitchBattery/SwitchChargePolicy.h"
#include "../SwitchDisplayPort/SwitchPDAltModeHook.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "SwitchPDPolicy.h"

namespace {
constexpr unsigned kBus = 1;
constexpr uint8_t kPD = 0x18;
constexpr unsigned kVconnGpio = TEGRA_GPIO(K, 5), kSourceRailGpio = TEGRA_GPIO(X, 3);
// The BQ24193's PSEL and OTG inputs (L4T tegra210b01-fric-gpio.dtsi "Charger IC (BQ24193) Charge Select 1/2", held
// low; Atmosphere's OtgFet1/2ForSdev): the boost runs only with OTG high (datasheet 8.3.1.5). Read for the report.
[[maybe_unused]] constexpr unsigned kChargeSelect1Gpio = TEGRA_GPIO(J, 5), kChargeSelect2Gpio = TEGRA_GPIO(L, 0);
constexpr unsigned kMaxListeners = 4;
constexpr unsigned kAlertBurst = 50;  // ALERTs in one second that mean the line is stuck low
constexpr uint32_t kRailRampMS = 10;  // v_vdd5v3 regulator-enable-ramp-delay (10000 us)
// Classify window: a dual-role partner that loses our Rp sees the port open (tPDDebounce 20 ms), toggles to source
// (tDRP 100 ms), debounces our Rd (tCCDebounce 200 ms) and raises VBUS (tVBUSON 275 ms): under 600 ms by the Type-C
// spec. Re-attach window: our own toggle and debounce of a sink-only partner, about 300 ms (r7w: 53 ms).
constexpr uint32_t kClassifyMS = 1000, kReattachMS = 1000;
// USB-PD request: source PDOs after a contract alert without ALERT_PDO (bm92t_event_handler's msleep(500)), a reply to
// a command (the chip answers in tens of ms; the old in-tree port's kReplyMS), the input limit 2 s after PS_RDY
// (bm92t_power_work), failed requests per session.
constexpr uint32_t kPdoWaitMS = 500, kReplyMS = 1500, kPowerDelayMS = 2000;
constexpr unsigned kMaxFailedRequests = 3;

enum : uint8_t {
    ALERT_STATUS = 0x02,  // clears on read; reading it releases ALERT
    STATUS1 = 0x03,
    STATUS2 = 0x04,
    COMMAND = 0x05,
    CONFIG1 = 0x06,
    READ_PDOS_SRC = 0x08,  // count byte (4 per PDO), up to 7 PDOs
    VENDOR_CONFIG = 0x1a,
    CURRENT_PDO = 0x28,  // count byte, PDO
    CURRENT_RDO = 0x2b,  // count byte, RDO
    SET_RDO = 0x30,      // count byte, RDO
    FW_TYPE = 0x4b,
    FW_REVISION = 0x4c,
};

enum : uint16_t {
    CONFIG1_SPDSRC_SHIFT = 14,
    CONFIG1_SPDSRC_MASK = 3u << 14,
    SPDSRC12_ON = 0,
    SPDSRC12_OFF = 3,
    VENDOR_CONFIG_OCP_DISABLE = 1u << 2,
    ALERT_CMD_DONE = 1u << 2,
    ALERT_PDO = 1u << 14,
    STATUS1_LASTCMD_SHIFT = 4,  // 0 complete, 2 aborted, 4 invalid, 6 rejected, 7 terminated
    STATUS1_LASTCMD_MASK = 7u << 4,
    LASTCMD_ABORTED = 2,
    SEND_RDO_CMD = 0x0707,
    PS_RDY_CMD = 0x0505,
};

struct Listener {
    void (*function)(void *, const SwitchPDState *);
    void *context;
};

int read16(uint8_t reg, uint16_t &value)
{
    uint8_t b[2] = {};
    int err = tegra_i2c_read(kBus, kPD, reg, b, 2);
    value = (uint16_t)(b[0] | b[1] << 8);
    return err;
}

int write16(uint8_t reg, uint16_t value)
{
    const uint8_t b[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return tegra_i2c_write(kBus, kPD, reg, b, 2);
}

// The USB-PD request within a sink session (SwitchPD.h).
enum class Nego : uint8_t {
    Idle,       // waiting for the controller's own contract
    WaitPdos,   // a contract alert without the source's PDOs yet: kPdoWaitMS
    RdoSent,    // SEND_RDO: waiting for the contract (kReplyMS)
    PsRdySent,  // contract verified, PS_RDY: waiting for it to complete (kReplyMS)
    Settling,   // PS_RDY done: kPowerDelayMS before publishing
    Done,       // published
    NotPd,      // no source capabilities: not a USB-PD source
    Failed,     // one line said why; the controller's own contract stays, nothing published
};

uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

// "15", "1.2", "2.25": milli-units as units without trailing zeros.
void formatMilli(char *out, size_t size, unsigned milli)
{
    const unsigned whole = milli / 1000, frac = milli % 1000;
    if (!frac) {
        snprintf(out, size, "%u", whole);
    } else if (frac % 100 == 0) {
        snprintf(out, size, "%u.%u", whole, frac / 100);
    } else if (frac % 10 == 0) {
        snprintf(out, size, "%u.%02u", whole, frac / 10);
    } else {
        snprintf(out, size, "%u.%03u", whole, frac);
    }
}

// "5 V 1.5 A, *15 V 2.6 A" (* = position `chosen`); other PDO types by name.
void formatPdos(char *out, size_t size, const uint32_t *pdos, unsigned count, unsigned chosen)
{
    size_t used = 0;
    out[0] = 0;
    for (unsigned i = 0; i < count && used + 1 < size; ++i) {
        char volts[12], amps[12];
        formatMilli(volts, sizeof(volts), charge::pdoMillivolts(pdos[i]));
        formatMilli(amps, sizeof(amps), charge::pdoMilliamps(pdos[i]));
        static const char *const kTypes[] = {"", "battery ", "variable ", "augmented "};
        const int n = charge::pdoType(pdos[i]) == 3
                          ? snprintf(out + used, size - used, "%saugmented", i ? ", " : "")
                          : snprintf(out + used, size - used, "%s%s%s%s V %s A", i ? ", " : "",
                                     i + 1 == chosen ? "*" : "", kTypes[charge::pdoType(pdos[i])], volts, amps);
        if (n < 0)
            break;
        used += (size_t)n < size - used ? (size_t)n : size - used - 1;
    }
}

// A pin for the report: "sfio" (special function), "in 0/1", "out 0/1".
[[maybe_unused]] const char *pinText(unsigned gpio)
{
    const int config = tegra_gpio_config(gpio);
    if (config < 0)
        return "?";
    if (!(config & 1))
        return "sfio";
    if (config & 2)
        return (config & 4) ? "out 1" : "out 0";
    return (config & 8) ? "in 1" : "in 0";
}
}  // namespace

class SwitchPD : public IOService {
    OSDeclareDefaultStructors(SwitchPD);

    IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *alert = nullptr;
    IOTimerEventSource *window = nullptr, *negoTimer = nullptr;
    Listener listeners[kMaxListeners] = {};
    unsigned listenerCount = 0;
    SwitchPDState state = {};
    switch_pd_phase phase = SWITCH_PD_EMPTY;
    bool allowSource = false, switchesOn = false, railsOn = false, sawDetach = false, stuck = false;
    bool reportedReadFailure = false, reportedSwitchFailure = false;
    bool asleepSinkOnly = false;  // systemSleep took the empty port's source switches off for this SC7
    unsigned alertCount = 0;
    uint64_t alertWindow = 0;
    // The USB-PD request (SwitchPD.h).
    Nego nego = Nego::Idle;
    charge::PdChoice choice = {};
    uint32_t sourcePdos[7] = {};
    unsigned sourcePdoCount = 0, failedRequests = 0;

    bool readStatus(uint16_t &status1, uint16_t &status2);
    void handle(uint16_t alertBits, uint16_t status1, uint16_t status2);
    void apply(const switch_pd_step &step);
    void enter(switch_pd_phase next, const char *why);
    void setSwitches(bool on);
    bool setRails(bool on);
    void startSourcing();
    void endSourcing(switch_pd_phase next, const char *why);
    void notify();
    void stopEvents();
    void negotiate(uint16_t alertBits);
    void choose();
    void verify();
    void publish();
    void withdraw();
    void fail(const char *why);
    void endNegotiation();
    bool readContract(uint32_t &pdo, uint32_t &rdo);
    static void alertFired(OSObject *owner, IOInterruptEventSource *, int);
    static void windowFired(OSObject *owner, IOTimerEventSource *);
    static void negoFired(OSObject *owner, IOTimerEventSource *);

public:
    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    bool begin(uint16_t &type, uint16_t &revision, uint16_t &config, bool &turnedOff);
    IOReturn addListener(Listener listener);
    void setAllowSource(bool allow);
    void systemSleep(bool entering);
    void describe(char *buffer, size_t size);
};

OSDefineMetaClassAndStructors(SwitchPD, IOService);

namespace {
SwitchPD *gPD;
uint32_t gPDStage;  // 0 never asked, 1 starting, 2 running, 3 absent
IOLock *gPDLock;

IOLock *pdLock()
{
    IOLock *lock = __atomic_load_n(&gPDLock, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *fresh = IOLockAlloc(), *none = nullptr;
    if (!fresh)
        return nullptr;
    if (__atomic_compare_exchange_n(&gPDLock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    IOLockFree(fresh);
    return none;
}
}  // namespace

// Before the interrupt: the source switches off whatever an earlier boot left (r7f left them on; the controller keeps
// CONFIG1 across reboots) and the source rails off, the latched ALERT released, the first state decided, then
// interrupts on.
bool SwitchPD::begin(uint16_t &type, uint16_t &revision, uint16_t &config, bool &turnedOff)
{
    int err = tegra_i2c_init(kBus);
    if (!err)
        err = read16(FW_TYPE, type);
    if (!err)
        err = read16(FW_REVISION, revision);
    if (err || revision <= 0x644) {  // bm92t_probe: older firmware is not supported
        IOLog("SwitchPD: BM92T36 not usable (I2C %d, firmware type %#x revision %#x)\n", err, type, revision);
        return false;
    }
    if (!read16(CONFIG1, config)) {
        switchesOn = ((config & CONFIG1_SPDSRC_MASK) >> CONFIG1_SPDSRC_SHIFT) != SPDSRC12_OFF;
        const bool wereOn = switchesOn;
        setSwitches(false);
        turnedOff = wereOn && !switchesOn;
    }
    railsOn = true;  // whatever the boot chain left: driven off
    (void)setRails(false);
    loop = IOWorkLoop::workLoop();
    window = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::windowFired) : nullptr;
    negoTimer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::negoFired) : nullptr;
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_PD_ALERT, TEGRA_GPIO_LEVEL_LOW);
    alert = nub && loop ? IOInterruptEventSource::interruptEventSource(this, &SwitchPD::alertFired, nub, 0) : nullptr;
    if (!window || loop->addEventSource(window) != kIOReturnSuccess || !negoTimer ||
        loop->addEventSource(negoTimer) != kIOReturnSuccess || !alert ||
        loop->addEventSource(alert) != kIOReturnSuccess) {
        panic("SwitchPD: cannot attach the BM92T36 ALERT (PK4, GPIO %u, level low)", TEGRA_GPIO_PD_ALERT);
    }
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchPD *>(owner);
            uint16_t alertBits = 0, status1 = 0, status2 = 0;
            (void)read16(ALERT_STATUS, alertBits);  // releases a latched ALERT; its event is the state read next
            if (self->readStatus(status1, status2))
                self->handle(0, status1, status2);
            return kIOReturnSuccess;
        },
        this);
    alert->enable();
    return true;
}

bool SwitchPD::readStatus(uint16_t &status1, uint16_t &status2)
{
    int err = read16(STATUS1, status1);
    if (!err)
        err = read16(STATUS2, status2);
    if (err && !reportedReadFailure)
        IOLog("SwitchPD: status read failed (I2C %d)\n", err);
    reportedReadFailure = err != 0;
    return !err;
}

// Work loop: an ALERT (the pin is masked while this runs and unmasked after, TegraGPIO level pins, so the
// ALERT_STATUS read here is what keeps it from firing again), a classify window's end, or a listener's first call.
void SwitchPD::handle(uint16_t alertBits, uint16_t status1, uint16_t status2)
{
    state.alert = alertBits;
    state.status1 = status1;
    state.status2 = status2;
    state.plugged = status1 & SWITCH_PD_STATUS1_INSERT;
    state.vbusPresent = status1 & SWITCH_PD_STATUS1_VSAFE;
    state.dataHost = state.plugged && ((status1 >> SWITCH_PD_STATUS1_DR_SHIFT) & 3) == SWITCH_PD_DATA_ROLE_DFP;
    apply(switch_pd_decide(phase, alertBits, status1, status2, allowSource));
    negotiate(alertBits);
    const auto atRest = [](const void *pd) -> bool {
        const Nego n = static_cast<const SwitchPD *>(pd)->nego;
        return n == Nego::Settling || n == Nego::Done || n == Nego::Failed;
    };
    const bool contractOurs = nego == Nego::Settling || nego == Nego::Done;
    switch_pd_alt_alert(this, loop, atRest, alertBits, status1,
                        phase == SWITCH_PD_SINK && state.plugged && !(status1 & SWITCH_PD_STATUS1_SRC_MODE),
                        contractOurs && choice.dock);
    notify();
}

void SwitchPD::notify()
{
    state.role = phase == SWITCH_PD_EMPTY    ? SWITCH_PD_ROLE_NONE
                 : phase == SWITCH_PD_SOURCE ? SWITCH_PD_ROLE_SOURCE
                                             : SWITCH_PD_ROLE_SINK;
    for (unsigned i = 0; i < listenerCount; ++i)
        listeners[i].function(listeners[i].context, &state);
}

void SwitchPD::apply(const switch_pd_step &step)
{
    if (step.cancel_window)
        window->cancelTimeout();
    switch (step.act) {
    case SWITCH_PD_NONE:
        break;
    case SWITCH_PD_SAW_DETACH:
        sawDetach = true;
        break;
    case SWITCH_PD_ENTER:
        enter(step.next, step.why);
        break;
    case SWITCH_PD_START:
        startSourcing();
        break;
    case SWITCH_PD_END:
        endSourcing(step.next, step.why);
        break;
    }
}

// A phase's outputs (SwitchPDPolicy.h): switches off first when they go off, the rails, then the switches on, so the
// controller never closes a switch onto a rail that is still ramping. A rail that cannot come up holds the port.
void SwitchPD::enter(switch_pd_phase next, const char *why)
{
    phase = next;
    sawDetach = false;
    const bool switches = switch_pd_switches_on(next, allowSource), rails = switch_pd_rails_on(next);
    if (!switches)
        setSwitches(false);
    const bool railsDone = setRails(rails);
    IOLog("SwitchPD: %s (alert %#x status1 %#x status2 %#x)\n", why, state.alert, state.status1, state.status2);
    if (rails && !railsDone) {
        enter(SWITCH_PD_HELD, "not sourcing: over-current protection or the source rails failed");
        return;
    }
    if (switches)
        setSwitches(true);
    if (next == SWITCH_PD_CLASSIFY)
        window->setTimeoutMS(kClassifyMS);
    if (next == SWITCH_PD_REATTACH)
        window->setTimeoutMS(kReattachMS);
}

// CONFIG1 SPDSRC: both source switches on (SPDSRC12_ON, bm92t_set_source_mode) or off (SPDSRC12_OFF).
void SwitchPD::setSwitches(bool on)
{
    if (on == switchesOn)
        return;
    uint16_t config = 0;
    int err = read16(CONFIG1, config);
    if (!err) {
        config =
            (uint16_t)((config & ~CONFIG1_SPDSRC_MASK) | (on ? SPDSRC12_ON : SPDSRC12_OFF) << CONFIG1_SPDSRC_SHIFT);
        err = write16(CONFIG1, config);
    }
    if (!err) {
        switchesOn = on;
    } else if (!reportedSwitchFailure) {
        IOLog("SwitchPD: source switches %s failed (I2C %d)\n", on ? "on" : "off", err);
    }
    reportedSwitchFailure = err != 0;
}

// The source rails, bm92t_probe + bm92t_extcon_cable_set_init_state order: over-current protection on, v_vdd5v3,
// VCONN, the ramp. Never half on: a failure turns both GPIOs off again. True when the rails are as asked.
bool SwitchPD::setRails(bool on)
{
    if (on == railsOn)
        return true;
    int err = 0;
    if (on) {
        uint16_t vendor = 0;
        err = read16(VENDOR_CONFIG, vendor);
        if (!err && (vendor & VENDOR_CONFIG_OCP_DISABLE))
            err = write16(VENDOR_CONFIG, vendor & ~VENDOR_CONFIG_OCP_DISABLE);
        if (!err)
            err = tegra_gpio_output(kSourceRailGpio, 1);
        if (!err)
            err = tegra_gpio_output(kVconnGpio, 1);
        if (!err) {
            IOSleep(kRailRampMS);
        } else {
            (void)tegra_gpio_output(kVconnGpio, 0);
            (void)tegra_gpio_output(kSourceRailGpio, 0);
        }
        railsOn = !err;
    } else {
        err = tegra_gpio_output(kVconnGpio, 0);
        if (const int rail = tegra_gpio_output(kSourceRailGpio, 0))
            err = rail;
        railsOn = err != 0;
    }
    if (err)
        IOLog("SwitchPD: source rails %s failed (%d)\n", on ? "on" : "off", err);
    return !err;
}

// Reattach: the partner proved sink-only and is back as our sink; the rails were up before it came, so our switch
// may already be closed (VSAFE ours). bm92t_event_handler's OTG branch: the boost (vbus-supply) on.
void SwitchPD::startSourcing()
{
    phase = SWITCH_PD_SOURCE;
    const bool ours = switch_pd_vbus_ours(state.status1);
    const int boost = switch_battery_set_otg_boost(true, ours);
    if (boost == SWITCH_BATTERY_BOOST_VBUS_PRESENT) {
        endSourcing(SWITCH_PD_SINK, "not sourcing: the charger sees VBUS that is not ours (sink until unplugged)");
    } else if (boost) {
        endSourcing(SWITCH_PD_HELD, "not sourcing: the BQ24193 boost failed");
    } else {
        IOLog("SwitchPD: sink-only partner: sourcing VBUS (v_vdd5v3, VCONN, OCP up before it came back; %s; boost "
              "on) (alert %#x status1 %#x status2 %#x)\n",
              ours ? "our switch closed" : "our switch closing", state.alert, state.status1, state.status2);
    }
}

// The one way out of Source, for a detach and for every error: the boost off, then the phase's outputs (rails off,
// switches as `next` has them: the empty-port state, or off for a sink session).
void SwitchPD::endSourcing(switch_pd_phase next, const char *why)
{
    (void)switch_battery_set_otg_boost(false, false);
    enter(next, why);
}

void SwitchPD::windowFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchPD *>(owner);
    uint16_t status1 = 0, status2 = 0;
    if (!self->readStatus(status1, status2))
        return;
    const switch_pd_step step = switch_pd_window_end(self->phase, self->sawDetach, status1);
    if (step.act == SWITCH_PD_NONE)
        return;
    self->state.alert = 0;
    self->state.status1 = status1;
    self->state.status2 = status2;
    self->apply(step);
    self->handle(0, status1, status2);  // the state as it is now, through the new phase
}

void SwitchPD::alertFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchPD *>(owner);
    const uint64_t now = mach_absolute_time();
    uint64_t elapsedNS = 0;
    absolutetime_to_nanoseconds(now - self->alertWindow, &elapsedNS);
    if (elapsedNS > 1000000000ull) {
        self->alertWindow = now;
        self->alertCount = 0;
    }
    if (++self->alertCount > kAlertBurst) {
        self->stopEvents();
        return;
    }
    uint16_t alertBits = 0, status1 = 0, status2 = 0;
    int err = read16(ALERT_STATUS, alertBits);
    if (err) {
        if (!self->reportedReadFailure)
            IOLog("SwitchPD: status read failed (I2C %d)\n", err);
        self->reportedReadFailure = true;
        return;
    }
    if (self->readStatus(status1, status2))
        self->handle(alertBits, status1, status2);
}

// ALERT stuck low (re-fires as fast as the work loop reads it): no storm. The pin is masked for the rest of the boot
// with one line, VBUS is never driven again (sourcing ends, switches and rails off) and the port stays sink-only; the
// charger, which does not need the PD controller to charge, and the battery's own slow re-read carry on.
void SwitchPD::stopEvents()
{
    char pin[256];
    tegra_gpio_describe(TEGRA_GPIO_PD_ALERT, pin, sizeof(pin));
    alert->disable();
    stuck = true;
    allowSource = false;
    window->cancelTimeout();
    endNegotiation();  // no events: no contract can be followed, so none stays published
    IOLog("SwitchPD: BM92T36 ALERT (PK4) fired %u times in 1 s, turned off (port sink-only until reboot): %s\n",
          alertCount, pin);
    if (phase == SWITCH_PD_SOURCE) {
        endSourcing(SWITCH_PD_SINK, "not sourcing: no PD events");
    } else {
        enter(SWITCH_PD_SINK, "sink: no PD events");
    }
    notify();
}

IOReturn SwitchPD::addListener(Listener listener)
{
    if (listenerCount == kMaxListeners)
        return kIOReturnNoResources;
    listeners[listenerCount++] = listener;
    listener.function(listener.context, &state);  // the state as it is now
    return kIOReturnSuccess;
}

void SwitchPD::setAllowSource(bool allow)
{
    if (allow == allowSource || stuck)
        return;
    allowSource = allow;
    if (!allow && phase == SWITCH_PD_SOURCE) {
        endSourcing(SWITCH_PD_HELD, "not sourcing: host mode is off");
        notify();
    } else if (!allow && (phase == SWITCH_PD_CLASSIFY || phase == SWITCH_PD_REATTACH)) {
        window->cancelTimeout();
        enter(SWITCH_PD_SINK, "sink: host mode is off");
        notify();
    } else if (phase == SWITCH_PD_EMPTY || phase == SWITCH_PD_HELD) {
        setSwitches(allow);
    }
}

// SC7 (switch_usb_suspend/resume). An empty port goes sink-only for the sleep, as a port that only takes power while
// asleep: a sink (a keyboard, a trackpad) then meets no source, nothing attaches and nothing raises ALERT, so it does
// not wake the console; a charger, a dock or a display that powers the port is a source, attaches, and its ALERT wakes
// it (SwitchSuspend's USB-PD wake event). A partner already on the port keeps what it has: a keyboard we source stays
// powered so that its key press can wake the console. After the wake the switches follow the phase again.
void SwitchPD::systemSleep(bool entering)
{
    if (entering) {
        asleepSinkOnly = phase == SWITCH_PD_EMPTY && switchesOn;
        if (asleepSinkOnly)
            setSwitches(false);
    } else if (asleepSinkOnly) {
        asleepSinkOnly = false;
        setSwitches(switch_pd_switches_on(phase, allowSource));
    }
}

#if DEVELOPMENT || DEBUG
// Work loop. The status registers are not clear-on-read (ALERT_STATUS, which is, stays alertFired's).
void SwitchPD::describe(char *buffer, size_t size)
{
    uint16_t status1 = 0, status2 = 0, config = 0, vendor = 0;
    int err = read16(STATUS1, status1);
    if (!err)
        err = read16(STATUS2, status2);
    if (!err)
        err = read16(CONFIG1, config);
    if (!err)
        err = read16(VENDOR_CONFIG, vendor);
    snprintf(
        buffer, size,
        "BM92T36 %s%s: STATUS1 %#x (switch1 %u switch2 %u VSAFE %u) STATUS2 %#x CONFIG1 %#x VENDOR %#x (OCP %s)%s; "
        "rails %s (v_vdd5v3 PX3 %s, VCONN PK5 %s); PSEL/OTG PJ5 %s PL0 %s",
        switch_pd_phase_name(phase), allowSource ? "" : " (host mode off)", status1,
        !!(status1 & SWITCH_PD_STATUS1_SPDSRC1), !!(status1 & SWITCH_PD_STATUS1_SPDSRC2),
        !!(status1 & SWITCH_PD_STATUS1_VSAFE), status2, config, vendor,
        (vendor & VENDOR_CONFIG_OCP_DISABLE) ? "off" : "on", err ? " (I2C error)" : "", railsOn ? "on" : "off",
        pinText(kSourceRailGpio), pinText(kVconnGpio), pinText(kChargeSelect1Gpio), pinText(kChargeSelect2Gpio));
}
#endif

// --- USB-PD request (SwitchPD.h) ---------------------------------------------------------------------------------
// From handle(), after decide(): only a sink session with a source partner negotiates; anything else ends it.
void SwitchPD::negotiate(uint16_t alertBits)
{
    const bool sinkSession = phase == SWITCH_PD_SINK && state.plugged && !(state.status1 & SWITCH_PD_STATUS1_SRC_MODE);
    if (!sinkSession) {
        if (nego != Nego::Idle || state.pdSource || state.contractMillivolts)
            endNegotiation();
        return;
    }
    if (alertBits & (SWITCH_PD_ALERT_CONTRACT | ALERT_PDO))
        state.pdSource = true;
    switch (nego) {
    case Nego::Idle:
    case Nego::NotPd:
        if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            if (alertBits & ALERT_PDO) {
                choose();
            } else {  // bm92t_event_handler: "In case of no new PDO, wait for it"
                nego = Nego::WaitPdos;
                negoTimer->setTimeoutMS(kPdoWaitMS);
            }
        } else if (nego == Nego::Idle && (state.status1 & SWITCH_PD_STATUS1_SPDSNK)) {
            choose();  // already sinking on a contract made before this boot (bm92txx first_init)
        }
        break;
    case Nego::WaitPdos:
        if (alertBits & ALERT_PDO)
            choose();
        break;
    case Nego::RdoSent:
        if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            verify();
        } else if ((alertBits & ALERT_CMD_DONE) && (state.status1 & STATUS1_LASTCMD_MASK)) {
            const unsigned last = (state.status1 & STATUS1_LASTCMD_MASK) >> STATUS1_LASTCMD_SHIFT;
            if (last == LASTCMD_ABORTED && ++failedRequests < kMaxFailedRequests) {
                // r8a: a MacBook's port aborts the first request at attach (the controller still busy with the
                // partner's own messages) and takes the same one a moment later: once more, kPdoWaitMS on.
                os_log(OS_LOG_DEFAULT, "SwitchPD: request aborted at attach (last command status 2): again in %u ms",
                       kPdoWaitMS);
                nego = Nego::WaitPdos;
                negoTimer->setTimeoutMS(kPdoWaitMS);
                break;
            }
            char why[64];
            snprintf(why, sizeof(why), "the request was refused (last command status %u)", last);
            fail(why);
        }
        break;
    case Nego::PsRdySent:
        if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            withdraw();
            choose();
        } else if (alertBits & ALERT_CMD_DONE) {
            nego = Nego::Settling;
            negoTimer->setTimeoutMS(kPowerDelayMS);
        }
        break;
    case Nego::Settling:
    case Nego::Done:
    case Nego::Failed:
        // A contract we did not ask for (a multi-port charger re-balancing, a source reset): start over.
        if (alertBits & SWITCH_PD_ALERT_CONTRACT) {
            withdraw();
            if (nego == Nego::Failed && failedRequests >= kMaxFailedRequests)
                break;
            choose();
        }
        break;
    }
}

bool SwitchPD::readContract(uint32_t &pdo, uint32_t &rdo)
{
    uint8_t p[5] = {}, r[5] = {};
    if (tegra_i2c_read(kBus, kPD, CURRENT_PDO, p, sizeof(p)) || tegra_i2c_read(kBus, kPD, CURRENT_RDO, r, sizeof(r))) {
        return false;
    }
    pdo = le32(p + 1);
    rdo = le32(r + 1);
    return true;
}

// The source's PDOs, the choice, and the request unless the contract already is the one we would ask for.
void SwitchPD::choose()
{
    negoTimer->cancelTimeout();
    uint8_t raw[29] = {};
    if (tegra_i2c_read(kBus, kPD, READ_PDOS_SRC, raw, sizeof(raw))) {
        fail("the source's PDOs could not be read");
        return;
    }
    sourcePdoCount = raw[0] / 4 > 7 ? 7 : raw[0] / 4;
    for (unsigned i = 0; i < sourcePdoCount; ++i)
        sourcePdos[i] = le32(raw + 1 + 4 * i);
    if (!sourcePdoCount) {
        if (nego != Nego::NotPd) {
            IOLog("SwitchPD: no USB-PD source capabilities: the charger keeps its own input detection (status1 %#x)\n",
                  state.status1);
        }
        nego = Nego::NotPd;
        return;
    }
    state.pdSource = true;
    choice = charge::choosePdo(sourcePdos, sourcePdoCount);
    if (!choice.position) {
        fail(choice.refusal);
        return;
    }
    uint32_t pdo = 0, rdo = 0;
    if (readContract(pdo, rdo) && charge::contractMatches(choice, pdo, rdo)) {
        publish();  // ours already (made by an earlier boot): nothing to request
        return;
    }
    const uint32_t request = charge::encodeRdo(choice);
    const uint8_t message[5] = {4, (uint8_t)request, (uint8_t)(request >> 8), (uint8_t)(request >> 16),
                                (uint8_t)(request >> 24)};
    if (tegra_i2c_write(kBus, kPD, SET_RDO, message, sizeof(message)) || write16(COMMAND, SEND_RDO_CMD)) {
        fail("the request could not be written");
        return;
    }
    nego = Nego::RdoSent;
    negoTimer->setTimeoutMS(kReplyMS);
}

// ALERT_CONTRACT after our request: it must be the one asked for; then PS_RDY (bm92t_event_handler NEW_PDO).
void SwitchPD::verify()
{
    negoTimer->cancelTimeout();
    uint32_t pdo = 0, rdo = 0;
    if (!readContract(pdo, rdo)) {
        fail("the new contract could not be read");
        return;
    }
    if (!charge::contractMatches(choice, pdo, rdo)) {
        char why[96];
        snprintf(why, sizeof(why), "the new contract is not the one requested (PDO %#x RDO %#x, asked RDO %#x)", pdo,
                 rdo, charge::encodeRdo(choice));
        fail(why);
        return;
    }
    if (write16(COMMAND, PS_RDY_CMD)) {
        fail("PS_RDY could not be written");
        return;
    }
    nego = Nego::PsRdySent;
    negoTimer->setTimeoutMS(kReplyMS);
}

// The contract becomes the input limit's (SwitchBattery, through notify()): one line.
void SwitchPD::publish()
{
    negoTimer->cancelTimeout();
    nego = Nego::Done;
    failedRequests = 0;
    state.contractMillivolts = (uint16_t)choice.millivolts;
    state.inputLimitMilliamps = (uint16_t)choice.inputLimit;
    char volts[12], amps[12], pdos[192];
    formatMilli(volts, sizeof(volts), choice.millivolts);
    formatMilli(amps, sizeof(amps), choice.inputLimit);
    formatPdos(pdos, sizeof(pdos), sourcePdos, sourcePdoCount, choice.position);
    IOLog("SwitchPD: contract %s V %s A (%s%s)\n", volts, amps, choice.dock ? "dock: " : "", pdos);
}

void SwitchPD::withdraw()
{
    state.contractMillivolts = 0;
    state.inputLimitMilliamps = 0;
}

// One line; the controller's own contract stays and nothing is published (the charger keeps its own detection).
void SwitchPD::fail(const char *why)
{
    negoTimer->cancelTimeout();
    withdraw();
    nego = Nego::Failed;
    ++failedRequests;
    char pdos[192];
    formatPdos(pdos, sizeof(pdos), sourcePdos, sourcePdoCount, 0);
    IOLog("SwitchPD: no USB-PD request: %s; the controller's own contract stays (source: %s)%s\n", why,
          sourcePdoCount ? pdos : "?",
          failedRequests >= kMaxFailedRequests ? "; no more requests until unplugged" : "");
}

void SwitchPD::endNegotiation()
{
    negoTimer->cancelTimeout();
    nego = Nego::Idle;
    failedRequests = 0;
    sourcePdoCount = 0;
    state.pdSource = false;
    withdraw();
}

void SwitchPD::negoFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchPD *>(owner);
    switch (self->nego) {
    case Nego::WaitPdos:
        self->choose();
        break;  // bm92txx goes on after its msleep(500) whatever came
    case Nego::RdoSent:
        self->fail("no contract within 1.5 s of the request");
        break;
    case Nego::PsRdySent:
        self->fail("PS_RDY was not completed within 1.5 s");
        break;
    case Nego::Settling:
        self->publish();
        break;
    default:
        return;
    }
    self->notify();
}

// ---------------------------------------------------------------------------
namespace {
SwitchPD *startPD()
{
    IOLock *lock = pdLock();
    if (!lock)
        return nullptr;
    IOLockLock(lock);
    if (gPDStage == 0) {
        gPDStage = 3;
        IOService *resources = IOService::getResourceService();
        SwitchPD *pd = OSTypeAlloc(SwitchPD);
        uint16_t type = 0, revision = 0, config = 0;
        bool turnedOff = false;
        if (resources && pd && pd->init() && pd->attach(resources)) {
            if (pd->begin(type, revision, config, turnedOff)) {
                pd->setName("SwitchPD");
                pd->registerService();
                gPD = pd;
                gPDStage = 2;
                IOLog("SwitchPD: started (BM92T36 firmware type %#x revision %#x, CONFIG1 %#x%s)\n", type, revision,
                      config, turnedOff ? ": source switches were on, now off" : "");
            }
            // A failed begin() leaves the service attached: its alert source may already be on the loop.
        } else {
            OSSafeReleaseNULL(pd);
        }
    }
    SwitchPD *pd = gPDStage == 2 ? gPD : nullptr;
    IOLockUnlock(lock);
    return pd;
}

IOReturn listenerAction(OSObject *owner, void *function, void *context, void *, void *)
{
    return static_cast<SwitchPD *>(owner)->addListener(
        Listener{reinterpret_cast<void (*)(void *, const SwitchPDState *)>(function), context});
}
}  // namespace

extern "C" int switch_pd_add_listener(void (*listener)(void *, const SwitchPDState *), void *context)
{
    SwitchPD *pd = startPD();
    if (!pd || !listener)
        return -1;
    return pd->getWorkLoop()->runAction(&listenerAction, pd, reinterpret_cast<void *>(listener), context) ==
                   kIOReturnSuccess
               ? 0
               : -1;
}

extern "C" void switch_pd_check_delivery(const char *who)
{
    if (__atomic_load_n(&gPDStage, __ATOMIC_ACQUIRE) == 2)
        tegra_gpio_check_delivery(TEGRA_GPIO_PD_ALERT, who);
}

extern "C" int switch_pd_allow_source(bool allow)
{
    SwitchPD *pd = startPD();
    if (!pd)
        return -1;
    pd->getWorkLoop()->runAction(
        [](OSObject *owner, void *allow, void *, void *, void *) -> IOReturn {
            static_cast<SwitchPD *>(owner)->setAllowSource(allow != nullptr);
            return kIOReturnSuccess;
        },
        pd, allow ? pd : nullptr);
    return 0;
}

extern "C" void switch_pd_sleep(bool entering)
{
    SwitchPD *pd = __atomic_load_n(&gPDStage, __ATOMIC_ACQUIRE) == 2 ? gPD : nullptr;
    if (!pd)
        return;
    pd->getWorkLoop()->runAction(
        [](OSObject *owner, void *down, void *, void *, void *) -> IOReturn {
            static_cast<SwitchPD *>(owner)->systemSleep(down != nullptr);
            return kIOReturnSuccess;
        },
        pd, entering ? pd : nullptr);
}

#if DEVELOPMENT || DEBUG
extern "C" int switch_pd_describe(char *buffer, size_t size)
{
    if (!buffer || !size)
        return -1;
    SwitchPD *pd = __atomic_load_n(&gPDStage, __ATOMIC_ACQUIRE) == 2 ? gPD : nullptr;
    if (!pd) {
        snprintf(buffer, size, "BM92T36 not running");
        return -1;
    }
    pd->getWorkLoop()->runAction(
        [](OSObject *owner, void *buffer, void *size, void *, void *) -> IOReturn {
            static_cast<SwitchPD *>(owner)->describe(static_cast<char *>(buffer), reinterpret_cast<size_t>(size));
            return kIOReturnSuccess;
        },
        pd, buffer, reinterpret_cast<void *>(size));
    return 0;
}
#else
// The register dump is for development only: release builds report nothing.
extern "C" int switch_pd_describe(char *buffer, size_t size)
{
    if (buffer != nullptr && size != 0) {
        buffer[0] = 0;
    }
    return 0;
}
#endif
