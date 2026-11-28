// SPDX-License-Identifier: BSD-2-Clause-Patent
// CoreMotion's fast path on the device motion service (SwitchHIDFastPath.cpp).
#pragma once
#include <IOKit/IOService.h>

// Before the device motion device starts: its IOHIDEventDriver takes fast path clients from the moment it is
// published.
void switch_hid_fastpath_watch(IOService *device);
