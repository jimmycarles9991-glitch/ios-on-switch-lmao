// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XUSB host's power-up: clocks, partitions, USB2 pad 0 and the IPFS/FPCI bridge; see XusbHost.h.
#include "XusbHost.h"
#include <IOKit/IOLib.h>
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPadctl.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "XusbHostRegisters.h"
#include "XusbLanes.h"
#include "XusbPartitions.h"
#include "XusbPorts.h"
#include "XusbUtmiPad.h"

namespace xhost {
namespace {
enum : uint32_t {  // PLLRE (L4T clk-tegra210.c, clk-tegra210b01.c pll_re_vco_params, pllre_nmp)
    PLLRE_BASE = 0x4c4,
    PLLRE_MISC0 = 0x4c8,
    PLL_ENABLE = 1u << 30,
    PLLRE_CLKIN_SEL = 1u << 22,
    PLLRE_BASE_DEFAULT_MASK = 0x1c000000,
    PLLRE_MNP_MASK = 0x1fu << 16 | 0xffu << 8 | 0xffu,  // DIVP 20:16, DIVN 15:8, DIVM 7:0
    PLLRE_MNP_672_FROM_38M4 = 70u << 8 | 4u,
    PLLRE_MNP_625_FROM_60M = 125u << 8 | 12u,
    PLLRE_MISC0_DEFAULT = 0x41000000,  // lock enable, IDDQ
    PLLRE_MISC0_IDDQ = 1u << 24,
    PLLRE_MISC0_LOCK = 1u << 27,
    FALCON_SRC_PLLRE_OUT = 5u << 29,  // clk-tegra-periph.c mux_clkm_pllp_pllre_idx
};

// pll_re_vco_params.lock_delay (L4T clk-tegra210.c) is 300 us, and clk_pll_wait_for_lock allows twice the lock time.
constexpr uint32_t kPllreLockMS = 1;
// tegra210_xusb_padctl_id_override: usleep_range(1000, 2000) between the VBUS override going off and ID grounded.
constexpr uint32_t kVbusOffMS = 2;
// The falcon at PLL_RE_OUT / 2 (XUSB divider 7.1, integer: field 2), 336 MHz from 672 or 312.5 from 625.
constexpr uint32_t kFalconDivisor = 2, kFalconDivider = (kFalconDivisor - 1) * 2;

constexpr uint32_t kFpciContext[] = {XUSB_CFG_ARU_CONTEXT_HS_PLS,
                                     XUSB_CFG_ARU_CONTEXT_FS_PLS,
                                     XUSB_CFG_ARU_CONTEXT_HSFS_SPEED,
                                     XUSB_CFG_ARU_CONTEXT_HSFS_PP,
                                     XUSB_CFG_ARU_CONTEXT,
                                     XUSB_CFG_AXI_CFG,
                                     XUSB_CFG_24,
                                     XUSB_CFG_16};
constexpr uint32_t kIpfsContext[] = {IPFS_MSI_BAR_SZ,   IPFS_MSI_AXI_BAR_ST,     IPFS_MSI_FPCI_BAR_ST, IPFS_MSI_VEC0,
                                     IPFS_MSI_EN_VEC0,  IPFS_FPCI_ERROR_MASKS,   IPFS_INTR_MASK,       IPFS_INTR_ENABLE,
                                     IPFS_UFPCI_CONFIG, IPFS_CLKGATE_HYSTERESIS, IPFS_MCCIF_FIFOCTRL};
static_assert(sizeof(kFpciContext) / sizeof(kFpciContext[0]) == Controller::kFpciContextRegs &&
                  sizeof(kIpfsContext) / sizeof(kIpfsContext[0]) == Controller::kIpfsContextRegs,
              "the context arrays are XusbHost.h's");
}  // namespace

bool Controller::map()
{
    if (isMapped)
        return true;
    car = switch_mmio_map(kCarPA, 0x1000);
    fusePage = switch_mmio_map(kFusePagePA, 0x1000);
    mc = switch_mmio_map(kMcPA, 0x1000);
    padctl = switch_mmio_map(kPadctlPA, 0x1000);
    regs = switch_mmio_map(kXhciPA, kXhciBytes);
    fpci = switch_mmio_map(kFpciPA, 0x1000);
    ipfs = switch_mmio_map(kIpfsPA, 0x1000);
    isMapped = (car != nullptr) && (fusePage != nullptr) && (mc != nullptr) && (padctl != nullptr) &&
               (regs != nullptr) && (fpci != nullptr) && (ipfs != nullptr);
    return isMapped;
}

// The XUSB top and pad controller clock on and resets released (shared with the XUDC and PCIe: never asserted).
void Controller::padctlUp()
{
    switch_mmio_write32(car, CLK_ENB_W_SET, W_XUSB);
    switch_mmio_write32(car, RST_DEV_W_CLR, W_XUSB | W_XUSB_PADCTL);
    IODelay(2);
}

// PLLRE as L4T's clock driver leaves it for the falcon (clk-pll.c clk_pll_enable over the pllre params and
// tegra210_pllre_set_defaults): VCO 672 MHz from pll_ref (38.4 MHz x 70 / 4), or on a T210B01 whose boot loader chose
// UTMIPLL's 60 MHz as its input (PLLRE_BASE CLKIN_SEL) 625 MHz (x 125 / 12; tegra210b01_clock_table_init), PLL_RE_OUT
// undivided. A PLLRE already locked at that rate is left running. Returns the falcon's rate in kHz, 0 if PLLRE did not
// lock.
uint32_t Controller::startPllre()
{
    const uint32_t base = switch_mmio_read32(car, PLLRE_BASE);
    const bool fromUtmipll = switch_car_soc_t210b01() && (base & PLLRE_CLKIN_SEL) != 0u;
    const uint32_t mnp = fromUtmipll ? PLLRE_MNP_625_FROM_60M : PLLRE_MNP_672_FROM_38M4;
    const uint32_t vcoKHz = fromUtmipll ? 625000 : 672000;
    const bool running = (base & PLL_ENABLE) != 0u && (switch_mmio_read32(car, PLLRE_MISC0) & PLLRE_MISC0_LOCK) != 0u;
    if (!running || (base & PLLRE_MNP_MASK) != mnp) {
        if (base & PLL_ENABLE) {
            switch_mmio_write32(car, PLLRE_BASE, base & ~PLL_ENABLE);
            IODelay(1);
        }
        switch_mmio_write32(car, PLLRE_BASE, base & ~(PLL_ENABLE | PLLRE_BASE_DEFAULT_MASK));
        switch_mmio_write32(car, PLLRE_MISC0, PLLRE_MISC0_DEFAULT);
        IODelay(1);
        // Tegra X1 TRM 5.1.5: IDDQ off, 5 us, then the dividers, then ENABLE.
        switch_mmio_rmw32(car, PLLRE_MISC0, PLLRE_MISC0_IDDQ, 0);
        IODelay(5);
        switch_mmio_rmw32(car, PLLRE_BASE, PLLRE_MNP_MASK, mnp);
        switch_mmio_rmw32(car, PLLRE_BASE, 0, PLL_ENABLE);
        IOSleep(kPllreLockMS);
        if (!(switch_mmio_read32(car, PLLRE_MISC0) & PLLRE_MISC0_LOCK))
            return 0;
        IODelay(2);
    }
    return vcoKHz / kFalconDivisor;
}

// A partition already on is left as it is but for its clock, which SC7 takes with the CAR. Otherwise its clock goes on
// only once the partition is (switch_pmc_power_up).
bool Controller::powerUp(unsigned partition, unsigned id, const switch_pmc_mc_client *client)
{
    if (switch_pmc_partition_is_on(partition)) {
        partitionsFound |= 1u << partition;
        switch_car_clock_enable(id);
        return true;
    }
    return switch_pmc_power_up(partition, id, id, client);
}

// tegra_xusb_probe order: clocks (their sources first, PLLU and UTMIPLL under FS and SS), the USB2 pad (shared with
// the XUDC; skipped when the XUDC already brought it up, since re-running bias tracking under a live link would glitch
// it), partitions SS (XUSBA) and host (XUSBC), then tegra_xusb_config (IPFS -> FPCI, BAR0, bus master, interrupt
// assertion) and tegra_xusb_disable_hsic_wake (T210B01). The XUSB top and pad controller are shared with the PCIe
// controller (drivers/SwitchWLAN): clocks on and resets released, never asserted.
bool Controller::powerOn(char *why, size_t whySize)
{
    const uint32_t asid = switch_mmio_read32(mc, TEGRA_SMMU_XUSB_HOST_ASID);
    if (!xusb_host_asid_usable(asid, TEGRA_SMMU_ASID_XUSB_HOST)) {
        snprintf(why, whySize, "XUSB_HOST is translated by another SMMU ASID (MC %#x = %#x)", TEGRA_SMMU_XUSB_HOST_ASID,
                 asid);
        return false;
    }
    padctlUp();
    if (!xpad::startPlls(car)) {
        snprintf(why, whySize, "PLLU or UTMIPLL did not lock (PLLU_BASE %#x UTMIPLL_HW_PWRDN_CFG0 %#x)",
                 switch_mmio_read32(car, PLLU_BASE), switch_mmio_read32(car, UTMIPLL_HW_PWRDN_CFG0));
        return false;
    }

    falconKHz = startPllre();
    if (!falconKHz) {
        snprintf(why, whySize, "PLLRE did not lock (PLLRE_BASE %#x MISC0 %#x)", switch_mmio_read32(car, PLLRE_BASE),
                 switch_mmio_read32(car, PLLRE_MISC0));
        return false;
    }

    // L4T's init table (clk-tegra210.c:3753-3759): host core 102 MHz from PLLP_OUT_XUSB, the falcon from PLL_RE_OUT at
    // up to 336 MHz, FS 48 MHz from PLLU, SS 120 MHz from PLLU's 480 MHz branch.
    switch_mmio_write32(car, CLK_SOURCE_XUSB_CORE_HOST,
                        (switch_mmio_read32(car, CLK_SOURCE_XUSB_CORE_HOST) & 0x1fffff00u) | 1u << 29 | 6);
    switch_mmio_write32(car, CLK_SOURCE_XUSB_FALCON,
                        (switch_mmio_read32(car, CLK_SOURCE_XUSB_FALCON) & 0x1fffff00u) | FALCON_SRC_PLLRE_OUT |
                            kFalconDivider);
    switch_mmio_write32(car, CLK_SOURCE_XUSB_FS,
                        (switch_mmio_read32(car, CLK_SOURCE_XUSB_FS) & 0x1fffff00u) | 2u << 29);
    switch_mmio_write32(car, CLK_SOURCE_XUSB_SS,
                        (switch_mmio_read32(car, CLK_SOURCE_XUSB_SS) & 0x1fffff00u) | 3u << 29 | 6);

    switch_padctl_modify(padctl, USB2_PAD_MUX, 3u << 18 | 3u, 1u << 18 | 1u);  // bias pad and OTG pad 0 to XUSB
    if (switch_mmio_read32(padctl, OTG_PAD0_CTL_0) & (OTG_PAD_PD | OTG_PAD_PD_ZI))
        xpad::powerOn(car, fusePage, padctl);
    switch_padctl_modify(padctl, SS_PORT_MAP, 0xfu, 0);  // SS port 0 paired with USB2 port 0 (as the XUDC)
    switch_pmc_rmw(USB_AO, 0xcu, 0);                     // ID and VBUS wake detectors powered (as the XUDC)

    // XUSBA before XUSBC, as L4T and our device boots have it; the TRM's C, B, A (22.8.7.5) is T210's ELPG exit.
    if (!powerUp(xusb::PART_XUSBA, xusb::kClockXusbSs, nullptr) ||
        !powerUp(xusb::PART_XUSBC, xusb::kClockXusbHost, &xusb::kXusbHostClient)) {
        snprintf(why, whySize, "XUSB partitions did not power up (PMC PWRGATE_STATUS %#x)",
                 switch_pmc_read(PWRGATE_STATUS));
        return false;
    }
    switch_mmio_write32(car, RST_DEV_W_CLR, W_XUSB_SS);
    switch_mmio_write32(car, RST_DEV_U_CLR, U_XUSB_HOST);
    IODelay(2);
    // tegra_xhci_exit_elpg: back from SC7, the host role's ID override goes back on before the configuration.
    if (contextSaved && hostRole) {
        const SwitchPadctlLocked locked;
        groundId();
    }

    // The xHCI's BAR as memory-mapped space, posted writes (Tegra X1 TRM 22.8.4 step 9: FPCI_BAR0_ACCESS_TYPE 0); Linux
    // leaves the reset value, non-posted I/O or config access.
    switch_mmio_rmw32(ipfs, IPFS_FPCI_BAR0, FPCI_BAR0_ACCESS_TYPE, 0);
    switch_mmio_rmw32(ipfs, IPFS_CONFIGURATION, 0, IPFS_EN_FPCI);
    IODelay(10);
    switch_mmio_rmw32(fpci, XUSB_CFG_4, CFG_4_BASE_MASK, static_cast<uint32_t>(kXhciPA) & CFG_4_BASE_MASK);
    IODelay(100);
    switch_mmio_rmw32(fpci, XUSB_CFG_1, 0, CFG_1_ENABLES);
    switch_mmio_rmw32(ipfs, IPFS_INTR_MASK, 0, IPFS_IP_INT_MASK);
    switch_mmio_write32(ipfs, IPFS_CLKGATE_HYSTERESIS, 0x80);

    if (switch_car_soc_t210b01()) {  // tegra210b01_soc.disable_hsic_wake; the T210 table leaves it off
        switch_mmio_rmw32(fpci, XUSB_CFG_ARU_C11PAGESEL, 0, HSP0);
        switch_mmio_rmw32(fpci, XUSB_CFG_HSPX_CORE_CTRL, HSIC_PLLCLK_VLD, 0);
        switch_mmio_rmw32(fpci, XUSB_CFG_ARU_C11PAGESEL, HSP0, 0);
    }
    if (contextSaved)
        restoreContext();
    isPowered = true;
    return true;
}

// tegra210_xusb_padctl_id_override: a VBUS override that is on goes off first, 1-2 ms, then ID grounded. Pad
// controller lock held.
void Controller::groundId()
{
    const uint32_t v = switch_mmio_read32(padctl, XUSB_USB2_VBUS_ID);
    if (v & XUSB_VBUS_ID_VBUS_ON) {
        switch_mmio_write32(padctl, XUSB_USB2_VBUS_ID, xusb_vbus_id_drop_vbus(v));
        IOSleep(kVbusOffMS);
    }
    switch_mmio_write32(padctl, XUSB_USB2_VBUS_ID, xusb_vbus_id_host(switch_mmio_read32(padctl, XUSB_USB2_VBUS_ID)));
}

// xhci-tegra.c tegra_xhci_save_context / tegra_xhci_restore_context: FPCI, then IPFS, in restore order.
void Controller::saveContext()
{
    for (unsigned i = 0; i < kFpciContextRegs; ++i)
        fpciContext[i] = switch_mmio_read32(fpci, kFpciContext[i]);
    for (unsigned i = 0; i < kIpfsContextRegs; ++i)
        ipfsContext[i] = switch_mmio_read32(ipfs, kIpfsContext[i]);
    contextSaved = true;
}

void Controller::restoreContext()
{
    for (unsigned i = 0; i < kFpciContextRegs; ++i)
        switch_mmio_write32(fpci, kFpciContext[i], fpciContext[i]);
    for (unsigned i = 0; i < kIpfsContextRegs; ++i)
        switch_mmio_write32(ipfs, kIpfsContext[i], ipfsContext[i]);
}

void Controller::setHostRole(bool host)
{
    hostRole = host;
    padctlUp();
    const SwitchPadctlLocked locked;
    switch_mmio_rmw32(padctl, XUSB_USB2_PORT_CAP, kPort0CapFields, XUSB_PORT0_CAP_OTG);
    if (!host) {
        switch_mmio_write32(padctl, XUSB_USB2_VBUS_ID,
                            xusb_vbus_id_device(switch_mmio_read32(padctl, XUSB_USB2_VBUS_ID)));
        return;
    }
    groundId();
    const uint32_t ctl0 = switch_mmio_read32(padctl, XUSB_CHRG_OTGPAD0_CTL0);
    const uint32_t ctl1 = switch_mmio_read32(padctl, XUSB_CHRG_OTGPAD0_CTL1);
    const uint32_t ocMap = switch_mmio_read32(padctl, XUSB_USB2_OC_MAP);
    if (ctl0 != xusb_chrg_ctl0_host(ctl0) || ctl1 != xusb_chrg_ctl1_host(ctl1) || ocMap != xusb_oc_map_host(ocMap)) {
        switch_mmio_write32(padctl, XUSB_CHRG_OTGPAD0_CTL0, xusb_chrg_ctl0_host(ctl0));
        switch_mmio_write32(padctl, XUSB_CHRG_OTGPAD0_CTL1, xusb_chrg_ctl1_host(ctl1));
        switch_mmio_write32(padctl, XUSB_USB2_OC_MAP, xusb_oc_map_host(ocMap));
        IOLog("SwitchUSB: host role: pad 0 had charger-detect or over-current settings left (CHRG_OTGPAD0 %#x/%#x, "
              "USB2_OC_MAP %#x), now %#x/%#x, %#x\n",
              ctl0, ctl1, ocMap, xusb_chrg_ctl0_host(ctl0), xusb_chrg_ctl1_host(ctl1), xusb_oc_map_host(ocMap));
    }
}

}  // namespace xhost
