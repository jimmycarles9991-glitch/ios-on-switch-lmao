// SPDX-License-Identifier: BSD-2-Clause-Patent
// The BM92T36 settings this boot changes, and their return on Shut Down and Restart. The controller keeps its registers
// for as long as it is powered, and it is powered from the battery side whether the console runs or not (it charges a
// switched-off console): a reboot or a PMIC power cycle leaves them, only its own SYS_RESET (which reloads its
// defaults: bm92txx turns over-current protection back on after every one) or the battery going flat clears them.
// Nothing here is non-volatile: neither bm92txx.c nor hekate's bm92t36.c (which only reads) ever writes the chip's
// configuration store, and bm92txx's bm92t_shutdown hands the next OS the source switches off for that same reason.
// What changes from as found: over-current protection on (VENDOR_CONFIG), incoming data role swaps accepted as UFP
// (CONFIG2), the DR_SWAP alert (ALERT_ENABLE) and the source switches (CONFIG1 SPDSRC). Each goes back to its found
// value, bit field by bit field, once nothing else will touch the controller this boot.
#include "SwitchPDService.h"

using namespace switchpd;

namespace {
uint32_t le32(const uint8_t b[4])
{
    return b[0] | static_cast<uint32_t>(b[1]) << 8 | static_cast<uint32_t>(b[2]) << 16 |
           static_cast<uint32_t>(b[3]) << 24;
}

int restoreField(uint8_t reg, uint16_t mask, uint16_t found)
{
    uint16_t now = 0;
    int err = read16(reg, now);
    const unsigned keep = static_cast<unsigned>(now) & ~static_cast<unsigned>(mask);
    const unsigned back = static_cast<unsigned>(found) & mask;
    if (!err && (static_cast<unsigned>(now) & mask) != back)
        err = write16(reg, static_cast<uint16_t>(keep | back));
    return err;
}
}  // namespace

// Before anything of ours is written, a start-up SYS_RESET included.
void SwitchPD::recordAsFound()
{
    found.valid = read16(CONFIG1, found.config1) == 0 && read16(CONFIG2, found.config2) == 0 &&
                  read16(DP_ALERT_EN, found.dpAlertEnable) == 0 && read16(VENDOR_CONFIG, found.vendor) == 0 &&
                  tegra_i2c_read(kBus, kPD, ALERT_ENABLE, found.alertEnable, sizeof(found.alertEnable)) == 0;
}

// bm92t_enable_ocp: VENDOR_CONFIG's OCP_DISABLE cleared.
int SwitchPD::enableOcp()
{
    uint16_t vendor = 0;
    int err = read16(VENDOR_CONFIG, vendor);
    if (!err && (vendor & VENDOR_CONFIG_OCP_DISABLE))
        err = write16(VENDOR_CONFIG, vendor & ~VENDOR_CONFIG_OCP_DISABLE);
    return err;
}

// An incoming DR_Swap or PR_Swap is answered by the controller itself, as CONFIG2 says. Always accepting DR_Swap
// lets hubs/monitors swap us UFP->DFP and lets a USB host (MacBook after PR_Swap) swap us DFP->UFP; accepting
// PR_Swap as source (rejecting as sink) lets a wall-powered DRP partner that initially presented Rd take back VBUS.
// DP_ALERT_EN is cleared to match bm92txx.c on boards without rohm,dp-alerts-enable.
void SwitchPD::configureRoleSwaps(bool announce)
{
    uint16_t config2 = 0;
    uint16_t dpAlertEnable = 0;
    uint8_t enable[4] = {};
    int err = read16(CONFIG2, config2);
    if (!err)
        err = read16(DP_ALERT_EN, dpAlertEnable);
    if (!err)
        err = tegra_i2c_read(kBus, kPD, ALERT_ENABLE, enable, sizeof(enable));
    if (err) {
        IOLog("SwitchPD: CONFIG2 or ALERT_ENABLE not readable (I2C %d): incoming data role swaps as found\n", err);
        return;
    }
    const uint32_t enabled = le32(enable);
    const uint16_t swapMask = CONFIG2_PR_SWAP_MASK | CONFIG2_DR_SWAP_MASK | CONFIG2_VSRC_SWAP;
    const uint16_t swapWant = CONFIG2_PR_SWAP_ACCEPT_SRC_REJECT_SNK | CONFIG2_DR_SWAP_ALWAYS_ACCEPT |
                              (vconnOn ? CONFIG2_VSRC_SWAP : 0u);
    const bool setDrSwap = (config2 & CONFIG2_DR_SWAP_MASK) != CONFIG2_DR_SWAP_ALWAYS_ACCEPT;
    const bool setPrSwap = (config2 & CONFIG2_PR_SWAP_MASK) != CONFIG2_PR_SWAP_ACCEPT_SRC_REJECT_SNK;
    const bool setConfig2 = (config2 & swapMask) != swapWant;
    const bool setDpAlert = dpAlertEnable != 0;
    const bool setAlert = (enabled & ALERT_DR_SWAP) == 0;
    if (setConfig2) {
        const uint16_t next = static_cast<uint16_t>((config2 & ~swapMask) | swapWant);
        err = write16(CONFIG2, next);
    }
    if (!err)
        vconnSwap = vconnOn;
    if (!err && setDpAlert)
        err = write16(DP_ALERT_EN, 0);
    if (!err && setAlert) {
        enable[1] = static_cast<uint8_t>(enable[1] | (ALERT_DR_SWAP >> 8));
        err = tegra_i2c_write(kBus, kPD, ALERT_ENABLE, enable, sizeof(enable));
    }
    if (announce || setConfig2 || setDpAlert || setAlert || err) {
        IOLog("SwitchPD: incoming role swaps configured%s (CONFIG2 %#x%s%s, DP_ALERT_EN %#x%s, ALERT_ENABLE %#x%s)\n",
              err ? ": write failed" : "", config2, setDrSwap ? " +DR_Swap always accept" : "",
              setPrSwap ? " +PR_Swap as source" : "", dpAlertEnable, setDpAlert ? " -> 0" : "", enabled,
              setAlert ? " +DR_SWAP" : "");
    }
}

void SwitchPD::setVconnSwap(bool accept)
{
    if (accept == vconnSwap)
        return;
    uint16_t config2 = 0;
    int err = read16(CONFIG2, config2);
    if (!err) {
        const uint16_t next = accept ? static_cast<uint16_t>(config2 | CONFIG2_VSRC_SWAP)
                                     : static_cast<uint16_t>(config2 & ~CONFIG2_VSRC_SWAP);
        if (next != config2)
            err = write16(CONFIG2, next);
    }
    if (!err)
        vconnSwap = accept;
    else
        IOLog("SwitchPD: CONFIG2 VCONN_Swap %s failed (I2C %d)\n", accept ? "on" : "off", err);
}

// Shut Down and Restart, on the work loop, before the PMIC goes down: no more events, the source rails and boost off
// (bm92t_shutdown's VCONN and vbus-source regulator), then each setting back as found.
void SwitchPD::leave()
{
    leaving = true;
    alert->disable();
    window->cancelTimeout();
    negoTimer->cancelTimeout();
    switch_pd_alt_leave();
    setSwitches(false);
    if (switch_pd_boost_on(phase))
        (void)switch_battery_set_otg_boost(false, false);
    (void)setRails(false, false);
    if (!found.valid) {
        IOLog("SwitchPD: leaving the BM92T36 as this boot set it: its settings as found were not readable\n");
        return;
    }
    int err = restoreField(CONFIG1, CONFIG1_SPDSRC_MASK, found.config1);
    if (!err)
        err = restoreField(CONFIG2, CONFIG2_PR_SWAP_MASK | CONFIG2_DR_SWAP_MASK | CONFIG2_VSRC_SWAP, found.config2);
    if (!err)
        err = restoreField(DP_ALERT_EN, 0xffffu, found.dpAlertEnable);
    if (!err)
        err = restoreField(VENDOR_CONFIG, VENDOR_CONFIG_OCP_DISABLE, found.vendor);
    uint8_t enable[4] = {};
    if (!err)
        err = tegra_i2c_read(kBus, kPD, ALERT_ENABLE, enable, sizeof(enable));
    const unsigned dr = ALERT_DR_SWAP >> 8;
    const unsigned drFound = found.alertEnable[1] & dr;
    if (!err && (enable[1] & dr) != drFound) {
        enable[1] = static_cast<uint8_t>((enable[1] & ~dr) | drFound);
        err = tegra_i2c_write(kBus, kPD, ALERT_ENABLE, enable, sizeof(enable));
    }
    IOLog("SwitchPD: BM92T36 settings %s as found (CONFIG1 %#x CONFIG2 %#x DP_ALERT_EN %#x VENDOR_CONFIG %#x "
          "ALERT_ENABLE %#x)\n",
          err ? "not all back" : "back", found.config1, found.config2, found.dpAlertEnable, found.vendor,
          le32(found.alertEnable));
}

IOReturn SwitchPD::powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t)
{
    if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) {
        auto *self = static_cast<SwitchPD *>(target);
        self->loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                static_cast<SwitchPD *>(owner)->leave();
                return kIOReturnSuccess;
            },
            self);
    }
    return kIOReturnSuccess;
}

// After begin(): registerPrioritySleepWakeInterest dereferences the root domain unchecked.
void SwitchPD::watchForExit()
{
    exitNotifier = getPMRootDomain() ? registerPrioritySleepWakeInterest(&SwitchPD::powerMessage, this) : nullptr;
    if (!exitNotifier)
        IOLog("SwitchPD: no Shut Down notification: the BM92T36 keeps this boot's settings after it\n");
}
