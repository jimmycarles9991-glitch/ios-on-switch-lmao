// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's events (SwitchXHCI.h): the falcon mailbox, the SuperSpeed interface reset, the role, SC7.
//
// Interrupts only: AppleUSBXHCI's own filter owns INTID 71, the mailbox filter (INTID 72) is ours. No timer runs.
// Routing (checked against the sources): the OLED DT's xusb@70090000 has interrupts GIC_SPI 39 (host) and 40
// (mailbox), IRQ_TYPE_LEVEL_HIGH, through the root interrupt-parent, the Tegra LIC, into the GIC: INTID 71 and 72.
// ArmGIC programs a level SPI (ICFGR edge bit clear), CPU0, priority 0xa0, and the LIC gate (IEP_CLASS IRQ, IER set)
// with GICD_ISENABLER at enable; xhost::makeNub gives AppleUSBXHCI the GIC nub's own interrupt specifier. The Tegra
// side is xhci-tegra.c tegra_xusb_config exactly: IPFS EN_FPCI, BAR0, FPCI CFG_1 bus master, IPFS INTR_MASK
// IP_INT_MASK, hysteresis 0x80 (IPFS INTR_ENABLE and the MSI vectors are left at reset, as Linux leaves them).
// IMAN0.IE and USBCMD.INTE are AppleUSBXHCI's. A mailbox line that keeps firing with nothing to take panics.
#include "SwitchXHCI.h"
#include "SwitchUSB.h"
#include "SwitchXHCIServices.h"
#include "XusbLanes.h"
#include "XusbSettle.h"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#if DEVELOPMENT || DEBUG
#include <os/log.h>
#endif
#include "../ArmGIC/SwitchInterrupts.h"

using xhost::gHost;

namespace {
constexpr uint32_t kUnclaimedLimit = 1000;  // mailbox interrupts in a row with no SMI to take: a line we cannot clear
bool gRemoteWake;  // the PMC's sleepwalk holds pad 0 for this SC7 (switchxhci_sleep's thread only)
}  // namespace

// SC7 (switchxhci_sleep): entering, nothing may read the block until it is back, so the mailbox line goes off; leaving,
// after the block is powered and its firmware runs again, the mailbox line comes back.
IOReturn SwitchXHCI::systemSleep(bool entering)
{
    if (entering) {
        mailbox->disable();
        if (xusb_mbox_wait_abandon(&commandWait))
            commandGate->commandWakeup(&commandWait);
        gHost.lostPower();
        // AppleUSBXHCI's Sleep (1) keeps its saved state for the restore; from Off (0) it raises through a reset.
        constexpr unsigned long kSavedPowerState = 1;
        __atomic_store_n(&stateLost, hold.held && getPowerState() == kSavedPowerState, __ATOMIC_RELEASE);
        xusb_hold_lost(&hold);
    } else {
        unclaimed = 0;
        mailbox->enable();
    }
    return kIOReturnSuccess;
}

// AppleUSBXHCIARM's hibernate power loss (24B119 0xfffffff009ca462c): as the system wakes, a controller whose saved
// state SC7 took is reported as having lost power (hardware exception 0x100), on AppleUSBXHCI's work loop. The exception
// is only queued for a thread call, so the raise before it still restores; its CRS keeps being answered with SRE
// (xusb_hold) so that the raise cannot run a falcon with nothing in it. Both codes go to the one recovery through Off.
IOReturn SwitchXHCI::powerStateWillChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber,
                                            IOService *whatDevice)
{
    if (whatDevice == this) {
#if DEVELOPMENT || DEBUG
        powerStateFrom = getPowerState();
#endif
        if (stateNumber != 0)
            __atomic_store_n(&powerState, static_cast<uint32_t>(stateNumber), __ATOMIC_RELEASE);
    }
    const IOReturn result = AppleUSBXHCI::powerStateWillChangeTo(capabilities, stateNumber, whatDevice);
    if (whatDevice == getPMRootDomain() && !(capabilities & kIOPMSleepCapability) &&
        __atomic_exchange_n(&stateLost, false, __ATOMIC_ACQ_REL)) {
        const auto lostPower = [](OSObject *owner, void *, void *, void *, void *) {
            constexpr uint32_t kLostPower = 0x100;
            static_cast<SwitchXHCI *>(owner)->hardwareException(kLostPower);
            return kIOReturnSuccess;
        };
        getWorkLoop()->runAction(lostPower, this);
    }
    return result;
}

// AppleUSBXHCI logs nothing when its power state change succeeds: development builds say which one it made.
IOReturn SwitchXHCI::powerStateDidChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber,
                                           IOService *whatDevice)
{
    const IOReturn result = AppleUSBXHCI::powerStateDidChangeTo(capabilities, stateNumber, whatDevice);
    if (whatDevice == this) {
#if DEVELOPMENT || DEBUG
        os_log(OS_LOG_DEFAULT, "SwitchUSB: controller power state %lu -> %lu", powerStateFrom, stateNumber);
#endif
        __atomic_store_n(&powerState, static_cast<uint32_t>(stateNumber), __ATOMIC_RELEASE);
        if (stateNumber == 0) {
            switch_usb_dma_controller_off();
            switch_usb_reconsider();
        }
    }
    return result;
}

// Primary interrupt context. INTID 72 with no SMI to acknowledge is ignored a few times (a spurious level); a line
// that keeps firing with nothing to take would storm CPU0, so after kUnclaimedLimit in a row it goes to the action
// (returning true masks the level source until then), which panics with the path.
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
        char state[200];
        gHost.describe(state, sizeof(state));
        panic("SwitchUSB: XUSB mailbox interrupt (INTID %u) fired %u times with no SMI to take: %s",
              xhost::kMailboxIntid, kUnclaimedLimit, state);
    }
    uint32_t answer = 0;
    uint32_t data = 0;
    if (!gHost.serviceMailbox(answer, data)) {
        char state[200];
        gHost.describe(state, sizeof(state));
        panic("SwitchUSB: XUSB falcon firmware hang (mailbox SMI FW_HANG): %s", state);
    }
    if (!answer)
        return;
    if (xusb_mbox_wait_answer(&self->commandWait, answer, data)) {
        self->commandGate->commandWakeup(&self->commandWait);
    } else {
        IOLog("SwitchUSB: falcon %s %#x with no command waiting for it\n", answer == xhost::MBOX_ACK ? "ACK" : "NAK",
              data);
    }
}

// tegraLoop's gate held (runAction). The firmware takes a command either by an answer (ACK or NAK, by mailboxAction) or by
// releasing the mailbox, which raises nothing (XusbMailbox.h): the sender sleeps on the answer with the gate open to
// mailboxAction, and looks at the owner after 1 ms and at tegra_xusb_mbox_send's 250 ms deadline, an error path only.
bool SwitchXHCI::sendCommand(uint32_t cmd, uint32_t data)
{
    constexpr uint32_t kFirstLookMs = 1;
    constexpr uint32_t kAnswerMs = 250;
    xusb_mbox_wait_begin(&commandWait, cmd);
    if (!gHost.sendMailbox(cmd, data)) {
        (void)xusb_mbox_wait_end(&commandWait);
        IOLog("SwitchUSB: falcon mailbox busy for command %u (owner %u)\n", cmd, gHost.mailboxOwner());
        return false;
    }
    uint64_t looks[2] = {};
    clock_interval_to_deadline(kFirstLookMs, kMillisecondScale, &looks[0]);
    clock_interval_to_deadline(kAnswerMs, kMillisecondScale, &looks[1]);
    const xusb_mbox_outcome outcome = xusb::awaitAnswer(
        commandWait,
        [this, &looks](unsigned look) {
            return commandGate->commandSleep(&commandWait, looks[look], THREAD_UNINT) != THREAD_TIMED_OUT;
        },
        [] {
            return xusb_mbox_released(gHost.mailboxOwner());
        });
    if (outcome == XUSB_MBOX_WAITING) {
        IOLog("SwitchUSB: falcon mailbox command %u not taken in %u ms (owner %u)\n", cmd, kAnswerMs,
              gHost.mailboxOwner());
    } else if (outcome == XUSB_MBOX_NAKED) {
        IOLog("SwitchUSB: falcon NAK %#x to mailbox command %u\n", commandWait.data, cmd);
    }
    return outcome == XUSB_MBOX_ACKED || outcome == XUSB_MBOX_RELEASED;
}

// xhci-tegra.c tegra_xusb_probe_finish's MSG_ENABLED, at the first start and after SC7, with the mailbox line on.
bool SwitchXHCI::enableMessages()
{
    const auto send = [](OSObject *owner, void *, void *, void *, void *) {
        return static_cast<SwitchXHCI *>(owner)->sendCommand(xhost::MBOX_MSG_ENABLED, 0) ? kIOReturnSuccess
                                                                                         : kIOReturnError;
    };
    return tegraLoop->runAction(send, this) == kIOReturnSuccess;
}

// The pad's role (the ID grounded before a device can connect, as tegra_xhci_set_host_mode). The ports' power is
// AppleUSBXHCI's: its root ports are power children of the controller (ArmDTPlatform PMRegisterDevice) and raise it.
void switchxhci_set_host_role(bool host)
{
    if (gHost.map())
        gHost.setHostRole(host);
}

#if DEVELOPMENT || DEBUG
void switchxhci_describe(char *buffer, size_t size)
{
    if (!size)
        return;
    if (!gHost.map()) {
        snprintf(buffer, size, "XUSB not mapped");
        return;
    }
    char pads[400];
    char ports[360];
    gHost.describePads(pads, sizeof(pads));
    gHost.describePorts(ports, sizeof(ports));
    snprintf(buffer, size, "%s; INTID 71 x%u 72 x%u; %s", ports, switch_interrupt_count(xhost::kHostIntid),
             switch_interrupt_count(xhost::kMailboxIntid), pads);
}
#endif

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
        gHost.saveContext();
        controller->tegraWorkLoop()->runAction(gated, controller, controller);
        return;
    }
    if (gRemoteWake) {
        xhost::Controller::disarmRemoteWake();
        gRemoteWake = false;
    }
    char why[160] = "";
    if (!gHost.powerOn(why, sizeof(why)) ||
        !gHost.loadFirmware(switch_xusb_firmware, xhost::firmwareBytes(), why, sizeof(why))) {
        panic("SwitchUSB: XUSB host did not come back from SC7: %s", why);
    }
    controller->tegraWorkLoop()->runAction(gated, controller, nullptr);
    if (!controller->enableMessages())
        IOLog("SwitchUSB: XUSB falcon did not take MSG_ENABLED after SC7\n");
}

// tegra_xhci_id_work's host branch on T210: the SuperSpeed port's PP off, then RESET_SSPI, with the controller running
// (xHCI takes PORTSC writes only then). The controller's reset leaves the port powered, and AppleUSBXHCI resets it each
// time it raises it from Off; so the reset is owed from the host role's start and from every controller reset, and is
// done on the role thread once the controller runs: one look at the power-off after 1 ms, the last at
// tegra_xhci_set_port_power's bound for a SuperSpeed port (1000 x 600-700 us). Right after its first run AppleUSBXHCI
// halts, saves, resets and runs the controller again: a halt or reset inside the window takes the power-off with it
// (the reset puts PP back), so RESET_SSPI is not sent then and the reset is owed to the next run. The port is not
// powered again here: AppleUSB30XHCIPort powers it when the USB 3 transport comes, which the role thread adds only
// after this.
bool SwitchXHCI::resetSuperSpeed()
{
    const unsigned port = gHost.ssPort();
    if (!superSpeedResetOwed())
        return true;
    if (!port || !controllerRuns())
        return false;
    // Taken before the work: a controller reset inside the power-off window owes it again (heldWrite32).
    __atomic_store_n(&sspiDue, false, __ATOMIC_RELEASE);
#if DEVELOPMENT || DEBUG
    const uint32_t before = gHost.portsc(port);
    os_log(OS_LOG_DEFAULT, "SwitchUSB: SuperSpeed reset requested on port %u (PORTSC %#x)", port, before);
#endif
    gHost.setPortPower(port, false);
    const auto interrupted = [this] {
        return superSpeedResetOwed() || !controllerRuns();
    };
    const bool off = xusb::settle("SuperSpeed root port power-off", 1, 700, [port, &interrupted] {
        return (gHost.portsc(port) & XHCI_PORTSC_PP) == 0 || interrupted();
    });
    if (!off && !interrupted()) {
        IOLog("SwitchUSB: SuperSpeed root port %u still powered 700 ms after PP off (PORTSC %#x)\n", port,
              gHost.portsc(port));
        superSpeedResetDue();
        return false;
    }

    // Looked at once more on the mailbox's loop just before RESET_SSPI goes out, and once after: a halt or reset
    // anywhere in the round owes the reset to the next run instead of counting this one.
    struct Attempt {
        SwitchXHCI *controller;
        bool sent, taken;
    } attempt{this, false, false};

    if (!interrupted()) {
        tegraLoop->runAction(
            [](OSObject *, void *arg, void *, void *, void *) {
                auto *round = static_cast<Attempt *>(arg);
                if (!round->controller->superSpeedResetOwed() && controllerRuns()) {
                    round->sent = true;
                    round->taken = round->controller->sendCommand(xhost::MBOX_RESET_SSPI, XUSB_MBOX_RESET_SSPI_DATA);
                }
                return kIOReturnSuccess;
            },
            this, &attempt);
    }
    if (!attempt.sent || interrupted()) {
        superSpeedResetDue();
#if DEVELOPMENT || DEBUG
        os_log(OS_LOG_DEFAULT,
               "SwitchUSB: SuperSpeed reset on port %u deferred: controller halted or reset (PORTSC %#x -> %#x)", port,
               before, gHost.portsc(port));
#endif
        return false;
    }
    if (!attempt.taken) {
        IOLog("SwitchUSB: XUSB falcon did not take RESET_SSPI\n");
        superSpeedResetDue();
    }
#if DEVELOPMENT || DEBUG
    os_log(OS_LOG_DEFAULT, "SwitchUSB: SuperSpeed reset taken on port %u (PORTSC %#x -> %#x, RESET_SSPI %s)", port,
           before, gHost.portsc(port), attempt.taken ? "taken" : "refused");
#endif
    return !superSpeedResetOwed();
}

void switchxhci_superspeed_reset_due()
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    if (controller)
        controller->superSpeedResetDue();
}

bool switchxhci_superspeed_reset_owed()
{
    const SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    return controller != nullptr && controller->superSpeedResetOwed();
}

bool switchxhci_reset_superspeed()
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    return controller == nullptr || controller->resetSuperSpeed();
}

IOService *switchxhci_copy_root_device()
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    IORegistryIterator *it =
        controller ? IORegistryIterator::iterateOver(controller, gIOServicePlane, kIORegistryIterateRecursively)
                   : nullptr;
    IOService *device = nullptr;
    if (it) {
        while (IORegistryEntry *entry = it->getNextObject()) {
            auto *candidate = OSDynamicCast(IOService, entry);
            if (!candidate || candidate->isInactive())
                continue;
            if (candidate->metaCast("IOUSBHostDevice") || candidate->metaCast("IOUSBHostInterface"))
                device = candidate;
        }
        if (device)
            device->retain();
        it->release();
    }
    return device;
}

bool switchxhci_is_off()
{
    const SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    return controller == nullptr || controller->isOff();
}

bool switchxhci_remote_wake_armed()
{
    return gRemoteWake;
}
