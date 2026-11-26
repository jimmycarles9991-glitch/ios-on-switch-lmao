// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB on the Switch's USB-C port, on Apple's own USB stacks.
//
// Device mode (on when the ADT has /arm-io/usb-device): the Tegra X1 XUSB device controller (XUDC, SwitchXUDC.h)
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
// mailbox 72, PD ALERT PK4). An interrupt that cannot be attached panics, and so does a mailbox line that keeps firing
// with nothing to take.
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
// and, in development builds, 1 s after each role or lane change one os_log record (log only):
//   SwitchUSB: <host|device> role +1 s (change <n>): xHCI USBCMD/USBSTS, pad 0's port and SS port 0's port with their
//   link state and speed in words ("U0 SuperSpeed 5 Gb/s"), PORTSC <port>:<value>...; PADCTL
//   PORT_CAP, VBUS_ID (ID, VBUS override), pad power, charger detect, OC maps | the SS lane (pad mux, port map,
//   clamps, UPHY PLL) | BM92T36 phase, STATUS1 (switch1/2, VSAFE), STATUS2, CONFIG1, VENDOR (OCP), rails and their
//   GPIOs (PX3, PK5), PSEL/OTG (PJ5, PL0) | BQ24193 REG01/05/08/09 (CHG_CONFIG, watchdog, VBUS_STAT, faults) and the
//   battery current
// The host role first hands the pad over (XUDC off the bus, ID grounded, charger-detect leftovers cleared), then gives
// the port's IOPort host-role transports as an iPad's port controller does (SwitchUSBPort.h): the connector's root
// ports power on, and raise the controller, only for those.
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

// SwitchXUDC.cpp, from the role manager: USB2 pad 0 to or from the host.
void switchxudc_set_host_owns_port(bool host);
// Whether a host's VBUS is on the port (SwitchPDPolicy.h switch_pd_usb_device_vbus); the XUDC's VBUS override follows
// it while it is on the bus.
void switchxudc_set_vbus(bool present, bool lane_ready);

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
// USB2 pad 0's role (XusbHost.h setHostRole; before or after switchxhci_start). Thread context.
void switchxhci_set_host_role(bool host);
// The falcon's RESET_SSPI for SS port 0 (tegra_xhci_id_work on T210), owed from the host role's start and from every
// controller reset. Role thread: switchxhci_reset_superspeed does it, with the port's PP off, once the controller runs
// (true when nothing is owed any more; false while it waits for the run, which wakes the role thread).
void switchxhci_superspeed_reset_due();
bool switchxhci_superspeed_reset_owed();
bool switchxhci_reset_superspeed();
// The deepest active USB host device or interface under the controller, retained, or NULL.
IOService *switchxhci_copy_root_device();
bool switchxhci_is_off();
// SwitchUSB.cpp: the role thread looks at the port again (from any context).
void switch_usb_reconsider();
// switch_usb_suspend/resume's host half: the Tegra side down for SC7, and powered with its firmware again after it.
void switchxhci_sleep(bool entering);
bool switchxhci_remote_wake_armed();  // a device on the port can wake this SC7 (PMC wake event 39)
#if DEVELOPMENT || DEBUG
// The role line's xHCI part: USBCMD, USBSTS, every PORTSC (once powered) and the pad controller.
void switchxhci_describe(char *buffer, size_t size);
#endif
#endif
