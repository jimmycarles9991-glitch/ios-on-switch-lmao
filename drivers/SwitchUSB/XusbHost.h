// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 (T210B01) XUSB host controller hardware: what Apple's AppleUSBXHCI cannot know about. After powerOn()
// and loadFirmware() the block at 0x70090000 is a plain xHCI 1.0 controller and AppleUSBXHCI (through SwitchXHCI)
// drives it; this layer keeps only the Tegra side: clocks and power partitions, the IPFS/FPCI bridge (BAR0, bus
// master, interrupt assertion), the falcon microcontroller that implements the xHCI and its firmware, the falcon
// mailbox (its own interrupt) and USB2 pad 0's role (the ID and VBUS overrides). SuperSpeed port 0's UPHY lane is
// XusbSuperSpeed.cpp's; its root port's power is here.
//
// Sources: CTCaer switch-l4t-kernel-4.9 drivers/usb/host/xhci-tegra.c (tegra210b01_soc, tegra_xusb_config,
// tegra_xhci_load_firmware, tegra_xusb_mbox_send/handle, tegra_xusb_disable_hsic_wake) and drivers/phy/tegra/
// xusb-tegra210.c (tegra210_xusb_padctl_id_override, vbus_override). OLED DTB xusb@70090000: xHCI 0x70090000
// (0x8000), FPCI 0x70098000, IPFS 0x70099000; interrupts SPI 39 host (INTID 71), SPI 40 mailbox (INTID 72);
// usb2-0 "otg"; iommus = XUSB_HOST: in bypass until UsbSmmuMapper.cpp translates it (ASID TEGRA_SMMU_ASID_XUSB_HOST).
//
// Waits on hardware status with no interrupt behind it (PLL lock, falcon DMA, CNR after the firmware starts) are bounded
// register polls at bring-up, as in Linux; events, the mailbox's answers included, are interrupt-driven only.
#pragma once
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "XusbMailbox.h"
#include <stddef.h>
#include <stdint.h>

namespace xhost {

constexpr uint64_t kXhciPA = 0x70090000, kXhciBytes = 0x8000;
constexpr uint32_t kHostIntid = 32 + 39, kMailboxIntid = 32 + 40;

// Falcon mailbox commands (xhci-tegra.c enum tegra_xusb_mbox_cmd).
enum MboxCmd : uint32_t {
    MBOX_MSG_ENABLED = 1,
    MBOX_INC_FALC_CLOCK = 2,
    MBOX_DEC_FALC_CLOCK = 3,
    MBOX_INC_SSPI_CLOCK = 4,
    MBOX_DEC_SSPI_CLOCK = 5,
    MBOX_SET_BW = 6,
    MBOX_SET_SS_PWR_GATING = 7,
    MBOX_SET_SS_PWR_UNGATING = 8,
    MBOX_SAVE_DFE_CTLE_CTX = 9,
    MBOX_AIRPLANE_MODE_ENABLED = 10,
    MBOX_AIRPLANE_MODE_DISABLED = 11,
    MBOX_START_HSIC_IDLE = 12,
    MBOX_STOP_HSIC_IDLE = 13,
    MBOX_DBC_WAKE_STACK = 14,
    MBOX_HSIC_PRETEND_CONNECT = 15,
    MBOX_RESET_SSPI = 16,
    MBOX_DISABLE_SS_LFPS_DETECTION = 17,
    MBOX_ENABLE_SS_LFPS_DETECTION = 18,
    MBOX_ACK = 128,
    MBOX_NAK = 129,
};

class Controller {
public:
    bool map();  // MMIO only, no access; once
    // PLLs and pad (shared with the XUDC), partitions XUSBA+XUSBC, clocks, IPFS/FPCI; false with the reason.
    bool powerOn(char *why, size_t whySize);
    bool loadFirmware(const uint8_t *image, size_t size, char *why, size_t whySize);

    bool powered() const
    {
        return isPowered;
    }  // powerOn succeeded: the xHCI registers can be read

    // Before SC7, while the block is still powered: the FPCI and IPFS registers xhci-tegra.c saves, which powerOn puts
    // back after SC7, with the host role's ID override (tegra_xhci_exit_elpg).
    void saveContext();
    static constexpr unsigned kFpciContextRegs = 8, kIpfsContextRegs = 11;

    // SC7 takes VDD_CORE, and with it the partitions, the falcon and its firmware: no register access until powerOn and
    // loadFirmware have run again (loadFirmware finds ILOAD_BASE back at reset and reloads).
    void lostPower()
    {
        isPowered = false;
    }

    // XusbSleepwalk.cpp: before SC7, with a device on `port` (USB2 pad 0's), the PMC's sleepwalk keeps its bus and
    // raises PMC wake event 39 when it signals resume; false (nothing armed) without a device. After SC7, before
    // powerOn: the pad back from the PMC.
    bool armRemoteWake(unsigned port);
    static void disarmRemoteWake();

    // USB2 port 0 (the USB-C port's D+/D-), XusbPorts.h: OTG capability, then the ID override (xusb-tegra210.c):
    // host grounds ID (the VBUS override off first, 1.5 ms, as Linux) and clears what a boot stage's charger detection
    // left on the pad (D+/D- pull overrides, sources, sinks) and port 0's over-current pin (one line if anything was
    // set); device floats ID (the XUDC sets its VBUS override when it connects). Works before powerOn (the pad
    // controller's clock and reset are taken as powerOn does).
    void setHostRole(bool host);

    // The xHCI port of USB2 pad 0: the first USB 2.0 port of the Supported Protocol capabilities (0: not found, or
    // not powered). Tegra XUSB: SS ports 1-4, USB2 ports 5-9 (pads 0-3, HSIC).
    unsigned otgPort();
    // The xHCI port of SS port 0 (XusbLanes.h xusb_ss_xhci_port; 0: not found, or not powered).
    unsigned ssPort();
    uint32_t portsc(unsigned port) const;
    // PORTSC.PP on or off, nothing else changed (the falcon firmware implements PORTSC). Not followed: see the caller.
    void setPortPower(unsigned port, bool on);
#if DEVELOPMENT || DEBUG
    // One-line reports for the role line (SwitchUSB.cpp): the pad controller's port 0 state; the xHCI's USBCMD,
    // USBSTS and every PORTSC (only when powered).
    void describePads(char *buffer, size_t size);
    void describePorts(char *buffer, size_t size);
#endif

    // xHCI registers through our own mapping (bring-up, the SuperSpeed interface reset).
    uint32_t xhci(uint32_t offset) const
    {
        return switch_mmio_read32(regs, offset);
    }

    uint32_t capLength() const
    {
        return xhci(0) & 0xff;
    }

    uint32_t runtimeOffset() const
    {
        return xhci(0x18) & ~0x1fu;
    }

    // Mailbox. takeMailboxInterrupt() runs in the primary interrupt filter: it acknowledges SMI_INTR (W1C) and says
    // whether a message waits (or the firmware hung). serviceMailbox() runs on the work loop: reads the message,
    // answers it (ACK/NAK), passes on the firmware's own ACK or NAK in `answer` (0 for anything else) and returns false
    // only on a firmware hang. sendMailbox takes ownership for a command and writes it; false when the mailbox is busy.
    bool takeMailboxInterrupt();
    bool serviceMailbox(uint32_t &answer, uint32_t &answerData, xusb_mbox_deferred &deferred);
    void finishMailboxReply(uint32_t requestCmd, uint32_t replyCmd, uint32_t data);
    bool sendMailbox(uint32_t cmd, uint32_t data);
    uint32_t mailboxOwner() const;
    // One line of the interrupt path, xHCI to IPFS: USBCMD/USBSTS/IMAN0/ERDP0, the OTG port's PORTSC, FPCI CFG_1, the
    // IPFS configuration/mask/enable/MSI, SMI and mailbox owner, the partitions powerOn found already on.
    void describe(char *buffer, size_t size) const;

private:
    volatile uint8_t *car = nullptr, *fusePage = nullptr, *mc = nullptr, *padctl = nullptr, *regs = nullptr,
                     *fpci = nullptr, *ipfs = nullptr;
    bool mailboxHang = false, isMapped = false, isPowered = false;
    bool hostRole = false;      // setHostRole's last role
    bool contextSaved = false;  // saveContext ran: powerOn is a return from SC7
    uint32_t fpciContext[kFpciContextRegs] = {}, ipfsContext[kIpfsContextRegs] = {};
    uint32_t falconKHz = 0;  // CLK_SOURCE_XUSB_FALCON's rate (startPllre), what INC/DEC_FALC_CLOCK are answered with
    unsigned usb2First = 0, usb2Count = 0, usb3First = 0, usb3Count = 0;
    uint32_t partitionsFound = 0;  // PMC partitions powerOn found already powered (bits as PWRGATE_STATUS)

    void padctlUp();

    bool powerUp(unsigned partition, unsigned id, const struct switch_pmc_mc_client *client);
    uint32_t startPllre();
    void groundId();
    void restoreContext();
    uint32_t csbRead(uint32_t offset);
    void csbWrite(uint32_t offset, uint32_t value);
};

}  // namespace xhost
