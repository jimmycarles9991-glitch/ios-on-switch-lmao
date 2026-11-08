// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB on the Switch's USB-C port, on Apple's own USB stacks.
//
// Device mode (on when the ADT has /arm-io/usb-device): the Tegra X1 XUSB device controller (XUDC, SwitchXUDC.cpp)
// under IOUSBDeviceFamily, so a Mac sees an iPad (usbmuxd: idevice*, iproxy, Finder, Xcode; ptpd photos; atc sync).
// The node carries an iPad usb-device node's identity, and IOUSBDeviceFamily reads it; configd's USBDeviceConfiguration
// plug-in picks the functions by configuration-string from /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist
// ("unknownHardware" = AppleUSBMux + PTP).
//
// Host mode (on when the ADT has /arm-io/usb-xhci): the XUSB host controller (XusbHost.cpp, falcon firmware
// tegra210b01_xusb_firmware) under IOUSBHostFamily + AppleUSBXHCI (SwitchXHCI.cpp), with Apple's hub, composite
// device, HID and mass storage drivers: keyboards, mice, flash drives, through a USB-C OTG adapter, a hub or the dock.
// SuperSpeed runs on SS port 0's UPHY lane 5 (XusbSuperSpeed.cpp) whenever DisplayPort alt mode leaves its pair free.
//
// The power role is SwitchPD's (SwitchPD.h: sink first; VBUS only to a partner proven sink-only, and SwitchPD alone
// turns it on and off). The data role follows it: SwitchPD sourcing (an OTG adapter, a keyboard, a flash drive) makes
// us the host: the host controller is brought up the first time, the XUDC goes off the bus and USB2 pad 0's ID is
// grounded. A source that leaves us the data role DFP (the dock after the alternate-mode engine's data role swap, a hub
// with power in) makes us its host the same way, while we take its power. When either ends, that is undone in reverse
// and the XUDC goes back on the bus. Nothing else changes the role (a bus suspend, a host asleep).
//
// Never blocks boot: everything runs on its own thread and work loops, and anything short of a working interrupt path
// (no node, rail, MMIO, PLL, firmware, kext or class registration) logs one line, "SwitchUSB: device mode off: <why>"
// or "SwitchUSB: host mode off: <why>", and leaves that mode alone. Interrupts only (XUDC INTID 76, xHCI 71, falcon
// mailbox 72, PD ALERT PK4). An interrupt that cannot be attached panics; a lost one panics only on positive evidence
// that it was due (the PD controller saw a cable, and the controller then shows a pending interrupt nobody took).
//
// State lines (kernel log; one per state change):
//   SwitchUSB: device mode ready (XUDC at 0x700d0000, configuration <configuration-string>)
//   SwitchUSB: functions registered, pull-up on (waiting for a host)
//   SwitchUSB: host connected (high speed)  /  bus suspended (host asleep or cable out)  /  host disconnected
//   SwitchPD: started (...)  /  one SwitchPD line per power-role decision (sink, classifying, dual-role, sink-only,
//   sourcing, not sourcing, detached), with the controller's ALERT_STATUS/STATUS1/STATUS2
//   SwitchUSB: XUSB firmware <version> (built <time>) running, HCSPARAMS1 <value>
//   SwitchUSB: host controller up (AppleUSBXHCI, <n> interrupters)
//   SwitchUSB: host role (a device on the port, VBUS on | a source that made us DFP, ...)  /  SwitchUSB: device role
//   SwitchUSB: SuperSpeed lane up|down (pairs <free|alt mode pending|DisplayPort's>, <role>, UPHY PLL <state>)
//   SwitchUSB: SuperSpeed root port 1 on: PP off ..., RESET_SSPI taken, PP on ..., UsbForcePower 1: 0  /  ... off
// and 1 s after each role or lane change and the dock's hub enable one os_log record (log only):
//   SwitchUSB: <host|device> role +1 s (change <n>): xHCI USBCMD/USBSTS, pad 0's port and SS port 0's port with their
//   link state and speed in words ("U0 SuperSpeed 5 Gb/s"), PORTSC <port>:<value>...; the last PP cycle; PADCTL
//   PORT_CAP, VBUS_ID (ID, VBUS override), pad power, charger detect, OC maps | the SS lane (pad mux, port map,
//   clamps, UPHY PLL) | BM92T36 phase, STATUS1 (switch1/2, VSAFE), STATUS2, CONFIG1, VENDOR (OCP), rails and their
//   GPIOs (PX3, PK5), PSEL/OTG (PJ5, PL0) | BQ24193 REG01/05/08/09 (CHG_CONFIG, watchdog, VBUS_STAT, faults) and the
//   battery current
// The host role first hands the pad over (XUDC off the bus, ID grounded, charger-detect leftovers cleared; later host
// roles also cycle the OTG port's PORTSC.PP as xhci-tegra.c does for T210), then reports the cable to the connector's
// root ports as an iPad's port controller does: AppleUSBXHCI powers a root port only for a cable (switchxhci_cable).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// From the board thread (SwitchRootDiag.h), thread context. Returns at once: 0 when started on its own thread or
// not configured (neither ADT node), -1 if the thread could not be created.
int switch_usb_start(void);

// System sleep (drivers/SwitchSuspend's sleep and wake actions, one thread). Device mode goes off the bus before SC7
// and is powered on whole after it, back on the bus if it was; a host controller's Tegra side goes down with SC7 and is
// powered with its firmware again before IOKit raises AppleUSBXHCI (switchxhci_sleep). No-ops without USB.
void switch_usb_suspend(void);
void switch_usb_resume(void);
// After switch_usb_suspend: a USB device on the host port can wake this sleep (PMC wake event 39, its key press).
bool switch_usb_remote_wake(void);

// Prelink cut stub (emit_prelink.py CUT_DEPENDENCIES): 8 zero bytes; a cut dependency's metaclass GOT entry points here.
extern const unsigned long long switch_prelink_cut_null;

// SwitchXUDC.cpp, from the role manager: a host's cable is in (its bus reset is due); USB2 pad 0 to or from the host.
void switchxudc_host_expected(void);
void switchxudc_set_host_owns_port(bool host);

#ifdef __cplusplus
}

class IOService;
// SwitchXUDC.cpp: loads IOUSBDeviceFamily and AppleUSBDeviceMux, registers SwitchXUDC, powers the XUDC and starts it
// on `provider`. False (with the reason in `why`) when device mode cannot run; the port is then left alone.
bool switchxudc_start(IOService *provider, char *why, size_t whySize);
// SwitchXUDC.cpp: switch_usb_suspend / switch_usb_resume's device-mode half, on the XUDC work loop.
void switchxudc_sleep(bool entering);
// SwitchXHCI.cpp: powers the XUSB host, loads its firmware and Apple's host stack, starts SwitchXHCI. Thread context.
bool switchxhci_start(char *why, size_t whySize);
// USB2 pad 0's role (XusbHost.h setHostRole; before or after switchxhci_start); with the controller running, host also
// cycles the OTG port's PORTSC.PP. Thread context.
void switchxhci_set_host_role(bool host);
void switchxhci_expect_port_change();  // a device appeared in host role: arm the lost-interrupt check
// The USB-C cable in or out in the host role: the connector's root ports powered for a device (SuperSpeed first, while
// SS port 0's lane is up), or held off. Thread context.
void switchxhci_cable(bool connected);
// SS port 0's lane came up or went down with the cable in: its root port follows (XusbLanes.h xusb_ss_port_follow).
// Role thread.
void switchxhci_superspeed();
// switch_usb_suspend/resume's host half: the Tegra side down for SC7, and powered with its firmware again after it.
void switchxhci_sleep(bool entering);
bool switchxhci_remote_wake_armed();  // a device on the port can wake this SC7 (PMC wake event 39)
// SwitchUSB.cpp: host mode off for this boot (a never-booted interrupt line did not deliver; the caller logged it).
// Thread context.
void switch_usb_host_failed();
// The role line's xHCI part: USBCMD, USBSTS, every PORTSC (once powered), the last PP cycle and the pad controller.
void switchxhci_describe(char *buffer, size_t size);
#endif
