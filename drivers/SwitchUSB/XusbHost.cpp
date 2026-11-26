// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 XUSB host controller hardware layer; see XusbHost.h.
#include "XusbHost.h"
#include "XusbHostRegisters.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "XudcHardware.h"
#include "XusbPorts.h"
#include <IOKit/IOLib.h>
#include <os/log.h>
#include "UsbSmmuMapper.h"
#include "XusbLanes.h"
#include "XusbMailbox.h"
#include "XusbSettle.h"
#include "XusbSuperSpeed.h"

static_assert(xhost::MBOX_OWNER_SW == XUSB_MBOX_OWNER_SW, "XusbMailbox.h's owner is XusbHostRegisters.h's");

namespace xhost {
namespace {
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

static_assert(MBOX_INC_FALC_CLOCK == XUSB_MBOX_INC_FALC_CLOCK && MBOX_DEC_FALC_CLOCK == XUSB_MBOX_DEC_FALC_CLOCK &&
                  MBOX_INC_SSPI_CLOCK == XUSB_MBOX_INC_SSPI_CLOCK && MBOX_DEC_SSPI_CLOCK == XUSB_MBOX_DEC_SSPI_CLOCK &&
                  MBOX_START_HSIC_IDLE == XUSB_MBOX_START_HSIC_IDLE &&
                  MBOX_STOP_HSIC_IDLE == XUSB_MBOX_STOP_HSIC_IDLE && MBOX_ACK == XUSB_MBOX_REPLY_ACK &&
                  MBOX_NAK == XUSB_MBOX_REPLY_NAK,
              "XusbMailbox.h's mailbox numbers are xhci-tegra.c's");
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
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

// The firmware image stays where the falcon reads it (ILOAD_BASE) for as long as the controller runs.
xudc::DmaBuffer gFirmware;
}  // namespace

namespace {
// NOLINTNEXTLINE(misc-const-correctness): xhci_find_ports's reader signature
uint32_t readCapability(void *context, uint32_t offset)
{
    volatile uint8_t *const regs = *static_cast<volatile uint8_t *const *>(context);
    return *reinterpret_cast<volatile uint32_t *>(regs + offset);
}
}  // namespace

// The CSB window: page select in C11_CSBRANGE, 512-byte page at CSB_BASE_ADDR (xhci-tegra.c csb_readl).
uint32_t Controller::csbRead(uint32_t offset)
{
    switch_mmio_write32(fpci, XUSB_CFG_ARU_C11_CSBRANGE, (offset >> 9) & 0x7fffff);
    return switch_mmio_read32(fpci, XUSB_CFG_CSB_BASE_ADDR + (offset & 0x1ff));
}

void Controller::csbWrite(uint32_t offset, uint32_t value)
{
    switch_mmio_write32(fpci, XUSB_CFG_ARU_C11_CSBRANGE, (offset >> 9) & 0x7fffff);
    switch_mmio_write32(fpci, XUSB_CFG_CSB_BASE_ADDR + (offset & 0x1ff), value);
}

// tegra_xhci_load_firmware: the falcon's boot code is DMAed from system memory into L2IMEM, the falcon starts at
// boot_codetag and, once it runs the xHCI, clears USBSTS.CNR. A controller whose ILOAD_BASE is already set kept its
// firmware (we never reset it after this).
bool Controller::loadFirmware(const uint8_t *image, size_t size, char *why, size_t whySize)
{
    if (csbRead(CSB_MP_ILOAD_BASE_LO) != 0)
        return true;
    const uint32_t length = size >= kFwHeader ? le32(image + kFwLength) : 0;
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
    switch_dcache_clean_invalidate(gFirmware.va, length);
    const uint32_t codeTag = le32(image + kFwCodeTag);
    const uint32_t codeSize = le32(image + kFwCodeSize);
    const uint32_t tagBlocks = (codeTag + IMEM_BLOCK - 1) / IMEM_BLOCK;
    const uint32_t sizeBlocks = (codeSize + IMEM_BLOCK - 1) / IMEM_BLOCK;
    const uint64_t base = static_cast<uint64_t>(gFirmware.pa) + kFwHeader;
    csbWrite(CSB_MP_ILOAD_ATTR, length);
    csbWrite(CSB_MP_ILOAD_BASE_HI, static_cast<uint32_t>(base >> 32));
    csbWrite(CSB_MP_ILOAD_BASE_LO, static_cast<uint32_t>(base));
    csbWrite(CSB_MP_APMAP, APMAP_BOOTPATH);
    csbWrite(CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_INVALIDATE_ALL);
    csbWrite(CSB_MP_L2IMEMOP_SIZE, (tagBlocks & 0x3ff) << 8 | (sizeBlocks & 0xff) << 24);
    csbWrite(CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_LOAD_LOCKED_RESULT);
    csbWrite(FALC_IMFILLCTL, sizeBlocks);
    csbWrite(FALC_IMFILLRNG1, (tagBlocks & 0xffff) | ((tagBlocks + sizeBlocks) & 0xffff) << 16);
    csbWrite(FALC_DMACTL, 0);
    // tegra_xusb_load_firmware polls RESULT_VLD for up to 10 ms, 50-60 us apart.
    const bool loaded = xusb::settle("falcon IMEM load", 1, 10, [this] {
        return (csbRead(CSB_MP_L2IMEMOP_RESULT) & L2IMEMOP_RESULT_VLD) != 0u;
    });
    if (!loaded) {
        snprintf(why, whySize, "falcon DMA not ready (L2IMEMOP_RESULT %#x)", csbRead(CSB_MP_L2IMEMOP_RESULT));
        return false;
    }
    csbWrite(FALC_BOOTVEC, codeTag);
    csbWrite(FALC_CPUCTL, CPUCTL_STARTCPU);
    const uint32_t usbsts = capLength() + 0x04;
    // tegra_xusb_load_firmware polls STS_CNR for up to 200 ms, 1-2 ms apart (xHCI 1.2c 4.2: no event for it).
    if (!xusb::settle("xHCI CNR after the falcon starts", 2, 200, [this, usbsts] {
            return (xhci(usbsts) & (1u << 11)) == 0u;
        })) {
        snprintf(why, whySize, "xHCI not ready after the firmware started (USBSTS %#x, falcon CPUCTL %#x)",
                 xhci(usbsts), csbRead(FALC_CPUCTL));
        return false;
    }
#if DEVELOPMENT || DEBUG
    const uint32_t hcc = xhci(0x10);
    IOLog(
        "SwitchUSB: XUSB firmware %x.%02x (built %u) running, HCIVERSION %#x HCSPARAMS1 %#x HCSPARAMS2 %#x HCCPARAMS1 "
        "%#x (AC64 %u PPC %u MaxPSASize %u) DBOFF %#x RTSOFF %#x, USB 2.0 HLC %u\n",
        image[kFwVersion + 3], image[kFwVersion + 2], le32(image + kFwCreated), xhci(0) >> 16, xhci(4), xhci(8), hcc,
        hcc & 1u, (hcc >> 3) & 1u, (hcc >> 12) & 0xfu, xhci(0x14), xhci(0x18),
        static_cast<unsigned>(xhci_usb2_hlc(&readCapability, &regs, static_cast<uint32_t>(kXhciBytes))));
#else
    IOLog("SwitchUSB: XUSB firmware %x.%02x (built %u) running, HCSPARAMS1 %#x\n", image[kFwVersion + 3],
          image[kFwVersion + 2], le32(image + kFwCreated), xhci(4));
#endif
    return true;
}

unsigned Controller::otgPort()
{
    if (!isPowered)
        return 0;
    if (!usb2First) {
        const xhci_port_range usb2 = xhci_find_ports(&readCapability, &regs, static_cast<uint32_t>(kXhciBytes), 2);
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
        const xhci_port_range usb3 = xhci_find_ports(&readCapability, &regs, static_cast<uint32_t>(kXhciBytes), 3);
        usb3First = usb3.first;
        usb3Count = usb3.count;
    }
    return xusb_ss_xhci_port(usb3First, usb3Count);
}

uint32_t Controller::portsc(unsigned port) const
{
    return xhci(xhci_portsc_offset(capLength(), port));
}

void Controller::setPortPower(unsigned port, bool on)
{
    const uint32_t at = xhci_portsc_offset(capLength(), port);
    switch_mmio_write32(regs, at, xhci_portsc_power(xhci(at), on));
}

#if DEVELOPMENT || DEBUG
void Controller::describePads(char *buffer, size_t size)
{
    if (!isMapped) {
        snprintf(buffer, size, "PADCTL not mapped");
        return;
    }
    padctlUp();
    const uint32_t vbusId = switch_mmio_read32(padctl, XUSB_USB2_VBUS_ID);
    snprintf(buffer, size,
             "PADCTL PORT_CAP %#x VBUS_ID %#x (ID %s, VBUS override %s) PAD_MUX %#x OTG_PAD0 %#x/%#x CHRG %#x/%#x "
             "OC_MAP %#x VBUS_OC_MAP %#x OC_DET %#x ELPG %#x/%#x SS_PORT_MAP %#x BIAS %#x",
             switch_mmio_read32(padctl, XUSB_USB2_PORT_CAP), vbusId, xusb_id_name(vbusId),
             (vbusId & XUSB_VBUS_ID_VBUS_ON) ? "on" : "off", switch_mmio_read32(padctl, USB2_PAD_MUX),
             switch_mmio_read32(padctl, OTG_PAD0_CTL_0), switch_mmio_read32(padctl, OTG_PAD0_CTL_1),
             switch_mmio_read32(padctl, XUSB_CHRG_OTGPAD0_CTL0), switch_mmio_read32(padctl, XUSB_CHRG_OTGPAD0_CTL1),
             switch_mmio_read32(padctl, XUSB_USB2_OC_MAP), switch_mmio_read32(padctl, XUSB_VBUS_OC_MAP),
             switch_mmio_read32(padctl, XUSB_OC_DET), switch_mmio_read32(padctl, ELPG_PROGRAM_0),
             switch_mmio_read32(padctl, ELPG_PROGRAM_1), switch_mmio_read32(padctl, SS_PORT_MAP),
             switch_mmio_read32(padctl, BIAS_PAD_CTL_0));
}

void Controller::describePorts(char *buffer, size_t size)
{
    if (!isPowered) {
        snprintf(buffer, size, "xHCI not powered");
        return;
    }
    const uint32_t op = capLength();
    const unsigned ports = xhci(4) >> 24;
    const unsigned pad = otgPort();
    const unsigned ss = ssPort();
    const uint32_t padPortsc = pad ? portsc(pad) : 0;
    const uint32_t ssPortsc = ss ? portsc(ss) : 0;
    int n = snprintf(buffer, size,
                     "xHCI USBCMD %#x USBSTS %#x, pad 0 = port %u (USB2 %u+%u): %s %s, SS port 0 = port %u (USB3 "
                     "%u+%u): %s %s; PORTSC",
                     xhci(op), xhci(op + 4), pad, usb2First, usb2Count, xhci_portsc_link_name(padPortsc),
                     xhci_portsc_speed_name(padPortsc), ss, usb3First, usb3Count, xhci_portsc_link_name(ssPortsc),
                     xhci_portsc_speed_name(ssPortsc));
    for (unsigned port = 1; port <= ports && port <= 16 && n > 0 && static_cast<size_t>(n) < size; ++port) {
        n += snprintf(buffer + n, size - static_cast<size_t>(n), " %u:%#x", port, xhci(xhci_portsc_offset(op, port)));
    }
}
#endif

bool Controller::takeMailboxInterrupt()
{
    const uint32_t smi = switch_mmio_read32(fpci, XUSB_CFG_ARU_SMI_INTR);
    if (!(smi & (SMI_INTR_EN | SMI_INTR_FW_HANG)))
        return false;
    switch_mmio_write32(fpci, XUSB_CFG_ARU_SMI_INTR, smi);
    (void)switch_mmio_read32(fpci, XUSB_CFG_ARU_SMI_INTR);  // Linux reads again against a spurious ARU SMI
    if (smi & SMI_INTR_FW_HANG)
        mailboxHang = true;
    return true;
}

// tegra_xusb_mbox_send: ACK/NAK go out while the firmware still owns the mailbox; anything else takes ownership first.
// The firmware answers a command by message (SwitchXHCI::sendCommand waits for it).
bool Controller::sendMailbox(uint32_t cmd, uint32_t data)
{
    if (cmd != MBOX_ACK && cmd != MBOX_NAK) {
        if (switch_mmio_read32(fpci, XUSB_CFG_ARU_MBOX_OWNER) != MBOX_OWNER_NONE)
            return false;
        switch_mmio_write32(fpci, XUSB_CFG_ARU_MBOX_OWNER, MBOX_OWNER_SW);
        if (switch_mmio_read32(fpci, XUSB_CFG_ARU_MBOX_OWNER) != MBOX_OWNER_SW)
            return false;
    }
    switch_mmio_write32(fpci, XUSB_CFG_ARU_MBOX_DATA_IN, (cmd & 0xff) << 24 | (data & 0xffffff));
    switch_mmio_rmw32(fpci, XUSB_CFG_ARU_MBOX_CMD, 0, MBOX_INT_EN | MBOX_DEST_FALC);
    return true;
}

uint32_t Controller::mailboxOwner() const
{
    return switch_mmio_read32(fpci, XUSB_CFG_ARU_MBOX_OWNER);
}

// tegra_xusb_mbox_thread + tegra_xusb_mbox_handle for T210B01: SET_BW, ACK and NAK need no answer and release the
// mailbox (an ACK or NAK answers our last command: `answer` gets it), the SuperSpeed messages (SS port 0's LFPS
// detection, the DFE/CTLE context T210 cannot save) are XusbLanes.h's and the others XusbMailbox.h's.
bool Controller::serviceMailbox(uint32_t &answer, uint32_t &answerData)
{
    answer = 0;
    if (mailboxHang)
        return false;
    const uint32_t message = switch_mmio_read32(fpci, XUSB_CFG_ARU_MBOX_DATA_OUT);
    const uint32_t cmd = message >> 24;
    const uint32_t data = message & 0xffffff;
    switch_mmio_rmw32(fpci, XUSB_CFG_ARU_MBOX_CMD, MBOX_DEST_SMI, 0);
    if (cmd == MBOX_SET_BW || cmd == MBOX_ACK || cmd == MBOX_NAK) {
        switch_mmio_write32(fpci, XUSB_CFG_ARU_MBOX_OWNER, MBOX_OWNER_NONE);
        if (cmd != MBOX_SET_BW) {
            answer = cmd;
            answerData = data;
        }
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
    const xusb_mbox_reply reply = xusb_mbox_answer(cmd, data, falconKHz);
    if (!reply.reply) {
        IOLog("SwitchUSB: unknown XUSB falcon mailbox message %#x\n", message);
        return true;
    }
    if (!sendMailbox(reply.cmd, reply.data))
        IOLog("SwitchUSB: falcon mailbox reply %u to %u failed\n", reply.cmd, cmd);
    return true;
}

// The interrupt path from the xHCI to the GIC, as tegra_xusb_config programs it: the interrupter (IMAN0 IP/IE), the
// controller (USBCMD.INTE, USBSTS.EINT), the FPCI bridge (CFG_1 memory and bus master), the IPFS wrapper (EN_FPCI,
// INTR_MASK.IP_INT_MASK; INTR_ENABLE and MSI_EN_VEC0 as the chip has them: Linux never writes them), the mailbox
// (SMI_INTR, owner), and whether powerOn found the partitions already on (a boot stage left them: reset not pulsed).
void Controller::describe(char *buffer, size_t size) const
{
    const uint32_t op = capLength();
    const uint32_t rt = runtimeOffset();
    const uint32_t iman = xhci(rt + 0x20);
    const uint32_t cmd = xhci(op);
    const uint32_t sts = xhci(op + 4);
    const unsigned port = usb2First;
    snprintf(
        buffer, size,
        "USBCMD %#x (INTE %u) USBSTS %#x (EINT %u) IMAN0 %#x (IP %u IE %u) ERDP0 %#x PORTSC%u %#x; FPCI CFG_1 %#x; "
        "IPFS CONFIGURATION %#x INTR_MASK %#x INTR_ENABLE %#x MSI_EN_VEC0 %#x; SMI %#x MBOX_OWNER %#x; "
        "partitions found on %#x",
        cmd, static_cast<unsigned>((cmd & XHCI_USBCMD_INTE) != 0u), sts,
        static_cast<unsigned>((sts & XHCI_USBSTS_EINT) != 0u), iman, static_cast<unsigned>((iman & XHCI_IMAN_IP) != 0u),
        static_cast<unsigned>((iman & XHCI_IMAN_IE) != 0u), xhci(rt + 0x38), port,
        port ? xhci(xhci_portsc_offset(op, port)) : 0, switch_mmio_read32(fpci, XUSB_CFG_1),
        switch_mmio_read32(ipfs, IPFS_CONFIGURATION), switch_mmio_read32(ipfs, IPFS_INTR_MASK),
        switch_mmio_read32(ipfs, IPFS_INTR_ENABLE), switch_mmio_read32(ipfs, IPFS_MSI_EN_VEC0),
        switch_mmio_read32(fpci, XUSB_CFG_ARU_SMI_INTR), switch_mmio_read32(fpci, XUSB_CFG_ARU_MBOX_OWNER),
        partitionsFound);
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
