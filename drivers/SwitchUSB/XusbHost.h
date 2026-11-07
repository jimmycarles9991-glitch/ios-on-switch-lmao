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
// Waits on hardware status with no interrupt behind it (PLL lock, falcon DMA, CNR after the firmware starts, mailbox
// ownership) are bounded register polls at bring-up, as in Linux; events are interrupt-driven only.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace xhost {

constexpr uint64_t kXhciPA = 0x70090000, kXhciBytes = 0x8000;
constexpr uint32_t kHostIntid = 32 + 39, kMailboxIntid = 32 + 40;
constexpr uint32_t kFalconKHz = 204000;  // CLK_SOURCE_XUSB_FALCON: PLLP 408 MHz / 2

// Falcon mailbox commands (xhci-tegra.c enum tegra_xusb_mbox_cmd).
enum MboxCmd : uint32_t {
    MBOX_MSG_ENABLED = 1,
    MBOX_INC_FALC_CLOCK,
    MBOX_DEC_FALC_CLOCK,
    MBOX_INC_SSPI_CLOCK,
    MBOX_DEC_SSPI_CLOCK,
    MBOX_SET_BW,
    MBOX_SET_SS_PWR_GATING,
    MBOX_SET_SS_PWR_UNGATING,
    MBOX_SAVE_DFE_CTLE_CTX,
    MBOX_AIRPLANE_MODE_ENABLED,
    MBOX_AIRPLANE_MODE_DISABLED,
    MBOX_START_HSIC_IDLE,
    MBOX_STOP_HSIC_IDLE,
    MBOX_DBC_WAKE_STACK,
    MBOX_HSIC_PRETEND_CONNECT,
    MBOX_RESET_SSPI,
    MBOX_DISABLE_SS_LFPS_DETECTION,
    MBOX_ENABLE_SS_LFPS_DETECTION,
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
    void disarmRemoteWake();

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
    // PORTSC.PP on or off, followed for up to 1 ms (the falcon firmware implements PORTSC); `trace` gets PORTSC before
    // and after. Call on AppleUSBXHCI's work loop. False if PP did not follow.
    bool setPortPower(unsigned port, bool on, uint32_t trace[2]);
    // tegra_xhci_set_host_mode (otg_set_port_power): PORTSC.PP off, then on again, each followed for up to 1 ms (Linux:
    // 10 x 10-20 us; the falcon firmware implements PORTSC); `trace` gets the three PORTSC values. Call on
    // AppleUSBXHCI's work loop (its port register writes happen there). False if PP did not follow.
    bool cyclePortPower(unsigned port, uint32_t trace[3]);
    uint32_t portsc(unsigned port) const;
    // One-line reports for the role line (SwitchUSB.cpp): the pad controller's port 0 state; the xHCI's USBCMD,
    // USBSTS and every PORTSC (only when powered).
    void describePads(char *buffer, size_t size);
    void describePorts(char *buffer, size_t size);

    // xHCI registers through our own mapping (bring-up, the lost-interrupt check).
    uint32_t xhci(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint32_t *>(regs + offset);
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
    // answers it (ACK/NAK), returns false only on a firmware hang.
    bool takeMailboxInterrupt();
    bool serviceMailbox();
    bool sendMailbox(uint32_t cmd, uint32_t data);  // takes ownership, waits (bounded) for the firmware to take it
    bool mailboxPending() const;                    // SMI_INTR raised and not yet acknowledged
    // One line of the interrupt path, xHCI to IPFS: USBCMD/USBSTS/IMAN0/ERDP0, the OTG port's PORTSC, FPCI CFG_1, the
    // IPFS configuration/mask/enable/MSI, SMI and mailbox owner, the partitions powerOn found already on.
    void describe(char *buffer, size_t size) const;
    // IMAN0, USBCMD, USBSTS, ERDP0 for the interrupt check (false before powerOn).
    bool interruptSample(uint32_t &iman, uint32_t &usbcmd, uint32_t &usbsts, uint32_t &erdp) const;

private:
    volatile uint8_t *car = nullptr, *pmcPage = nullptr, *fusePage = nullptr, *mc = nullptr, *padctl = nullptr,
                     *regs = nullptr, *fpci = nullptr, *ipfs = nullptr;
    bool mailboxHang = false, isMapped = false, isPowered = false;
    unsigned usb2First = 0, usb2Count = 0, usb3First = 0, usb3Count = 0;
    uint32_t partitionsFound = 0;  // PMC partitions powerOn found already powered (bits as PWRGATE_STATUS)

    void padctlUp();

    static uint32_t r(volatile uint8_t *b, uint32_t o)
    {
        return *reinterpret_cast<volatile uint32_t *>(b + o);
    }

    static void w(volatile uint8_t *b, uint32_t o, uint32_t v)
    {
        *reinterpret_cast<volatile uint32_t *>(b + o) = v;
    }

    static void rmw(volatile uint8_t *b, uint32_t o, uint32_t clear, uint32_t set)
    {
        w(b, o, (r(b, o) & ~clear) | set);
    }

    static bool waitBits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t value, uint32_t us);
    uint32_t pmc(uint32_t o) const;
    void pmcWrite(uint32_t o, uint32_t v);

    void pmcRmw(uint32_t o, uint32_t clear, uint32_t set)
    {
        pmcWrite(o, (pmc(o) & ~clear) | set);
    }

    bool powerUp(uint32_t partition, uint32_t clockSet, uint32_t resetSet, uint32_t bit);
    uint32_t csbRead(uint32_t offset);
    void csbWrite(uint32_t offset, uint32_t value);
};

}  // namespace xhost
