// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB on the USB-C port: start-up and the role manager (see SwitchUSB.h).
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSData.h>
#include <os/log.h>
#include <string.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../TegraI2C/TegraI2C.h"
#include "../../kits/SwitchCAR/SwitchUPHY.h"
#include "../../kits/SwitchDT/SwitchDTProperty.h"
#include "SwitchPD.h"
#include "SwitchPDPolicy.h"
#include "SwitchUSB.h"
#include "UsbDmaMapper.h"
#include "XusbLanes.h"
#include "XusbSuperSpeed.h"

extern "C" __attribute__((used)) const unsigned long long switch_prelink_cut_null = 0;

namespace {
IOService *gDevicePort;
bool gHostPackaged;

// Data role manager: SwitchPD decides the power role (and owns VBUS); the PD listener records it, a thread call (it may
// sleep: kext loads, firmware) follows it with the data role. Everything below gLock is touched only with it held,
// the apply step only from the thread call.
IOLock *gLock;
thread_call_t gRoleCall, gReportCall;
bool gWantHost, gHostCable;            // from the last PD state
bool gPadLost;                         // SC7 took PADCTL's host role with it: the role thread puts it back
bool gHostUp, gHostBroken, gHostRole;  // what has been done
bool gHostFailed;                      // switch_usb_host_failed (SwitchXHCI's interrupt check), any thread
uint32_t gRoleChanges;                 // role changes applied (the report names the one it follows)
constexpr uint32_t kReportMS = 1000;   // the role line: this long after a change, when the port has settled
// The dock (a PD source that swapped us to DFP) wants the host role too. The SuperSpeed pair follows DisplayPort's
// decision (switch_usb_set_alt_lanes, below gLock); the lane itself moves on the role thread, and is clamped on PCIe's
// thread while the UPHY PLL is recalibrated (uphyChanged).
bool gDockHost;
enum switch_usb3_lanes gLanes = SWITCH_USB3_LANES_FREE;
bool gDockHub;
bool gSsInit;              // the lane's pad controller and lock (switch_usb_ss_init)
bool gSsUp, gDockHubSeen;  // role thread: the lane as last reported

// avddio_usb (OLED DTB xudc@700d0000 / xusb@70090000) is MAX77620 LDO1 at 1.05 V, shared with PCIe (drivers/SwitchWLAN
// TegraPCIe rail(): same register, same value); normally on already. hvdd_usb (3.3 V) and avdd_pll_utmip (SD3, 1.8 V)
// are always on.
bool railOn(char *why, size_t whySize)
{
    constexpr uint8_t kPmic = 0x3C;
    constexpr uint8_t kLdo1Cfg = 0x25;
    constexpr uint8_t kModeNormal = 3u << 6;
    constexpr uint8_t kLdo1_1V05 = (1050000 - 800000) / 25000;
    uint8_t value = 0;
    int err = tegra_i2c_init(5);
    if (!err)
        err = tegra_i2c_read(5, kPmic, kLdo1Cfg, &value, 1);
    if (!err && !(value & kModeNormal)) {
        const uint8_t on = kModeNormal | kLdo1_1V05;
        err = tegra_i2c_write(5, kPmic, kLdo1Cfg, &on, 1);
        IOSleep(1);
    }
    if (err)
        snprintf(why, whySize, "PMIC LDO1 (avddio_usb) not readable: I2C5 error %d", err);
    return err == 0;
}

// SwitchPD's work loop: short. Our VBUS on a sink-only partner wants the host role; a cable from a host while we are
// its device means a bus reset is due. Nothing else (a bus suspend included) changes the role.
void pdChanged(void *, const SwitchPDState *state)
{
    IOLockLock(gLock);
    const bool sourcing = state->role == SWITCH_PD_ROLE_SOURCE;
    const bool usbHost = switch_pd_usb_host(sourcing, state->status1);
    const bool wantHost = gHostPackaged && usbHost;
    gDockHost = usbHost && !sourcing;
    const bool hostCable =
        state->role == SWITCH_PD_ROLE_SINK && state->plugged && state->vbusPresent && !state->dataHost;
    const bool changed = wantHost != gWantHost || (hostCable && !gHostCable);
    gWantHost = wantHost;
    gHostCable = hostCable;
    IOLockUnlock(gLock);
    if (changed)
        thread_call_enter(gRoleCall);
}

// The role line, kReportMS after a role change (its own thread call, re-armed by every change: the last one wins):
// one os_log record with where the port stands, so a boot shows where host mode stops. The xHCI (USBCMD, USBSTS,
// every PORTSC, the last PP cycle) and the pad controller (port 0's capability, ID and VBUS overrides, pad power,
// charger detection, over-current maps); the PD controller (phase, STATUS1/2, switches, rails' GPIOs); the charger
// (REG01/05/08/09, battery current). Log only, never the boot console.
void reportRole(thread_call_param_t, thread_call_param_t change)
{
    char xhci[720];
    char lane[200];
    char pd[300];
    char charger[260];
    switchxhci_describe(xhci, sizeof(xhci));
    switch_usb_ss_describe(lane, sizeof(lane));
    (void)switch_pd_describe(pd, sizeof(pd));
    (void)switch_battery_describe_otg(charger, sizeof(charger));
    const char *role = gHostRole ? "host" : "device";
    const unsigned number = static_cast<unsigned>(reinterpret_cast<uintptr_t>(change));
    // Under the 1 KiB os_log record (kprof keeps its lines under 900): one line, or two when it would not fit.
    if (strlen(xhci) + strlen(lane) + strlen(pd) + strlen(charger) < 820) {
        os_log(OS_LOG_DEFAULT, "SwitchUSB: %s role +1 s (change %u): %s | %s | %s | %s", role, number, xhci, lane, pd,
               charger);
    } else {
        os_log(OS_LOG_DEFAULT, "SwitchUSB: %s role +1 s (change %u, 1/2): %s", role, number, xhci);
        os_log(OS_LOG_DEFAULT, "SwitchUSB: %s role +1 s (change %u, 2/2): %s | %s | %s", role, number, lane, pd,
               charger);
    }
}

void armReport()
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(kReportMS, kMillisecondScale, &deadline);
    thread_call_cancel(gReportCall);
    thread_call_enter1_delayed(gReportCall,
                               reinterpret_cast<thread_call_param_t>(static_cast<uintptr_t>(++gRoleChanges)), deadline);
}

// Role thread: SS port 0's lane up for a host whose SuperSpeed pair DisplayPort leaves free, once PCIe has calibrated
// the UPHY PLL; down otherwise (XusbLanes.h). The lane's pads only: the root ports are switchxhci_cable's and
// switchxhci_superspeed's.
void followLane()
{
    IOLockLock(gLock);
    const enum switch_usb3_lanes lanes = gLanes;
    IOLockUnlock(gLock);
    const bool host = gHostRole && gHostUp;
    const bool up = gSsInit && switch_usb_ss_lane(xusb_ss_lane_wanted(host, lanes, switch_uphy_ready()));
    if (up == gSsUp)
        return;
    gSsUp = up;
    IOLog("SwitchUSB: SuperSpeed lane %s (pairs %s, %s role, UPHY PLL %s)\n", up ? "up" : "down",
          xusb_lanes_name(lanes), gHostRole ? "host" : "device", switch_uphy_ready() ? "calibrated" : "not calibrated");
    armReport();
}

// Role thread, at the end of every pass: the lane, its root port, and the dock hub's port event.
void applyLanes()
{
    followLane();
    switchxhci_superspeed();
    IOLockLock(gLock);
    const bool dockHub = gDockHub;
    const bool dockHost = gDockHost;
    IOLockUnlock(gLock);
    const bool host = gHostRole && gHostUp;
    if (dockHost && dockHub && host && !gDockHubSeen) {
        gDockHubSeen = true;
        switchxhci_expect_port_change();
        armReport();
    }
    if (!dockHub || !dockHost)
        gDockHubSeen = false;
}

// PCIe's thread (kits/SwitchCAR/SwitchUPHY.h): the lane is clamped before PCIe resets the UPHY; either way the role
// thread follows (the root port off, or the lane and the root port back on).
void uphyChanged(void *, bool ready)
{
    if (!ready)
        switch_usb_ss_clamp();
    thread_call_enter(gRoleCall);
}

// Thread call (THREAD_CALL_OPTIONS_ONCE: never two at once; entered again while it runs, it runs again after).
// Host: the XUDC off the bus first (as a cable pull), the pad to the host (ID grounded; PP cycled when the controller
// already runs), then the controller the first time, then SS port 0's lane, then the cable to the connector's root
// ports (switchxhci_cable), which powers them for the device already on the port. Device: the reverse, the ports held
// off and the lane clamped before the pad goes.
void applyRole(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    const bool wantHost = gWantHost;
    const bool hostCable = gHostCable;
    const bool padLost = gPadLost;
    gPadLost = false;
    IOLockUnlock(gLock);
    if (__atomic_load_n(&gHostFailed, __ATOMIC_ACQUIRE))
        gHostBroken = true;
    if (padLost && gHostRole && wantHost) {
        switchxhci_set_host_role(true);
        switchxhci_cable(true);
    }
    if (wantHost && !gHostRole && !gHostBroken) {
        char why[160] = "";
        switchxudc_set_host_owns_port(true);
        switchxhci_set_host_role(true);
        if (!gHostUp && !switchxhci_start(why, sizeof(why))) {
            IOLog("SwitchUSB: host mode off: %s\n", why);
            gHostBroken = true;
            switchxhci_set_host_role(false);
            switchxudc_set_host_owns_port(false);
            (void)switch_pd_allow_source(false);  // SwitchPD takes VBUS off and keeps the port sink-only
            return;
        }
        __atomic_store_n(&gHostUp, true, __ATOMIC_RELEASE);
        gHostRole = true;
        followLane();
        switchxhci_cable(true);
        // The dock's hub appears only once its HUB_CONTROL is acknowledged: its port event is due then (applyLanes).
        IOLog("SwitchUSB: host role (%s)\n",
              gDockHost ? "a source that made us DFP, the dock or a powered hub" : "a device on the port, VBUS on");
        if (!gDockHost)
            switchxhci_expect_port_change();
        armReport();
    } else if (!wantHost && gHostRole) {
        switchxhci_cable(false);
        gHostRole = false;
        followLane();
        switchxhci_set_host_role(false);
        switchxudc_set_host_owns_port(false);
        IOLog("SwitchUSB: device role\n");
        armReport();
    }
    if (hostCable && !gHostRole)
        switchxudc_host_expected();
    applyLanes();
}

void usbThread(void *, wait_result_t)
{
    char why[160] = "";
    if (gDevicePort) {
        if (railOn(why, sizeof(why)) && switchxudc_start(gDevicePort, why, sizeof(why))) {
            const char *configuration = switch_dt_string(gDevicePort, "configuration-string");
            IOLog("SwitchUSB: device mode ready (XUDC at 0x700d0000, configuration %s)\n",
                  configuration ? configuration : "?");
        } else {
            IOLog("SwitchUSB: device mode off: %s\n", why);
        }
    }
    if (gHostPackaged && !gDevicePort && !railOn(why, sizeof(why))) {
        IOLog("SwitchUSB: host mode off: %s\n", why);
        gHostBroken = true;
    }
    if (gHostPackaged && !gHostBroken && switch_usb_dma_reserve()) {
        IOLog("SwitchUSB: host mode off: no 8 MiB DMA bounce pool below 4 GiB\n");
        gHostBroken = true;
    }
    // The current state comes at once; the role thread call does the rest. Then a sink may attach.
    if (switch_pd_add_listener(&pdChanged, nullptr)) {
        if (gHostPackaged && !gHostBroken)
            IOLog("SwitchUSB: host mode off: no PD controller\n");
    } else if (gHostPackaged && !gHostBroken) {
        (void)switch_pd_allow_source(true);
    }
}  // returning ends the thread (Call_continuation -> thread_terminate)

IOService *publishedNode(const char *path)
{
    // ArmDTPlatform publishes every /arm-io child as an IOPlatformDevice that takes the node's place in the device
    // tree plane, so the path names the nub itself.
    const IORegistryEntry *node = IORegistryEntry::fromPath(path, gIODTPlane);
    IOService *nub = OSDynamicCast(IOService, node);
    if (node && !nub) {
        IOLog("SwitchUSB: %s was not published\n", path);
        node->release();
    }
    return nub;
}
}  // namespace

// SwitchXHCI's interrupt check gave host mode up for this boot (it logged why): SwitchPD stops sourcing and keeps the
// port sink-only, which ends the host role through pdChanged; the role thread never takes the host role again.
void switch_usb_host_failed()
{
    __atomic_store_n(&gHostFailed, true, __ATOMIC_RELEASE);
    (void)switch_pd_allow_source(false);
}

extern "C" int switch_usb_start(void)
{
    static uint32_t started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL))
        return 0;
    gDevicePort = publishedNode("/arm-io/usb-device");
    if (const IOService *host = publishedNode("/arm-io/usb-xhci")) {
        gHostPackaged = true;
        host->release();
    }
    if (!gDevicePort && !gHostPackaged)
        return 0;  // not packaged with USB
    gLock = IOLockAlloc();
    gRoleCall = gLock ? thread_call_allocate_with_options(&applyRole, nullptr, THREAD_CALL_PRIORITY_KERNEL,
                                                          THREAD_CALL_OPTIONS_ONCE)
                      : nullptr;
    gReportCall = gRoleCall ? thread_call_allocate_with_options(&reportRole, nullptr, THREAD_CALL_PRIORITY_LOW,
                                                                THREAD_CALL_OPTIONS_ONCE)
                            : nullptr;
    gSsInit = (gReportCall != nullptr) && gHostPackaged && switch_usb_ss_init();
    if (gSsInit) {
        switch_uphy_listen(&uphyChanged, nullptr);
    } else if (gReportCall && gHostPackaged) {
        IOLog("SwitchUSB: SuperSpeed off: no pad controller mapping or lock\n");
    }
    thread_t thread = THREAD_NULL;
    if (!gReportCall || kernel_thread_start(&usbThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("SwitchUSB: device mode off: no thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}

// SC7 takes the pad controller and the UPHY PLL: the lane is down and the PLL uncalibrated until PCIe's next
// calibration announces it (SwitchUPHY.h), the PLL first so the role thread cannot take the lane up in between.
extern "C" void switch_usb_suspend(void)
{
    if (gDevicePort)
        switchxudc_sleep(true);
    if (__atomic_load_n(&gHostUp, __ATOMIC_ACQUIRE))
        switchxhci_sleep(true);
    switch_uphy_lost();
    switch_usb_ss_lost();
    switch_pd_sleep(true);
}

extern "C" bool switch_usb_remote_wake()
{
    return __atomic_load_n(&gHostUp, __ATOMIC_ACQUIRE) && switchxhci_remote_wake_armed();
}

// The host block comes back here, before IOKit raises AppleUSBXHCI; the pad's host role, which the XUDC's own power-up
// does not keep, from the role thread.
extern "C" void switch_usb_resume(void)
{
    if (gDevicePort)
        switchxudc_sleep(false);
    if (__atomic_load_n(&gHostUp, __ATOMIC_ACQUIRE)) {
        switchxhci_sleep(false);
        IOLockLock(gLock);
        gPadLost = true;
        IOLockUnlock(gLock);
    }
    switch_pd_sleep(false);  // last: a sink attached while asleep is found now, with the host block already back
    if (gRoleCall)
        thread_call_enter(gRoleCall);  // a cable that came or went while asleep
}

// DisplayPort's sequencer, on the PD work loop: record and let the role thread follow.
void switch_usb_set_alt_lanes(enum switch_usb3_lanes lanes, bool dock_hub_enabled)
{
    if (!gLock || !gRoleCall)
        return;
    IOLockLock(gLock);
    const bool changed = lanes != gLanes || dock_hub_enabled != gDockHub;
    gLanes = lanes;
    gDockHub = dock_hub_enabled;
    IOLockUnlock(gLock);
    if (changed)
        thread_call_enter(gRoleCall);
}
