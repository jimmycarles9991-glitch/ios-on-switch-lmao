// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB2 pad 0's remote wake across SC7, in plain C with no kernel dependency, so the host tests check this same header.
// VDD_CORE goes in SC7 and the XUSB host with it; the PMC (always on) keeps the bus of a suspended device in its idle J
// state with its own pull-downs and, when the device signals resume (a K), walks the pad through the sleepwalk phases
// (it keeps driving the K the device started) and raises PMC wake event 39 (UTMIP port 0, active high: the xusb node's
// nvidia,pmc-wakeup on the Jetson Nano and the Pixel C; 44 is SuperSpeed port 0).
//
// Sources: the Tegra X1 TRM's PMC UTMIP sleepwalk registers as Linux drivers/phy/tegra/xusb-tegra210.c names them
// (PMC_UTMIP_* at the PMC base 0x7000e400, tegra210_pmc_utmi_enable_phy_sleepwalk / _disable_phy_sleepwalk), pad 0's
// offsets and bits only.
#ifndef XUSB_SLEEPWALK_H
#define XUSB_SLEEPWALK_H

#include <stdbool.h>
#include <stdint.h>

// PMC registers, from the PMC base (UTMIP port 0's offsets and bits).
#define XUSB_PMC_USB_DEBOUNCE_DEL    0x0ecu
#define XUSB_PMC_USB_AO              0x0f0u
#define XUSB_PMC_UTMIP_TRIGGERS      0x1ecu
#define XUSB_PMC_UTMIP_SAVED_STATE   0x1f0u
#define XUSB_PMC_UTMIP_TERM_PAD_CFG  0x1f8u
#define XUSB_PMC_UTMIP_SLEEP_CFG     0x1fcu
#define XUSB_PMC_UTMIP_SLEEPWALK_CFG 0x200u
#define XUSB_PMC_UTMIP_SLEEPWALK_P0  0x204u
#define XUSB_PMC_UTMIP_FAKE          0x218u
#define XUSB_PMC_UTMIP_LINE_WAKEUP   0x26cu
#define XUSB_PMC_UTMIP_MASTER_CONFIG 0x274u
#define XUSB_PMC_UTMIP_PAD_CFG0      0x4c0u
#define XUSB_PMC_UTMIP_SLEEP_CFG1    0x4d0u
#define XUSB_SLEEP_MASTER_ENABLE     (1u << 0)
#define XUSB_SLEEP_USE_PMC           ((1u << 1) | (1u << 2) | (1u << 3))  // FS/LS, PCTRL and TCTRL
#define XUSB_SLEEP_WAKE_VAL_MASK     (0xfu << 4)
#define XUSB_SLEEP_WAKE_VAL_NONE     (12u << 4)
#define XUSB_SLEEP_WAKE_VAL_ANY      (15u << 4)
#define XUSB_SLEEP_CFG1_USE_PMC      ((1u << 8) | (1u << 16))  // RPU switch low, RPD control
#define XUSB_MASTER_CONFIG_PWR       (1u << 0)
#define XUSB_DEBOUNCE_LINE_MASK      (0xfu << 16)
#define XUSB_DEBOUNCE_LINE_ONE       (1u << 16)
#define XUSB_FAKE_MASK               0xfu
#define XUSB_LINE_WAKEUP_EN          (1u << 0)
#define XUSB_USB_AO_LINE_PD          ((1u << 0) | (1u << 1))  // USBOP and USBON line-state detectors down
#define XUSB_SAVED_SPEED_MASK        3u
#define XUSB_WALK_EN                 (1u << 7)
#define XUSB_TRIGGER_ARM             ((1u << 0) | (1u << 4) | (1u << 12))  // walk pointer, capture, clear alarm
#define XUSB_TRIGGER_CLEAR_ALARM     (1u << 12)
#define XUSB_TERM_MASK               ((0x3fu << 1) | (0x3fu << 7))
#define XUSB_PAD_CFG_RPD_MASK        (0x1fu << 22)
// XUSB pad controller: the bias pad's tracked TCTRL/PCTRL and pad 0's RPD_CTRL, which the PMC takes over.
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL1 0x288u
#define XUSB_PADCTL_USB2_OTG_PAD0_CTL1 0x08cu

enum xusb_utmi_speed {
    XUSB_UTMI_HS = 0,
    XUSB_UTMI_FS = 1,
    XUSB_UTMI_LS = 2,
    XUSB_UTMI_RESET = 3,  // no device signalling at a known speed: the pad stays high-Z
};

// PORTSC's speed (the Tegra firmware's default protocol speed IDs: 1 full, 2 low, 3 high) as the PMC saves it.
static inline enum xusb_utmi_speed xusb_utmi_speed(uint32_t portsc)
{
    switch ((portsc >> 10) & 0xfu) {
    case 1:
        return XUSB_UTMI_FS;
    case 2:
        return XUSB_UTMI_LS;
    case 3:
        return XUSB_UTMI_HS;
    default:
        return XUSB_UTMI_RESET;
    }
}

// SLEEPWALK_P0: phases A to D, each with D+ and D- pulled down; phase A holds the idle state high-Z, B to D drive the
// K (D- for full and high speed, D+ for low speed, whose J and K are the other way round).
static inline uint32_t xusb_sleepwalk_phases(enum xusb_utmi_speed speed)
{
    const uint32_t pulldowns = 0x03030303u;
    const uint32_t highzA = 1u << 6, apA = 1u << 4, anA = 1u << 5;
    const uint32_t anBCD = (1u << 13) | (1u << 21) | (1u << 29), apBCD = (1u << 12) | (1u << 20) | (1u << 28);
    const uint32_t highzBCD = (1u << 14) | (1u << 22) | (1u << 30);
    switch (speed) {
    case XUSB_UTMI_HS:
    case XUSB_UTMI_FS:
        return pulldowns | highzA | apA | anBCD;
    case XUSB_UTMI_LS:
        return pulldowns | highzA | anA | apBCD;
    case XUSB_UTMI_RESET:
        break;
    }
    return pulldowns | highzA | highzBCD;
}

// TERM_PAD_CFG's PCTRL (bits 1-6) and TCTRL (bits 7-12) from USB2_BIAS_PAD_CTL1 (TCTRL bits 0-5, PCTRL bits 6-11).
static inline uint32_t xusb_sleepwalk_term(uint32_t term_pad_cfg, uint32_t bias_pad_ctl1)
{
    const uint32_t tctrl = bias_pad_ctl1 & 0x3fu, pctrl = (bias_pad_ctl1 >> 6) & 0x3fu;
    return (term_pad_cfg & ~XUSB_TERM_MASK) | pctrl << 1 | tctrl << 7;
}

// PAD_CFG0's RPD_CTRL (bits 22-26) from USB2_OTG_PAD0_CTL1 (bits 26-30).
static inline uint32_t xusb_sleepwalk_rpd(uint32_t pad_cfg, uint32_t otg_pad_ctl1)
{
    return (pad_cfg & ~XUSB_PAD_CFG_RPD_MASK) | ((otg_pad_ctl1 >> 26) & 0x1fu) << 22;
}

#endif
