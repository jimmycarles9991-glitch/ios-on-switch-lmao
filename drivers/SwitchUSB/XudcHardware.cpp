// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 XUSB device controller hardware layer; see XudcHardware.h.
#include "XudcHardware.h"
#include "XudcSettle.h"
#include "UsbDmaCache.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "XusbPadctl.h"
#include "XusbUtmiPad.h"
#include "XusbPartitions.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>

namespace xudc {
namespace {
// Physical bases: Tegra X1 TRM; hekate bdk/soc/t210.h. Page-aligned maps.
constexpr uint64_t kCarPA = 0x60006000, kFusePagePA = 0x7000F000, kMcPA = 0x70019000, kPadctlPA = 0x7009F000,
                   kXudcPA = 0x700D0000;
constexpr uint32_t kXudcBytes = 0xA000;

enum : uint32_t {  // clock and reset controller (hekate bdk/soc/clock.h)
    RST_DEVICES_U = 0x00c,
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
};

enum : uint32_t {
    FUSE_USB_CALIB = 0x1f0,
    FUSE_USB_CALIB_EXT = 0x350
};  // from 0x7000F800

enum : uint32_t {  // XUSB pad controller (hekate usb_t210.h)
    USB2_PAD_MUX = 0x004,
    USB2_PORT_CAP = 0x008,
    SS_PORT_MAP = 0x014,
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
    EP_STOPPED = 0x78,
    SSPX_CORE_CNT0 = 0x610,
    SSPX_CORE_CNT30 = 0x688,
    SSPX_CORE_CNT32 = 0x690,
    SSPX_CORE_PADCTL4 = 0x750,
    CFG_DEV_SSPI_XFER = 0x858,
    CFG_DEV_FE = 0x85c,
    FPCI_CFG_1 = 0x8004,
    FPCI_CFG_4 = 0x8010,
    FPCI_CFG_5 = 0x8014,
    IPFS_FPCI_BAR0 = 0x9080,
    IPFS_CONFIGURATION = 0x9180,
    IPFS_INTR_MASK = 0x9188,
    IPFS_INTR_CODE = 0x918c,       // T210 TRM 22.16.13.35
    IPFS_INTR_SIGNATURE = 0x9190,  // 22.16.13.36
};

enum : uint32_t {
    CTRL_RUN = 1u << 0,
    CTRL_LSE = 1u << 1,
    CTRL_IE = 1u << 4,
    CTRL_ENABLE = 1u << 31,
    ST_RC = 1u << 0,
    ST_IP = 1u << 4,
    ST_DSE = 1u << 5,  // an error on the FPCI wrapper (T210 TRM, XUSB_DEV_XHCI_ST)
    EREP_ECS = 1u << 0,
    ERDP_EHB = 1u << 3,
    PORTSC_CCS = 1u << 0,
    PORTSC_PED = 1u << 1,
    PORTSC_PR = 1u << 4,
    PORTSC_PLS_MASK = 0xfu << 5,
    PORTSC_PLS_U0 = 0u << 5,
    PORTSC_PLS_U3 = 3u << 5,
    PORTSC_PLS_RXDETECT = 5u << 5,
    PORTSC_LWS = 1u << 16,
    PORTSC_CSC = 1u << 17,
    PORTSC_WRC = 1u << 19,
    PORTSC_PRC = 1u << 21,
    PORTSC_PLC = 1u << 22,
    PORTSC_CEC = 1u << 23,
    PORTSC_WPR = 1u << 30,
    PORTSC_CHANGES = PORTSC_CSC | PORTSC_WRC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC,
    PORTPM_L1S_MASK = 3u,
    PORTPM_L1S_NYET = 2u,  // L1 refused for now: no USB2 LPM (Linux tegra210 and tegra210b01 soc: lpm_enable = false)
    PORTHALT_HALT_LTSSM = 1u << 0,
    PORTHALT_STCHG_REQ = 1u << 20,
    PORTHALT_STCHG_INTR_EN = 1u << 24,
    FE_PORTREGSEL_MASK = 3u,
    FE_PORTREGSEL_SS = 1u,
    FE_PORTREGSEL_HSFS = 2u,
    FE_INFINITE_SS_RETRY = 1u << 29,
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
constexpr unsigned kPortPasses = 8;  // Hardware::portChange

const char *registerName(uint32_t offset)
{
    switch (offset) {
    case EP_HALT:
        return "EP_HALT";
    case EP_PAUSE:
        return "EP_PAUSE";
    case EP_RELOAD:
        return "EP_RELOAD";
    case EP_STCHG:
        return "EP_STCHG";
    case EP_STOPPED:
        return "EP_STOPPED";
    default:
        return "EP_THREAD_ACTIVE";
    }
}

inline void barrier()
{
    __asm__ volatile("dsb sy" ::: "memory");
}
}  // namespace

bool DmaBuffer::allocate(uint32_t bytes, bool writeCombined)
{
    if (!switch_dma_alloc(&buffer, bytes, kSwitchDMABelow4G, writeCombined ? kIOMapWriteCombineCache : 0))
        return false;
    va = buffer.va;
    pa = static_cast<uint32_t>(buffer.pa);
    size = bytes;
    return true;
}

void DmaBuffer::release()
{
    switch_dma_free(&buffer);
    va = nullptr;
    pa = 0;
    size = 0;
}

bool Hardware::map()
{
    car = switch_mmio_map(kCarPA, 0x1000);
    fusePage = switch_mmio_map(kFusePagePA, 0x1000);
    mc = switch_mmio_map(kMcPA, 0x1000);
    padctl = switch_mmio_map(kPadctlPA, 0x1000);
    xudc = switch_mmio_map(kXudcPA, kXudcBytes);
    return (car != nullptr) && (fusePage != nullptr) && (mc != nullptr) && (padctl != nullptr) && (xudc != nullptr);
}

// A partition already on (the boot chain's, the host's) is left as it is but for its clock, which SC7 takes with the
// CAR. Otherwise its clock goes on only once the partition is (switch_pmc_power_up).
bool Hardware::powerUp(unsigned partition, unsigned id, const switch_pmc_mc_client *client)
{
    if (switch_pmc_partition_is_on(partition)) {
        switch_car_clock_enable(id);
        return true;
    }
    return switch_pmc_power_up(partition, id, id, client);
}

// hekate xusb_device_init(): clocks, pads, FPCI/IPFS, event ring, EP0. The XUSB top and pad controller are shared
// with the PCIe controller (drivers/SwitchWLAN): their clocks are enabled and resets released, never asserted.
bool Hardware::powerOn(char *why, size_t whySize)
{
    const uint32_t asid = switch_mmio_read32(mc, TEGRA_SMMU_XUSB_DEV_ASID);
    if ((asid & 0x80000000u) && (asid & 0x7fu) != TEGRA_SMMU_ASID_XUSB_DEV) {
        snprintf(why, whySize, "XUSB_DEV is translated by another SMMU ASID (MC %#x = %#x)", TEGRA_SMMU_XUSB_DEV_ASID,
                 asid);
        return false;
    }
    if ((!control.va && !control.allocate(kControlBytes, true)) ||
        (!events.va && !events.allocate(kEventBytes, true))) {
        snprintf(why, whySize, "no DMA memory below 4 GiB");
        return false;
    }
    // EP0's ring in the control area; data endpoints get their own at their first initEndpoint.
    rings[0].trb = reinterpret_cast<volatile uint32_t *>(control.va + kRingOffset);
    rings[0].pa = control.pa + kRingOffset;
    rings[0].trbs = kRingTrbs;

    // The legacy USB2 device controller stays off; XUSB and its pad block on.
    switch_mmio_write32(car, RST_DEV_L_SET, L_USBD);
    switch_mmio_write32(car, CLK_ENB_L_CLR, L_USBD);
    switch_mmio_write32(car, CLK_ENB_W_SET, W_XUSB);
    switch_mmio_write32(car, RST_DEV_W_CLR, W_XUSB | W_XUSB_PADCTL);
    IODelay(2);
    if (!xpad::startPlls(car)) {
        snprintf(why, whySize, "PLLU or UTMIPLL did not lock (PLLU_BASE 0x%08x UTMIPLL_HW_PWRDN_CFG0 0x%08x)",
                 switch_mmio_read32(car, PLLU_BASE), switch_mmio_read32(car, UTMIPLL_HW_PWRDN_CFG0));
        return false;
    }

    // Clock sources, the port assignment and pad, then partitions XUSBB and XUSBA (each partition's clock after it).
    // Device core 102 MHz from PLLP, FS logic from FO 48 MHz, SS 120 MHz from HSIC 480.
    switch_mmio_write32(car, PLLU_OUTA, switch_mmio_read32(car, PLLU_OUTA) | 1);
    IODelay(2);
    switch_mmio_write32(car, CLK_SOURCE_XUSB_CORE_DEV,
                        (switch_mmio_read32(car, CLK_SOURCE_XUSB_CORE_DEV) & 0x1fffff00u) | 1u << 29 | 6);
    switch_mmio_write32(car, CLK_SOURCE_XUSB_FS,
                        (switch_mmio_read32(car, CLK_SOURCE_XUSB_FS) & 0x1fffff00u) | 2u << 29);
    switch_mmio_write32(car, CLK_SOURCE_XUSB_SS,
                        (switch_mmio_read32(car, CLK_SOURCE_XUSB_SS) & 0x1fffff00u) | 3u << 29 | 6);
    // OTG pad 0 and the bias pad to XUSB while the controller is still in reset: "the XUSB and USB2 controllers should be
    // under reset when updating the port assignments" (Tegra X1 TRM 22.8.4 step 5, p.1350).
    switch_padctl_modify(padctl, USB2_PAD_MUX, 3u << 18 | 3u, 1u << 18 | 1u);
    xpad::powerOn(car, fusePage, padctl);
    switch_padctl_modify(padctl, SS_PORT_MAP, 0xfu, 0);
    switch_pmc_rmw(USB_AO, 0xcu, 0);  // ID and VBUS wake detectors powered
    IODelay(1);
    // Tegra X1 TRM 22.8.7.5 (ELPG exit): XUSBB before XUSBA, one at a time, as tegra_xudc_unpowergate.
    if (!powerUp(xusb::PART_XUSBB, xusb::kClockXusbDev, &xusb::kXusbDevClient) ||
        !powerUp(xusb::PART_XUSBA, xusb::kClockXusbSs, nullptr)) {
        snprintf(why, whySize, "XUSB partitions did not power up (PMC PWRGATE_STATUS 0x%08x)",
                 switch_pmc_read(PWRGATE_STATUS));
        return false;
    }
    switch_mmio_write32(car, RST_DEV_W_CLR, W_XUSB_SS);
    switch_mmio_write32(car, RST_DEV_U_CLR, U_XUSB_DEV);
    IODelay(2);

    // IPFS bridge, then the controller's PCI-style config space: BAR0 = its own base (Linux tegra_xudc_fpci_ipfs_init).
    // The controller's BAR as memory-mapped space, posted writes (Tegra X1 TRM 22.8.4 step 9: FPCI_BAR0_ACCESS_TYPE 0);
    // Linux leaves the reset value, non-posted I/O or config access.
    xw(IPFS_FPCI_BAR0, xr(IPFS_FPCI_BAR0) & ~1u);
    xw(IPFS_CONFIGURATION, xr(IPFS_CONFIGURATION) | 1);
    IODelay(10);
    xw(FPCI_CFG_1, xr(FPCI_CFG_1) | 6);  // memory space, bus master
    IODelay(1);
    xw(FPCI_CFG_4, static_cast<uint32_t>(kXudcPA));
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
    // Linux tegra_xudc_gadget_start: a link state change request (PORTHALT_STCHG_REQ, the LTSSM held until software
    // releases it) raises a port status event instead of waiting for the next one.
    xw(PORTHALT, xr(PORTHALT) | PORTHALT_STCHG_INTR_EN);
    xw(ECPLO, control.pa + kContextOffset);
    xw(ECPHI, 0);
    // L4T tegra_xudc_device_params_init: LFPS and link-configuration timing, and the SSPI transaction timeout.
    xw(SSPX_CORE_CNT0, (xr(SSPX_CORE_CNT0) & ~0xffu) | 0xau);
    xw(SSPX_CORE_CNT30, (xr(SSPX_CORE_CNT30) & ~0xfffffu) | 0x978u);
    xw(SSPX_CORE_CNT32, (xr(SSPX_CORE_CNT32) & ~0xffu) | 0xb0u);
    xw(SSPX_CORE_PADCTL4, (xr(SSPX_CORE_PADCTL4) & ~0xfffffu) | 0x5dc0u);
    xw(CFG_DEV_SSPI_XFER, 0xf000u);
    xw(CFG_DEV_FE, xr(CFG_DEV_FE) | FE_INFINITE_SS_RETRY);
    return true;
}

// hekate xusb_device_enumerate(): VBUS/ID overrides, enable, port to RxDetect, pull-up on; interrupt moderation as
// Linux tegra_xudc_init sets it (IMODI and the counter, kImodUs). Of the ELPG registers hekate zeroes, only what
// tegra_xudc_device_mode_on's phy_power_on(utmi) changes (the fake SS port, XusbPadctl.h): ELPG_PROGRAM_0's wake bits
// and the other ports' and PCIe's AUX clamps in ELPG_PROGRAM_1 are left to their owners.
void Hardware::connect()
{
    xw(RT_IMOD, (kImodUs * 4) << 16 | (kImodUs * 4));
    switch_usb_padctl_fake_port_up(padctl);
    switch_padctl_modify(padctl, USB2_VBUS_ID, VBUS_ID_VBUS_OVR_MASK | VBUS_ID_SRC_MASK,
                         VBUS_ID_VBUS_OVR_EN | VBUS_ID_SRC_ID_OVR_EN);
    xw(PORTHALT, xr(PORTHALT) & ~PORTHALT_HALT_LTSSM);
    // A change latched while the port was off the bus (the host role drives the same pad) is not this connection's:
    // cleared in both port instances (Linux tegra_xudc_device_params_init's PORTREGSEL) before the controller is enabled.
    constexpr uint32_t kInstances[] = {FE_PORTREGSEL_HSFS, FE_PORTREGSEL_SS};
    for (const uint32_t instance : kInstances) {
        xw(CFG_DEV_FE, (xr(CFG_DEV_FE) & ~FE_PORTREGSEL_MASK) | instance);
        if (const uint32_t latched = xr(PORTSC) & PORTSC_CHANGES)
            clearPortChange(latched);
    }
    xw(CFG_DEV_FE, xr(CFG_DEV_FE) & ~FE_PORTREGSEL_MASK);
    xw(CTRL, xr(CTRL) | CTRL_ENABLE);
    for (const uint32_t instance : kInstances) {
        xw(CFG_DEV_FE, (xr(CFG_DEV_FE) & ~FE_PORTREGSEL_MASK) | instance);
        xw(PORTSC, (xr(PORTSC) & ~(PORTSC_PLS_MASK | PORTSC_CHANGES)) | PORTSC_LWS | PORTSC_PLS_RXDETECT);
    }
    xw(CFG_DEV_FE, xr(CFG_DEV_FE) & ~FE_PORTREGSEL_MASK);
    switch_padctl_modify(padctl, USB2_VBUS_ID, VBUS_ID_OVR_MASK, VBUS_ID_OVR_FLOAT);
}

void Hardware::setVbus(bool present)
{
    switch_padctl_modify(padctl, USB2_VBUS_ID, VBUS_ID_VBUS_ON, present ? VBUS_ID_VBUS_ON : 0);
}

// Device mode off and the VBUS override low: the pull-up goes away, and SS port 0 is clamped again.
void Hardware::disconnect()
{
    xw(CTRL, xr(CTRL) & ~(CTRL_ENABLE | CTRL_RUN));
    switch_padctl_modify(padctl, USB2_VBUS_ID, VBUS_ID_VBUS_ON, 0);
    switch_usb_padctl_fake_port_down(padctl);
}

bool Hardware::nextEvent(Event &event)
{
    volatile uint32_t *e = reinterpret_cast<volatile uint32_t *>(events.va) + static_cast<size_t>(eventIndex) * 4;
    const uint32_t d3 = e[3];
    if ((d3 & TRB_CYCLE) != eventCycle)
        return false;
    switch_dma_consume();
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

bool Hardware::eventPending() const
{
    volatile uint32_t *e = reinterpret_cast<volatile uint32_t *>(events.va) + static_cast<size_t>(eventIndex) * 4;
    return (e[3] & TRB_CYCLE) == eventCycle;
}

bool Hardware::nextTransferEventReady(uint32_t blockedDcis) const
{
    volatile uint32_t *e = reinterpret_cast<volatile uint32_t *>(events.va) + static_cast<size_t>(eventIndex) * 4;
    const uint32_t d3 = e[3];
    if ((d3 & TRB_CYCLE) != eventCycle)
        return true;
    const uint32_t type = (d3 >> 10) & 0x3fu;
    const uint32_t dci = (d3 >> 16) & 0x1fu;
    return type == TRB_TRANSFER_EVENT && dci >= 2u && (blockedDcis & (1u << dci)) == 0u;
}

void Hardware::setSettleWait(SettleSleep fn, void *context)
{
    settleSleep = fn;
    settleContext = context;
}

// Linux tegra_xudc_process_event_ring(): the new dequeue pointer with EHB written as 1 (W1C), which re-enables the
// interrupt. ST.IP is not touched here: an IP raised for events that arrived after the drain must survive.
void Hardware::finishEvents()
{
    xw(ERDPHI, 0);
    xw(ERDPLO, (events.pa + eventIndex * 16) | ERDP_EHB);
}

// Linux tegra_xudc_irq(): acknowledge ST.IP alone (ST.RC is W1C too), before the ring is drained.
// ST.DSE is acknowledged here too, with the IPFS's error code and signature kept for the work loop's one line (code 0
// re-arms the IPFS's capture). The BAR takes posted writes (powerOn), so the acknowledgement is read back before the
// filter returns: the level line must have dropped before the GIC's end of interrupt, or it fires again for nothing.
bool Hardware::takeInterrupt()
{
    const uint32_t ack = xr(ST) & (ST_IP | ST_DSE);
    if (ack == 0u)
        return false;
    if ((ack & ST_DSE) != 0u && !systemError.seen) {
        systemError = {true, xr(IPFS_INTR_CODE), xr(IPFS_INTR_SIGNATURE)};
        xw(IPFS_INTR_CODE, 0);
    }
    xw(ST, ack);
    (void)xr(ST);
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
    const Ring &ring = rings[0];
    volatile uint32_t *c = context(0);
    c[2] = ((ring.pa + ring.enqueue * 16) & ~0xfu) | ring.cycle;
    c[3] = 0;
    c[5] &= 0x00ffffffu;  // rsvd, partial_td, splitxstate, seq_num
    c[11] &= ~0x7fu;      // device address
    c[1] = (c[1] & 0x0000ffffu) | ep0Packet(portSpeed(xr(PORTSC))) << 16;
    switch_dma_publish();
    xw(CTRL, xr(CTRL) & ~(0x7fu << 24));
    if (const uint32_t halted = xr(EP_HALT))  // Linux ep_unhalt_all
        (void)changeState(EP_HALT, halted, 0);
    (void)reload(0);
    (void)toggleBit(EP_PAUSE, 0, false);
}

// Linux __tegra_xudc_handle_port_status(), run until no change is left (tegra_xudc_handle_port_status). Port reset and
// connect also get Linux's tegra_xudc_port_connect() setup: USB2 LPM answered NYET, ST.RC cleared.
PortChange Hardware::portChange()
{
    PortChange change;
    for (unsigned pass = 0;; ++pass) {
        if (pass == kPortPasses) {
            IOLog("SwitchUSB: port changes still set after %u passes (PORTSC %#x, PORTHALT %#x): left for the next "
                  "port event\n",
                  kPortPasses, xr(PORTSC), xr(PORTHALT));
            break;
        }
        const uint32_t halt = xr(PORTHALT);
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
                resetEp0();
                change.connected = connectNow = true;
            } else {
                resetEp0();
                change.disconnected = true;
            }
        }
        if (connectNow) {
            change.speed = portSpeed(xr(PORTSC));
            if (change.speed == kSpeedSuper)
                xw(PORTPM, xr(PORTPM) & ~(0xffu << 16));  // T210/T210B01 disable U1, as L4T.
            else
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

volatile uint32_t *Hardware::context(uint32_t dci) const
{
    return reinterpret_cast<volatile uint32_t *>(control.va + kContextOffset) + static_cast<size_t>(dci) * 16;
}

void Hardware::resetRing(uint32_t dci)
{
    Ring &ring = rings[dci];
    for (uint32_t i = 0; i < ring.trbs * 4; ++i)
        ring.trb[i] = 0;
    volatile uint32_t *link = ring.trb + static_cast<size_t>(ring.trbs - 1) * 4;
    link[0] = ring.pa;
    link[3] = trbType(TRB_LINK) | TRB_TOGGLE;
    ring.enqueue = 0;
    ring.cycle = 1;
    switch_dma_publish();
}

// hekate xusb_ep_ctx_t / Linux tegra_xudc_ep_context_setup: state running, CErr/CEC 3, dequeue = ring start with
// DCS 1, Linux's average TRB length per type (control 8, interrupt 1024, bulk and isochronous 3072), the max burst
// (SuperSpeed; at high speed a periodic endpoint's extra transactions per microframe); EP0 keeps the current device
// address.
bool Hardware::initEndpoint(uint32_t dci, uint32_t type, uint32_t maxPacket, uint32_t interval, uint32_t maxBurst)
{
    Ring &ring = rings[dci];
    if (dci && !ring.trb) {
        if (!ring.written)
            ring.written = IONewZero(Written, kDataRingTrbs);
        if (!ring.written || !ring.own.allocate(kDataRingTrbs * 16, true))
            return false;
        ring.trb = reinterpret_cast<volatile uint32_t *>(ring.own.va);
        ring.pa = ring.own.pa;
        ring.trbs = kDataRingTrbs;
    }
    resetRing(dci);
    volatile uint32_t *c = context(dci);
    const uint32_t address = dci == 0 ? (xr(CTRL) >> 24) & 0x7f : 0;
    const bool interrupt = type == EP_INTR_IN || type == EP_INTR_OUT;
    const bool periodic = interrupt || type == EP_ISOCH_IN || type == EP_ISOCH_OUT;
    uint32_t average = interrupt ? 1024 : 3072;
    if (type == EP_CONTROL)
        average = 8;
    const uint32_t esit = periodic ? maxPacket * (maxBurst + 1) : 0;
    for (uint32_t i = 0; i < 16; ++i)
        c[i] = 0;
    c[0] = 1 | interval << 16;
    if ((type == EP_BULK_IN || type == EP_BULK_OUT) && portSpeed(xr(PORTSC)) == kSpeedSuper)
        c[0] |= 1u << 15;
    c[1] = 3u << 1 | type << 3 | (maxBurst & 0xff) << 8 | maxPacket << 16;
    c[2] = (rings[dci].pa & ~0xfu) | 1;
    c[4] = average | esit << 16;
    c[6] = 3u << 18;
    c[11] = address;
    switch_dma_publish();
    return true;
}

// Endpoint state changes raise no interrupt (T210 TRM 22.9.4-5): check immediately and poll briefly for microsecond
// completions, then release the work-loop gate via settleSleep if still pending up to kStateChangeMs.
bool Hardware::settled(uint32_t offset, uint32_t mask, uint32_t value)
{
    uint32_t seen = 0;
    const bool done = settleRegister(
        mask, value, kFastSettleUs, kStateChangeMs,
        [this, offset] {
            return xr(offset);
        },
        [] {
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
            return ns;
        },
        [this, mask](uint64_t remainingNs) {
            if (settleSleep) {
                settleSleep(settleContext, mask, remainingNs);
            } else {
                const uint32_t ms = static_cast<uint32_t>((remainingNs + 999999ULL) / 1000000ULL);
                IOSleep(ms ? ms : 1u);
            }
        },
        &seen);
    if (done)
        return true;
    IOLog("SwitchUSB: DCI bits %#x: %s reads %#x, not %#x, %u ms after the change\n", mask, registerName(offset), seen,
          value, kStateChangeMs);
    return false;
}

// Linux ep_reload.
bool Hardware::reload(uint32_t dci)
{
    xw(EP_RELOAD, 1u << dci);
    return settled(EP_RELOAD, 1u << dci, 0);
}

// T210 TRM 22.9.4.2, Set_Configuration in the addressed state, which each endpoint added later follows too: Reload,
// CTRL.RUN right after the first Reload, then the wait for Reload to clear; Pause cleared, its EP_STCHG waited for and
// cleared; Halt cleared and its EP_STCHG cleared, not waited on.
bool Hardware::enableEndpoint(uint32_t dci)
{
    const uint32_t bit = 1u << dci;
    xw(EP_RELOAD, bit);
    run();
    const bool reloaded = settled(EP_RELOAD, bit, 0);
    (void)toggleBit(EP_PAUSE, dci, false);
    xw(EP_HALT, xr(EP_HALT) & ~bit);
    xw(EP_STCHG, bit);
    return reloaded;
}

// Linux __tegra_xudc_ep_enable: reload, unpause, unhalt. Only the reload decides: Linux ignores every one of these
// waits, and an EP_STCHG miss is logged but has not been seen to leave an endpoint unusable.
bool Hardware::reloadEndpoint(uint32_t dci)
{
    const bool reloaded = reload(dci);
    (void)toggleBit(EP_PAUSE, dci, false);
    (void)toggleBit(EP_HALT, dci, false);
    return reloaded;
}

// Linux ep_pause/ep_halt/ep_unhalt_all: change the bits, wait for EP_STCHG to report them, acknowledge it whatever the
// wait saw (write 1 to clear). The bits are cleared first too, so a report left from an earlier change cannot pass this
// wait. With CTRL.RUN clear the controller runs no endpoint and reports nothing: the bits are written for when it does,
// and not waited on (T210 TRM 22.9.5.3-4 step 1).
bool Hardware::changeState(uint32_t reg, uint32_t bits, uint32_t value)
{
    if ((xr(CTRL) & CTRL_RUN) == 0u) {
        xw(reg, (xr(reg) & ~bits) | value);
        return true;
    }
    xw(EP_STCHG, bits);
    xw(reg, (xr(reg) & ~bits) | value);
    const bool done = settled(EP_STCHG, bits, bits);
    xw(EP_STCHG, bits);
    return done;
}

bool Hardware::toggleBit(uint32_t reg, uint32_t dci, bool set)
{
    const uint32_t bit = 1u << dci;
    if (((xr(reg) & bit) != 0u) == set)
        return true;
    return changeState(reg, bit, set ? bit : 0);
}

void Hardware::setHalt(uint32_t dci, bool halt)
{
    (void)toggleBit(EP_HALT, dci, halt);
}

void Hardware::setPause(uint32_t dci, bool pause)
{
    (void)toggleBit(EP_PAUSE, dci, pause);
}

bool Hardware::halted(uint32_t dci)
{
    return ((xr(EP_HALT) >> dci) & 1) != 0u;
}

// T210 TRM 22.9.4.2, removing an endpoint: Pause and its state change; Disabled, with the context past the xHCI
// fields (DW5 on: sequence number, partial TD, the controller's own state) cleared; Reload and its state change,
// acknowledged. EP_STOPPED acknowledged as Linux __tegra_xudc_ep_disable does.
void Hardware::disableEndpoint(uint32_t dci)
{
    const uint32_t bit = 1u << dci;
    (void)toggleBit(EP_PAUSE, dci, true);
    volatile uint32_t *c = context(dci);
    c[0] &= ~7u;
    for (uint32_t i = 5; i < 16; ++i)
        c[i] = 0;
    switch_dma_publish();
    if ((xr(CTRL) & CTRL_RUN) != 0u) {
        xw(EP_STCHG, bit);
        xw(EP_RELOAD, bit);
        (void)settled(EP_STCHG, bit, bit);
        xw(EP_STCHG, bit);
    } else {
        xw(EP_RELOAD, bit);
    }
    if (xr(EP_STOPPED) & bit)
        xw(EP_STOPPED, bit);
}

// Linux __tegra_xudc_ep_set_halt(false): disable, reload, running with the sequence number and partial-TD state
// cleared, reload, unpause, unhalt.
void Hardware::resetSequence(uint32_t dci)
{
    volatile uint32_t *c = context(dci);
    c[0] &= ~7u;
    switch_dma_publish();
    (void)reload(dci);
    c[0] = (c[0] & ~7u) | 1;
    c[5] &= 0x00ffffffu;  // rsvd, partial_td, splitxstate, seq_num
    switch_dma_publish();
    (void)reloadEndpoint(dci);
}

// Linux __tegra_xudc_ep_dequeue (busy case): pause, wait until idle, move the dequeue pointer to the enqueue position,
// clear partial-TD state, reload. The sequence number (data toggle) survives.
void Hardware::dropQueued(uint32_t dci)
{
    const Ring &ring = rings[dci];
    (void)toggleBit(EP_PAUSE, dci, true);
    (void)settled(EP_THREAD_ACTIVE, 1u << dci, 0);
    volatile uint32_t *c = context(dci);
    c[2] = ((ring.pa + ring.enqueue * 16) & ~0xfu) | ring.cycle;
    c[3] = 0;
    c[5] &= 0xf8000000u;  // keep seq_num; clear edtla, rsvd, partial_td, splitxstate
    switch_dma_publish();
    (void)reload(dci);
    (void)toggleBit(EP_PAUSE, dci, false);
}

uint32_t Hardware::push(uint32_t dci, uint32_t bufferPA, uint32_t length, uint32_t flags)
{
    const uint32_t index = pushData(dci, bufferPA, length, 0, flags);
    return rings[dci].pa + index * 16;
}

uint32_t Hardware::pushData(uint32_t dci, uint64_t bufferPA, uint32_t length, uint32_t tdSize, uint32_t flags)
{
    const Slot slot = reserveData(dci, flags);
    fillData(dci, slot, bufferPA, length, tdSize, flags);
    return slot.index;
}

// The link TRB (XudcTransfer.h linkControl) is handed over when the ring wraps; the controller cannot reach it before
// the slot it follows is written.
Hardware::Slot Hardware::reserveData(uint32_t dci, uint32_t flags)
{
    Ring &ring = rings[dci];
    const Slot slot = {ring.enqueue, ring.cycle};
    if (++ring.enqueue == ring.trbs - 1) {
        volatile uint32_t *link = ring.trb + static_cast<size_t>(ring.enqueue) * 4;
        link[3] = linkControl(link[3], flags, ring.cycle);
        ring.enqueue = 0;
        ring.cycle ^= 1;
    }
    return slot;
}

// The TRB, its cycle bit stored last: the controller may be reading the ring.
void Hardware::fillData(uint32_t dci, Slot slot, uint64_t bufferPA, uint32_t length, uint32_t tdSize, uint32_t flags)
{
    const Ring &ring = rings[dci];
    volatile uint32_t *t = ring.trb + static_cast<size_t>(slot.index) * 4;
    t[0] = static_cast<uint32_t>(bufferPA);
    t[1] = static_cast<uint32_t>(bufferPA >> 32);
    t[2] = (length & 0x1ffff) | (tdSize > 31 ? 31 : tdSize) << 17;
    switch_dma_publish();
    t[3] = flags | slot.cycle;
    if (ring.written)
        ring.written[slot.index] = {bufferPA, length & 0x1ffff};
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
    xw(DB, dci << 8 | (dci == 0 ? static_cast<uint32_t>(sequence) << 16 : 0));
}

// SET_ADDRESS: CTRL.DEVADDR and EP0's context (Linux tegra_xudc_ep0_set_address), before the status stage.
void Hardware::setAddress(uint32_t address)
{
    xw(CTRL, (xr(CTRL) & ~(0x7fu << 24)) | (address & 0x7f) << 24);
    volatile uint32_t *c = context(0);
    c[11] = (c[11] & ~0x7fu) | (address & 0x7f);
    switch_dma_publish();
}

// After a babble (the host sent more than a data packet's length): the controller stops the endpoint; once it reports
// that, it is acknowledged and the endpoint halted, so the host's next request is answered with STALL (T210 TRM
// 22.9.8.7; Linux tegra_xudc waits for EP_STOPPED the same way).
void Hardware::haltAfterBabble(uint32_t dci)
{
    const uint32_t bit = 1u << dci;
    (void)settled(EP_STOPPED, bit, bit);
    xw(EP_STOPPED, bit);
    (void)toggleBit(EP_HALT, dci, true);
}

// Back to the addressed state once no data endpoint is left (T210 TRM 22.9.4.2, SET_CONFIGURATION 0; Linux
// __tegra_xudc_ep_disable): the controller gives up the endpoint contexts.
void Hardware::stop()
{
    xw(CTRL, xr(CTRL) & ~CTRL_RUN);
}

void Hardware::run()
{
    if (xr(CTRL) & CTRL_RUN)
        return;
    xw(CTRL, xr(CTRL) | CTRL_RUN);
    xw(ST, ST_RC);  // W1C: writing ST back would also clear a pending IP
}

}  // namespace xudc
