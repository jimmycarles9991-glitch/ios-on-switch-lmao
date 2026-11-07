// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 XUSB device controller hardware layer; see XudcHardware.h.
#include "XudcHardware.h"
#include <IOKit/IOLib.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace xudc {
namespace {
// Physical bases: Tegra X1 TRM; hekate bdk/soc/t210.h. Page-aligned maps.
constexpr uint64_t kCarPA = 0x60006000, kPmcPagePA = 0x7000E000, kFusePagePA = 0x7000F000, kMcPA = 0x70019000,
                   kPadctlPA = 0x7009F000, kXudcPA = 0x700D0000;
constexpr uint32_t kPmcOffset = 0x400, kFuseOffset = 0x800, kXudcBytes = 0xA000;

enum : uint32_t {  // clock and reset controller (hekate bdk/soc/clock.h)
    RST_DEVICES_U = 0x00c,
    CLK_OUT_ENB_H = 0x014,
    CLK_OUT_ENB_U = 0x018,
    PLLU_BASE = 0x0c0,
    PLLU_OUTA = 0x0c4,
    PLLU_MISC = 0x0cc,
    CLK_ENB_Y_SET = 0x29c,
    CLK_ENB_Y_CLR = 0x2a0,
    RST_DEV_L_SET = 0x300,
    RST_DEV_U_SET = 0x310,
    RST_DEV_U_CLR = 0x314,
    CLK_ENB_L_CLR = 0x324,
    CLK_ENB_H_SET = 0x328,
    CLK_ENB_H_CLR = 0x32c,
    CLK_ENB_U_SET = 0x330,
    RST_DEVICES_W = 0x35c,
    CLK_OUT_ENB_W = 0x364,
    RST_DEV_W_SET = 0x438,
    RST_DEV_W_CLR = 0x43c,
    CLK_ENB_W_SET = 0x448,
    UTMIP_PLL_CFG0 = 0x480,
    UTMIP_PLL_CFG1 = 0x484,
    UTMIP_PLL_CFG2 = 0x488,
    UTMIPLL_HW_PWRDN_CFG0 = 0x52c,
    CLK_SOURCE_XUSB_FS = 0x608,
    CLK_SOURCE_XUSB_CORE_DEV = 0x60c,
    CLK_SOURCE_XUSB_SS = 0x610,
    CLK_SOURCE_USB2_HSIC_TRK = 0x6cc,
};

enum : uint32_t {
    L_USBD = 1u << 22,
    H_FUSE = 1u << 7,
    U_XUSB_DEV = 1u << 31,
    Y_USB2_TRK = 1u << 18,
    W_XUSB_PADCTL = 1u << 14,
    W_XUSB = 1u << 15,
    W_XUSB_SS = 1u << 28,
    PLL_LOCK = 1u << 27,
    PLL_ENABLE = 1u << 30,
    UTMIPLL_LOCK = 1u << 31,
};

enum : uint32_t {  // PMC (from 0x7000E400; hekate bdk/soc/pmc_t210.h)
    PWRGATE_TOGGLE = 0x30,
    REMOVE_CLAMPING = 0x34,
    PWRGATE_STATUS = 0x38,
    USB_AO = 0xf0,
    TOGGLE_START = 1u << 8,
    PART_XUSBA = 20,
    PART_XUSBB = 21,
};

enum : uint32_t {
    FUSE_USB_CALIB = 0x1f0,
    FUSE_USB_CALIB_EXT = 0x350
};  // from 0x7000F800

enum : uint32_t {
    MC_SMMU_XUSB_DEV_ASID = 0x28c
};  // Linux tegra210.c, bit 31 = translate

enum : uint32_t {  // XUSB pad controller (hekate usb_t210.h)
    USB2_PAD_MUX = 0x004,
    USB2_PORT_CAP = 0x008,
    SS_PORT_MAP = 0x014,
    ELPG_PROGRAM_0 = 0x020,
    ELPG_PROGRAM_1 = 0x024,
    BATTERY_CHRG_OTGPAD0_CTL0 = 0x080,
    BATTERY_CHRG_OTGPAD0_CTL1 = 0x084,
    OTG_PAD0_CTL_0 = 0x088,
    OTG_PAD0_CTL_1 = 0x08c,
    BIAS_PAD_CTL_0 = 0x284,
    BIAS_PAD_CTL_1 = 0x288,
    USB2_VBUS_ID = 0xc60,
};

enum : uint32_t {  // XUDC: xHCI-like block at +0, FPCI config at +0x8000, IPFS at +0x9000 (Linux tegra_xudc.c)
    DB = 0x04,
    ERSTSZ = 0x08,
    ERST0BALO = 0x10,
    ERST0BAHI = 0x14,
    ERST1BALO = 0x18,
    ERST1BAHI = 0x1c,
    ERDPLO = 0x20,
    ERDPHI = 0x24,
    EREPLO = 0x28,
    EREPHI = 0x2c,
    CTRL = 0x30,
    ST = 0x34,
    RT_IMOD = 0x38,
    PORTSC = 0x3c,
    ECPLO = 0x40,
    ECPHI = 0x44,
    PORTPM = 0x4c,
    EP_HALT = 0x50,
    EP_PAUSE = 0x54,
    EP_RELOAD = 0x58,
    EP_STCHG = 0x5c,
    PORTHALT = 0x6c,
    EP_THREAD_ACTIVE = 0x74,
    CFG_DEV_FE = 0x85c,
    FPCI_CFG_1 = 0x8004,
    FPCI_CFG_4 = 0x8010,
    FPCI_CFG_5 = 0x8014,
    IPFS_CONFIGURATION = 0x9180,
    IPFS_INTR_MASK = 0x9188,
};

enum : uint32_t {
    CTRL_RUN = 1u << 0,
    CTRL_LSE = 1u << 1,
    CTRL_IE = 1u << 4,
    CTRL_ENABLE = 1u << 31,
    ST_RC = 1u << 0,
    ST_IP = 1u << 4,
    EREP_ECS = 1u << 0,
    ERDP_EHB = 1u << 3,
    PORTSC_CCS = 1u << 0,
    PORTSC_PED = 1u << 1,
    PORTSC_PR = 1u << 4,
    PORTSC_PLS_MASK = 0xfu << 5,
    PORTSC_PLS_U0 = 0u << 5,
    PORTSC_PLS_U3 = 3u << 5,
    PORTSC_PLS_RXDETECT = 5u << 5,
    PORTSC_PS_FS = 1u << 10,
    PORTSC_PS_MASK = 0xfu << 10,
    PORTSC_LWS = 1u << 16,
    PORTSC_CSC = 1u << 17,
    PORTSC_WRC = 1u << 19,
    PORTSC_PRC = 1u << 21,
    PORTSC_PLC = 1u << 22,
    PORTSC_CEC = 1u << 23,
    PORTSC_WPR = 1u << 30,
    PORTSC_CHANGES = PORTSC_CSC | PORTSC_WRC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC,
    PORTPM_L1S_MASK = 3u,
    PORTPM_L1S_NYET = 2u,  // no USB2 LPM (Linux tegra210 soc: lpm_enable = false)
    PORTHALT_HALT_LTSSM = 1u << 0,
    PORTHALT_STCHG_REQ = 1u << 20,
    FE_PORTREGSEL_MASK = 3u,
    FE_PORTREGSEL_HSFS = 2u,
    VBUS_ID_VBUS_OVR_MASK = 3u << 12,
    VBUS_ID_VBUS_OVR_EN = 1u << 12,
    VBUS_ID_VBUS_ON = 1u << 14,
    VBUS_ID_SRC_MASK = 3u << 16,
    VBUS_ID_SRC_ID_OVR_EN = 1u << 16,
    VBUS_ID_OVR_MASK = 0xfu << 18,
    VBUS_ID_OVR_FLOAT = 8u << 18,
};

// Control area: 32 endpoint contexts, EP0's transfer ring (data endpoints have rings of their own). The event ring,
// two segments of kEventSegmentTrbs (XudcTransfer.h: sized for every event the rings can owe), is a buffer of its own.
constexpr uint32_t kContextOffset = 0x0800, kRingOffset = 0x1000, kControlBytes = 0x4000;
constexpr uint32_t kEventBytes = 2 * kEventSegmentTrbs * 16;

inline void barrier()
{
    __asm__ volatile("dsb sy" ::: "memory");
}
}  // namespace

void cacheFlush(const volatile void *p, size_t n)
{
    uintptr_t at = (uintptr_t)p & ~(uintptr_t)63, end = (uintptr_t)p + n;
    for (; at < end; at += 64)
        __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
    barrier();
}

bool DmaBuffer::allocate(uint32_t bytes)
{
    md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                                                          static_cast<IOOptionBits>(kIODirectionInOut) |
                                                              static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
                                                          bytes, 0x00000000fffff000ull);
    uint64_t physical = md ? md->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) : 0;
    va = md ? static_cast<uint8_t *>(md->getBytesNoCopy()) : nullptr;
    if (!va || !physical || physical + bytes > (1ull << 32)) {
        release();
        return false;
    }
    pa = static_cast<uint32_t>(physical);
    size = bytes;
    bzero(va, bytes);
    cacheFlush(va, bytes);
    return true;
}

void DmaBuffer::release()
{
    if (md)
        md->release();
    md = nullptr;
    va = nullptr;
    pa = 0;
    size = 0;
}

uint32_t Hardware::pmc(uint32_t o)
{
    return r(pmcPage, kPmcOffset + o);
}

void Hardware::pmcWrite(uint32_t o, uint32_t v)
{
    w(pmcPage, kPmcOffset + o, v);
}

// Bounded register handshakes of the bring-up and endpoint commands (microseconds); never event waits.
bool Hardware::waitBits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t value, uint32_t us)
{
    for (uint32_t i = 0; i <= us; ++i) {
        if ((r(b, o) & mask) == value)
            return true;
        IODelay(1);
    }
    return false;
}

bool Hardware::map()
{
    car = (volatile uint8_t *)ml_io_map(kCarPA, 0x1000);
    pmcPage = (volatile uint8_t *)ml_io_map(kPmcPagePA, 0x1000);
    fusePage = (volatile uint8_t *)ml_io_map(kFusePagePA, 0x1000);
    mc = (volatile uint8_t *)ml_io_map(kMcPA, 0x1000);
    padctl = (volatile uint8_t *)ml_io_map(kPadctlPA, 0x1000);
    xudc = (volatile uint8_t *)ml_io_map(kXudcPA, kXudcBytes);
    return car && pmcPage && fusePage && mc && padctl && xudc;
}

// Linux tegra_powergate_power_up order: module reset asserted, partition on, module clock on, clamps removed. The
// caller releases the reset later. A partition already on (the boot chain's) is left as it is.
bool Hardware::powerUp(uint32_t partition, uint32_t clockSet, uint32_t resetSet, uint32_t bit)
{
    if (pmc(PWRGATE_STATUS) & (1u << partition))
        return true;
    w(car, resetSet, bit);
    if (!waitBits(pmcPage, kPmcOffset + PWRGATE_TOGGLE, TOGGLE_START, 0, 5000))
        return false;
    pmcWrite(PWRGATE_TOGGLE, partition | TOGGLE_START);
    if (!waitBits(pmcPage, kPmcOffset + PWRGATE_STATUS, 1u << partition, 1u << partition, 5000))
        return false;
    IODelay(10);
    w(car, clockSet, bit);
    IODelay(10);
    pmcWrite(REMOVE_CLAMPING, 1u << partition);
    IODelay(10);
    return true;
}

// hekate _xusb_init_phy(): PLLU, UTMIPLL, OTG pad 0 calibration from fuses, bias pad tracking. Each PLL is started
// only when it is not already running. Shared with the host controller (XusbHost.cpp): the pad is the same UTMI pad 0
// whichever controller owns the port.
bool usb2PadInit(volatile uint8_t *car, volatile uint8_t *fusePage, volatile uint8_t *padctl)
{
    auto r = [](volatile uint8_t *b, uint32_t o) {
        return *(volatile uint32_t *)(b + o);
    };
    auto w = [](volatile uint8_t *b, uint32_t o, uint32_t v) {
        *(volatile uint32_t *)(b + o) = v;
    };
    auto waitBits = [&](volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t value, uint32_t us) {
        for (uint32_t i = 0; i <= us; ++i) {
            if ((r(b, o) & mask) == value)
                return true;
            IODelay(1);
        }
        return false;
    };
    uint32_t pllu = r(car, PLLU_BASE);
    if (!(pllu & PLL_ENABLE) || !(pllu & PLL_LOCK)) {  // hekate clock_enable_pllu
        w(car, PLLU_MISC, r(car, PLLU_MISC) | (1u << 29));
        uint32_t cfg = (pllu & 0xffe00000u) | (1u << 24) | (1u << 16) | (0x19u << 8) | 2;
        w(car, PLLU_BASE, cfg);
        w(car, PLLU_BASE, cfg | PLL_ENABLE);
        if (!waitBits(car, PLLU_BASE, PLL_LOCK, PLL_LOCK, 1000))
            return false;
        IODelay(8);
    }
    w(car, PLLU_BASE, r(car, PLLU_BASE) | 0x2e00000u);  // USB, HSIC, ICUSB and 48 MHz outputs

    if (!(r(car, UTMIPLL_HW_PWRDN_CFG0) & UTMIPLL_LOCK)) {  // hekate clock_enable_utmipll: 38.4 MHz * 25 = 960 MHz
        w(car, UTMIPLL_HW_PWRDN_CFG0, (r(car, UTMIPLL_HW_PWRDN_CFG0) & ~3u) | 1);  // software IDDQ control, IDDQ off
        w(car, UTMIP_PLL_CFG0, (r(car, UTMIP_PLL_CFG0) & 0xff0000ffu) | (25u << 16) | (1u << 8));
        w(car, UTMIP_PLL_CFG2, (r(car, UTMIP_PLL_CFG2) & 0xff00003fu) | (24u << 18));
        w(car, UTMIP_PLL_CFG1, (r(car, UTMIP_PLL_CFG1) & 0x07ffa000u) | (1u << 15) | 375);
        (void)waitBits(car, UTMIPLL_HW_PWRDN_CFG0, UTMIPLL_LOCK, UTMIPLL_LOCK, 1000);  // hekate does not insist either
    }
    w(car, UTMIP_PLL_CFG2, (r(car, UTMIP_PLL_CFG2) & 0xfeffffe8u) | 0x2000008u | 0x20 | 2);
    IODelay(2);

    // Pad calibration from fuses; the fuse block needs its clock for reads.
    bool fuseClock = r(car, CLK_OUT_ENB_H) & H_FUSE;
    if (!fuseClock) {
        w(car, CLK_ENB_H_SET, H_FUSE);
        IODelay(2);
    }
    uint32_t calib = r(fusePage, kFuseOffset + FUSE_USB_CALIB);
    uint32_t calibExt = r(fusePage, kFuseOffset + FUSE_USB_CALIB_EXT);
    if (!fuseClock)
        w(car, CLK_ENB_H_CLR, H_FUSE);
    w(padctl, OTG_PAD0_CTL_0, (r(padctl, OTG_PAD0_CTL_0) & 0xffffffc0u) | (calib & 0x3f));  // HS_CURR_LEVEL
    w(padctl, OTG_PAD0_CTL_1,
      (r(padctl, OTG_PAD0_CTL_1) & 0x83ffff87u) |                       // TERM_RANGE_ADJ,
          ((calib & 0x780) >> 4) | ((uint32_t)(calibExt << 27) >> 1));  // RPD_CTRL
    w(padctl, BATTERY_CHRG_OTGPAD0_CTL1, (r(padctl, BATTERY_CHRG_OTGPAD0_CTL1) & 0xfffffe3fu) | 0x80);  // VREG_LEV 1

    w(padctl, OTG_PAD0_CTL_0, r(padctl, OTG_PAD0_CTL_0) & 0xdbffffffu);                        // PD, PD_ZI
    w(padctl, OTG_PAD0_CTL_1, r(padctl, OTG_PAD0_CTL_1) & 0xfffffffbu);                        // PD_DR
    w(padctl, BATTERY_CHRG_OTGPAD0_CTL0, r(padctl, BATTERY_CHRG_OTGPAD0_CTL0) & 0xfffffffeu);  // PD_CHG
    w(padctl, BIAS_PAD_CTL_0, r(padctl, BIAS_PAD_CTL_0) & 0xfffff7ffu);                        // bias PD
    (void)r(padctl, OTG_PAD0_CTL_1);

    // Bias pad tracking, two cycles, then tracking powered down again.
    w(car, CLK_ENB_Y_SET, Y_USB2_TRK);
    w(car, CLK_SOURCE_USB2_HSIC_TRK, (r(car, CLK_SOURCE_USB2_HSIC_TRK) & 0xffffff00u) | 6);
    w(padctl, BIAS_PAD_CTL_1, 0x451e000);
    w(padctl, BIAS_PAD_CTL_1, 0x051e000);
    IODelay(100);
    w(padctl, BIAS_PAD_CTL_1, 0x451e000);
    IODelay(3);
    w(padctl, BIAS_PAD_CTL_1, 0x051e000);
    IODelay(100);
    w(padctl, BIAS_PAD_CTL_1, r(padctl, BIAS_PAD_CTL_1) | 0x4000000);
    w(car, CLK_ENB_Y_CLR, Y_USB2_TRK);
    IODelay(30);
    return true;
}

bool Hardware::initPhy()
{
    return usb2PadInit(car, fusePage, padctl);
}

// hekate xusb_device_init(): clocks, pads, FPCI/IPFS, event ring, EP0. The XUSB top and pad controller are shared
// with the PCIe controller (drivers/SwitchWLAN): their clocks are enabled and resets released, never asserted.
bool Hardware::powerOn(char *why, size_t whySize)
{
    if (r(mc, MC_SMMU_XUSB_DEV_ASID) & 0x80000000u) {
        snprintf(why, whySize, "SMMU translation is on for XUSB_DEV (ASID 0x%08x)", r(mc, MC_SMMU_XUSB_DEV_ASID));
        return false;
    }
    if ((!control.va && !control.allocate(kControlBytes)) || (!events.va && !events.allocate(kEventBytes))) {
        snprintf(why, whySize, "no DMA memory below 4 GiB");
        return false;
    }
    // EP0's ring in the control area; data endpoints get their own at their first initEndpoint.
    rings[0].trb = (volatile uint32_t *)(control.va + kRingOffset);
    rings[0].pa = control.pa + kRingOffset;
    rings[0].trbs = kRingTrbs;

    // The legacy USB2 device controller stays off; XUSB and its pad block on.
    w(car, RST_DEV_L_SET, L_USBD);
    w(car, CLK_ENB_L_CLR, L_USBD);
    w(car, CLK_ENB_W_SET, W_XUSB);
    w(car, RST_DEV_W_CLR, W_XUSB | W_XUSB_PADCTL);
    IODelay(2);
    if (!powerUp(PART_XUSBA, CLK_ENB_W_SET, RST_DEV_W_SET, W_XUSB_SS) ||
        !powerUp(PART_XUSBB, CLK_ENB_U_SET, RST_DEV_U_SET, U_XUSB_DEV)) {
        snprintf(why, whySize, "XUSB partitions did not power up (PMC PWRGATE_STATUS 0x%08x)", pmc(PWRGATE_STATUS));
        return false;
    }

    // OTG pad 0 and the bias pad belong to XUSB; port 0 is a device port.
    w(padctl, USB2_PAD_MUX, (r(padctl, USB2_PAD_MUX) & ~(3u << 18 | 3u)) | 1u << 18 | 1u);
    if (!initPhy()) {
        snprintf(why, whySize, "PLLU did not lock (PLLU_BASE 0x%08x)", r(car, PLLU_BASE));
        return false;
    }
    w(padctl, USB2_PORT_CAP, (r(padctl, USB2_PORT_CAP) & ~3u) | 2);
    w(padctl, SS_PORT_MAP, r(padctl, SS_PORT_MAP) & ~0xfu);
    pmcWrite(USB_AO, pmc(USB_AO) & 0xfffffff3u);  // ID and VBUS wake detectors powered
    IODelay(1);

    // Device core 102 MHz from PLLP, FS logic from FO 48 MHz, SS 120 MHz from HSIC 480.
    w(car, PLLU_OUTA, r(car, PLLU_OUTA) | 1);
    IODelay(2);
    w(car, CLK_ENB_U_SET, U_XUSB_DEV);
    w(car, CLK_SOURCE_XUSB_CORE_DEV, (r(car, CLK_SOURCE_XUSB_CORE_DEV) & 0x1fffff00u) | 1u << 29 | 6);
    IODelay(2);
    w(car, CLK_SOURCE_XUSB_FS, (r(car, CLK_SOURCE_XUSB_FS) & 0x1fffffffu) | 2u << 29);
    w(car, CLK_ENB_W_SET, W_XUSB_SS);
    w(car, CLK_SOURCE_XUSB_SS, (r(car, CLK_SOURCE_XUSB_SS) & 0x1fffff00u) | 3u << 29 | 6);
    w(car, RST_DEV_W_CLR, W_XUSB_SS);
    w(car, RST_DEV_U_CLR, U_XUSB_DEV);
    IODelay(2);

    // IPFS bridge, then the controller's PCI-style config space: BAR0 = its own base (Linux tegra_xudc_fpci_ipfs_init).
    xw(IPFS_CONFIGURATION, xr(IPFS_CONFIGURATION) | 1);
    IODelay(10);
    xw(FPCI_CFG_1, xr(FPCI_CFG_1) | 7);  // IO, memory, bus master
    IODelay(1);
    xw(FPCI_CFG_4, (uint32_t)kXudcPA);
    xw(FPCI_CFG_5, 0);
    IODelay(100);
    xw(IPFS_INTR_MASK, xr(IPFS_INTR_MASK) | 1u << 16);

    // Event ring: two segments, consumer cycle state 1.
    xw(ERST0BALO, events.pa);
    xw(ERST0BAHI, 0);
    xw(ERST1BALO, events.pa + kEventSegmentTrbs * 16);
    xw(ERST1BAHI, 0);
    xw(ERSTSZ, kEventSegmentTrbs << 16 | kEventSegmentTrbs);
    xw(EREPLO, (xr(EREPLO) & 0xe) | events.pa | EREP_ECS);
    xw(EREPHI, 0);
    xw(ERDPLO, (xr(ERDPLO) & 0xf) | events.pa);
    xw(ERDPHI, 0);
    eventIndex = 0;
    eventCycle = 1;

    initEndpoint(0, EP_CONTROL, 64, 0);
    xw(CTRL, xr(CTRL) | CTRL_IE | CTRL_LSE);
    xw(ECPLO, control.pa + kContextOffset);
    xw(ECPHI, 0);
    return true;
}

// hekate xusb_device_enumerate(): VBUS/ID overrides, enable, port to RxDetect, pull-up on; interrupt moderation as
// Linux tegra_xudc_init sets it (IMODI and the counter, kImodUs).
void Hardware::connect()
{
    xw(RT_IMOD, (kImodUs * 4) << 16 | (kImodUs * 4));
    w(padctl, ELPG_PROGRAM_0, 0);
    w(padctl, ELPG_PROGRAM_1, 0);
    w(padctl, USB2_VBUS_ID,
      (r(padctl, USB2_VBUS_ID) & ~(VBUS_ID_VBUS_OVR_MASK | VBUS_ID_SRC_MASK)) | VBUS_ID_VBUS_OVR_EN |
          VBUS_ID_SRC_ID_OVR_EN);
    xw(PORTHALT, xr(PORTHALT) & ~PORTHALT_HALT_LTSSM);
    xw(CTRL, xr(CTRL) | CTRL_ENABLE);
    xw(CFG_DEV_FE, (xr(CFG_DEV_FE) & ~FE_PORTREGSEL_MASK) | FE_PORTREGSEL_HSFS);
    xw(PORTSC, (xr(PORTSC) & ~(PORTSC_PLS_MASK | PORTSC_CHANGES)) | PORTSC_LWS | PORTSC_PLS_RXDETECT);
    xw(CFG_DEV_FE, xr(CFG_DEV_FE) & ~FE_PORTREGSEL_MASK);
    w(padctl, USB2_VBUS_ID, (r(padctl, USB2_VBUS_ID) & ~VBUS_ID_OVR_MASK) | VBUS_ID_OVR_FLOAT | VBUS_ID_VBUS_ON);
}

// Device mode off and the VBUS override low: the pull-up goes away.
void Hardware::disconnect()
{
    xw(CTRL, xr(CTRL) & ~(CTRL_ENABLE | CTRL_RUN));
    w(padctl, USB2_VBUS_ID, r(padctl, USB2_VBUS_ID) & ~VBUS_ID_VBUS_ON);
}

bool Hardware::nextEvent(Event &event)
{
    volatile uint32_t *e = (volatile uint32_t *)events.va + eventIndex * 4;
    cacheFlush(e, 16);
    uint32_t d3 = e[3];
    if ((d3 & TRB_CYCLE) != eventCycle)
        return false;
    event.d[0] = e[0];
    event.d[1] = e[1];
    event.d[2] = e[2];
    event.d[3] = d3;
    if (++eventIndex == 2 * kEventSegmentTrbs) {
        eventIndex = 0;
        eventCycle ^= 1;
    }
    return true;
}

// The CPU never writes the event ring: cacheFlush's clean of a dirty line would overwrite events written since.
bool Hardware::eventPending()
{
    volatile uint32_t *e = (volatile uint32_t *)events.va + eventIndex * 4;
    cacheFlush(e, 16);
    return (e[3] & TRB_CYCLE) == eventCycle;
}

// Linux tegra_xudc_process_event_ring(): the new dequeue pointer with EHB written as 1 (W1C), which re-enables the
// interrupt. ST.IP is not touched here: an IP raised for events that arrived after the drain must survive.
void Hardware::finishEvents()
{
    xw(ERDPHI, 0);
    xw(ERDPLO, (events.pa + eventIndex * 16) | ERDP_EHB);
}

bool Hardware::interruptPending()
{
    return xr(ST) & ST_IP;
}

void Hardware::describeInterrupt(char *buffer, size_t size)
{
    snprintf(buffer, size, "ST %#x CTRL %#x ERDP %#x EREP %#x PORTSC %#x IPFS_INTR_MASK %#x", xr(ST), xr(CTRL),
             xr(ERDPLO), xr(EREPLO), xr(PORTSC), xr(IPFS_INTR_MASK));
}

// Linux tegra_xudc_irq(): acknowledge ST.IP alone (ST.RC is W1C too), before the ring is drained.
bool Hardware::takeInterrupt()
{
    if (!(xr(ST) & ST_IP))
        return false;
    xw(ST, ST_IP);
    return true;
}

// Linux clear_port_change(): the other change bits written as 0 so they stay pending.
void Hardware::clearPortChange(uint32_t change)
{
    xw(PORTSC, (xr(PORTSC) & ~PORTSC_CHANGES) | change);
}

// Linux tegra_xudc_reset() for EP0: drop what is queued (dequeue = enqueue), sequence and partial-TD state cleared,
// every endpoint unhalted, EP0 reloaded and unpaused. The device address goes back to 0 with the bus reset.
void Hardware::resetEp0()
{
    Ring &ring = rings[0];
    volatile uint32_t *c = context(0);
    cacheFlush(c, 64);
    c[2] = ((ring.pa + ring.enqueue * 16) & ~0xfu) | ring.cycle;
    c[3] = 0;
    c[5] &= 0x00ffffffu;                      // rsvd, partial_td, splitxstate, seq_num
    c[11] &= ~0x7fu;                          // device address
    c[1] = (c[1] & 0x0000ffffu) | 64u << 16;  // max packet 64 below SuperSpeed
    cacheFlush(c, 64);
    xw(CTRL, xr(CTRL) & ~(0x7fu << 24));
    if (uint32_t halted = xr(EP_HALT)) {  // Linux ep_unhalt_all
        xw(EP_HALT, 0);
        if (waitBits(xudc, EP_STCHG, halted, halted, 1000))
            xw(EP_STCHG, halted);
    }
    xw(EP_RELOAD, 1u);
    (void)waitBits(xudc, EP_RELOAD, 1u, 0, 1000);
    toggleBit(EP_PAUSE, 0, false);
}

// Linux __tegra_xudc_handle_port_status(), run until no change is left (tegra_xudc_handle_port_status). Port reset and
// connect also get Linux's tegra_xudc_port_connect() setup: USB2 LPM answered NYET, ST.RC cleared.
PortChange Hardware::portChange()
{
    PortChange change;
    for (unsigned pass = 0; pass < 8; ++pass) {
        uint32_t halt = xr(PORTHALT);
        if ((halt & PORTHALT_STCHG_REQ) && (halt & PORTHALT_HALT_LTSSM))
            xw(PORTHALT, halt & ~PORTHALT_HALT_LTSSM);

        uint32_t sc = xr(PORTSC);
        if ((sc & PORTSC_PRC) && (sc & PORTSC_PR))
            clearPortChange(PORTSC_PRC | PORTSC_PED);  // reset still going
        bool connectNow = false;
        if ((sc & PORTSC_PRC) && !(sc & PORTSC_PR)) {
            clearPortChange(PORTSC_PRC | PORTSC_PED);
            resetEp0();
            change.reset = connectNow = true;
        }
        sc = xr(PORTSC);
        if (sc & PORTSC_WRC) {
            clearPortChange(PORTSC_WRC | PORTSC_PED);
            if (!(xr(PORTSC) & PORTSC_WPR)) {
                resetEp0();
                change.reset = connectNow = true;
            }
        }
        sc = xr(PORTSC);
        if (sc & PORTSC_CSC) {
            clearPortChange(PORTSC_CSC);
            if (sc & PORTSC_CCS) {
                change.connected = connectNow = true;
            } else {
                resetEp0();
                change.disconnected = true;
            }
        }
        if (connectNow) {
            change.fullSpeed = (xr(PORTSC) & PORTSC_PS_MASK) == PORTSC_PS_FS;
            xw(PORTPM, (xr(PORTPM) & ~PORTPM_L1S_MASK) | PORTPM_L1S_NYET);
            if (xr(ST) & ST_RC)
                xw(ST, ST_RC);
        }
        sc = xr(PORTSC);
        if (sc & PORTSC_PLC) {
            clearPortChange(PORTSC_PLC);
            if ((sc & PORTSC_PLS_MASK) == PORTSC_PLS_U3)
                change.suspended = true;
            if ((sc & PORTSC_PLS_MASK) == PORTSC_PLS_U0)
                change.resumed = true;
        }
        if (sc & PORTSC_CEC)
            clearPortChange(PORTSC_CEC);
        if (!(xr(PORTSC) & PORTSC_CHANGES) && !(xr(PORTHALT) & PORTHALT_STCHG_REQ))
            break;
    }
    return change;
}

volatile uint32_t *Hardware::context(uint32_t dci)
{
    return (volatile uint32_t *)(control.va + kContextOffset) + dci * 16;
}

void Hardware::resetRing(uint32_t dci)
{
    Ring &ring = rings[dci];
    for (uint32_t i = 0; i < ring.trbs * 4; ++i)
        ring.trb[i] = 0;
    volatile uint32_t *link = ring.trb + (ring.trbs - 1) * 4;
    link[0] = ring.pa;
    link[3] = trbType(TRB_LINK) | TRB_TOGGLE;
    ring.enqueue = 0;
    ring.cycle = 1;
    cacheFlush(ring.trb, ring.trbs * 16);
}

// hekate xusb_ep_ctx_t / Linux tegra_xudc_ep_context_setup: state running, CErr/CEC 3, dequeue = ring start with
// DCS 1, Linux's average TRB length per type (control 8, interrupt 1024, bulk and isochronous 3072), the max burst
// (SuperSpeed; at high speed a periodic endpoint's extra transactions per microframe); EP0 keeps the current device
// address.
bool Hardware::initEndpoint(uint32_t dci, uint32_t type, uint32_t maxPacket, uint32_t interval, uint32_t maxBurst)
{
    Ring &ring = rings[dci];
    if (dci && !ring.trb) {
        if (!ring.own.allocate(kDataRingTrbs * 16))
            return false;
        ring.trb = (volatile uint32_t *)ring.own.va;
        ring.pa = ring.own.pa;
        ring.trbs = kDataRingTrbs;
    }
    resetRing(dci);
    volatile uint32_t *c = context(dci);
    uint32_t address = dci == 0 ? (xr(CTRL) >> 24) & 0x7f : 0;
    const bool interrupt = type == EP_INTR_IN || type == EP_INTR_OUT;
    const bool periodic = interrupt || type == EP_ISOCH_IN || type == EP_ISOCH_OUT;
    uint32_t average = type == EP_CONTROL ? 8 : interrupt ? 1024 : 3072;
    uint32_t esit = periodic ? maxPacket * (maxBurst + 1) : 0;
    for (uint32_t i = 0; i < 16; ++i)
        c[i] = 0;
    c[0] = 1 | interval << 16;
    c[1] = 3u << 1 | type << 3 | (maxBurst & 0xff) << 8 | maxPacket << 16;
    c[2] = (rings[dci].pa & ~0xfu) | 1;
    c[4] = average | esit << 16;
    c[6] = 3u << 18;
    c[11] = address;
    cacheFlush(c, 64);
    return true;
}

bool Hardware::reloadEndpoint(uint32_t dci)
{
    xw(EP_RELOAD, 1u << dci);
    if (!waitBits(xudc, EP_RELOAD, 1u << dci, 0, 1000))
        return false;
    xw(EP_PAUSE, xr(EP_PAUSE) & ~(1u << dci));
    xw(EP_HALT, xr(EP_HALT) & ~(1u << dci));
    return true;
}

// Linux ep_pause/ep_halt pattern: change the bit, wait for EP_STCHG, ack it.
void Hardware::toggleBit(uint32_t reg, uint32_t dci, bool set)
{
    uint32_t bit = 1u << dci, value = xr(reg);
    if (!!(value & bit) == set)
        return;
    xw(reg, set ? value | bit : value & ~bit);
    if (waitBits(xudc, EP_STCHG, bit, bit, 1000))
        xw(EP_STCHG, bit);
}

void Hardware::setHalt(uint32_t dci, bool halt)
{
    toggleBit(EP_HALT, dci, halt);
}

void Hardware::setPause(uint32_t dci, bool pause)
{
    toggleBit(EP_PAUSE, dci, pause);
}

bool Hardware::halted(uint32_t dci)
{
    return (xr(EP_HALT) >> dci) & 1;
}

void Hardware::disableEndpoint(uint32_t dci)
{
    volatile uint32_t *c = context(dci);
    cacheFlush(c, 64);
    c[0] &= ~7u;
    cacheFlush(c, 64);
    xw(EP_RELOAD, 1u << dci);
    (void)waitBits(xudc, EP_RELOAD, 1u << dci, 0, 1000);
}

// Linux __tegra_xudc_ep_set_halt(false): disable, reload, running with the sequence number and partial-TD state
// cleared, reload, unpause, unhalt.
void Hardware::resetSequence(uint32_t dci)
{
    volatile uint32_t *c = context(dci);
    cacheFlush(c, 64);
    c[0] &= ~7u;
    cacheFlush(c, 64);
    (void)reloadEndpoint(dci);
    cacheFlush(c, 64);
    c[0] = (c[0] & ~7u) | 1;
    c[5] &= 0x00ffffffu;  // rsvd, partial_td, splitxstate, seq_num
    cacheFlush(c, 64);
    (void)reloadEndpoint(dci);
    toggleBit(EP_PAUSE, dci, false);
    toggleBit(EP_HALT, dci, false);
}

// Linux __tegra_xudc_ep_dequeue (busy case): pause, wait until idle, move the dequeue pointer to the enqueue position,
// clear partial-TD state, reload. The sequence number (data toggle) survives.
void Hardware::dropQueued(uint32_t dci)
{
    Ring &ring = rings[dci];
    toggleBit(EP_PAUSE, dci, true);
    (void)waitBits(xudc, EP_THREAD_ACTIVE, 1u << dci, 0, 1000);
    volatile uint32_t *c = context(dci);
    cacheFlush(c, 64);
    c[2] = ((ring.pa + ring.enqueue * 16) & ~0xfu) | ring.cycle;
    c[3] = 0;
    c[5] &= 0xf8000000u;  // keep seq_num; clear edtla, rsvd, partial_td, splitxstate
    cacheFlush(c, 64);
    xw(EP_RELOAD, 1u << dci);
    (void)waitBits(xudc, EP_RELOAD, 1u << dci, 0, 1000);
    toggleBit(EP_PAUSE, dci, false);
}

uint32_t Hardware::push(uint32_t dci, uint32_t bufferPA, uint32_t length, uint32_t flags)
{
    const uint32_t index = pushData(dci, bufferPA, length, 0, flags);
    return rings[dci].pa + index * 16;
}

// The TRB (the controller reads it once its line is cleaned), and the link TRB (XudcTransfer.h linkControl) when the
// ring wraps.
uint32_t Hardware::pushData(uint32_t dci, uint64_t bufferPA, uint32_t length, uint32_t tdSize, uint32_t flags)
{
    Ring &ring = rings[dci];
    const uint32_t index = ring.enqueue;
    volatile uint32_t *t = ring.trb + index * 4;
    t[0] = (uint32_t)bufferPA;
    t[1] = (uint32_t)(bufferPA >> 32);
    t[2] = (length & 0x1ffff) | (tdSize > 31 ? 31 : tdSize) << 17;
    t[3] = flags | ring.cycle;
    cacheFlush(t, 16);
    if (++ring.enqueue == ring.trbs - 1) {
        volatile uint32_t *link = ring.trb + ring.enqueue * 4;
        link[3] = linkControl(link[3], flags, ring.cycle);
        cacheFlush(link, 16);
        ring.enqueue = 0;
        ring.cycle ^= 1;
    }
    return index;
}

bool Hardware::trbIndex(uint32_t dci, uint32_t trbPA, uint32_t *index) const
{
    const Ring &ring = rings[dci];
    if (!ring.trb || trbPA < ring.pa || trbPA >= ring.pa + (ring.trbs - 1) * 16 || (trbPA & 15))
        return false;
    *index = (trbPA - ring.pa) / 16;
    return true;
}

void Hardware::doorbell(uint32_t dci, uint16_t sequence)
{
    barrier();
    xw(DB, dci << 8 | (dci == 0 ? (uint32_t)sequence << 16 : 0));
}

// SET_ADDRESS: CTRL.DEVADDR and EP0's context (Linux tegra_xudc_ep0_set_address), before the status stage.
void Hardware::setAddress(uint32_t address)
{
    xw(CTRL, (xr(CTRL) & ~(0x7fu << 24)) | (address & 0x7f) << 24);
    volatile uint32_t *c = context(0);
    cacheFlush(c, 64);
    c[11] = (c[11] & ~0x7fu) | (address & 0x7f);
    cacheFlush(c, 64);
}

void Hardware::run()
{
    if (xr(CTRL) & CTRL_RUN)
        return;
    xw(CTRL, xr(CTRL) | CTRL_RUN);
    xw(ST, ST_RC);  // W1C: writing ST back would also clear a pending IP
}

}  // namespace xudc
