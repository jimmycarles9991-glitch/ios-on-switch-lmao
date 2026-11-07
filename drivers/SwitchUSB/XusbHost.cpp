// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 XUSB host controller hardware layer; see XusbHost.h.
#include "XusbHost.h"
#include "XudcHardware.h"
#include "XusbPorts.h"
#include <IOKit/IOLib.h>
#include "../TegraSMMU/TegraSMMU.h"
#include "UsbSmmuMapper.h"
#include "XusbLanes.h"
#include "XusbSuperSpeed.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace xhost {
namespace {
// Physical bases: Tegra X1 TRM, hekate bdk/soc/t210.h, the OLED DTB.
constexpr uint64_t kCarPA = 0x60006000, kPmcPagePA = 0x7000E000, kFusePagePA = 0x7000F000, kMcPA = 0x70019000,
                   kPadctlPA = 0x7009F000, kFpciPA = 0x70098000, kIpfsPA = 0x70099000;
constexpr uint32_t kPmcOffset = 0x400;

enum : uint32_t {  // clock and reset controller
    RST_DEV_U_SET = 0x310,
    RST_DEV_U_CLR = 0x314,
    CLK_ENB_U_SET = 0x330,
    RST_DEV_W_SET = 0x438,
    RST_DEV_W_CLR = 0x43c,
    CLK_ENB_W_SET = 0x448,
    CLK_SOURCE_XUSB_CORE_HOST = 0x600,
    CLK_SOURCE_XUSB_FALCON = 0x604,
    CLK_SOURCE_XUSB_FS = 0x608,
    CLK_SOURCE_XUSB_SS = 0x610,
};

enum : uint32_t {
    U_XUSB_HOST = 1u << 25,
    W_XUSB_PADCTL = 1u << 14,
    W_XUSB = 1u << 15,
    W_XUSB_SS = 1u << 28,
};

enum : uint32_t {  // PMC (from 0x7000E400)
    PWRGATE_TOGGLE = 0x30,
    REMOVE_CLAMPING = 0x34,
    PWRGATE_STATUS = 0x38,
    USB_AO = 0xf0,
    TOGGLE_START = 1u << 8,
    PART_XUSBA = 20,
    PART_XUSBC = 22,
};

enum : uint32_t {  // FPCI configuration space of the host (xhci-tegra.c)
    XUSB_CFG_1 = 0x004,
    XUSB_CFG_4 = 0x010,
    XUSB_CFG_ARU_MBOX_CMD = 0x0e4,
    XUSB_CFG_ARU_MBOX_DATA_IN = 0x0e8,
    XUSB_CFG_ARU_MBOX_DATA_OUT = 0x0ec,
    XUSB_CFG_ARU_MBOX_OWNER = 0x0f0,
    XUSB_CFG_ARU_C11PAGESEL = 0x404,
    XUSB_CFG_ARU_C11_CSBRANGE = 0x41c,
    XUSB_CFG_ARU_SMI_INTR = 0x428,
    XUSB_CFG_HSPX_CORE_CTRL = 0x600,
    XUSB_CFG_CSB_BASE_ADDR = 0x800,
    CFG_1_ENABLES = 7,  // IO space, memory space, bus master
    CFG_4_BASE_MASK = 0x1ffffu << 15,
    HSP0 = 1u << 12,
    HSIC_PLLCLK_VLD = 1u << 24,
    MBOX_DEST_FALC = 1u << 27,
    MBOX_DEST_SMI = 1u << 29,
    MBOX_INT_EN = 1u << 31,
    MBOX_OWNER_NONE = 0,
    MBOX_OWNER_SW = 2,
    SMI_INTR_FW_HANG = 1u << 1,
    SMI_INTR_EN = 1u << 3,
};

enum : uint32_t {  // IPFS
    IPFS_MSI_EN_VEC0 = 0x140,
    IPFS_CONFIGURATION = 0x180,
    IPFS_INTR_MASK = 0x188,
    IPFS_INTR_ENABLE = 0x198,  // xhci-tegra.c XUSB_HOST_IPFS_INTR_ENABLE_0: saved and restored, never set
    IPFS_CLKGATE_HYSTERESIS = 0x1bc,
    IPFS_EN_FPCI = 1u << 0,
    IPFS_IP_INT_MASK = 1u << 16,
};

enum : uint32_t {  // falcon and its memory pool, through the CSB window (xhci-tegra.c)
    FALC_CPUCTL = 0x100,
    FALC_BOOTVEC = 0x104,
    FALC_DMACTL = 0x10c,
    FALC_IMFILLRNG1 = 0x154,
    FALC_IMFILLCTL = 0x158,
    CSB_MP_ILOAD_ATTR = 0x101a00,
    CSB_MP_ILOAD_BASE_LO = 0x101a04,
    CSB_MP_ILOAD_BASE_HI = 0x101a08,
    CSB_MP_L2IMEMOP_SIZE = 0x101a10,
    CSB_MP_L2IMEMOP_TRIG = 0x101a14,
    CSB_MP_L2IMEMOP_RESULT = 0x101a18,
    CSB_MP_APMAP = 0x10181c,
    CPUCTL_STARTCPU = 1u << 1,
    APMAP_BOOTPATH = 1u << 31,
    L2IMEMOP_INVALIDATE_ALL = 0x40u << 24,
    L2IMEMOP_LOAD_LOCKED_RESULT = 0x11u << 24,
    L2IMEMOP_RESULT_VLD = 1u << 31,
    IMEM_BLOCK = 256,
};

enum : uint32_t {  // XUSB pad controller (xusb-tegra210.c); port 0's role registers are XusbPorts.h's
    USB2_PAD_MUX = 0x004,
    SS_PORT_MAP = 0x014,
    ELPG_PROGRAM_0 = 0x020,
    ELPG_PROGRAM_1 = 0x024,
    OTG_PAD0_CTL_0 = 0x088,
    OTG_PAD0_CTL_1 = 0x08c,
    BIAS_PAD_CTL_0 = 0x284,
    VREG_FIX18 = 1u << 6,
    OTG_PAD_PD = 1u << 26,
    OTG_PAD_PD_ZI = 1u << 29,
};

static_assert(MBOX_SAVE_DFE_CTLE_CTX == XUSB_MBOX_SAVE_DFE_CTLE_CTX && MBOX_RESET_SSPI == XUSB_MBOX_RESET_SSPI &&
                  MBOX_DISABLE_SS_LFPS_DETECTION == XUSB_MBOX_DISABLE_SS_LFPS_DETECTION &&
                  MBOX_ENABLE_SS_LFPS_DETECTION == XUSB_MBOX_ENABLE_SS_LFPS_DETECTION && MBOX_ACK == XUSB_MBOX_ACK &&
                  MBOX_NAK == XUSB_MBOX_NAK,
              "XusbLanes.h's mailbox numbers are xhci-tegra.c's");

// Firmware header (xhci-tegra.c struct tegra_xusb_fw_header, 256 bytes before the image proper).
constexpr size_t kFwHeader = 256, kFwCodeTag = 8, kFwCodeSize = 12, kFwCreated = 44, kFwVersion = 72, kFwLength = 100,
                 kFwHsicPorts = 116;

inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// The firmware image stays where the falcon reads it (ILOAD_BASE) for as long as the controller runs.
xudc::DmaBuffer gFirmware;
}  // namespace

bool Controller::waitBits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t value, uint32_t us)
{
    for (uint32_t i = 0; i <= us; ++i) {
        if ((r(b, o) & mask) == value)
            return true;
        IODelay(1);
    }
    return false;
}

uint32_t Controller::pmc(uint32_t o) const
{
    return r(pmcPage, kPmcOffset + o);
}

void Controller::pmcWrite(uint32_t o, uint32_t v)
{
    w(pmcPage, kPmcOffset + o, v);
}

bool Controller::map()
{
    if (isMapped)
        return true;
    car = (volatile uint8_t *)ml_io_map(kCarPA, 0x1000);
    pmcPage = (volatile uint8_t *)ml_io_map(kPmcPagePA, 0x1000);
    fusePage = (volatile uint8_t *)ml_io_map(kFusePagePA, 0x1000);
    mc = (volatile uint8_t *)ml_io_map(kMcPA, 0x1000);
    padctl = (volatile uint8_t *)ml_io_map(kPadctlPA, 0x1000);
    regs = (volatile uint8_t *)ml_io_map(kXhciPA, kXhciBytes);
    fpci = (volatile uint8_t *)ml_io_map(kFpciPA, 0x1000);
    ipfs = (volatile uint8_t *)ml_io_map(kIpfsPA, 0x1000);
    isMapped = car && pmcPage && fusePage && mc && padctl && regs && fpci && ipfs;
    return isMapped;
}

// The XUSB top and pad controller clock on and resets released (shared with the XUDC and PCIe: never asserted).
void Controller::padctlUp()
{
    w(car, CLK_ENB_W_SET, W_XUSB);
    w(car, RST_DEV_W_CLR, W_XUSB | W_XUSB_PADCTL);
    IODelay(2);
}

// Linux tegra_powergate_power_up order (as XudcHardware::powerUp): reset asserted, partition on, module clock on,
// clamps removed. The caller releases the reset. A partition already on is left as it is.
bool Controller::powerUp(uint32_t partition, uint32_t clockSet, uint32_t resetSet, uint32_t bit)
{
    if (pmc(PWRGATE_STATUS) & (1u << partition)) {
        partitionsFound |= 1u << partition;
        return true;
    }
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

// tegra_xusb_probe order: the USB2 pad (shared with the XUDC; skipped when the XUDC already brought it up, since
// re-running bias tracking under a live link would glitch it), clocks, partitions SS (XUSBA) and host (XUSBC), then
// tegra_xusb_config (IPFS -> FPCI, BAR0, bus master, interrupt assertion) and tegra_xusb_disable_hsic_wake (T210B01).
// The XUSB top and pad controller are shared with the PCIe controller (drivers/SwitchWLAN): clocks on and resets
// released, never asserted.
bool Controller::powerOn(char *why, size_t whySize)
{
    const uint32_t asid = r(mc, TEGRA_SMMU_XUSB_HOST_ASID);
    if (!xusb_host_asid_usable(asid, TEGRA_SMMU_ASID_XUSB_HOST)) {
        snprintf(why, whySize, "XUSB_HOST is translated by another SMMU ASID (MC %#x = %#x)", TEGRA_SMMU_XUSB_HOST_ASID,
                 asid);
        return false;
    }
    padctlUp();
    rmw(padctl, USB2_PAD_MUX, 3u << 18 | 3u, 1u << 18 | 1u);  // bias pad and OTG pad 0 to XUSB
    if ((r(padctl, OTG_PAD0_CTL_0) & (OTG_PAD_PD | OTG_PAD_PD_ZI)) && !xudc::usb2PadInit(car, fusePage, padctl)) {
        snprintf(why, whySize, "PLLU did not lock");
        return false;
    }
    rmw(padctl, SS_PORT_MAP, 0xfu, 0);                   // SS port 0 paired with USB2 port 0 (as the XUDC)
    rmw(padctl, XUSB_CHRG_OTGPAD0_CTL1, 0, VREG_FIX18);  // T210B01: fixed 1.8 V regulator on the USB2 pads
    pmcWrite(USB_AO, pmc(USB_AO) & 0xfffffff3u);         // ID and VBUS wake detectors powered (as the XUDC)

    // Host core 102 MHz and falcon 204 MHz from PLLP, FS 48 MHz from PLLU FO, SS 120 MHz from the HSIC 480 MHz
    // branch: the L4T device tree's assigned rates, sources as XudcHardware.
    w(car, CLK_SOURCE_XUSB_CORE_HOST, (r(car, CLK_SOURCE_XUSB_CORE_HOST) & 0x1fffff00u) | 1u << 29 | 6);
    w(car, CLK_SOURCE_XUSB_FALCON, (r(car, CLK_SOURCE_XUSB_FALCON) & 0x1fffff00u) | 1u << 29 | 2);
    w(car, CLK_SOURCE_XUSB_FS, (r(car, CLK_SOURCE_XUSB_FS) & 0x1fffffffu) | 2u << 29);
    w(car, CLK_SOURCE_XUSB_SS, (r(car, CLK_SOURCE_XUSB_SS) & 0x1fffff00u) | 3u << 29 | 6);
    IODelay(2);
    if (!powerUp(PART_XUSBA, CLK_ENB_W_SET, RST_DEV_W_SET, W_XUSB_SS) ||
        !powerUp(PART_XUSBC, CLK_ENB_U_SET, RST_DEV_U_SET, U_XUSB_HOST)) {
        snprintf(why, whySize, "XUSB partitions did not power up (PMC PWRGATE_STATUS %#x)", pmc(PWRGATE_STATUS));
        return false;
    }
    w(car, CLK_ENB_W_SET, W_XUSB_SS);
    w(car, CLK_ENB_U_SET, U_XUSB_HOST);
    w(car, RST_DEV_W_CLR, W_XUSB_SS);
    w(car, RST_DEV_U_CLR, U_XUSB_HOST);
    IODelay(2);

    rmw(ipfs, IPFS_CONFIGURATION, 0, IPFS_EN_FPCI);
    IODelay(10);
    rmw(fpci, XUSB_CFG_4, CFG_4_BASE_MASK, (uint32_t)kXhciPA & CFG_4_BASE_MASK);
    IODelay(100);
    rmw(fpci, XUSB_CFG_1, 0, CFG_1_ENABLES);
    rmw(ipfs, IPFS_INTR_MASK, 0, IPFS_IP_INT_MASK);
    w(ipfs, IPFS_CLKGATE_HYSTERESIS, 0x80);

    rmw(fpci, XUSB_CFG_ARU_C11PAGESEL, 0, HSP0);
    rmw(fpci, XUSB_CFG_HSPX_CORE_CTRL, HSIC_PLLCLK_VLD, 0);
    rmw(fpci, XUSB_CFG_ARU_C11PAGESEL, HSP0, 0);
    isPowered = true;
    return true;
}

// The CSB window: page select in C11_CSBRANGE, 512-byte page at CSB_BASE_ADDR (xhci-tegra.c csb_readl).
uint32_t Controller::csbRead(uint32_t offset)
{
    w(fpci, XUSB_CFG_ARU_C11_CSBRANGE, (offset >> 9) & 0x7fffff);
    return r(fpci, XUSB_CFG_CSB_BASE_ADDR + (offset & 0x1ff));
}

void Controller::csbWrite(uint32_t offset, uint32_t value)
{
    w(fpci, XUSB_CFG_ARU_C11_CSBRANGE, (offset >> 9) & 0x7fffff);
    w(fpci, XUSB_CFG_CSB_BASE_ADDR + (offset & 0x1ff), value);
}

// tegra_xhci_load_firmware: the falcon's boot code is DMAed from system memory into L2IMEM, the falcon starts at
// boot_codetag and, once it runs the xHCI, clears USBSTS.CNR. A controller whose ILOAD_BASE is already set kept its
// firmware (we never reset it after this).
bool Controller::loadFirmware(const uint8_t *image, size_t size, char *why, size_t whySize)
{
    if (csbRead(CSB_MP_ILOAD_BASE_LO) != 0)
        return true;
    uint32_t length = size >= kFwHeader ? le32(image + kFwLength) : 0;
    if (length < kFwHeader || length > size) {
        snprintf(why, whySize, "XUSB firmware header says %u bytes, the image has %zu", length, size);
        return false;
    }
    if (!gFirmware.va && !gFirmware.allocate((length + 0xfffu) & ~0xfffu)) {
        snprintf(why, whySize, "no DMA memory below 4 GiB for the firmware");
        return false;
    }
    memcpy(gFirmware.va, image, length);
    gFirmware.va[kFwHsicPorts] = 0;  // header->num_hsic_port: no HSIC on this board
    xudc::cacheFlush(gFirmware.va, length);
    uint32_t codeTag = le32(image + kFwCodeTag), codeSize = le32(image + kFwCodeSize);
    uint32_t tagBlocks = (codeTag + IMEM_BLOCK - 1) / IMEM_BLOCK, sizeBlocks = (codeSize + IMEM_BLOCK - 1) / IMEM_BLOCK;
    uint64_t base = (uint64_t)gFirmware.pa + kFwHeader;
    csbWrite(CSB_MP_ILOAD_ATTR, length);
    csbWrite(CSB_MP_ILOAD_BASE_HI, (uint32_t)(base >> 32));
    csbWrite(CSB_MP_ILOAD_BASE_LO, (uint32_t)base);
    csbWrite(CSB_MP_APMAP, APMAP_BOOTPATH);
    csbWrite(CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_INVALIDATE_ALL);
    csbWrite(CSB_MP_L2IMEMOP_SIZE, (tagBlocks & 0x3ff) << 8 | (sizeBlocks & 0xff) << 24);
    csbWrite(CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_LOAD_LOCKED_RESULT);
    csbWrite(FALC_IMFILLCTL, sizeBlocks);
    csbWrite(FALC_IMFILLRNG1, (tagBlocks & 0xffff) | ((tagBlocks + sizeBlocks) & 0xffff) << 16);
    csbWrite(FALC_DMACTL, 0);
    bool loaded = false;
    for (int i = 0; i < 200 && !loaded; ++i) {  // Linux: 10 ms in 50 us steps
        loaded = csbRead(CSB_MP_L2IMEMOP_RESULT) & L2IMEMOP_RESULT_VLD;
        if (!loaded)
            IODelay(50);
    }
    if (!loaded) {
        snprintf(why, whySize, "falcon DMA not ready (L2IMEMOP_RESULT %#x)", csbRead(CSB_MP_L2IMEMOP_RESULT));
        return false;
    }
    csbWrite(FALC_BOOTVEC, codeTag);
    csbWrite(FALC_CPUCTL, CPUCTL_STARTCPU);
    const uint32_t usbsts = capLength() + 0x04;
    for (int i = 0; i < 200 && (xhci(usbsts) & (1u << 11)); ++i)
        IOSleep(1);  // STS_CNR, Linux: 200 ms
    if (xhci(usbsts) & (1u << 11)) {
        snprintf(why, whySize, "xHCI not ready after the firmware started (USBSTS %#x, falcon CPUCTL %#x)",
                 xhci(usbsts), csbRead(FALC_CPUCTL));
        return false;
    }
    IOLog("SwitchUSB: XUSB firmware %x.%02x (built %u) running, HCSPARAMS1 %#x\n", image[kFwVersion + 3],
          image[kFwVersion + 2], le32(image + kFwCreated), xhci(4));
    return true;
}

void Controller::setHostRole(bool host)
{
    padctlUp();
    rmw(padctl, XUSB_USB2_PORT_CAP, XUSB_PORT0_CAP_MASK, XUSB_PORT0_CAP_OTG);
    const uint32_t v = r(padctl, XUSB_USB2_VBUS_ID);
    if (!host) {
        w(padctl, XUSB_USB2_VBUS_ID, xusb_vbus_id_device(v));
        return;
    }
    if (v & XUSB_VBUS_ID_VBUS_ON) {  // tegra210_xusb_padctl_id_override: VBUS override off, 1-2 ms, then ID
        w(padctl, XUSB_USB2_VBUS_ID, xusb_vbus_id_drop_vbus(v));
        IODelay(1500);
    }
    w(padctl, XUSB_USB2_VBUS_ID, xusb_vbus_id_host(r(padctl, XUSB_USB2_VBUS_ID)));
    const uint32_t ctl0 = r(padctl, XUSB_CHRG_OTGPAD0_CTL0), ctl1 = r(padctl, XUSB_CHRG_OTGPAD0_CTL1);
    const uint32_t ocMap = r(padctl, XUSB_USB2_OC_MAP);
    if (ctl0 != xusb_chrg_ctl0_host(ctl0) || ctl1 != xusb_chrg_ctl1_host(ctl1) || ocMap != xusb_oc_map_host(ocMap)) {
        w(padctl, XUSB_CHRG_OTGPAD0_CTL0, xusb_chrg_ctl0_host(ctl0));
        w(padctl, XUSB_CHRG_OTGPAD0_CTL1, xusb_chrg_ctl1_host(ctl1));
        w(padctl, XUSB_USB2_OC_MAP, xusb_oc_map_host(ocMap));
        IOLog("SwitchUSB: host role: pad 0 had charger-detect or over-current settings left (CHRG_OTGPAD0 %#x/%#x, "
              "USB2_OC_MAP %#x), now %#x/%#x, %#x\n",
              ctl0, ctl1, ocMap, xusb_chrg_ctl0_host(ctl0), xusb_chrg_ctl1_host(ctl1), xusb_oc_map_host(ocMap));
    }
}

namespace {
uint32_t readCapability(void *context, uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(static_cast<volatile uint8_t *>(context) + offset);
}
}  // namespace

unsigned Controller::otgPort()
{
    if (!isPowered)
        return 0;
    if (!usb2First) {
        const xhci_port_range usb2 = xhci_find_ports(&readCapability, (void *)regs, (uint32_t)kXhciBytes, 2);
        usb2First = usb2.first;
        usb2Count = usb2.count;
    }
    return usb2First;
}

unsigned Controller::ssPort()
{
    if (!isPowered)
        return 0;
    if (!usb3Count) {
        const xhci_port_range usb3 = xhci_find_ports(&readCapability, (void *)regs, (uint32_t)kXhciBytes, 3);
        usb3First = usb3.first;
        usb3Count = usb3.count;
    }
    return xusb_ss_xhci_port(usb3First, usb3Count);
}

uint32_t Controller::portsc(unsigned port) const
{
    return xhci(xhci_portsc_offset(capLength(), port));
}

bool Controller::setPortPower(unsigned port, bool on, uint32_t trace[2])
{
    const uint32_t at = xhci_portsc_offset(capLength(), port);
    trace[0] = xhci(at);
    w(regs, at, xhci_portsc_power(trace[0], on));
    bool followed = false;
    for (int i = 0; i < 100 && !followed; ++i) {
        followed = ((xhci(at) & XHCI_PORTSC_PP) != 0) == on;
        if (!followed)
            IODelay(10);
    }
    trace[1] = xhci(at);
    return followed;
}

bool Controller::cyclePortPower(unsigned port, uint32_t trace[3])
{
    uint32_t offTrace[2], onTrace[2];
    const bool off = setPortPower(port, false, offTrace);
    const bool on = setPortPower(port, true, onTrace);
    trace[0] = offTrace[0];
    trace[1] = offTrace[1];
    trace[2] = onTrace[1];
    return off && on;
}

void Controller::describePads(char *buffer, size_t size)
{
    if (!isMapped) {
        snprintf(buffer, size, "PADCTL not mapped");
        return;
    }
    padctlUp();
    const uint32_t vbusId = r(padctl, XUSB_USB2_VBUS_ID);
    snprintf(buffer, size,
             "PADCTL PORT_CAP %#x VBUS_ID %#x (ID %s, VBUS override %s) PAD_MUX %#x OTG_PAD0 %#x/%#x CHRG %#x/%#x "
             "OC_MAP %#x VBUS_OC_MAP %#x OC_DET %#x ELPG %#x/%#x SS_PORT_MAP %#x BIAS %#x",
             r(padctl, XUSB_USB2_PORT_CAP), vbusId, xusb_id_name(vbusId),
             (vbusId & XUSB_VBUS_ID_VBUS_ON) ? "on" : "off", r(padctl, USB2_PAD_MUX), r(padctl, OTG_PAD0_CTL_0),
             r(padctl, OTG_PAD0_CTL_1), r(padctl, XUSB_CHRG_OTGPAD0_CTL0), r(padctl, XUSB_CHRG_OTGPAD0_CTL1),
             r(padctl, XUSB_USB2_OC_MAP), r(padctl, XUSB_VBUS_OC_MAP), r(padctl, XUSB_OC_DET),
             r(padctl, ELPG_PROGRAM_0), r(padctl, ELPG_PROGRAM_1), r(padctl, SS_PORT_MAP), r(padctl, BIAS_PAD_CTL_0));
}

void Controller::describePorts(char *buffer, size_t size)
{
    if (!isPowered) {
        snprintf(buffer, size, "xHCI not powered");
        return;
    }
    const uint32_t op = capLength();
    const unsigned ports = xhci(4) >> 24, pad = otgPort(), ss = ssPort();
    const uint32_t padPortsc = pad ? portsc(pad) : 0, ssPortsc = ss ? portsc(ss) : 0;
    int n = snprintf(buffer, size,
                     "xHCI USBCMD %#x USBSTS %#x, pad 0 = port %u (USB2 %u+%u): %s %s, SS port 0 = port %u (USB3 "
                     "%u+%u): %s %s; PORTSC",
                     xhci(op), xhci(op + 4), pad, usb2First, usb2Count, xhci_portsc_link_name(padPortsc),
                     xhci_portsc_speed_name(padPortsc), ss, usb3First, usb3Count, xhci_portsc_link_name(ssPortsc),
                     xhci_portsc_speed_name(ssPortsc));
    for (unsigned port = 1; port <= ports && port <= 16 && n > 0 && (size_t)n < size; ++port) {
        n += snprintf(buffer + n, size - (size_t)n, " %u:%#x", port, xhci(xhci_portsc_offset(op, port)));
    }
}

bool Controller::takeMailboxInterrupt()
{
    uint32_t smi = r(fpci, XUSB_CFG_ARU_SMI_INTR);
    if (!(smi & (SMI_INTR_EN | SMI_INTR_FW_HANG)))
        return false;
    w(fpci, XUSB_CFG_ARU_SMI_INTR, smi);
    (void)r(fpci, XUSB_CFG_ARU_SMI_INTR);  // Linux reads again against a spurious ARU SMI
    if (smi & SMI_INTR_FW_HANG)
        mailboxHang = true;
    return true;
}

bool Controller::mailboxPending() const
{
    return r(fpci, XUSB_CFG_ARU_SMI_INTR) & (SMI_INTR_EN | SMI_INTR_FW_HANG);
}

// tegra_xusb_mbox_send: ACK/NAK go out while the firmware still owns the mailbox; anything else takes ownership first
// and waits (bounded: a bring-up handshake, as Linux) until the firmware has read it.
bool Controller::sendMailbox(uint32_t cmd, uint32_t data)
{
    const bool reply = cmd == MBOX_ACK || cmd == MBOX_NAK;
    if (!reply) {
        if (r(fpci, XUSB_CFG_ARU_MBOX_OWNER) != MBOX_OWNER_NONE)
            return false;
        w(fpci, XUSB_CFG_ARU_MBOX_OWNER, MBOX_OWNER_SW);
        if (r(fpci, XUSB_CFG_ARU_MBOX_OWNER) != MBOX_OWNER_SW)
            return false;
    }
    w(fpci, XUSB_CFG_ARU_MBOX_DATA_IN, (cmd & 0xff) << 24 | (data & 0xffffff));
    rmw(fpci, XUSB_CFG_ARU_MBOX_CMD, 0, MBOX_INT_EN | MBOX_DEST_FALC);
    if (reply)
        return true;
    for (int i = 0; i < 250; ++i) {
        if (r(fpci, XUSB_CFG_ARU_MBOX_OWNER) == MBOX_OWNER_NONE)
            return true;
        IOSleep(1);
    }
    return false;
}

// tegra_xusb_mbox_thread + tegra_xusb_mbox_handle for T210B01 (no SS clock scaling, no HSIC): the falcon clock is
// fixed (ACK only when it asks for the rate it has), SSPI scaling is acknowledged as is, SET_BW needs no answer, and
// the SuperSpeed messages (SS port 0's LFPS detection, the DFE/CTLE context T210 cannot save) are XusbLanes.h's.
bool Controller::serviceMailbox()
{
    if (mailboxHang)
        return false;
    uint32_t message = r(fpci, XUSB_CFG_ARU_MBOX_DATA_OUT), cmd = message >> 24, data = message & 0xffffff;
    rmw(fpci, XUSB_CFG_ARU_MBOX_CMD, MBOX_DEST_SMI, 0);
    if (cmd == MBOX_SET_BW || cmd == MBOX_ACK || cmd == MBOX_NAK) {
        w(fpci, XUSB_CFG_ARU_MBOX_OWNER, MBOX_OWNER_NONE);
        return true;
    }
    const xusb_mbox_ss ss = xusb_mbox_ss_decide(cmd, data);
    if (ss.handled) {
        if (ss.lfps)
            switch_usb_ss_lfps(ss.lfps_enable);
        if (ss.lfps_settle)
            IOSleep(1);
        if (!sendMailbox(ss.reply, ss.data))
            IOLog("SwitchUSB: falcon mailbox reply %u to %u failed\n", ss.reply, cmd);
        return true;
    }
    uint32_t reply = MBOX_ACK, replyData = data;
    switch (cmd) {
    case MBOX_INC_FALC_CLOCK:
    case MBOX_DEC_FALC_CLOCK:
        replyData = kFalconKHz;
        reply = data == kFalconKHz ? MBOX_ACK : MBOX_NAK;
        break;
    case MBOX_INC_SSPI_CLOCK:
    case MBOX_DEC_SSPI_CLOCK:
    case MBOX_START_HSIC_IDLE:
    case MBOX_STOP_HSIC_IDLE:
        break;
    default:
        IOLog("SwitchUSB: unknown XUSB falcon mailbox message %#x\n", message);
        reply = MBOX_NAK;
        break;
    }
    if (!sendMailbox(reply, replyData))
        IOLog("SwitchUSB: falcon mailbox reply %u to %u failed\n", reply, cmd);
    return true;
}

// The interrupt path from the xHCI to the GIC, as tegra_xusb_config programs it: the interrupter (IMAN0 IP/IE), the
// controller (USBCMD.INTE, USBSTS.EINT), the FPCI bridge (CFG_1 memory and bus master), the IPFS wrapper (EN_FPCI,
// INTR_MASK.IP_INT_MASK; INTR_ENABLE and MSI_EN_VEC0 as the chip has them: Linux never writes them), the mailbox
// (SMI_INTR, owner), and whether powerOn found the partitions already on (a boot stage left them: reset not pulsed).
void Controller::describe(char *buffer, size_t size) const
{
    const uint32_t op = capLength(), rt = runtimeOffset(), iman = xhci(rt + 0x20), cmd = xhci(op), sts = xhci(op + 4);
    const unsigned port = usb2First;
    snprintf(
        buffer, size,
        "USBCMD %#x (INTE %u) USBSTS %#x (EINT %u) IMAN0 %#x (IP %u IE %u) ERDP0 %#x PORTSC%u %#x; FPCI CFG_1 %#x; "
        "IPFS CONFIGURATION %#x INTR_MASK %#x INTR_ENABLE %#x MSI_EN_VEC0 %#x; SMI %#x MBOX_OWNER %#x; "
        "partitions found on %#x",
        cmd, !!(cmd & XHCI_USBCMD_INTE), sts, !!(sts & XHCI_USBSTS_EINT), iman, !!(iman & XHCI_IMAN_IP),
        !!(iman & XHCI_IMAN_IE), xhci(rt + 0x38), port, port ? xhci(xhci_portsc_offset(op, port)) : 0,
        r(fpci, XUSB_CFG_1), r(ipfs, IPFS_CONFIGURATION), r(ipfs, IPFS_INTR_MASK), r(ipfs, IPFS_INTR_ENABLE),
        r(ipfs, IPFS_MSI_EN_VEC0), r(fpci, XUSB_CFG_ARU_SMI_INTR), r(fpci, XUSB_CFG_ARU_MBOX_OWNER), partitionsFound);
}

bool Controller::interruptSample(uint32_t &iman, uint32_t &usbcmd, uint32_t &usbsts, uint32_t &erdp) const
{
    if (!isPowered)
        return false;
    const uint32_t op = capLength(), rt = runtimeOffset();
    iman = xhci(rt + 0x20);
    usbcmd = xhci(op);
    usbsts = xhci(op + 4);
    erdp = xhci(rt + 0x38);
    return true;
}

}  // namespace xhost

bool switch_xusb_firmware_range(uint64_t *pa, uint64_t *bytes)
{
    if (!xhost::gFirmware.va)
        return false;
    *pa = xhost::gFirmware.pa;
    *bytes = xhost::gFirmware.size;
    return true;
}
