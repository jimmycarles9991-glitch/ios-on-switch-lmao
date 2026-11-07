// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's events (SwitchXHCI.h): the falcon mailbox, the lost-interrupt check, the role, the USB-C cable, SC7.
//
// Interrupts only: AppleUSBXHCI's own filter owns INTID 71, the mailbox filter (INTID 72) is ours. No timer runs in
// steady state. Routing (checked against the sources): the OLED DT's xusb@70090000 has interrupts GIC_SPI 39 (host)
// and 40 (mailbox), IRQ_TYPE_LEVEL_HIGH, through the root interrupt-parent, the Tegra LIC, into the GIC: INTID 71 and
// 72. ArmGIC programs a level SPI (ICFGR edge bit clear), CPU0, priority 0xa0, and the LIC gate (IEP_CLASS IRQ, IER
// set) with GICD_ISENABLER at enable; xhost::makeNub gives AppleUSBXHCI the GIC nub's own interrupt specifier. The
// Tegra side is xhci-tegra.c tegra_xusb_config exactly: IPFS EN_FPCI, BAR0, FPCI CFG_1 bus master, IPFS INTR_MASK
// IP_INT_MASK, hysteresis 0x80 (IPFS INTR_ENABLE and the MSI vectors are left at reset, as Linux leaves them).
// IMAN0.IE and USBCMD.INTE are AppleUSBXHCI's.
// Neither line had fired on this hardware when this was written, so their check is a never-booted one until they
// have (XusbPorts.h xusb_irq_judge): when the PD controller reports a device on the port in host role a port-status
// event is due (SwitchUSB.cpp calls switchxhci_expect_port_change); 2 s later, and again 50 ms after that, a line that
// has not reached its handler once this boot (switch_interrupt_count) with an event waiting, or, after the
// controller's first start, with nothing delivered at all, logs one line with the whole path (xHCI, FPCI, IPFS, GIC)
// and turns host mode off for the boot (switch_usb_host_failed: device mode stays). Once a line has fired, IMAN0.IP
// pending (with IE and INTE) and ERDP0 unmoved, or a mailbox interrupt pending with no handler run, panics as for every
// proven line. A mailbox line that fires with nothing to take is turned off with one line, never left to storm CPU0.
#include "SwitchXHCI.h"
#include "SwitchUSB.h"
#include "SwitchXHCIServices.h"
#include "XusbLanes.h"
#include "XusbSuperSpeed.h"
#include <IOKit/IOLib.h>
#include "../ArmGIC/SwitchInterrupts.h"

using xhost::gHost;

namespace {
constexpr uint32_t kCheckMS = 2000, kConfirmMS = 50;
constexpr uint32_t kUnclaimedLimit = 1000;  // mailbox interrupts in a row with no SMI to take: a line we cannot clear
constexpr uint64_t kPortWaitNS = 2000ULL * 1000 * 1000;  // AppleUSBXHCI's ports registering after its start
bool gRemoteWake;  // the PMC's sleepwalk holds pad 0 for this SC7 (switchxhci_sleep's thread only)
bool gCable;       // role thread: the USB-C cable reported in (switchxhci_cable)
bool gSsHeld;      // role thread: SS port 0's root port held On
}  // namespace

// SC7 (switchxhci_sleep): entering, nothing may read the block until it is back, so the mailbox line goes off and the
// interrupt check stops; leaving, after the block is powered and its firmware runs again, the mailbox line comes back.
IOReturn SwitchXHCI::systemSleep(bool entering)
{
    if (entering) {
        check->cancelTimeout();
        checkStage = 0;
        mailbox->disable();
        gHost.lostPower();
    } else {
        unclaimed = 0;
        mailbox->enable();
    }
    return kIOReturnSuccess;
}

// Primary interrupt context. INTID 72 with no SMI to acknowledge is ignored a few times (a spurious level); a line
// that keeps firing with nothing to take would storm CPU0, so after kUnclaimedLimit in a row it goes to the action
// (returning true masks the level source until then), which turns it off for good with one line.
bool SwitchXHCI::mailboxFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchXHCI *>(owner);
    if (gHost.takeMailboxInterrupt()) {
        self->unclaimed = 0;
        __atomic_fetch_add(&self->mailboxInterrupts, 1, __ATOMIC_RELAXED);
        return true;
    }
    if (++self->unclaimed < kUnclaimedLimit)
        return false;
    self->mailboxStorm = true;
    return true;
}

void SwitchXHCI::mailboxAction(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchXHCI *>(owner);
    if (self->mailboxStorm) {
        self->mailbox->disable();
        char state[200];
        gHost.describe(state, sizeof(state));
        IOLog("SwitchUSB: XUSB mailbox interrupt (INTID %u) fired %u times with no SMI to take, turned off: %s\n",
              xhost::kMailboxIntid, kUnclaimedLimit, state);
        return;
    }
    if (!gHost.serviceMailbox()) {
        char state[200];
        gHost.describe(state, sizeof(state));
        panic("SwitchUSB: XUSB falcon firmware hang (mailbox SMI FW_HANG): %s", state);
    }
}

// A device just appeared on the port in host role: its port-status event is due. The first time after the
// controller's start, a port event is due even if none is pending (the device was on the port as it powered up).
IOReturn SwitchXHCI::expectPortChange()
{
    if (checkStage || gaveUp)
        return kIOReturnSuccess;
    firstCheck = !checked;
    checked = true;
    armCheck(kCheckMS, 1);
    return kIOReturnSuccess;
}

xusb_irq_sample SwitchXHCI::hostSample() const
{
    uint32_t iman = 0, usbcmd = 0, usbsts = 0, erdp = 0;
    (void)gHost.interruptSample(iman, usbcmd, usbsts, erdp);
    return {switch_interrupt_count(xhost::kHostIntid), erdp, xusb_xhci_pending(iman, usbsts),
            xusb_xhci_deliverable(iman, usbcmd)};
}

xusb_irq_sample SwitchXHCI::mailboxSample() const
{
    return {switch_interrupt_count(xhost::kMailboxIntid), 0, !mailboxStorm && gHost.mailboxPending(), true};
}

void SwitchXHCI::armCheck(uint32_t ms, uint8_t stage)
{
    checkStage = stage;
    hostAtCheck = hostSample();
    mailboxAtCheck = mailboxSample();
    check->setTimeoutMS(ms);
}

void SwitchXHCI::checkFired(IOTimerEventSource *)
{
    const xusb_irq_sample host = hostSample(), mailboxNow = mailboxSample();
    const xusb_irq_verdict hostVerdict = xusb_irq_judge(hostAtCheck, host, firstCheck, checkStage);
    const xusb_irq_verdict mailboxVerdict = xusb_irq_judge(mailboxAtCheck, mailboxNow, false, checkStage);
    const bool hostSide = hostVerdict >= mailboxVerdict;
    const xusb_irq_verdict verdict = hostSide ? hostVerdict : mailboxVerdict;
    if (verdict == XUSB_IRQ_FINE) {
        checkStage = 0;
        firstCheck = false;
        return;
    }
    if (verdict == XUSB_IRQ_CONFIRM) {
        armCheck(kConfirmMS, 2);
        return;
    }
    const uint32_t intid = hostSide ? xhost::kHostIntid : xhost::kMailboxIntid;
    char state[480], gic[192];
    gHost.describe(state, sizeof(state));
    switch_interrupt_describe(intid, gic, sizeof(gic));
    if (verdict == XUSB_IRQ_PANIC) {
        panic("SwitchUSB: XUSB %s interrupt lost (INTID %u): %s, %s", hostSide ? "host" : "mailbox", intid, state, gic);
    }
    checkStage = 0;
    gaveUp = true;
    const bool waiting = hostSide ? host.pending : mailboxNow.pending;
    IOLog("SwitchUSB: host mode off for this boot: the XUSB %s interrupt (INTID %u, never fired this boot) did not "
          "reach the CPU %s: %s; %s\n",
          hostSide ? "host" : "mailbox", intid,
          waiting ? "with an event waiting" : "2 s after the controller's first start (no event waiting either)", state,
          gic);
    switch_usb_host_failed();
}

namespace {
// The last PP cycle (SwitchUSB.cpp's role line): port, the three PORTSC values, whether PP followed. Written on
// AppleUSBXHCI's work loop, read by the report a second later.
struct PowerCycle {
    unsigned port;
    uint32_t trace[3];
    bool followed;
} gCycle;
}  // namespace

// The pad first (the ID grounded before the port is powered, as tegra_xhci_set_host_mode), then, with the controller
// already running, the OTG port's PP off and on again so the firmware powers it with the pad in the host role. The
// first time, AppleUSBXHCI::start powers the ports itself, after this. Device: ID floating; PP is left on (the XUDC
// ran with it on after every r7w host session).
void switchxhci_set_host_role(bool host)
{
    if (!gHost.map())
        return;
    gHost.setHostRole(host);
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    if (!host || !controller)
        return;
    controller->getWorkLoop()->runAction(
        [](OSObject *, void *, void *, void *, void *) -> IOReturn {
            PowerCycle cycle = {gHost.otgPort(), {}, false};
            if (cycle.port)
                cycle.followed = gHost.cyclePortPower(cycle.port, cycle.trace);
            gCycle = cycle;
            return kIOReturnSuccess;
        },
        controller);
}

void switchxhci_describe(char *buffer, size_t size)
{
    if (!size)
        return;
    if (!gHost.map()) {
        snprintf(buffer, size, "XUSB not mapped");
        return;
    }
    char pads[400], ports[360];
    gHost.describePads(pads, sizeof(pads));
    gHost.describePorts(ports, sizeof(ports));
    const PowerCycle cycle = gCycle;
    const unsigned host = switch_interrupt_count(xhost::kHostIntid),
                   mailbox = switch_interrupt_count(xhost::kMailboxIntid);
    if (cycle.port) {
        snprintf(buffer, size, "%s; INTID 71 x%u 72 x%u; last PP cycle port %u %#x>%#x>%#x%s; %s", ports, host, mailbox,
                 cycle.port, cycle.trace[0], cycle.trace[1], cycle.trace[2],
                 cycle.followed ? "" : " (PP did not follow)", pads);
    } else {
        snprintf(buffer, size, "%s; INTID 71 x%u 72 x%u; no PP cycle; %s", ports, host, mailbox, pads);
    }
}

void switchxhci_expect_port_change(void)
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    IOWorkLoop *loop = controller ? controller->tegraLoopForCheck() : nullptr;
    if (loop)
        loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) {
                return static_cast<SwitchXHCI *>(owner)->expectPortChange();
            },
            controller);
}

// SwitchUSB.cpp's suspend and resume, thread context. IOKit has taken AppleUSBXHCI to Sleep or Off before the platform
// sleep actions run, and raises it only after the wake actions, so the Tegra side is down for exactly that window. A
// device AppleUSBXHCI left suspended on the port gets the PMC's sleepwalk, so a key press wakes the console. SC7
// takes VDD_CORE: the partitions, the falcon and its firmware go, and AppleUSBXHCI finds a controller that lost its
// state (as an xHC that lost power in S3) and initializes it again. Back from SC7 the Tegra bring-up runs whole, as at
// the first start; a block that does not come back would leave AppleUSBXHCI reading unclocked registers, so it panics
// with the reason instead.
void switchxhci_sleep(bool entering)
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    if (!controller)
        return;
    const auto gated = [](OSObject *owner, void *down, void *, void *, void *) {
        return static_cast<SwitchXHCI *>(owner)->systemSleep(down != nullptr);
    };
    if (entering) {
        gRemoteWake = gHost.armRemoteWake(gHost.otgPort());
        controller->tegraLoopForCheck()->runAction(gated, controller, controller);
        return;
    }
    if (gRemoteWake) {
        gHost.disarmRemoteWake();
        gRemoteWake = false;
    }
    char why[160] = "";
    if (!gHost.powerOn(why, sizeof(why)) ||
        !gHost.loadFirmware(switch_xusb_firmware, xhost::firmwareBytes(), why, sizeof(why))) {
        panic("SwitchUSB: XUSB host did not come back from SC7: %s", why);
    }
    controller->tegraLoopForCheck()->runAction(gated, controller, nullptr);
    if (!gHost.sendMailbox(xhost::MBOX_MSG_ENABLED, 0))
        IOLog("SwitchUSB: XUSB falcon did not take MSG_ENABLED after SC7\n");
}

namespace {
struct PortPower {
    unsigned port;
    bool on, followed;
    uint32_t trace[2];
};

IOReturn setPortPowerGated(OSObject *, void *request, void *, void *, void *)
{
    auto *power = static_cast<PortPower *>(request);
    power->followed = gHost.setPortPower(power->port, power->on, power->trace);
    return kIOReturnSuccess;
}

PortPower portPower(SwitchXHCI *controller, unsigned port, bool on)
{
    PortPower power = {port, on, false, {}};
    controller->getWorkLoop()->runAction(&setPortPowerGated, controller, &power);
    return power;
}

// On the mailbox's own loop, so the falcon's messages and this command never take the mailbox at once.
bool resetSuperSpeedInterface(SwitchXHCI *controller)
{
    bool taken = false;
    controller->tegraLoopForCheck()->runAction(
        [](OSObject *, void *result, void *, void *, void *) {
            *static_cast<bool *>(result) = gHost.sendMailbox(xhost::MBOX_RESET_SSPI, XUSB_MBOX_RESET_SSPI_DATA);
            return kIOReturnSuccess;
        },
        controller, &taken);
    return taken;
}

// SS port 0's root port as the cable and the lane stand (XusbLanes.h xusb_ss_port_follow). Role thread; nothing while
// SC7 has the block (the role thread follows again once switchxhci_sleep has it back).
void followSuperSpeed(SwitchXHCI *controller)
{
    if (!gHost.powered())
        return;
    const unsigned usb2 = gHost.otgPort(), number = gHost.ssPort();
    const bool usb2Connected = usb2 && (gHost.portsc(usb2) & XHCI_PORTSC_CCS);
    const xusb_ss_port_step step = xusb_ss_port_follow(gCable, switch_usb_ss_up(), gSsHeld, usb2Connected);
    if (!step.power_on && !step.power_off)
        return;
    IOService *port = number ? xhost::waitForRootPort(controller, number, kPortWaitNS) : nullptr;
    if (!port) {
        static bool reported;
        if (!reported)
            IOLog("SwitchUSB: no SuperSpeed root port for SS port 0 (xHCI port %u, controller power %u)\n", number,
                  static_cast<unsigned>(controller->getPowerState()));
        reported = true;
        return;
    }
    if (step.power_off) {
        const IOReturn released = xhost::forcePower(port, false);
        gSsHeld = false;
        IOLog("SwitchUSB: SuperSpeed root port %u off (UsbForcePower 0: %#x)\n", number, released);
        port->release();
        return;
    }
    const IOReturn raised = xhost::forcePower(controller, true);
    const PortPower off = portPower(controller, number, false);
    const bool reset = resetSuperSpeedInterface(controller);
    const PortPower on = portPower(controller, number, true);
    const IOReturn held = xhost::forcePower(port, true);
    gSsHeld = true;
    IOLog("SwitchUSB: SuperSpeed root port %u on: PP off %#x>%#x%s, RESET_SSPI %s, PP on %#x>%#x%s, UsbForcePower 1: "
          "%#x (controller %#x)%s\n",
          number, off.trace[0], off.trace[1], off.followed ? "" : " (did not follow)", reset ? "taken" : "not taken",
          on.trace[0], on.trace[1], on.followed ? "" : " (did not follow)", held, raised,
          step.usb2_cycle ? "; USB 2.0 port cycled for the device already on it" : "");
    if (step.usb2_cycle) {
        (void)portPower(controller, usb2, false);
        (void)portPower(controller, usb2, true);
    }
    port->release();
}
}  // namespace

void switchxhci_superspeed(void)
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    if (controller)
        followSuperSpeed(controller);
}

// The USB-C cable in or out in the host role, from SwitchUSB.cpp's role thread (it waits on command gates).
// AppleUSBHostController's power desire starts at Off and an idle AppleUSBXHCI powers its controller down, so a device
// that arrives is seen only on a root port that something powered for it: on Apple's boards the port controller's cable
// change (xhost::forcePower on the port). A port reads PORTSC only while its controller is powered (AppleUSBXHCIPort
// powerOn otherwise times out on a cached 0, as on the 23:23 boot), so a cable in raises the controller first.
void switchxhci_cable(bool connected)
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    if (!controller)
        return;
    const unsigned number = gHost.otgPort();
    IOService *port = xhost::waitForRootPort(controller, number, kPortWaitNS);
    if (!port) {
        char ports[400];
        xhost::describePorts(controller, ports, sizeof(ports));
        IOLog("SwitchUSB: AppleUSBXHCI registered no root port %u for the USB-C cable (controller power %u; %s)\n",
              number, static_cast<unsigned>(controller->getPowerState()), ports);
        return;
    }
    const unsigned controllerBefore = controller->getPowerState(), portBefore = port->getPowerState();
    const IOReturn raised = connected ? xhost::forcePower(controller, true) : kIOReturnSuccess;
    // SuperSpeed before USB 2.0 going on and off, as tegra_xhci_set_host_mode: a USB 3 device that finds its USB2 port
    // powered first may settle there.
    gCable = connected;
    followSuperSpeed(controller);
    const IOReturn result = xhost::forcePower(port, connected);
    IOLog("SwitchUSB: USB-C cable %s: controller power %u -> %u (UsbForcePower %#x), root port %u (%s) power %u -> %u "
          "(UsbForcePower %u: %#x)\n",
          connected ? "in" : "out", controllerBefore, static_cast<unsigned>(controller->getPowerState()), raised,
          number, port->getMetaClass()->getClassName(), portBefore, static_cast<unsigned>(port->getPowerState()),
          connected ? 1U : 0U, result);
    port->release();
}

bool switchxhci_remote_wake_armed()
{
    return gRemoteWake;
}
