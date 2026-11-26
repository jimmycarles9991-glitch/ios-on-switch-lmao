// SPDX-License-Identifier: GPL-2.0-only
// The device-mode fake port (XusbPadctl.h), as CTCaer switch-l4t-kernel-4.9 drivers/phy/tegra/xusb-tegra210.c
// tegra210_usb2_phy_power_on (usb3_port_fake: SS_PORT_MAP, then ELPG_PROGRAM_1's VCORE_DOWN, CLAMP_EN_EARLY and
// CLAMP_EN cleared 100-200 us apart, under padctl->lock) and tegra210_usb2_phy_power_off (CLAMP_EN_EARLY, CLAMP_EN
// 100-200 us later, VCORE_DOWN 250-350 us after that, then the port unmapped), the Tegra X1 TRM's ELPG exit and entry
// orders (22.8.7.5, 22.8.7.1).
#include "XusbPadctl.h"
#include <IOKit/IOLib.h>
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "XusbLanes.h"
#include "XusbSuperSpeed.h"

void switch_usb_padctl_fake_port_up(volatile uint8_t *padctl)
{
    const SwitchPadctlLocked locked;
    if (switch_usb_ss_up())
        return;
    switch_mmio_write32(padctl, XUSB_PADCTL_SS_PORT_MAP,
                        xusb_ss_port_map(switch_mmio_read32(padctl, XUSB_PADCTL_SS_PORT_MAP)));
    for (const xusb_elpg_step &step : xusb_ss_up_steps) {
        switch_mmio_write32(padctl, XUSB_PADCTL_ELPG_PROGRAM_1,
                            xusb_elpg_apply(switch_mmio_read32(padctl, XUSB_PADCTL_ELPG_PROGRAM_1), step));
        if (step.delay_us)
            IODelay(step.delay_us);
    }
}

void switch_usb_padctl_fake_port_down(volatile uint8_t *padctl)
{
    const SwitchPadctlLocked locked;
    if (switch_usb_ss_up())
        return;
    for (const xusb_elpg_step &step : xusb_ss_down_steps) {
        switch_mmio_write32(padctl, XUSB_PADCTL_ELPG_PROGRAM_1,
                            xusb_elpg_apply(switch_mmio_read32(padctl, XUSB_PADCTL_ELPG_PROGRAM_1), step));
        if (step.delay_us)
            IODelay(step.delay_us);
    }
    switch_mmio_write32(padctl, XUSB_PADCTL_SS_PORT_MAP,
                        switch_mmio_read32(padctl, XUSB_PADCTL_SS_PORT_MAP) |
                            XUSB_SS_PORT_MAP(XUSB_SS_PORT, XUSB_SS_PORT_MAP_DISABLED));
}
