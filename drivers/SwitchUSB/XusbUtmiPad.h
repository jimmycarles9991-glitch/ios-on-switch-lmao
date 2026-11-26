// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB2 pad 0 (the USB-C port's D+/D-), the same UTMI pad whichever controller owns the port: the XUDC
// (XudcHardware.cpp) and the XUSB host (XusbHost.cpp). Thread context; fusePage is 0x7000F000.
#pragma once
#include <stdint.h>

namespace xpad {

// PLLU with its USB, HSIC, ICUSB and 48 MHz outputs, and UTMIPLL, each started only when it is not already running.
// False if either did not lock.
bool startPlls(volatile uint8_t *car);
// The pad's fuse calibration, power and bias tracking.
void powerOn(volatile uint8_t *car, const volatile uint8_t *fusePage, volatile uint8_t *padctl);

}  // namespace xpad
