// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB-C port as iPadOS sees it: Apple's IOPort on /arm-io/port-usb-c-1 (IOAccessoryManager's "IOPort Auto Match",
// tools/package/adt_edit.py) and its USB transports, which carry the port's data role (XusbPortTransports.h).
// The connector's two root ports resolve that IOPort (SwitchXHCI::createPort) and are connected, so powered, only while
// its newest USB transport's data role is host.
//
// The role thread (SwitchUSB.cpp) is the only caller of switch_usb_port_follow; the IOPort, found by a matching
// notification, only wakes that thread.
#pragma once
#include <stdint.h>
#include "XusbLanes.h"

struct thread_call;

// IOAccessoryManager's port type table (iOS 17 and 27): 2 USB-C, 12 Lightning.
inline constexpr uint32_t kSwitchUSBCPortType = 2;
inline constexpr uint32_t kSwitchUSBCPortNumber = 1;

// Once, from SwitchUSB's start: the role thread call to enter when the IOPort appears. False without the matching
// notification (the port then never gets transports and its root ports stay off).
bool switch_usb_port_watch(struct thread_call *roleCall);
// Role thread: the transports for the role, with a USB 3 one while SS port 0's lane is up and, in the host role, once
// the falcon's SuperSpeed interface has been reset (superSpeedReset: nothing owed).
void switch_usb_port_follow(enum xusb_port_role role, bool laneUp, bool superSpeedReset);
bool switch_usb_port_holding_retired();
