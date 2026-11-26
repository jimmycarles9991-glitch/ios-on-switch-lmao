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
#include "SwitchPDService.h"

using namespace switchpd;

namespace {
// A pin for the report: "sfio" (special function), "in 0/1", "out 0/1".
[[maybe_unused]] const char *pinText(unsigned gpio)
{
    const int config = tegra_gpio_config(gpio);
    if (config < 0)
        return "?";
    const auto bits = static_cast<unsigned>(config);
    if (!(bits & 1u))
        return "sfio";
    if (bits & 2u)
        return (bits & 4u) ? "out 1" : "out 0";
    return (bits & 8u) ? "in 1" : "in 0";
}
}  // namespace

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
    IOLock *fresh = IOLockAlloc();
    IOLock *none = nullptr;
    if (!fresh)
        return nullptr;
    if (__atomic_compare_exchange_n(&gPDLock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    IOLockFree(fresh);
    return none;
}
}  // namespace

// Before the interrupt: the settings as found recorded (SwitchPDSettings.cpp puts them back at Shut Down and Restart),
// the source switches off whatever an earlier boot left (r7f left them on: the controller stays powered across a
// reboot) and the source rails off, the latched ALERT released, the first state decided, then interrupts on.
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
    recordAsFound();
    uint16_t status1 = 0;
    uint16_t dpStatus = 0;
    if (!read16(STATUS1, status1) && !read16(DP_STATUS, dpStatus) && switch_pd_reset_at_start(status1, dpStatus)) {
        state.status1 = status1;
        systemReset(kStartResetMS, "found plugged as DFP, in DisplayPort mode or after a failed command");
    }
    if (const int ocp = enableOcp())
        IOLog("SwitchPD: over-current protection on failed (I2C %d)\n", ocp);
    configureRoleSwaps(true);
    if (!read16(CONFIG1, config)) {
        switchesOn = ((config & CONFIG1_SPDSRC_MASK) >> CONFIG1_SPDSRC_SHIFT) != SPDSRC12_OFF;
        const bool wereOn = switchesOn;
        setSwitches(false);
        turnedOff = wereOn && !switchesOn;
    }
    vbusRailOn = true;  // whatever the boot chain left: driven off
    vconnOn = true;
    (void)setRails(false, false);
    loop = IOWorkLoop::workLoop();
    window = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::windowFired) : nullptr;
    negoTimer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::negoFired) : nullptr;
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_PD_ALERT, TEGRA_GPIO_LEVEL_LOW);
    alert = nub && loop ? IOInterruptEventSource::interruptEventSource(this, &SwitchPD::alertFired, nub, 0) : nullptr;
    if (!loop || !window || loop->addEventSource(window) != kIOReturnSuccess || !negoTimer ||
        loop->addEventSource(negoTimer) != kIOReturnSuccess || !alert ||
        loop->addEventSource(alert) != kIOReturnSuccess) {
        panic("SwitchPD: cannot attach the BM92T36 ALERT (PK4, GPIO %u, level low)", TEGRA_GPIO_PD_ALERT);
    }
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchPD *>(owner);
            uint16_t alertBits = 0;
            uint16_t status1 = 0;
            uint16_t status2 = 0;
            (void)read16(ALERT_STATUS, alertBits);  // releases a latched ALERT; its event is the state read next
            if (self->readStatus(status1, status2))
                self->handle(alertBits, status1, status2);
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
    return err == 0;
}

// Work loop: an ALERT (the pin is masked while this runs and unmasked after, TegraGPIO level pins, so the
// ALERT_STATUS read here is what keeps it from firing again), a classify window's end, or a listener's first call.
void SwitchPD::handle(uint16_t alertBits, uint16_t status1, uint16_t status2)
{
    if (leaving)
        return;
    if (resetPending)
        return;
    const bool inserted = (status1 & SWITCH_PD_STATUS1_INSERT) != 0;
    if (state.plugged && (!inserted || (alertBits & SWITCH_PD_ALERT_PLUGPULL))) {
        endNegotiation();
        switch_pd_alt_leave();
    }
    state.alert = alertBits;
    state.status1 = status1;
    state.status2 = status2;
    state.plugged = inserted;
    state.vbusPresent = ((status1 & SWITCH_PD_STATUS1_VSAFE) != 0u);
    state.dataHost = state.plugged &&
                     ((static_cast<unsigned>(status1) >> SWITCH_PD_STATUS1_DR_SHIFT) & 3u) == SWITCH_PD_DATA_ROLE_DFP;
    // The dock can be gone and a new Rd present before one PLUGPULL is serviced. The sink phase had its switches off.
    if (phase == SWITCH_PD_SINK && allowSource && !asleepSinkOnly && (alertBits & SWITCH_PD_ALERT_PLUGPULL) &&
        state.plugged && !state.vbusPresent && (status1 & SWITCH_PD_STATUS1_SRC_MODE) &&
        (status2 & SWITCH_PD_STATUS2_OTG_INSERT) && !(status1 & SWITCH_PD_STATUS1_FAULT_MASK) &&
        !(alertBits & SWITCH_PD_ALERT_SRC_FAULT))
        enter(SWITCH_PD_WAIT_SOURCE, "replacement partner presents Rd without VBUS");
#if DEVELOPMENT || DEBUG
    if (alertBits && (phase == SWITCH_PD_WAIT_SOURCE || phase == SWITCH_PD_SOURCE))
        IOLog("SwitchPD: %s alert %#x status1 %#x status2 %#x\n", switch_pd_phase_name(phase), alertBits, status1,
              status2);
#endif
    const bool wasSource = phase == SWITCH_PD_SOURCE;
    apply(switch_pd_decide(phase, alertBits, status1, status2, allowSource && !asleepSinkOnly && switchesOn));
    if (handleFault(alertBits, status1)) {
        notify();
        return;
    }
    if (phase == SWITCH_PD_SINK) {
        const bool wantVconn = switch_pd_vconn_on(phase, status1, status2);
#if DEVELOPMENT || DEBUG
        const bool hadVconn = vconnOn;
#endif
        (void)setRails(false, wantVconn);
#if DEVELOPMENT || DEBUG
        if (hadVconn && !vconnOn)
            IOLog("SwitchPD: sink cleared VCONN (alert %#x status1 %#x status2 %#x)\n", alertBits, status1, status2);
#endif
    }
    if (wasSource && phase == SWITCH_PD_SOURCE) {
        if (prSwap == PrSwap::WaitingReady && (status1 & STATUS1_CMD_BUSY) == 0) {
            window->cancelTimeout();
            sendPrSwap();
        } else if (prSwap == PrSwap::Sent && (alertBits & ALERT_CMD_DONE)) {
            window->cancelTimeout();
            const unsigned last = (status1 & STATUS1_LASTCMD_MASK) >> STATUS1_LASTCMD_SHIFT;
#if DEVELOPMENT || DEBUG
            IOLog("SwitchPD: PR_Swap probe result %u (alert %#x status1 %#x status2 %#x)\n", last, alertBits, status1,
                  status2);
#endif
            if (last == LASTCMD_ABORTED && !prSwapRetried) {
                prSwapRetried = true;
                prSwap = PrSwap::RetryWait;
                window->setTimeoutMS(kPrSwapRetryMS);
            } else {
                prSwap = PrSwap::Done;
            }
        }
    }
    negotiate(alertBits);
    notifyAlt(alertBits);
    notify();
}

void SwitchPD::notifyAlt(uint16_t alertBits)
{
    const auto atRest = [](const void *pd) -> bool {
        const auto *self = static_cast<const SwitchPD *>(pd);
        if (self->phase == SWITCH_PD_SOURCE)
            return self->prSwap == PrSwap::Done;
        const Nego n = self->nego;
        return n == Nego::Settling || n == Nego::Done || n == Nego::Failed;
    };
    const bool contractOurs = nego == Nego::Settling || nego == Nego::Done;
    const uint16_t status1 = state.status1;
    const bool isSource = phase == SWITCH_PD_SOURCE;
    const bool altSession = explicitContract && state.plugged &&
                            ((phase == SWITCH_PD_SINK && (status1 & SWITCH_PD_STATUS1_SRC_MODE) == 0u) ||
                             (isSource && state.dataHost));
    switch_pd_alt_alert(this, loop, atRest, alertBits, status1, altSession, contractOurs && choice.dock, isSource);
    if (phase == SWITCH_PD_SOURCE && switch_pd_alt_partner_is_usb_host()) {
        switch_pd_alt_leave();
        enter(SWITCH_PD_CLASSIFY, "USB host partner did not take power role: source switches off 1 s");
    }
}

// bm92t_event_handler's faults, after the policy has taken VBUS off: an internal fault ends the event (true), with a
// SYS_RESET when switch_pd_reset_on_fault says so; a source fault gets a PD hard reset.
bool SwitchPD::handleFault(uint16_t alertBits, uint16_t status1)
{
    if (!switch_pd_internal_fault(status1) && !switch_pd_hard_reset(alertBits, status1))
        return false;
    endNegotiation();
    switch_pd_alt_leave();
    window->cancelTimeout();
    if (switch_pd_internal_fault(status1)) {
        if (phase != SWITCH_PD_HELD)
            enter(SWITCH_PD_HELD, "internal fault: isolating the source supplies");
        if (switch_pd_reset_on_fault(alertBits, status1) && write16(COMMAND, SYS_RESET_CMD) == 0) {
            resetPending = true;
            alert->disable();
            window->setTimeoutMS(kFaultResetMS + kAfterResetMS);
        }
    } else {
        const int err = write16(COMMAND, PD_HARD_RST_CMD);
        IOLog("SwitchPD: source fault: PD hard reset%s (alert %#x status1 %#x)\n", err ? " failed" : "", alertBits,
              status1);
    }
    return true;
}

// SYS_RESET, then `settleMS`, then ALERT_STATUS read to release the ALERT the reset raised (bm92t_system_reset_auto).
void SwitchPD::systemReset(uint32_t settleMS, const char *why) const
{
    const int err = write16(COMMAND, SYS_RESET_CMD);
    IOLog("SwitchPD: BM92T36 SYS_RESET%s: %s (status1 %#x)\n", err ? " failed" : "", why, state.status1);
    if (err)
        return;
    IOSleep(settleMS);
    uint16_t released = 0;
    (void)read16(ALERT_STATUS, released);
}

void SwitchPD::notify()
{
    state.role = SWITCH_PD_ROLE_SINK;
    if (phase == SWITCH_PD_EMPTY) {
        state.role = SWITCH_PD_ROLE_NONE;
    } else if (phase == SWITCH_PD_SOURCE) {
        state.role = SWITCH_PD_ROLE_SOURCE;
    }
    state.settled = phase != SWITCH_PD_HELD && phase != SWITCH_PD_CLASSIFY && phase != SWITCH_PD_WAIT_SOURCE &&
                    !(phase == SWITCH_PD_SOURCE && prSwap != PrSwap::Done);
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
    case SWITCH_PD_END:
        enter(step.next, step.why);
        break;
    case SWITCH_PD_START:
        startSourcing();
        break;
    }
}

// A phase's outputs (SwitchPDPolicy.h): switches off first when they go off, the rails, then the switches on.
// Empty -> WaitSource keeps SPDSRC12_ON armed while SPDSRC1/2 FETs are open so Try.SNK partners do not lose Rp.
// NOLINTNEXTLINE(misc-no-recursion): HELD has no source rails, so the nested enter() goes no deeper
void SwitchPD::enter(switch_pd_phase next, const char *why)
{
    const switch_pd_phase previous = phase;
    const bool boosting = switch_pd_boost_on(previous);
    const bool preserveContract = previous == SWITCH_PD_SOURCE && next == SWITCH_PD_SINK && state.plugged &&
                                  !(state.alert & SWITCH_PD_ALERT_PLUGPULL);
    if (next != previous) {
        window->cancelTimeout();
        prSwap = PrSwap::None;
        prSwapRetried = false;
        if (!preserveContract)
            explicitContract = false;
    }
    phase = next;
    sawDetach = false;
    const bool armed = allowSource && !asleepSinkOnly;
    const bool switches = switch_pd_switches_on(next, armed);
    const bool vbus = armed && switch_pd_vbus_rail_on(next);
    const bool vconn = switch_pd_vconn_on(next, state.status1, state.status2) && (next == SWITCH_PD_SINK || armed);
    const bool boost = armed && switch_pd_boost_on(next);
    if (!switches || (boosting && !boost))
        setSwitches(false);
    if (boosting && !boost)
        (void)switch_battery_set_otg_boost(false, false);
    if (boost && !boosting) {
        const int result =
            switch_battery_set_otg_boost(true, vbusRailOn && switchesOn && switch_pd_vbus_ours(state.status1));
        if (result) {
            enter(result == SWITCH_BATTERY_BOOST_VBUS_PRESENT ? SWITCH_PD_SINK : SWITCH_PD_HELD,
                  result == SWITCH_BATTERY_BOOST_VBUS_PRESENT ? "not sourcing: the charger sees external VBUS"
                                                              : "not sourcing: the BQ24193 boost failed");
            return;
        }
    }
    const bool railsDone = setRails(vbus, vconn);
    IOLog("SwitchPD: %s (alert %#x status1 %#x status2 %#x)\n", why, state.alert, state.status1, state.status2);
    if (vbus && !railsDone) {
        enter(SWITCH_PD_HELD, "not sourcing: over-current protection or the source rails failed");
        return;
    }
    if (switches)
        setSwitches(true);
    if (next == SWITCH_PD_CLASSIFY)
        window->setTimeoutMS(kClassifyMS);
    if (next == SWITCH_PD_WAIT_SOURCE)
        window->setTimeoutMS(kWaitSourceMS);
}

// CONFIG1 SPDSRC: both source switches on (SPDSRC12_ON, bm92t_set_source_mode) or off (SPDSRC12_OFF).
void SwitchPD::setSwitches(bool on)
{
    if (on == switchesOn)
        return;
    uint16_t config = 0;
    int err = read16(CONFIG1, config);
    if (!err) {
        config = static_cast<uint16_t>((config & ~CONFIG1_SPDSRC_MASK) | (on ? SPDSRC12_ON : SPDSRC12_OFF)
                                                                             << CONFIG1_SPDSRC_SHIFT);
        err = write16(CONFIG1, config);
    }
    if (!err) {
        switchesOn = on;
    } else if (!reportedSwitchFailure) {
        IOLog("SwitchPD: source switches %s failed (I2C %d)\n", on ? "on" : "off", err);
    }
    reportedSwitchFailure = err != 0;
}

// Source rails: OCP, v_vdd5v3 (PX3), ramp, VCONN (PK5). When raising rails in WaitSource while SPDSRC1/2 are still
// open, CONFIG1 stays SPDSRC12_ON so Rp is never dropped; if our VBUS switch is already closed, open switches first.
bool SwitchPD::setRails(bool vbus, bool vconn)
{
    if (vbus == vbusRailOn && vconn == vconnOn && vconnSwap == vconnOn)
        return true;
    if (vbus && switch_pd_vbus_ours(state.status1)) {
        setSwitches(false);
        if (switchesOn)
            return false;
    }
    int err = 0;
    if (vbus && !vbusRailOn) {
        err = enableOcp();
        if (!err)
            err = tegra_gpio_output(kSourceRailGpio, 1);
        if (!err)
            IOSleep(kRailRampMS);
        if (!err && vconn && !vconnOn)
            err = tegra_gpio_output(kVconnGpio, 1);
        if (err) {
            setVconnSwap(false);
            (void)tegra_gpio_output(kVconnGpio, 0);
            (void)tegra_gpio_output(kSourceRailGpio, 0);
            vbusRailOn = false;
            vconnOn = false;
            IOLog("SwitchPD: source rails on failed (%d)\n", err);
            return false;
        }
        vbusRailOn = true;
        if (vconn)
            vconnOn = true;
    }
    if (!vconn && vconnOn) {
        setVconnSwap(false);
        const int vconnErr = tegra_gpio_output(kVconnGpio, 0);
        if (vconnErr)
            err = vconnErr;
        vconnOn = vconnErr != 0;
    }
    if (!vbus && vbusRailOn) {
        const int railErr = tegra_gpio_output(kSourceRailGpio, 0);
        if (railErr)
            err = railErr;
        vbusRailOn = railErr != 0;
    }
    if (vconn && !vconnOn) {
        const int vconnErr = tegra_gpio_output(kVconnGpio, 1);
        if (vconnErr)
            err = vconnErr;
        vconnOn = vconnErr == 0;
    }
    setVconnSwap(vconnOn);
    if (err)
        IOLog("SwitchPD: source rails %s failed (%d)\n", (vbus || vconn) ? "on" : "off", err);
    return err == 0;
}

void SwitchPD::sendPrSwap()
{
    const int err = write16(COMMAND, PR_SWAP_CMD);
    if (err) {
        prSwap = PrSwap::Done;
#if DEVELOPMENT || DEBUG
        IOLog("SwitchPD: PR_Swap probe write failed (I2C %d)\n", err);
#endif
        return;
    }
    prSwap = PrSwap::Sent;
    window->setTimeoutMS(kPrSwapTimeoutMS);
#if DEVELOPMENT || DEBUG
    IOLog("SwitchPD: PR_Swap probe sent (status1 %#x status2 %#x)\n", state.status1, state.status2);
#endif
}

// VBUS is prepared in WaitSource as bm92t_event_handler enables it on the initial OTG event; once sourcing, send
// PR_SWAP_CMD so a dual-role partner caught in Rd can take back VBUS.
void SwitchPD::startSourcing()
{
    phase = SWITCH_PD_SOURCE;
    explicitContract = true;
    prSwapRetried = false;
    const bool ours = switch_pd_vbus_ours(state.status1);
    IOLog("SwitchPD: sink-only partner: sourcing VBUS (v_vdd5v3, VCONN, OCP up; %s; boost on) "
          "(alert %#x status1 %#x status2 %#x)\n",
          ours ? "our switch closed" : "our switch closing", state.alert, state.status1, state.status2);
    if ((state.status1 & STATUS1_CMD_BUSY) == 0) {
        sendPrSwap();
    } else {
        prSwap = PrSwap::WaitingReady;
        window->setTimeoutMS(kPrSwapRetryMS);
#if DEVELOPMENT || DEBUG
        IOLog("SwitchPD: PR_Swap probe waiting for CMD_BUSY (status1 %#x)\n", state.status1);
#endif
    }
}

void SwitchPD::windowFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchPD *>(owner);
    if (self->leaving)
        return;
    uint16_t status1 = 0;
    uint16_t status2 = 0;
    if (self->resetPending) {
        self->resetPending = false;
        uint16_t bits = 0;
        uint16_t config = 0;
        if (!read16(ALERT_STATUS, bits) && self->readStatus(status1, status2) && !read16(CONFIG1, config)) {
            self->switchesOn = (config & CONFIG1_SPDSRC_MASK) != CONFIG1_SPDSRC_MASK;
            self->setSwitches(false);
            if (!self->switchesOn && !self->enableOcp() && !switch_pd_internal_fault(status1)) {
                self->configureRoleSwaps(false);
                const bool sink = (status1 & SWITCH_PD_STATUS1_INSERT) &&
                                  (!(status1 & SWITCH_PD_STATUS1_SRC_MODE) || (status1 & SWITCH_PD_STATUS1_VSAFE));
                self->enter(sink ? SWITCH_PD_SINK : SWITCH_PD_EMPTY, "power role after controller reset");
                self->handle(bits, status1, status2);
            }
        }
        if (!self->resetPending)
            self->alert->enable();
        return;
    }
    if (!self->readStatus(status1, status2))
        return;
    if (self->phase == SWITCH_PD_SOURCE) {
        if (!(status1 & SWITCH_PD_STATUS1_INSERT) || !(status1 & SWITCH_PD_STATUS1_SRC_MODE)) {
            self->handle(0, status1, status2);
            return;
        }
        self->state.alert = 0;
        self->state.status1 = status1;
        self->state.status2 = status2;
        if (self->prSwap == PrSwap::WaitingReady || self->prSwap == PrSwap::RetryWait) {
            if ((status1 & STATUS1_CMD_BUSY) == 0) {
                self->sendPrSwap();
            } else {
                self->prSwap = PrSwap::Done;
#if DEVELOPMENT || DEBUG
                IOLog("SwitchPD: PR_Swap probe skipped: CMD_BUSY stuck (status1 %#x)\n", status1);
#endif
            }
            if (self->prSwap == PrSwap::Done) {
                self->notifyAlt(0);
                self->notify();
            }
        } else if (self->prSwap == PrSwap::Sent) {
            self->prSwap = PrSwap::Done;
#if DEVELOPMENT || DEBUG
            IOLog("SwitchPD: PR_Swap probe timed out (status1 %#x status2 %#x)\n", status1, status2);
#endif
            self->notifyAlt(0);
            self->notify();
        }
        return;
    }
    const switch_pd_step step = switch_pd_window_end(self->phase, self->sawDetach, status1, status2);
    if (step.act == SWITCH_PD_NONE)
        return;
    self->state.alert = 0;
    self->state.status1 = status1;
    self->state.status2 = status2;
    self->apply(step);
    if (self->phase == step.next)
        self->handle(0, status1, status2);
    else
        self->notify();
}

void SwitchPD::alertFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchPD *>(owner);
    const uint64_t now = mach_absolute_time();
    uint64_t elapsedNS = 0;
    absolutetime_to_nanoseconds(now - self->alertWindow, &elapsedNS);
    if (elapsedNS > 1000000000ULL) {
        self->alertWindow = now;
        self->alertCount = 0;
    }
    if (++self->alertCount > kAlertBurst) {
        self->stopEvents();
        return;
    }
    uint16_t alertBits = 0;
    uint16_t status1 = 0;
    uint16_t status2 = 0;
    const int err = read16(ALERT_STATUS, alertBits);
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
    switch_pd_alt_leave();
    IOLog("SwitchPD: BM92T36 ALERT (PK4) fired %u times in 1 s, turned off (port sink-only until reboot): %s\n",
          alertCount, pin);
    if (phase == SWITCH_PD_SOURCE) {
        enter(SWITCH_PD_SINK, "not sourcing: no PD events");
    } else {
        enter(SWITCH_PD_SINK, "sink: no PD events");
    }
    (void)setRails(false, false);
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
    if (resetPending)
        return;
    if (!allow && phase == SWITCH_PD_SOURCE) {
        enter(SWITCH_PD_HELD, "not sourcing: host mode is off");
        notify();
    } else if (!allow && (phase == SWITCH_PD_CLASSIFY || phase == SWITCH_PD_WAIT_SOURCE)) {
        window->cancelTimeout();
        enter(SWITCH_PD_SINK, "sink: host mode is off");
        notify();
    } else if (phase == SWITCH_PD_EMPTY || phase == SWITCH_PD_HELD || phase == SWITCH_PD_SINK) {
        const bool arm = allow && !asleepSinkOnly;
        setSwitches(switch_pd_switches_on(phase, arm));
        if (!arm)
            (void)setRails(false, switch_pd_vconn_on(phase, state.status1, state.status2));
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
        asleepSinkOnly = phase == SWITCH_PD_EMPTY || phase == SWITCH_PD_CLASSIFY || phase == SWITCH_PD_WAIT_SOURCE;
        if (phase == SWITCH_PD_CLASSIFY || phase == SWITCH_PD_WAIT_SOURCE) {
            window->cancelTimeout();
            enter(SWITCH_PD_EMPTY, "source startup canceled for sleep");
            notify();
        }
        if (asleepSinkOnly) {
            setSwitches(false);
            (void)setRails(false, false);
        }
    } else if (asleepSinkOnly) {
        asleepSinkOnly = false;
        setSwitches(switch_pd_switches_on(phase, allowSource));
    }
    if (!entering && explicitContract) {
        uint16_t bits = 0;
        uint16_t status1 = 0;
        uint16_t status2 = 0;
        if (read16(ALERT_STATUS, bits) || !readStatus(status1, status2))
            return;
        if (bits || status1 != state.status1 || status2 != state.status2)
            handle(bits, status1, status2);
        if (explicitContract)
            switch_pd_alt_resume();
    }
}

#if DEVELOPMENT || DEBUG
// Work loop. The status registers are not clear-on-read (ALERT_STATUS, which is, stays alertFired's).
void SwitchPD::describe(char *buffer, size_t size)
{
    uint16_t status1 = 0;
    uint16_t status2 = 0;
    uint16_t config = 0;
    uint16_t vendor = 0;
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
        static_cast<unsigned>((status1 & SWITCH_PD_STATUS1_SPDSRC1) != 0u),
        static_cast<unsigned>((status1 & SWITCH_PD_STATUS1_SPDSRC2) != 0u),
        static_cast<unsigned>((status1 & SWITCH_PD_STATUS1_VSAFE) != 0u), status2, config, vendor,
        (vendor & VENDOR_CONFIG_OCP_DISABLE) ? "off" : "on", err ? " (I2C error)" : "",
        (vbusRailOn || vconnOn) ? "on" : "off", pinText(kSourceRailGpio), pinText(kVconnGpio),
        pinText(kChargeSelect1Gpio), pinText(kChargeSelect2Gpio));
}
#endif

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
        uint16_t type = 0;
        uint16_t revision = 0;
        uint16_t config = 0;
        bool turnedOff = false;
        if (resources && pd && pd->init() && pd->attach(resources)) {
            if (pd->begin(type, revision, config, turnedOff)) {
                pd->setName("SwitchPD");
                pd->registerService();
                pd->watchForExit();
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
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): gPD keeps the started SwitchPD for good
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
