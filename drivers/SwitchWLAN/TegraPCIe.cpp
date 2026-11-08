// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 (T210B01, Switch OLED) PCIe root complex, root port 1 only: the BCM4356A3 Wi-Fi chip's link.
//
// Ported from MyEcoria/nxk user/wlan/src/pcie.rs and user/clk/src/main.rs (`pcie_up`, `plle`, `ungate`; MIT OR
// Apache-2.0; verified on an Erista Switch, link up and ping), with the T210B01 differences from CTCaer's
// switch-l4t-kernel-4.9 (the vendor L4T kernel switchroot runs on this exact SoC):
//   * UPHY: tegra210_pex_uphy_enable() on a "tegra210b01" padctl programs the PLL through UPHY_PLL_P0_CTL10 with
//     usb3_pll_g1_init_data instead of CTL2 CAL_CTRL / CTL5 DCO_CTRL, and gives the PCIe lane its defaults through
//     UPHY_MISC_PAD_P0_CTL8 (xusb-tegra210.c:465-545). Its register file runs on the "uphy_mgmt" clock,
//     PLL_P_UPHY_OUT = pll_p -> PLLP_MISC1 hsio/xusb gates -> CAR 0x6d0 divider (7.1) + gate bit 8, 102 MHz
//     (clk-tegra210b01.c:229, 2510-2518, 2559; tegra210b01-soc-base.dtsi xusb_padctl pcie clocks).
//   * Controller: tegra210b01_pcie_data (pci-tegra.c:3294): no MBIST WAR, no clamp threshold, no PEX IO DPD
//     handling; the B01 prod settings (tegra210b01-soc-prods.dtsi pcie@1003000: PADS_REFCLK_CFG0, RP_ECTL_1_R1/R2
//     TX_DRV_AMP) replace the T210 ECTL table nxk applies; only the DT-enabled port (pci@2,0 = port 1) is enabled.
//   * PLLE: clk_plle_tegra210_enable with the B01 spread-spectrum coefficients (clk-tegra210b01.c:448-454).
// Like nxk, the UPHY PLL and PLLE stay under software control: neither the XUSBIO PLL nor the PLLE hardware
// sequencer is started, which is the path proven on silicon. SuperSpeed USB's lane runs on the same PLL: every
// calibration is announced to it (kits/SwitchCAR/SwitchUPHY.h), so its lane is clamped while the PLL is redone.
//
// Board (switchroot tegra210b01-nx-common.dtsi:366-428, -fric-pinmux.dtsi, -fric-gpio.dtsi): pcie@1003000 port 1
// (pci@2,0) on UPHY lane pcie-0 (function pcie-x1), rails max77620 ldo1 (1.05 V) and sd3 (1.8 V, always on).
// WL_REG_ON is GPIO PH1 ("Wi-Fi Reset. Powered on by default"); PH0 is the panel's IRQ on the OLED, never touched.
// PERST# is the AFI's PEX_L1_RST_N, CLKREQ# is bi-directional per the B01 DT (tristate off, input on).
//
// Every wait here polls a status bit that has no interrupt (PLL lock, calibration, power partition, link
// training), bounded, as Linux and nxk do.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "SwitchWLANPrivate.h"
#include "../TegraI2C/TegraI2C.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchCAR/SwitchUPHY.h"

namespace swlan {
namespace {

// Register blocks (hekate bdk/soc/t210.h; Linux tegra210.dtsi pcie@1003000).
constexpr uint64_t kCarPA = 0x60006000, kGpioPA = 0x6000D000, kPinmuxPA = 0x70003000, kPmcPA = 0x7000E000;
constexpr uint64_t kMcPA = 0x70019000, kPadctlPA = 0x7009F000, kPciePA = 0x01000000, kCsPA = 0x02000000;
constexpr uint64_t kMselectPA = 0x50060000;
// Downstream windows given to the endpoint's BARs: BAR0 32 KiB + BAR2 4 MiB (4 MiB aligned), both
// non-prefetchable, inside the 208 MiB non-prefetchable aperture (nxk pcie.rs MEM_WINDOW).
constexpr uint32_t kMemWindow = 0x13000000, kMemWindowLength = 0x800000;
constexpr uint32_t kPrefetchWindow = 0x20000000, kPrefetchWindowLength = 0x400000;

// CAR (hekate bdk/soc/clock.h; nxk clk Bank tables).
enum : uint32_t {
    RST_DEV_U = 0x00C,
    CLK_OUT_ENB_U = 0x018,
    RST_DEV_U_SET = 0x310,
    RST_DEV_U_CLR = 0x314,
    CLK_ENB_U_SET = 0x330,
    CLK_ENB_U_CLR = 0x334,
    RST_DEV_W = 0x35C,
    RST_DEV_W_CLR = 0x43C,
    RST_DEV_Y = 0x2A4,
    RST_DEV_Y_SET = 0x2A8,
    RST_DEV_Y_CLR = 0x2AC,
    PLLE_SS_CNTL = 0x068,
    PLLE_BASE = 0x0E8,
    PLLE_MISC = 0x0EC,
    PLLE_AUX = 0x48C,
    PLLP_MISC1 = 0x680,
    UPHY_MGMT_DIV = 0x6D0,
};

constexpr uint32_t kUPcie = 1u << 6, kUAfi = 1u << 8, kUPcieX = 1u << 10;  // bank U
constexpr uint32_t kWPadctl = 1u << 14;                                    // bank W, reset only
constexpr uint32_t kYPexUphy = 1u << 13;                                   // bank Y, reset only (Linux reset 205)
// PLLE (CTCaer clk-pll.c clk_plle_tegra210_enable; B01 table m 2, n 125, cml 14 from 38.4 MHz -> 100 MHz).
constexpr uint32_t kPlleEnable = 1u << 31, kPlleLockOverride = 1u << 30;
constexpr uint32_t kPlleBase = (14u << 24) | (125u << 8) | 2u;
constexpr uint32_t kPlleMiscLock = 1u << 11, kPlleMiscLockEnable = 1u << 9, kPlleMiscPts = 1u << 8;
constexpr uint32_t kPlleMiscIddqSwValue = 1u << 13, kPlleMiscIddqSwCtrl = 1u << 14;
constexpr uint32_t kPlleMiscVreg = (3u << 4) | (2u << 2);  // VREG_BG_CTRL | VREG_CTRL, cleared as Linux does
constexpr uint32_t kPlleAuxUseLockdet = 1u << 3, kPlleAuxEnableSwctl = 1u << 4, kPlleAuxSsSwctl = 1u << 6;
constexpr uint32_t kPlleAuxSeqEnable = 1u << 24;
constexpr uint32_t kSsBypass = 1u << 10, kSsInterpReset = 1u << 11, kSsSscByp = 1u << 12, kSsCenter = 1u << 14;
constexpr uint32_t kSsInvert = 1u << 15, kSsCoeffMask = 0x1FFu | (0xFFu << 16) | (0x3Fu << 24);
constexpr uint32_t kSsCoeffB01 = 0x25u | (0x1u << 16) | (0x20u << 24);
// PLL_P_UPHY_OUT: 408 MHz * 2 / (6 + 2) = 102 MHz.
constexpr uint32_t kPllpMisc1Xusb = 1u << 28, kPllpMisc1Hsio = 1u << 29;
constexpr uint32_t kUphyMgmtGate = 1u << 8, kUphyMgmtDivider = 6;

// PMC (hekate bdk/soc/pmc.h, from 0x7000E400; nxk clk ungate/pex_pads_on; drivers/SwitchUSB toggles XUSB the same way).
enum : uint32_t {
    PMC_CLAMP_STATUS = 0x42C,
    PMC_PWRGATE_TOGGLE = 0x430,
    PMC_REMOVE_CLAMPING = 0x434,
    PMC_PWRGATE_STATUS = 0x438,
    PMC_IO_DPD_REQ = 0x5B8,
    PMC_IO_DPD_STATUS = 0x5BC,
};

constexpr uint32_t kPartPcie = 3;                              // TEGRA_POWERGATE_PCIE
constexpr uint32_t kDpdPexPads = 7u << 4, kDpdOff = 1u << 30;  // PEX_BIAS, PEX_CLK1, PEX_CLK2 (Linux pmc.c)

// MC SMMU (hekate bdk/mem/mc_t210.h): bit 31 of a client's ASID register routes its DMA through the SMMU.
constexpr uint32_t kMcSmmuAfiAsid = 0x238, kAsidEnable = 1u << 31;

// Pinmux pads (hekate bdk/soc/pinmux.h; nxk PADS) and their fields.
enum : uint32_t {
    PIN_PEX_WAKE = 0x040,
    PIN_PEX_L1_RST = 0x044,
    PIN_PEX_L1_CLKREQ = 0x048,
    PIN_WIFI_RST = 0x1B8
};

constexpr uint32_t kPinPm = 3u, kPinPupd = 3u << 2, kPinTristate = 1u << 4, kPinPark = 1u << 5;
constexpr uint32_t kPinInput = 1u << 6, kPinIoHv = 1u << 10, kPinOd = 1u << 11;

// GPIO port H (bank 1, port 3): CNF/OE/OUT/IN, and the masked-write views at +0x80 (bit n+8 enables bit n), so
// PH1 is changed without a read-modify-write racing TegraGPIO on the other pins of the port.
constexpr uint32_t kGpioH = 0x10C, kGpioIn = 0x30, kGpioMskCnf = 0x80, kGpioMskOe = 0x90, kGpioMskOut = 0xA0;
constexpr unsigned kWlRegOn = 1;  // PH1

// XUSB pad control (CTCaer xusb-tegra210.c).
enum : uint32_t {
    PADCTL_ELPG_PROGRAM1 = 0x024,
    PADCTL_USB3_PAD_MUX = 0x028,
    UPHY_PLL_P0_CTL1 = 0x360,
    UPHY_PLL_P0_CTL2 = 0x364,
    UPHY_PLL_P0_CTL4 = 0x36C,
    UPHY_PLL_P0_CTL8 = 0x37C,
    UPHY_PLL_P0_CTL10 = 0x384,
    UPHY_MISC_PAD_P0_CTL1 = 0x460,
    UPHY_MISC_PAD_P0_CTL8 = 0x47C,
};

constexpr uint32_t kAuxMuxLp0[3] = {1u << 29, 1u << 30, 1u << 31};  // CLAMP_EN, CLAMP_EN_EARLY, VCORE_DOWN
constexpr uint32_t kLane0FunctionMask = 3u << 12;                   // pcie-0 function 0 = pcie-x1
constexpr uint32_t kLane0IddqDisable = 1u << 1;                     // FORCE_PCIE_PAD_IDDQ_DISABLE(0)
constexpr uint32_t kCtl1Iddq = 1u << 0, kCtl1Sleep = 3u << 1, kCtl1Enable = 1u << 3, kCtl1PwrOvrd = 1u << 4;
constexpr uint32_t kCtl1Lockdet = 1u << 15, kCtl1Divs = (3u << 16) | (0xFFu << 20), kCtl1NdivUsb = 0x19u << 20;
constexpr uint32_t kCtl2CalEn = 1u << 0, kCtl2CalDone = 1u << 1, kCtl2CalOvrd = 1u << 2;
constexpr uint32_t kCtl4RefclkSel = 0xFu << 4, kCtl4RefclkbufEn = 1u << 8, kCtl4TxclkrefSel = 3u << 12;
constexpr uint32_t kCtl4TxclkrefUsb = 2u << 12, kCtl4TxclkrefEn = 1u << 15;
constexpr uint32_t kCtl8RcalEn = 1u << 12, kCtl8RcalClkEn = 1u << 13, kCtl8RcalOvrd = 1u << 15;
constexpr uint32_t kCtl8RcalDone = 1u << 31;
constexpr uint32_t kAuxRxIdleTh = 3u << 24, kAuxRxIdleTh1 = 1u << 24;

// UPHY register-file writes: CFG_ADDR(a) | CFG_WDATA(d) | CFG_RESET | CFG_WS.
constexpr uint32_t uphyCfg(uint32_t address, uint32_t data)
{
    return ((address & 0xFF) << 16) | (data & 0xFFFF) | (1u << 27) | (1u << 24);
}

constexpr uint32_t kPllG1Init[4][2] = {{0x02, 0x0000}, {0x03, 0x7051}, {0x25, 0x0130}, {0x1E, 0x0017}};
constexpr uint32_t kPcieLaneInit[2] = {0x97, 0x0080};

// PCIe block: root port 1 at +0x1000, PADS at +0x3000, AFI at +0x3800 (pci-tegra.c register list).
constexpr uint32_t kRp = 0x1000, kPads = 0x3000, kAfi = 0x3800;

enum : uint32_t {
    AFI_AXI_BAR0_SZ = 0x00,
    AFI_AXI_BAR0_START = 0x18,
    AFI_FPCI_BAR0 = 0x30,
    AFI_MSI_BAR_SZ = 0x60,
    AFI_MSI_FPCI_BAR_ST = 0x64,
    AFI_MSI_AXI_BAR_ST = 0x68,
    AFI_MSI_VEC0 = 0x6C,
    AFI_MSI_EN_VEC0 = 0x8C,
    AFI_CONFIGURATION = 0xAC,
    AFI_FPCI_ERROR_MASKS = 0xB0,
    AFI_INTR_MASK = 0xB4,
    AFI_INTR_CODE = 0xB8,
    AFI_INTR_SIGNATURE = 0xBC,
    AFI_SM_INTR_ENABLE = 0xC4,
    AFI_AFI_INTR_ENABLE = 0xC8,
    AFI_PCIE_CONFIG = 0xF8,
    AFI_FUSE = 0x104,
    AFI_PEX1_CTRL = 0x118,
    AFI_PEXBIAS_CTRL = 0x168,
    PADS_REFCLK_CFG0 = 0xC8,
};

constexpr uint32_t kAfiEnFpci = 1u << 0, kAfiClkenOverride = 1u << 31;
constexpr uint32_t kAfiIntrMaskInt = 1u << 0, kAfiIntrMaskMsi = 1u << 8;
constexpr uint32_t kAfiIntrErrors = 0x7F;  // INI/TGT slave and decode errors, write error, DFPCI/AXI decode
constexpr uint32_t kAfiCfgDisableAll = 7u << 1, kAfiCfgClkreqAsGpioAll = 7u << 29, kAfiCfgXbarMask = 0xFu << 20;
constexpr uint32_t kAfiCfgXbarX4X1 = 1u << 20, kAfiCfgPort1 = (1u << 2) | (1u << 30);
constexpr uint32_t kAfiFuseGen2Disable = 1u << 2;
constexpr uint32_t kPexRst = 1u << 0, kPexClkreqEn = 1u << 1, kPexRefclkEn = 1u << 3, kPexOverrideEn = 1u << 4;
constexpr uint32_t kPadsRefclkCfg0B01 = 0xB0B880B8;  // tegra210b01-soc-prods.dtsi prod_c_pad

// Root port registers (pci-tegra.c:141-370).
enum : uint32_t {
    RP_INTR_BCR = 0x03C,
    RP_LINK_CONTROL_STATUS = 0x090,
    RP_LINK_CONTROL_STATUS_2 = 0x0B0,
    RP_L1_PM_SS_CYA = 0xC00,
    RP_L1_PM_SS_1_CYA = 0xC04,
    RP_L1_PM_SS_2_CYA = 0xC08,
    RP_L1SS_SPARE = 0xC24,
    RP_TIMEOUT0 = 0xE24,
    RP_TIMEOUT1 = 0xE28,
    RP_ECTL_1_R1 = 0xE80,
    RP_ECTL_1_R2 = 0xEA0,
    RP_VEND_XP = 0xF00,
    RP_VEND_XP1 = 0xF04,
    RP_XP_REF = 0xF30,
    RP_VEND_CTL0 = 0xF44,
    RP_VEND_XP_BIST = 0xF4C,
    RP_VEND_XP_PAD_PWRDN = 0xF50,
    RP_PRIV_MISC = 0xFE0,
};

constexpr uint32_t kLinkDlActive = 1u << 29, kVendXpDlUp = 1u << 30;
constexpr uint32_t kVendXpOppAck = 1u << 27, kVendXpOppUpdateFc = 1u << 28, kVendXpUpdateFcMask = 0xFFu << 18;
constexpr uint32_t kPrivMiscPrsntMask = 0xF, kPrivMiscPrsnt = 0xE;
constexpr uint32_t kPrivMiscClkClamps = (1u << 23) | (1u << 31);

// PCI configuration space.
enum : uint32_t {
    CFG_ID = 0x00,
    CFG_COMMAND = 0x04,
    CFG_BAR0 = 0x10,
    CFG_BUS = 0x18,
    CFG_IO = 0x1C,
    CFG_MEM = 0x20,
    CFG_PREF = 0x24,
    CFG_PREF_BASE_UPPER = 0x28,
    CFG_PREF_LIMIT_UPPER = 0x2C,
    CFG_CAP_PTR = 0x34,
};

constexpr uint32_t kCmdIo = 1u << 0, kCmdMemory = 1u << 1, kCmdMaster = 1u << 2, kCmdIntxOff = 1u << 10;
constexpr uint32_t kCapMsi = 0x05;

uint32_t bridgeWindow(uint32_t base, uint32_t length)
{
    const auto field = [](uint32_t a) {
        return (a >> 16) & 0xFFF0u;
    };
    return (field(base + length - 1) << 16) | field(base);
}

uint32_t fpciBar(uint32_t axi)
{
    return ((((axi >> 12) & 0x0FFFFFFFu) << 4) | 1u);
}

}  // namespace

uint64_t nowNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

bool TegraPcie::map()
{
    if (mapped_)
        return true;
    mapped_ = car_.map(kCarPA, 0x1000) && gpio_.map(kGpioPA, 0x1000) && pinmux_.map(kPinmuxPA, 0x1000) &&
              pmc_.map(kPmcPA, 0x1000) && mc_.map(kMcPA, 0x1000) && padctl_.map(kPadctlPA, 0x1000) &&
              pcie_.map(kPciePA, 0x4000) && cs_.map(kCsPA, 0x1000) && mselect_.map(kMselectPA, 0x1000);
    return mapped_;
}

uint32_t TegraPcie::rpRead(uint32_t offset) const
{
    return pcie_.r32(kRp + offset);
}

void TegraPcie::rpWrite(uint32_t offset, uint32_t value) const
{
    pcie_.w32(kRp + offset, value);
}

void TegraPcie::rpModify(uint32_t offset, uint32_t clear, uint32_t set) const
{
    pcie_.modify(kRp + offset, clear, set);
}

// PCIe/XUSB rail: MAX77620 LDO1 at 1.05 V (25 mV steps from 0.8 V, mode bits 7:6 = 3 normal; hekate max7762x,
// nxk power `pcie_power`). XUSB shares it and is normally on already; only switched on if it is off.
bool TegraPcie::rail(char *why, size_t whyLength)
{
    constexpr uint8_t kPmic = 0x3C;
    constexpr uint8_t kLdo1Cfg = 0x25;
    constexpr uint8_t kModeNormal = 3u << 6;
    constexpr uint8_t kLdo1_1V05 = (1050000 - 800000) / 25000;
    int err = tegra_i2c_init(5);
    uint8_t value = 0;
    if (!err)
        err = tegra_i2c_read(5, kPmic, kLdo1Cfg, &value, 1);
    if (!err && !(value & kModeNormal)) {
        const uint8_t on = kModeNormal | kLdo1_1V05;
        err = tegra_i2c_write(5, kPmic, kLdo1Cfg, &on, 1);
        IOSleep(1);
        IOLog("SwitchWLAN: PMIC LDO1 was off (cfg %02x), switched on at 1.05 V\n", value);
    }
    if (err)
        snprintf(why, whyLength, "PMIC LDO1 (I2C5 0x3c reg 0x25): I2C error %d", err);
    return err == 0;
}

// clk_plle_tegra210_enable, B01 values. Left alone if the hardware sequencer already drives it or it is locked.
bool TegraPcie::plle(char *why, size_t whyLength)
{
    if ((car_.r32(PLLE_AUX) & kPlleAuxSeqEnable) ||
        ((car_.r32(PLLE_BASE) & kPlleEnable) && (car_.r32(PLLE_MISC) & kPlleMiscLock)))
        return true;
    car_.modify(PLLE_BASE, kPlleLockOverride, 0);
    car_.modify(PLLE_AUX, kPlleAuxSeqEnable, kPlleAuxEnableSwctl | kPlleAuxSsSwctl);
    IODelay(1);
    car_.modify(PLLE_MISC, kPlleMiscIddqSwValue | kPlleMiscVreg,
                kPlleMiscIddqSwCtrl | kPlleMiscLockEnable | kPlleMiscPts);
    IODelay(5);
    car_.modify(PLLE_SS_CNTL, 0, kSsBypass | kSsInterpReset | kSsSscByp);
    car_.w32(PLLE_BASE, kPlleBase);
    IODelay(1);
    car_.w32(PLLE_BASE, kPlleBase | kPlleEnable);
    if (!car_.wait(PLLE_MISC, kPlleMiscLock, kPlleMiscLock, 1000)) {
        snprintf(why, whyLength, "PLLE no lock (base %08x misc %08x aux %08x)", car_.r32(PLLE_BASE),
                 car_.r32(PLLE_MISC), car_.r32(PLLE_AUX));
        return false;
    }
    // Spread spectrum on: coefficients, then the bypasses off, then the interpolator out of reset.
    uint32_t ss = car_.r32(PLLE_SS_CNTL);
    ss = (ss & ~(kSsCenter | kSsInvert | kSsCoeffMask)) | kSsCoeffB01;
    car_.w32(PLLE_SS_CNTL, ss);
    ss &= ~(kSsSscByp | kSsBypass);
    car_.w32(PLLE_SS_CNTL, ss);
    IODelay(1);
    car_.w32(PLLE_SS_CNTL, ss & ~kSsInterpReset);
    IODelay(1);
    car_.modify(PLLE_AUX, 0, kPlleAuxUseLockdet);
    return true;
}

// nxk clk `pcie_up` (Linux tegra_powergate_sequence_power_up order): AFI/PCIE/PCIEXCLK/UPHY resets held, AFI and
// PCIE clocks on, partition on and unclamped, PLLE, PEX pads out of deep power-down, pad control and UPHY resets
// released, AFI and PCIE out of reset. PCIEXCLK stays held until the AFI is programmed (Linux check_ports).
// The AFI answers only after all of it: a read before hangs the bus, which is why nothing reads it earlier.
bool TegraPcie::powerUp(char *why, size_t whyLength)
{
    car_.w32(RST_DEV_U_SET, kUAfi | kUPcie | kUPcieX);
    car_.w32(RST_DEV_Y_SET, kYPexUphy);
    car_.w32(CLK_ENB_U_SET, kUAfi | kUPcie);
    IODelay(2);

    const uint32_t part = 1u << kPartPcie;
    if (!(pmc_.r32(PMC_PWRGATE_STATUS) & part)) {
        if (!switch_pmc_partition_on(kPartPcie)) {
            car_.w32(CLK_ENB_U_CLR, kUAfi | kUPcie);  // a block left half powered hangs its bus
            snprintf(why, whyLength, "PCIe power partition stays gated (PMC status %08x toggle %08x)",
                     pmc_.r32(PMC_PWRGATE_STATUS), pmc_.r32(PMC_PWRGATE_TOGGLE));
            return false;
        }
        IODelay(10);
    }
    pmc_.w32(PMC_REMOVE_CLAMPING, part);
    if (!pmc_.wait(PMC_CLAMP_STATUS, part, 0, 5000)) {
        snprintf(why, whyLength, "PCIe partition stays clamped (PMC clamp %08x)", pmc_.r32(PMC_CLAMP_STATUS));
        return false;
    }
    if (!plle(why, whyLength))
        return false;
    if (pmc_.r32(PMC_IO_DPD_STATUS) & kDpdPexPads) {
        pmc_.w32(PMC_IO_DPD_REQ, kDpdOff | kDpdPexPads);
        pmc_.wait(PMC_IO_DPD_STATUS, kDpdPexPads, 0, 250000);
    }
    // The pad control block is shared with XUSB (drivers/SwitchUSB): released, never put back in reset.
    car_.w32(RST_DEV_W_CLR, kWPadctl);
    car_.w32(RST_DEV_U_CLR, kUAfi | kUPcie);
    (void)car_.r32(RST_DEV_U);
    return true;
}

// The UPHY register file's clock (T210B01 only). The divider is left as found if the gate is already open.
bool TegraPcie::uphyManagementClock(char *why, size_t whyLength)
{
    car_.modify(PLLP_MISC1, 0, kPllpMisc1Hsio | kPllpMisc1Xusb);
    const uint32_t mgmt = car_.r32(UPHY_MGMT_DIV);
    if (!(mgmt & kUphyMgmtGate)) {
        car_.w32(UPHY_MGMT_DIV, (mgmt & ~0x1FFu) | kUphyMgmtDivider);
        IODelay(2);
        car_.w32(UPHY_MGMT_DIV, (mgmt & ~0x1FFu) | kUphyMgmtDivider | kUphyMgmtGate);
        IODelay(2);
    }
    if (!(car_.r32(UPHY_MGMT_DIV) & kUphyMgmtGate)) {
        snprintf(why, whyLength, "UPHY management clock gate stays closed (CAR 0x6d0 %08x, PLLP_MISC1 %08x)",
                 car_.r32(UPHY_MGMT_DIV), car_.r32(PLLP_MISC1));
        return false;
    }
    return true;
}

// tegra210_pex_uphy_enable for a tegra210b01 padctl with PCIe on lane 0, then the rest of tegra210_uphy_init:
// lane 0 out of IDDQ and the AUX mux clamps released. Always calibrated from scratch (nxk: after a restart it
// still reads locked, yet the link only trains once it is redone).
bool TegraPcie::uphy(char *why, size_t whyLength)
{
    const Mmio &p = padctl_;
    car_.w32(RST_DEV_Y_CLR, kYPexUphy);
    IODelay(10);
    // Lane 0 (PCIe): idle detect threshold 1 (compliance), then its B01 register-file defaults.
    p.modify(UPHY_MISC_PAD_P0_CTL1, kAuxRxIdleTh, kAuxRxIdleTh1);
    p.w32(UPHY_MISC_PAD_P0_CTL8, uphyCfg(kPcieLaneInit[0], kPcieLaneInit[1]));
    for (const auto &init : kPllG1Init)
        p.w32(UPHY_PLL_P0_CTL10, uphyCfg(init[0], init[1]));

    p.modify(UPHY_PLL_P0_CTL1, 0, kCtl1PwrOvrd);
    p.modify(UPHY_PLL_P0_CTL2, 0, kCtl2CalOvrd);
    p.modify(UPHY_PLL_P0_CTL8, 0, kCtl8RcalOvrd);
    p.modify(UPHY_PLL_P0_CTL4, kCtl4TxclkrefSel | kCtl4RefclkSel, kCtl4TxclkrefUsb | kCtl4TxclkrefEn);
    p.modify(UPHY_PLL_P0_CTL1, kCtl1Divs, kCtl1NdivUsb);
    p.modify(UPHY_PLL_P0_CTL1, kCtl1Iddq, 0);
    p.modify(UPHY_PLL_P0_CTL1, kCtl1Sleep, 0);
    IODelay(20);
    p.modify(UPHY_PLL_P0_CTL4, 0, kCtl4RefclkbufEn);

    const char *step = nullptr;
    p.modify(UPHY_PLL_P0_CTL2, 0, kCtl2CalEn);
    if (!p.wait(UPHY_PLL_P0_CTL2, kCtl2CalDone, kCtl2CalDone, 100000))
        step = "calibration start";
    if (!step) {
        p.modify(UPHY_PLL_P0_CTL2, kCtl2CalEn, 0);
        if (!p.wait(UPHY_PLL_P0_CTL2, kCtl2CalDone, 0, 100000))
            step = "calibration stop";
    }
    if (!step) {
        p.modify(UPHY_PLL_P0_CTL1, 0, kCtl1Enable);
        if (!p.wait(UPHY_PLL_P0_CTL1, kCtl1Lockdet, kCtl1Lockdet, 100000))
            step = "lock";
    }
    if (!step) {
        p.modify(UPHY_PLL_P0_CTL8, 0, kCtl8RcalEn | kCtl8RcalClkEn);
        if (!p.wait(UPHY_PLL_P0_CTL8, kCtl8RcalDone, kCtl8RcalDone, 100000))
            step = "resistor calibration start";
    }
    if (!step) {
        p.modify(UPHY_PLL_P0_CTL8, kCtl8RcalEn, 0);
        if (!p.wait(UPHY_PLL_P0_CTL8, kCtl8RcalDone, 0, 100000))
            step = "resistor calibration stop";
    }
    if (step) {
        snprintf(why, whyLength, "UPHY PLL %s timed out (CTL1 %08x CTL2 %08x CTL4 %08x CTL8 %08x, mgmt clk %08x)", step,
                 p.r32(UPHY_PLL_P0_CTL1), p.r32(UPHY_PLL_P0_CTL2), p.r32(UPHY_PLL_P0_CTL4), p.r32(UPHY_PLL_P0_CTL8),
                 car_.r32(UPHY_MGMT_DIV));
        return false;
    }
    p.modify(UPHY_PLL_P0_CTL8, kCtl8RcalClkEn, 0);
    // Power and calibration back to hardware control (nxk; Linux then starts the XUSBIO sequencer, not done here).
    p.modify(UPHY_PLL_P0_CTL1, kCtl1PwrOvrd, 0);
    p.modify(UPHY_PLL_P0_CTL2, kCtl2CalOvrd, 0);
    p.modify(UPHY_PLL_P0_CTL8, kCtl8RcalOvrd, 0);
    IODelay(20);
    // Lane 0 to pcie-x1 and out of IDDQ; the other lanes' functions stay as the loader set them (XUSB's).
    p.modify(PADCTL_USB3_PAD_MUX, kLane0FunctionMask, kLane0IddqDisable);
    // AUX mux clamps off in Linux's order (aux_mux_lp0_clamp disable): CLAMP_EN, CLAMP_EN_EARLY, VCORE_DOWN.
    for (const uint32_t bit : kAuxMuxLp0) {
        p.modify(PADCTL_ELPG_PROGRAM1, bit, 0);
        IODelay(200);
    }
    switch_uphy_announce(true);
    return true;
}

// PEX port-1 pads as the B01 DT sets them (function pe1/pe = 0, no pulls, PARK off: a parked pad follows neither
// its function nor its GPIO), and WL_REG_ON's pad driven (pull-down kept, as the DT).
void TegraPcie::pads()
{
    const uint32_t outClear = kPinPm | kPinPupd | kPinTristate | kPinPark | kPinInput | kPinOd | kPinIoHv;
    pinmux_.modify(PIN_PEX_L1_RST, outClear, 0);
    pinmux_.modify(PIN_PEX_L1_CLKREQ, outClear, kPinInput);  // bi-directional
    pinmux_.modify(PIN_PEX_WAKE, kPinPm | kPinPupd | kPinPark | kPinOd | kPinIoHv, kPinTristate | kPinInput);
    pinmux_.modify(PIN_WIFI_RST, kPinPm | kPinTristate | kPinPark | kPinInput | kPinOd, 0);
    (void)pinmux_.r32(PIN_WIFI_RST);
}

// The chip from cold, with PERST# asserted (AFI_PEX1_CTRL.RST is 0 out of reset): WL_REG_ON low 10 ms, high, then
// PCIe T_PVPERL (power stable 100 ms before PERST# lifts). Replaces brcmfmac's ChipCommon watchdog reset: the chip
// starts from whatever the boot chain left (it is "powered on by default"), and this puts it in a known state.
void TegraPcie::moduleReset()
{
    const uint32_t bit = 1u << kWlRegOn;
    const uint32_t enable = bit << 8;
    gpio_.w32(kGpioH + kGpioMskOut, enable);  // value 0 first, so the pin never glitches high
    gpio_.w32(kGpioH + kGpioMskOe, enable | bit);
    gpio_.w32(kGpioH + kGpioMskCnf, enable | bit);
    (void)gpio_.r32(kGpioH + kGpioMskOut);
    IOSleep(10);
    gpio_.w32(kGpioH + kGpioMskOut, enable | bit);
    (void)gpio_.r32(kGpioH + kGpioMskOut);
    IOSleep(100);
}

// tegra_pcie_enable_controller + tegra_pcie_setup_translations, port 1 only, PCIEXCLK still held.
void TegraPcie::controller()
{
    pcie_.w32(kPads + PADS_REFCLK_CFG0, kPadsRefclkCfg0B01);
    pcie_.w32(kAfi + AFI_PEXBIAS_CTRL, 0);
    uint32_t config = pcie_.r32(kAfi + AFI_PCIE_CONFIG);
    config |= kAfiCfgDisableAll | kAfiCfgClkreqAsGpioAll;
    config &= ~(kAfiCfgPort1 | kAfiCfgXbarMask);
    config |= kAfiCfgXbarX4X1;
    pcie_.w32(kAfi + AFI_PCIE_CONFIG, config);
    pcie_.modify(kAfi + AFI_FUSE, kAfiFuseGen2Disable, 0);
    pcie_.modify(kAfi + AFI_CONFIGURATION, 0, kAfiEnFpci | kAfiClkenOverride);
    pcie_.w32(kAfi + AFI_AFI_INTR_ENABLE, kAfiIntrErrors);
    pcie_.w32(kAfi + AFI_SM_INTR_ENABLE, 0xFFFFFFFF);
    pcie_.w32(kAfi + AFI_INTR_MASK, kAfiIntrMaskInt);
    pcie_.w32(kAfi + AFI_FPCI_ERROR_MASKS, 0);
    // Window 0: one configuration page, retargeted per access (selectConfigPage). 1: no I/O. 2/3: the endpoint's
    // memory. 4/5 and MSI unused until msiEnable.
    const uint32_t windows[6][3] = {
        {static_cast<uint32_t>(kCsPA), 1, 0},
        {0, 0, 0},
        {kPrefetchWindow, kPrefetchWindowLength >> 12, fpciBar(kPrefetchWindow)},
        {kMemWindow, kMemWindowLength >> 12, fpciBar(kMemWindow)},
        {0, 0, 0},
        {0, 0, 0},
    };
    for (uint32_t i = 0; i < 6; ++i) {
        pcie_.w32(kAfi + AFI_AXI_BAR0_START + 4 * i, windows[i][0]);
        pcie_.w32(kAfi + AFI_AXI_BAR0_SZ + 4 * i, windows[i][1]);
        if (i)
            pcie_.w32(kAfi + AFI_FPCI_BAR0 + 4 * i, windows[i][2]);
    }
    cfgPage_ = ~0u;
    pcie_.w32(kAfi + AFI_MSI_FPCI_BAR_ST, 0);
    pcie_.w32(kAfi + AFI_MSI_AXI_BAR_ST, 0);
    pcie_.w32(kAfi + AFI_MSI_BAR_SZ, 0);
}

void TegraPcie::portReset()
{
    pcie_.modify(kAfi + AFI_PEX1_CTRL, kPexRst, 0);
    IOSleep(2);
    pcie_.modify(kAfi + AFI_PEX1_CTRL, 0, kPexRst);
}

// tegra_pcie_port_enable + tegra_pcie_enable_rp_features + tegra_pcie_apply_sw_war (B01 flags) + the B01
// RP prods. REFCLK is forced on (CLKREQ_EN | OVERRIDE_EN, nxk): the link does not depend on the chip's CLKREQ#.
void TegraPcie::portEnable()
{
    pcie_.modify(kAfi + AFI_PEX1_CTRL, 0, kPexRefclkEn | kPexClkreqEn | kPexOverrideEn);
    portReset();
    rpModify(RP_ECTL_1_R1, 0x3F, 0x27);  // TX_DRV_AMP_1C (prod_c_rp)
    rpModify(RP_ECTL_1_R2, 0x3F, 0x27);
    rpModify(RP_VEND_XP, 0, kVendXpOppAck | kVendXpOppUpdateFc);
    rpModify(RP_PRIV_MISC, 0, kPrivMiscClkClamps);
    rpModify(RP_VEND_XP1, 0, 1u << 21);      // L1 ASPM support advertised (not enabled)
    rpModify(RP_VEND_XP_BIST, 0, 1u << 28);  // DLLPs finish before L1/L2 entry
    // Gen1 only for training (some endpoints never train when both speeds are offered); Gen2 is not attempted yet.
    rpModify(RP_LINK_CONTROL_STATUS_2, 0xF, 1);
    rpModify(RP_L1SS_SPARE, (1u << 16) | (1u << 17), 0);  // no LTR message interrupts
    rpModify(RP_INTR_BCR, 0, 0xFF);
    rpModify(RP_VEND_XP_PAD_PWRDN, 0,
             (1u << 2) | (1u << 1) | (1u << 0) | (1u << 15) | (3u << 5) | (3u << 3) | (3u << 16));
    rpModify(RP_L1_PM_SS_1_CYA, 0x1FFu << 13, 0x27u << 13);  // l1ss_rp_wakeup_war
    rpModify(RP_VEND_CTL0, 0xFu << 12, 0x9u << 12);          // link_speed_war
    rpModify(RP_VEND_XP, kVendXpUpdateFcMask, 0x60u << 18);  // updateFC_timer_expire_war
    // clk_m is 19.2 MHz: the timer settings Linux applies then.
    rpModify(RP_TIMEOUT0, 0xFFu | (0xFFFFu << 8) | (0xFFu << 24), 0xAu | (0x180u << 8) | (0xAu << 24));
    rpModify(RP_TIMEOUT1, (0xFFu << 16) | (0xFFu << 24), (0x10u << 16) | (0x74u << 24));
    rpModify(RP_XP_REF, 0xFFu | (0x1FFFFu << 14), 0x14u | (1u << 8) | (1u << 13) | (0x1770u << 14));
    rpModify(RP_L1_PM_SS_1_CYA, 0x1FFF, 0x26);
    rpModify(RP_L1_PM_SS_2_CYA, 0x1FFFu | (0xFFu << 13) | (0xFu << 21), 0x4Du | (0x13u << 13) | (0x2u << 21));
    rpModify(RP_L1_PM_SS_CYA, 0xFFu << 8, 0x1Eu << 8);
    rpModify(RP_L1_PM_SS_CYA, (3u << 16) | (0x1Fu << 19), (1u << 16) | (7u << 19));
    rpModify(RP_PRIV_MISC, kPrivMiscPrsntMask, kPrivMiscPrsnt);  // presence detection overridden: present
}

// tegra_pcie_port_check_link: data link up, then DL_LINK_ACTIVE, each up to ~200 ms, three tries with a PERST#
// pulse between them.
bool TegraPcie::portLink()
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (pcie_.waitMs(kRp + RP_VEND_XP, kVendXpDlUp, kVendXpDlUp, 200) &&
            pcie_.waitMs(kRp + RP_LINK_CONTROL_STATUS, kLinkDlActive, kLinkDlActive, 200))
            return true;
        portReset();
    }
    return false;
}

// AFI configuration window 0 aimed at the page holding `address` (tegra_pcie_conf_offset, bus 1 device 0; nxk
// cfg_read): FPCI type-1 extended configuration space starts at 0xfe10000000.
void TegraPcie::selectConfigPage(uint32_t address)
{
    const uint32_t page = address & ~0xFFFu;
    if (page == cfgPage_)
        return;
    pcie_.w32(kAfi + AFI_FPCI_BAR0, 0xFE100000u + (page >> 8));
    (void)pcie_.r32(kAfi + AFI_FPCI_BAR0);
    cfgPage_ = page;
}

uint32_t TegraPcie::epConfigRead(uint32_t offset)
{
    const uint32_t address = ((offset & 0xF00u) << 16) | (1u << 16) | (offset & 0xFCu);
    selectConfigPage(address);
    return cs_.r32(address & 0xFFC);
}

void TegraPcie::epConfigWrite(uint32_t offset, uint32_t value)
{
    const uint32_t address = ((offset & 0xF00u) << 16) | (1u << 16) | (offset & 0xFCu);
    selectConfigPage(address);
    cs_.w32(address & 0xFFC, value);
    if (cs_.r32(address & 0xFFC) != value)
        cs_.w32(address & 0xFFC, value);
}

// nxk `enumerate`: root port 1 as a bridge to bus 1 with its memory windows, then the endpoint's BARs sized and
// placed (BAR0 32 KiB, BAR2 4 MiB aligned), memory decoding and bus mastering on.
bool TegraPcie::enumerate(Endpoint &ep, char *why, size_t whyLength)
{
    rpWrite(CFG_BUS, (1u << 16) | (1u << 8));
    rpWrite(CFG_IO, 0xF0);  // I/O base above limit: no I/O window
    rpWrite(CFG_MEM, bridgeWindow(kMemWindow, kMemWindowLength));
    rpWrite(CFG_PREF, bridgeWindow(kPrefetchWindow, kPrefetchWindowLength));
    rpWrite(CFG_PREF_BASE_UPPER, 0);
    rpWrite(CFG_PREF_LIMIT_UPPER, 0);
    rpWrite(CFG_COMMAND, (rpRead(CFG_COMMAND) & 0xFFFF) | kCmdMemory | kCmdMaster);

    const uint32_t id = epConfigRead(CFG_ID);
    if (id == 0 || id == 0xFFFFFFFF) {
        snprintf(why, whyLength, "no endpoint behind port 1 (config id %08x, AFI code %u at %08x)", id,
                 pcie_.r32(kAfi + AFI_INTR_CODE) & 0x1F, pcie_.r32(kAfi + AFI_INTR_SIGNATURE));
        return false;
    }
    ep.vendor = static_cast<uint16_t>(id);
    ep.device = static_cast<uint16_t>(id >> 16);
    ep.bar0 = ep.bar2 = 0;

    uint32_t next[2] = {kMemWindow, kPrefetchWindow};
    const uint32_t end[2] = {kMemWindow + kMemWindowLength, kPrefetchWindow + kPrefetchWindowLength};
    for (uint32_t i = 0; i < 6;) {
        const uint32_t offset = CFG_BAR0 + 4 * i;
        epConfigWrite(offset, 0xFFFFFFFF);
        const uint32_t probe = epConfigRead(offset);
        epConfigWrite(offset, 0);
        const bool memory = (probe != 0u) && ((probe & 1) == 0u);
        if (!memory) {
            ++i;
            continue;
        }
        const bool wide = (probe & 6) == 4;
        const bool prefetchable = (probe & 8) != 0;
        const uint32_t size = ~(probe & ~0xFu) + 1;
        if (!size) {
            i += wide ? 2 : 1;
            continue;
        }
        uint32_t &at = next[prefetchable];
        const uint32_t align = size < 0x1000 ? 0x1000 : size;
        const uint32_t address = (at + align - 1) & ~(align - 1);
        if (address + size > end[prefetchable] || address + size < address) {
            snprintf(why, whyLength, "BAR%u of %u bytes does not fit the %s window", i, size,
                     prefetchable ? "prefetchable" : "memory");
            return false;
        }
        epConfigWrite(offset, address);
        if (wide)
            epConfigWrite(offset + 4, 0);
        if (i == 0)
            ep.bar0 = address;
        if (i == 2)
            ep.bar2 = address;
        at = address + size;
        i += wide ? 2 : 1;
    }
    const uint32_t command = epConfigRead(CFG_COMMAND) & 0xFFFF;
    epConfigWrite(CFG_COMMAND, (command & ~kCmdIo) | kCmdMemory | kCmdMaster);
    if (!ep.bar0 || !ep.bar2) {
        snprintf(why, whyLength, "endpoint %04x:%04x without BAR0/BAR2 (got %08x/%08x)", ep.vendor, ep.device, ep.bar0,
                 ep.bar2);
        return false;
    }
    const uint32_t link = rpRead(RP_LINK_CONTROL_STATUS);
    ep.linkSpeed = static_cast<uint8_t>((link >> 16) & 0xF);
    ep.linkWidth = static_cast<uint8_t>((link >> 20) & 0x3F);
    return true;
}

void TegraPcie::linkReport(char *out, size_t length)
{
    snprintf(out, length,
             "pads rst/clkreq/wake %03x/%03x/%03x PH in %02x; PEX1_CTRL %x link %08x xp %08x; UPHY CTL1 %08x ELPG1 "
             "%08x mux %08x mgmt %08x; PLLE %08x/%08x; DPD %x; rst U %08x Y %08x",
             pinmux_.r32(PIN_PEX_L1_RST) & 0xFFF, pinmux_.r32(PIN_PEX_L1_CLKREQ) & 0xFFF,
             pinmux_.r32(PIN_PEX_WAKE) & 0xFFF, gpio_.r32(kGpioH + kGpioIn) & 0xFF, pcie_.r32(kAfi + AFI_PEX1_CTRL),
             rpRead(RP_LINK_CONTROL_STATUS), rpRead(RP_VEND_XP), padctl_.r32(UPHY_PLL_P0_CTL1),
             padctl_.r32(PADCTL_ELPG_PROGRAM1), padctl_.r32(PADCTL_USB3_PAD_MUX), car_.r32(UPHY_MGMT_DIV),
             car_.r32(PLLE_BASE), car_.r32(PLLE_MISC), (pmc_.r32(PMC_IO_DPD_STATUS) & kDpdPexPads) >> 4,
             car_.r32(RST_DEV_U), car_.r32(RST_DEV_Y));
}

void TegraPcie::describe(char *out, size_t length)
{
    snprintf(out, length, "AFI code %u sig %08x msi %08x mask %08x; port1 link %08x xp %08x; ep cmd %08x",
             pcie_.r32(kAfi + AFI_INTR_CODE) & 0x1F, pcie_.r32(kAfi + AFI_INTR_SIGNATURE),
             pcie_.r32(kAfi + AFI_MSI_VEC0), pcie_.r32(kAfi + AFI_INTR_MASK), rpRead(RP_LINK_CONTROL_STATUS),
             rpRead(RP_VEND_XP), epConfigRead(CFG_COMMAND));
}

bool TegraPcie::bringUp(Endpoint &ep, char *why, size_t whyLength)
{
    if (!map()) {
        snprintf(why, whyLength, "register mapping failed");
        return false;
    }
    // powerUp holds the UPHY in reset and uphy() recalibrates its PLL: SuperSpeed's lane is clamped first.
    switch_uphy_announce(false);
    if (!rail(why, whyLength))
        return false;
    // MSELECT: wrap transactions for normal non-cacheable/GRE mappings, no error responses (tegra_pcie_enable_wrap;
    // hekate's L4T launcher sets the same bits).
    mselect_.modify(0, 1u << 24, 1u << 28);
    // The AFI's DMA must not go through the SMMU, which nothing here programs (MC_SMMU_AFI_ASID bit 31).
    if (mc_.r32(kMcSmmuAfiAsid) & kAsidEnable) {
        IOLog("SwitchWLAN: AFI was routed through the SMMU (asid %08x), now direct\n", mc_.r32(kMcSmmuAfiAsid));
        mc_.w32(kMcSmmuAfiAsid, 0);
    }
    if (!powerUp(why, whyLength) || !uphyManagementClock(why, whyLength))
        return false;
    // First AFI/root-port access: powered, clocked and out of reset by now. A hang right after this line means the
    // partition or clocks are not what their status registers said.
    IOLog("SwitchWLAN: PCIe controller powered (PLLE %08x, partition on), probing the root port\n",
          car_.r32(PLLE_MISC));
    const uint32_t rpId = rpRead(CFG_ID);
    if ((rpId & 0xFFFF) != 0x10DE) {
        snprintf(why, whyLength, "no NVIDIA root port at port 1 (read %08x)", rpId);
        return false;
    }
    pads();
    moduleReset();
    if (!uphy(why, whyLength))
        return false;
    controller();
    portEnable();
    IODelay(100);  // clocks latch before PCIEXCLK leaves reset (tegra_pcie_check_ports)
    car_.w32(RST_DEV_U_CLR, kUPcieX);
    (void)car_.r32(RST_DEV_U);
    IOSleep(20);  // receiver detection takes at most 20 ms
    if (!portLink()) {
        char report[320];
        linkReport(report, sizeof(report));
        snprintf(why, whyLength, "no link on port 1 [%s]", report);
        return false;
    }
    return enumerate(ep, why, whyLength);
}

bool TegraPcie::restart(Endpoint &ep, char *why, size_t whyLength)
{
    pcie_.modify(kAfi + AFI_PEX1_CTRL, kPexRst, 0);  // PERST# asserted while the chip powers off and on
    (void)pcie_.r32(kAfi + AFI_PEX1_CTRL);
    moduleReset();
    pcie_.modify(kAfi + AFI_PEX1_CTRL, 0, kPexRst);
    IOSleep(20);
    if (!portLink()) {
        char report[320];
        linkReport(report, sizeof(report));
        snprintf(why, whyLength, "no link after the chip reset [%s]", report);
        return false;
    }
    cfgPage_ = ~0u;
    return enumerate(ep, why, whyLength);
}

bool TegraPcie::endpointDecodes()
{
    return (epConfigRead(CFG_COMMAND) & kCmdMemory) != 0;
}

bool TegraPcie::msiEnable(uint32_t msiPA, char *why, size_t whyLength)
{
    // The chip's MSI capability (brcmfmac: at 0x58, 64-bit address). Found through the capability list.
    uint32_t at = epConfigRead(CFG_CAP_PTR) & 0xFC;
    uint32_t cap = 0;
    for (int hops = 0; hops < 16 && at >= 0x40; ++hops) {
        const uint32_t header = epConfigRead(at);
        if ((header & 0xFF) == kCapMsi) {
            cap = at;
            break;
        }
        at = (header >> 8) & 0xFC;
    }
    if (!cap) {
        snprintf(why, whyLength, "chip has no MSI capability");
        return false;
    }

    pcie_.w32(kAfi + AFI_MSI_FPCI_BAR_ST, msiPA >> 8);
    pcie_.w32(kAfi + AFI_MSI_AXI_BAR_ST, msiPA);
    pcie_.w32(kAfi + AFI_MSI_BAR_SZ, 1);   // 4 KiB units
    pcie_.w32(kAfi + AFI_MSI_EN_VEC0, 1);  // vector 0 only
    pcie_.w32(kAfi + AFI_MSI_VEC0, 0xFFFFFFFF);
    pcie_.modify(kAfi + AFI_INTR_MASK, 0, kAfiIntrMaskMsi);

    const uint32_t control = epConfigRead(cap) >> 16;
    const bool wide = (control & (1u << 7)) != 0u;
    epConfigWrite(cap + 4, msiPA);
    if (wide)
        epConfigWrite(cap + 8, 0);
    const uint32_t dataOffset = cap + (wide ? 0xC : 0x8);
    epConfigWrite(dataOffset, epConfigRead(dataOffset) & 0xFFFF0000u);      // message data 0 = vector 0
    epConfigWrite(cap, (epConfigRead(cap) & ~(0x7Fu << 16)) | (1u << 16));  // one vector, enabled
    epConfigWrite(CFG_COMMAND, (epConfigRead(CFG_COMMAND) & 0xFFFF) | kCmdIntxOff);
    return true;
}

uint32_t TegraPcie::msiTake()
{
    const uint32_t pending = pcie_.r32(kAfi + AFI_MSI_VEC0);
    if (pending)
        pcie_.w32(kAfi + AFI_MSI_VEC0, pending);
    return pending;
}

uint32_t TegraPcie::msiPending() const
{
    return pcie_.r32(kAfi + AFI_MSI_VEC0);
}

}  // namespace swlan
