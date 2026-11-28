// SPDX-License-Identifier: BSD-2-Clause-Patent
// J181's Motion & Fitness gate on the motion services, 24B119 only (SwitchHIDMotionAccess.cpp).
#pragma once
#include <IOKit/IOService.h>

#if SWITCH_IPSW_KIND_24B119
// Before the device starts, from the thread that creates the motion devices.
void switch_hid_motion_restrict(IOService *device);
#endif
