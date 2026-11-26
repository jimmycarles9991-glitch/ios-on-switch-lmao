// SPDX-License-Identifier: GPL-2.0-only
// Device mode on USB2 pad 0: SS port 0 out of ELPG as the board's nvidia,usb3-port-fake = <0> has
// tegra210_usb2_phy_power_on take it out, and nothing else in ELPG_PROGRAM_0/1. Under the pad controller lock
// (kits/SwitchCAR/SwitchPadctl.h); thread context.
#pragma once
#include <stdint.h>
#include "../../kits/SwitchCAR/SwitchPadctl.h"

#ifdef __cplusplus
extern "C" {
#endif

void switch_usb_padctl_fake_port_up(volatile uint8_t *padctl);
// Device mode off: SS port 0 clamped again and unmapped, as tegra210_usb2_phy_power_off does, so the SuperSpeed lane
// finds it clamped whenever it programs the port.
void switch_usb_padctl_fake_port_down(volatile uint8_t *padctl);

#ifdef __cplusplus
}
#endif
