// SPDX-License-Identifier: GPL-2.0-only
// SS port 0's UPHY lane up and down (XusbLanes.h). The role thread (SwitchUSB.cpp) moves the lane; PCIe clamps it
// before it resets the UPHY (SwitchUPHY.h, on PCIe's thread); the falcon mailbox (SwitchXHCI's work loop) sets its LFPS
// detection, a register the lane steps never write. The pad controller lock (XusbPadctl.h) serialises the first two
// with the XUDC and the host: all read-modify-write SS_PORT_MAP and ELPG_PROGRAM_1, and the lane also USB3_PAD_MUX,
// which PCIe writes too for its own lane 0 and the AUX clamps. Fixed ELPG delays only, as Linux; no timer.
#include <IOKit/IOLib.h>
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPadctl.h"
#include "../../kits/SwitchCAR/SwitchUPHY.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "XusbLanes.h"
#include "XusbSuperSpeed.h"

namespace {
constexpr uint64_t kPadctlPA = 0x7009F000;

volatile uint8_t *gPadctl;
bool gUp;            // under the pad controller lock; read without it for switch_usb_ss_up
bool gUnusedPortUp;  // under the pad controller lock

uint32_t r(uint32_t offset)
{
    return switch_mmio_read32(gPadctl, offset);
}

void w(uint32_t offset, uint32_t value)
{
    switch_mmio_write32(gPadctl, offset, value);
}

bool pllLocked()
{
    return (r(XUSB_PADCTL_UPHY_PLL_P0_CTL1) & XUSB_UPHY_PLL_LOCKDET) != 0u;
}

// Lock held.
void runSteps(const xusb_elpg_step *steps)
{
    for (uint32_t i = 0; i < XUSB_SS_STEPS; ++i) {
        w(XUSB_PADCTL_ELPG_PROGRAM_1, xusb_elpg_apply(r(XUSB_PADCTL_ELPG_PROGRAM_1), steps[i]));
        if (steps[i].delay_us)
            IODelay(steps[i].delay_us);
    }
}

// Lock held, once the UPHY runs (L4T: right after tegra210_uphy_init starts it), again after SC7.
void unusedPortUp()
{
    if (gUnusedPortUp || !switch_uphy_ready() || !switch_car_soc_t210b01())
        return;
    w(XUSB_PADCTL_SS_PORT_MAP, xusb_ss_unused_port_map(r(XUSB_PADCTL_SS_PORT_MAP)));
    runSteps(xusb_ss_unused_port_steps);
    gUnusedPortUp = true;
}

// Lock held, lane clamped. The lane's register-file data is a T210B01's (L4T tegra210_pex_uphy_enable writes
// tegra210_xusb_lane_defaults only when t210b01_compatible).
void laneUp()
{
    const bool b01 = switch_car_soc_t210b01();
    const unsigned lane = xusb_ss_lane(b01);
    if (b01) {
        for (const auto &write : xusb_b01_usb3_lane_data)
            w(XUSB_PADCTL_UPHY_MISC_CTL8(lane), xusb_uphy_cfg(write[0], write[1]));
    }
    w(XUSB_PADCTL_USB3_PAD_MUX, xusb_pad_mux_ss(r(XUSB_PADCTL_USB3_PAD_MUX), b01));
    w(XUSB_PADCTL_SS_PORT_MAP, xusb_ss_port_map(r(XUSB_PADCTL_SS_PORT_MAP)));
    w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 1), xusb_ectl1(r(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 1)), b01));
    w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 2), xusb_ectl2(r(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 2))));
    w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 3), XUSB_ECTL3);
    w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 4), xusb_ectl4(r(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 4))));
    w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 6), XUSB_ECTL6);
    runSteps(xusb_ss_up_steps);
}
}  // namespace

bool switch_usb_ss_init(void)
{
    if (!gPadctl)
        gPadctl = switch_mmio_map(kPadctlPA, 0x1000);
    return gPadctl != nullptr;
}

// The UPHY announcement is read under the lock: PCIe sets it to "not ready" before its clamp takes the lock, so a lane
// that goes up here either comes before that clamp or sees the PLL is not ready.
bool switch_usb_ss_lane(bool up)
{
    if (!gPadctl)
        return false;
    switch_padctl_lock();
    unusedPortUp();
    if (up && !gUp && switch_uphy_ready() && pllLocked()) {
        laneUp();
        __atomic_store_n(&gUp, true, __ATOMIC_RELEASE);
    } else if (!up && gUp) {
        runSteps(xusb_ss_down_steps);
        w(XUSB_PADCTL_SS_PORT_MAP, xusb_ss_port_unmap(r(XUSB_PADCTL_SS_PORT_MAP)));
        __atomic_store_n(&gUp, false, __ATOMIC_RELEASE);
    }
    const bool now = gUp;
    switch_padctl_unlock();
    return now;
}

bool switch_usb_ss_up(void)
{
    return __atomic_load_n(&gUp, __ATOMIC_ACQUIRE);
}

void switch_usb_ss_clamp(void)
{
    (void)switch_usb_ss_lane(false);
}

void switch_usb_ss_lost(void)
{
    if (!gPadctl)
        return;
    const SwitchPadctlLocked locked;
    __atomic_store_n(&gUp, false, __ATOMIC_RELEASE);
    gUnusedPortUp = false;
}

void switch_usb_ss_lfps(bool enable)
{
    if (!gPadctl)
        return;
    const unsigned lane = xusb_ss_lane(switch_car_soc_t210b01());
    w(XUSB_PADCTL_UPHY_MISC_CTL1(lane), xusb_lfps_detect(r(XUSB_PADCTL_UPHY_MISC_CTL1(lane)), enable));
}

#if DEVELOPMENT || DEBUG
void switch_usb_ss_describe(char *buffer, size_t size)
{
    if (!gPadctl) {
        snprintf(buffer, size, "SS lane: PADCTL not mapped");
        return;
    }
    // The PLL's lock bit only once PCIe has calibrated it: until then the UPHY may still be held in reset.
    const uint32_t elpg = r(XUSB_PADCTL_ELPG_PROGRAM_1);
    const bool ready = switch_uphy_ready();
    const char *pll = "not calibrated by PCIe yet";
    if (ready)
        pll = pllLocked() ? "calibrated, locked" : "calibrated, not locked";
    snprintf(buffer, size, "SS lane %s (USB3_PAD_MUX %#x SS_PORT_MAP %#x ELPG_1 %#x%s, UPHY PLL %s)",
             switch_usb_ss_up() ? "up" : "down", r(XUSB_PADCTL_USB3_PAD_MUX), r(XUSB_PADCTL_SS_PORT_MAP), elpg,
             xusb_ss_clamped(elpg) ? " clamped" : "", pll);
}
#endif
