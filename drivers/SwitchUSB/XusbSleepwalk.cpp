// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB2 pad 0's remote wake across SC7 (XusbSleepwalk.h): the PMC's sleepwalk on the pad of a suspended device.
#include "XusbHost.h"
#include "XusbPorts.h"
#include "XusbSleepwalk.h"
#include <IOKit/IOLib.h>

namespace xhost {

// tegra210_pmc_utmi_enable_phy_sleepwalk's order, after AppleUSBXHCI has suspended the port for system sleep: the
// sleepwalk off and its detectors down while it is set up, the speed and pad trims the PMC needs, the phases, the
// detectors up, then the pad's electrical control to the PMC, the trigger on any line change and the wake on.
bool Controller::armRemoteWake(unsigned port)
{
    const uint32_t id = r(padctl, XUSB_USB2_VBUS_ID) & (XUSB_VBUS_ID_ID_SRC_OVR | XUSB_VBUS_ID_ID_MASK);
    if (!isPowered || !port || id != (XUSB_VBUS_ID_ID_SRC_OVR | XUSB_VBUS_ID_ID_GROUNDED))
        return false;  // the pad is not in the host role (setHostRole grounds the ID override): not ours to hand over
    const uint32_t portsc = xhci(xhci_portsc_offset(capLength(), port));
    if (!(portsc & XHCI_PORTSC_CCS))
        return false;
    const enum xusb_utmi_speed speed = xusb_utmi_speed(portsc);
    const uint32_t bias = r(padctl, XUSB_PADCTL_USB2_BIAS_PAD_CTL1), otg = r(padctl, XUSB_PADCTL_USB2_OTG_PAD0_CTL1);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, XUSB_SLEEP_MASTER_ENABLE, 0);
    pmcRmw(XUSB_PMC_UTMIP_MASTER_CONFIG, 0, XUSB_MASTER_CONFIG_PWR);
    pmcRmw(XUSB_PMC_USB_DEBOUNCE_DEL, XUSB_DEBOUNCE_LINE_MASK, XUSB_DEBOUNCE_LINE_ONE);
    pmcRmw(XUSB_PMC_UTMIP_FAKE, XUSB_FAKE_MASK, 0);
    pmcRmw(XUSB_PMC_UTMIP_LINE_WAKEUP, XUSB_LINE_WAKEUP_EN, 0);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, XUSB_SLEEP_WAKE_VAL_MASK, XUSB_SLEEP_WAKE_VAL_NONE);
    pmcRmw(XUSB_PMC_USB_AO, 0, XUSB_USB_AO_LINE_PD);
    pmcRmw(XUSB_PMC_UTMIP_SAVED_STATE, XUSB_SAVED_SPEED_MASK, speed);
    pmcRmw(XUSB_PMC_UTMIP_SLEEPWALK_CFG, 0, XUSB_WALK_EN);
    pmcRmw(XUSB_PMC_UTMIP_TRIGGERS, 0, XUSB_TRIGGER_ARM);
    pmcWrite(XUSB_PMC_UTMIP_TERM_PAD_CFG, xusb_sleepwalk_term(pmc(XUSB_PMC_UTMIP_TERM_PAD_CFG), bias));
    pmcWrite(XUSB_PMC_UTMIP_PAD_CFG0, xusb_sleepwalk_rpd(pmc(XUSB_PMC_UTMIP_PAD_CFG0), otg));
    pmcWrite(XUSB_PMC_UTMIP_SLEEPWALK_P0, xusb_sleepwalk_phases(speed));
    pmcRmw(XUSB_PMC_USB_AO, XUSB_USB_AO_LINE_PD, 0);
    IODelay(100);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, 0, XUSB_SLEEP_USE_PMC);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG1, 0, XUSB_SLEEP_CFG1_USE_PMC);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, XUSB_SLEEP_WAKE_VAL_MASK, XUSB_SLEEP_WAKE_VAL_ANY);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, 0, XUSB_SLEEP_MASTER_ENABLE);
    pmcRmw(XUSB_PMC_UTMIP_LINE_WAKEUP, 0, XUSB_LINE_WAKEUP_EN);
    return true;
}

// tegra210_pmc_utmi_disable_phy_sleepwalk: the wake off, the pad back to the XUSB, the detectors down, the alarm
// cleared. The PMC keeps its registers through SC7; this runs before the host block is powered again.
void Controller::disarmRemoteWake()
{
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, XUSB_SLEEP_MASTER_ENABLE, 0);
    pmcRmw(XUSB_PMC_UTMIP_LINE_WAKEUP, XUSB_LINE_WAKEUP_EN, 0);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, XUSB_SLEEP_USE_PMC, 0);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG1, XUSB_SLEEP_CFG1_USE_PMC, 0);
    pmcRmw(XUSB_PMC_UTMIP_SLEEP_CFG, XUSB_SLEEP_WAKE_VAL_MASK, XUSB_SLEEP_WAKE_VAL_NONE);
    pmcRmw(XUSB_PMC_USB_AO, 0, XUSB_USB_AO_LINE_PD);
    pmcRmw(XUSB_PMC_UTMIP_TRIGGERS, 0, XUSB_TRIGGER_CLEAR_ALARM);
}

}  // namespace xhost
