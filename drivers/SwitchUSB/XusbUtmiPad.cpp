// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB2 pad 0's bring-up; see XusbUtmiPad.h.
#include "XusbUtmiPad.h"
#include <IOKit/IOLib.h>
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPadctl.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace xpad {
namespace {
enum : uint32_t {  // clock and reset controller (hekate bdk/soc/clock.h)
    PLLU_BASE = 0x0c0,
    PLLU_MISC = 0x0cc,
    CLK_ENB_Y_SET = 0x29c,
    CLK_ENB_Y_CLR = 0x2a0,
    UTMIP_PLL_CFG0 = 0x480,
    UTMIP_PLL_CFG1 = 0x484,
    UTMIP_PLL_CFG2 = 0x488,
    UTMIPLL_HW_PWRDN_CFG0 = 0x52c,
    CLK_SOURCE_USB2_HSIC_TRK = 0x6cc,
    Y_USB2_TRK = 1u << 18,
    PLL_LOCK = 1u << 27,
    PLL_ENABLE = 1u << 30,
    UTMIPLL_LOCK = 1u << 31,
    PLLU_IDDQ = 1u << 31,  // PLLU_MISC
    PLLU_EN_LCKDET = 1u << 29,
};

enum : uint32_t {  // from 0x7000F800
    FUSE_USB_CALIB = 0x1f0,
    FUSE_USB_CALIB_EXT = 0x350
};

constexpr uint32_t kFuseOffset = 0x800;

enum : uint32_t {  // XUSB pad controller (hekate usb_t210.h)
    USB2_PORT_CAP = 0x008,
    BATTERY_CHRG_OTGPAD0_CTL0 = 0x080,
    BATTERY_CHRG_OTGPAD0_CTL1 = 0x084,
    OTG_PAD0_CTL_0 = 0x088,
    OTG_PAD0_CTL_1 = 0x08c,
    BIAS_PAD_CTL_0 = 0x284,
    BIAS_PAD_CTL_1 = 0x288,
};

enum : uint32_t {           // L4T xusb-tegra210.c
    PORT0_CAP_MASK = 0xfu,  // CAP 1:0, INTERNAL 2, REVERSE_ID 3: "PORTN_CAP = OTG_CAP, REVERSE_ID = NO" (TRM 22.9.11.1)
    PORT0_CAP_OTG = 3u,
    VREG_FIX18 = 1u << 6,
    VREG_LEV_MASK = 3u << 7,
    VREG_LEV_1 = 1u << 7,
    HS_CURR_LEVEL_MASK = 0x3fu,
    TERM_RANGE_ADJ_MASK = 0xfu << 3,
    RPD_CTRL_MASK = 0x1fu << 26,
    PD_CHG = 1u << 0,
    RPD_RPU_OVRD = 1u << 16 | 1u << 18 | 1u << 20 | 1u << 22,  // USBOP_RPD, USBOP_RPU, USBON_RPD, USBON_RPU
    USB2_OTG_PD2 = 1u << 27,
    USB2_OTG_PD2_OVRD_EN = 1u << 28,
    USB2_OTG_PD = 1u << 26,
    USB2_OTG_PD_ZI = 1u << 29,
    USB2_OTG_PD_DR = 1u << 2,
    BIAS_PAD_PD = 1u << 11,
    HS_DISCON_LEVEL_MASK = 7u << 3,
    HS_SQUELCH_LEVEL_MASK = 7u << 0,
    USB2_PD_TRK = 1u << 26,
};

// tegra210_utmi_bias_pad_power_on's HS_DISCON_LEVEL; FUSE_USB_CALIB 31:29 is the squelch level (HS_SQUELCH_SHIFT).
constexpr uint32_t kHsDisconLevel = 7u << 3, kFuseSquelchShift = 29;

// prod_c_utmi0 (mask 0x20, value 0x40: bit 5 cleared) over BATTERY_CHRG_OTGPAD0_CTL1, tegra210-prods.dtsi:602-606 and
// tegra210b01-soc-prods.dtsi:895-899 alike.
constexpr uint32_t kProdUtmiMask = 1u << 5;

// prod_c_bias over BIAS_PAD_CTL_0: l4t-hardware-soc-t210 tegra210-prods.dtsi:592-596 (T210), and on T210B01
// tegra210b01-soc-prods.dtsi:878-889 its asic and char tuples, both applied (tegra_prod.c reads every child node).
constexpr uint32_t kProdBiasMask = 0x3f, kProdBias = 0x3a, kProdBiasB01Mask = 0x700, kProdBiasB01 = 0x300;

// usb2-0's nvidia,hs_curr_level_offset in every T210B01 Switch's device tree (switch-l4t-platform-t210-nx
// tegra210b01-nx-common.dtsi:412; the T210 boards' tegra210-odin-common.dtsi has none), added to the fuse's level and
// masked to the field as tegra210_usb2_phy_power_on does.
constexpr uint32_t kB01HsCurrLevelOffset = 6;

constexpr uint32_t kUsb2Pads = 4, kUsb2PadStride = 0x40;

// Tegra X1 TRM 5.1.5 (p.56): IDDQ off, then 5 us (10 us for UTMIPLL) before the PLL is programmed and enabled. Lock
// takes 7-10 us for PLLU and 10-15 us for UTMIPLL (XUSB_PLL_CFG0, p.171); each is read once after twice its maximum,
// as clk_pll_wait_for_lock allows twice the lock time.
constexpr uint32_t kPlluIddqUs = 5, kUtmipllIddqUs = 10, kPlluLockUs = 20, kUtmipllLockUs = 30;

// USB2_TRK_START_TIMER 18:12 and USB2_TRK_DONE_RESET_TIMER 25:19.
constexpr uint32_t kTrkTimersMask = 0x7fu << 12 | 0x7fu << 19, kTrkTimers = 0x1eu << 12 | 0xau << 19;
}  // namespace

// hekate clock_enable_pllu and clock_enable_utmipll, in the Tegra X1 TRM's PLL start order (5.1.5).
bool startPlls(volatile uint8_t *car)
{
    const uint32_t pllu = switch_mmio_read32(car, PLLU_BASE);
    if (!(pllu & PLL_ENABLE) || !(pllu & PLL_LOCK)) {
        switch_mmio_write32(car, PLLU_MISC, (switch_mmio_read32(car, PLLU_MISC) & ~PLLU_IDDQ) | PLLU_EN_LCKDET);
        IODelay(kPlluIddqUs);
        const uint32_t cfg = (pllu & 0xffe00000u) | (1u << 24) | (1u << 16) | (0x19u << 8) | 2;
        switch_mmio_write32(car, PLLU_BASE, cfg);
        switch_mmio_write32(car, PLLU_BASE, cfg | PLL_ENABLE);
        IODelay(kPlluLockUs);
        if (!(switch_mmio_read32(car, PLLU_BASE) & PLL_LOCK))
            return false;
        IODelay(8);
    }
    switch_mmio_write32(car, PLLU_BASE,
                        switch_mmio_read32(car, PLLU_BASE) | 0x2e00000u);  // USB, HSIC, ICUSB and 48 MHz outputs

    if (!(switch_mmio_read32(car, UTMIPLL_HW_PWRDN_CFG0) & UTMIPLL_LOCK)) {  // 38.4 MHz * 25 = 960 MHz
        switch_mmio_write32(car, UTMIPLL_HW_PWRDN_CFG0,
                            (switch_mmio_read32(car, UTMIPLL_HW_PWRDN_CFG0) & ~3u) |
                                1);  // software IDDQ control, IDDQ off
        IODelay(kUtmipllIddqUs);
        switch_mmio_write32(car, UTMIP_PLL_CFG0,
                            (switch_mmio_read32(car, UTMIP_PLL_CFG0) & 0xff0000ffu) | (25u << 16) | (1u << 8));
        switch_mmio_write32(car, UTMIP_PLL_CFG2, (switch_mmio_read32(car, UTMIP_PLL_CFG2) & 0xff00003fu) | (24u << 18));
        switch_mmio_write32(car, UTMIP_PLL_CFG1,
                            (switch_mmio_read32(car, UTMIP_PLL_CFG1) & 0x07ffa000u) | (1u << 15) | 375);
        IODelay(kUtmipllLockUs);
        if (!(switch_mmio_read32(car, UTMIPLL_HW_PWRDN_CFG0) & UTMIPLL_LOCK))
            return false;
    }
    switch_mmio_write32(car, UTMIP_PLL_CFG2,
                        (switch_mmio_read32(car, UTMIP_PLL_CFG2) & 0xfeffffe8u) | 0x2000008u | 0x20 | 2);
    IODelay(2);
    return true;
}

// L4T xusb-tegra210.c tegra210_usb2_phy_power_on, then tegra210_utmi_pad_power_on.
void powerOn(volatile uint8_t *car, const volatile uint8_t *fusePage, volatile uint8_t *padctl)
{
    switch_car_fuse_readable();
    const uint32_t calib = switch_mmio_read32(fusePage, kFuseOffset + FUSE_USB_CALIB);
    const uint32_t calibExt = switch_mmio_read32(fusePage, kFuseOffset + FUSE_USB_CALIB_EXT);
    const SwitchPadctlLocked locked;
    const bool b01 = switch_car_soc_t210b01();
    if (b01) {
        // tegra210_xusb_padctl_disable_pad_protection, at L4T's UPHY init and resume on a T210B01: every USB2 pad.
        for (uint32_t pad = 0; pad < kUsb2Pads; ++pad)
            switch_mmio_rmw32(padctl, BATTERY_CHRG_OTGPAD0_CTL1 + pad * kUsb2PadStride, 0, VREG_FIX18);
    }
    switch_mmio_rmw32(padctl, BATTERY_CHRG_OTGPAD0_CTL1, kProdUtmiMask, 0);
    switch_mmio_rmw32(padctl, BIAS_PAD_CTL_0, kProdBiasMask, kProdBias);
    if (b01)
        switch_mmio_rmw32(padctl, BIAS_PAD_CTL_0, kProdBiasB01Mask, kProdBiasB01);
    switch_mmio_rmw32(padctl, USB2_PORT_CAP, PORT0_CAP_MASK, PORT0_CAP_OTG);
    const uint32_t hsCurrLevel = ((calib & 0x3f) + (b01 ? kB01HsCurrLevelOffset : 0)) & HS_CURR_LEVEL_MASK;
    switch_mmio_rmw32(padctl, OTG_PAD0_CTL_0, USB2_OTG_PD_ZI | HS_CURR_LEVEL_MASK, hsCurrLevel);
    switch_mmio_rmw32(padctl, OTG_PAD0_CTL_1, TERM_RANGE_ADJ_MASK | RPD_CTRL_MASK,
                      ((calib >> 7) & 0xf) << 3 | (calibExt & 0x1f) << 26);
    switch_mmio_rmw32(padctl, BATTERY_CHRG_OTGPAD0_CTL1, VREG_LEV_MASK, VREG_LEV_1);

    // Charger detection off, as tegra210_xusb_padctl_utmi_pad_charger_detect_off leaves the pad after a detection.
    switch_mmio_rmw32(padctl, BATTERY_CHRG_OTGPAD0_CTL1, RPD_RPU_OVRD, 0);
    switch_mmio_rmw32(padctl, BATTERY_CHRG_OTGPAD0_CTL0, 0, PD_CHG);
    switch_mmio_rmw32(padctl, OTG_PAD0_CTL_0, USB2_OTG_PD2 | USB2_OTG_PD2_OVRD_EN, 0);

    // L4T tegra210_utmi_bias_pad_power_on: tracking runs, its clock on, for as long as the pad is powered.
    switch_mmio_write32(car, CLK_ENB_Y_SET, Y_USB2_TRK);
    switch_mmio_write32(car, CLK_SOURCE_USB2_HSIC_TRK,
                        (switch_mmio_read32(car, CLK_SOURCE_USB2_HSIC_TRK) & 0xffffff00u) | 6);
    switch_mmio_rmw32(padctl, BIAS_PAD_CTL_1, kTrkTimersMask, kTrkTimers);
    switch_mmio_rmw32(padctl, BIAS_PAD_CTL_0, BIAS_PAD_PD | HS_DISCON_LEVEL_MASK | HS_SQUELCH_LEVEL_MASK,
                      kHsDisconLevel | calib >> kFuseSquelchShift);
    IODelay(1);
    switch_mmio_rmw32(padctl, BIAS_PAD_CTL_1, USB2_PD_TRK, 0);

    // tegra210_utmi_pad_power_on.
    IODelay(2);
    switch_mmio_rmw32(padctl, OTG_PAD0_CTL_0, USB2_OTG_PD, 0);
    switch_mmio_rmw32(padctl, OTG_PAD0_CTL_1, USB2_OTG_PD_DR, 0);
}

}  // namespace xpad
