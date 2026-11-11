// SPDX-License-Identifier: BSD-2-Clause-Patent
// The host role's register arithmetic, in plain C with no kernel dependency, so the host tests
// (test/switch_usb_host_test.cpp) check this same header: which xHCI port is the USB-C port's USB 2.0 pad, how to
// write a PORTSC without touching its write-1-to-clear bits, and the XUSB pad controller values for each role.
//
// Sources: xHCI 1.2 (5.3.6 HCCPARAMS1.xECP, 7.2 Supported Protocol capability, 5.4.8 PORTSC), Linux
// drivers/usb/host/xhci.h (XHCI_PORT_RO, XHCI_PORT_RWS: xhci_port_state_to_neutral), CTCaer switch-l4t-kernel-4.9
// drivers/phy/tegra/xusb-tegra210.c (USB2_VBUS_ID id/vbus overrides, BATTERY_CHRG_OTGPAD charger detect on/off,
// USB2_OC_MAP) and drivers/usb/host/xhci-tegra.c tegra_xhci_set_host_mode (T210/T210B01 otg_set_port_power: after the
// ID override, the OTG port's PORTSC.PP is set, so the firmware sees the port powered with the pad in the host role).
#ifndef XUSB_PORTS_H
#define XUSB_PORTS_H

#include <stdbool.h>
#include <stdint.h>

// --- xHCI ---------------------------------------------------------------------------------------------------------
#define XHCI_HCCPARAMS1          0x10u
#define XHCI_XECP_SUPPORTED_PROT 2u
#define XHCI_PORTSC_BASE         0x400u  // from the operational registers, 0x10 per port, port 1 first
#define XHCI_PORTSC_CCS          (1u << 0)
#define XHCI_PORTSC_PED          (1u << 1)
#define XHCI_PORTSC_OCA          (1u << 3)
#define XHCI_PORTSC_PR           (1u << 4)
#define XHCI_PORTSC_PLS_SHIFT    5
#define XHCI_PORTSC_PP           (1u << 9)
#define XHCI_PORTSC_SPEED_SHIFT  10
#define XHCI_PORTSC_CSC          (1u << 17)
// Linux XHCI_PORT_RO (CCS, OCA, speed, device removable) and XHCI_PORT_RWS (link state, power, indicator, wake
// enables): written back as read. Everything else is a write-1 action (PED disables the port, PR resets it, LWS, WPR,
// and the seven change bits clear): written 0.
#define XHCI_PORTSC_KEEP \
    ((1u << 0) | (1u << 3) | (0xfu << 10) | (1u << 30) | (0xfu << 5) | (1u << 9) | (3u << 14) | (7u << 25))

#define XHCI_PORTSC_PLS(portsc)   (((portsc) >> XHCI_PORTSC_PLS_SHIFT) & 0xfu)
#define XHCI_PORTSC_SPEED(portsc) (((portsc) >> XHCI_PORTSC_SPEED_SHIFT) & 0xfu)
#define XHCI_SPEED_SUPER          4u

// The link state and speed of a PORTSC in words, for the role line (xHCI 1.2 5.4.8, 7.2.2.1.1 default speed IDs).
static inline const char *xhci_portsc_link_name(uint32_t portsc)
{
    static const char *const names[16] = {"U0",       "U1",      "U2",       "U3",       "Disabled",   "RxDetect",
                                          "Inactive", "Polling", "Recovery", "HotReset", "Compliance", "Test",
                                          "PLS12",    "PLS13",   "PLS14",    "Resume"};
    return names[XHCI_PORTSC_PLS(portsc)];
}

static inline const char *xhci_portsc_speed_name(uint32_t portsc)
{
    if (!(portsc & XHCI_PORTSC_CCS))
        return "nothing";
    switch (XHCI_PORTSC_SPEED(portsc)) {
    case 1:
        return "full speed";
    case 2:
        return "low speed";
    case 3:
        return "high speed";
    case XHCI_SPEED_SUPER:
        return "SuperSpeed 5 Gb/s";
    case 5:
        return "SuperSpeedPlus";
    default:
        return "unknown speed";
    }
}

static inline uint32_t xhci_portsc_offset(uint32_t cap_length, unsigned port)
{
    return cap_length + XHCI_PORTSC_BASE + 0x10u * (port - 1u);
}

// The value to write for PP on or off, nothing else changed.
static inline uint32_t xhci_portsc_power(uint32_t portsc, bool on)
{
    const uint32_t neutral = portsc & XHCI_PORTSC_KEEP;
    return on ? (neutral | XHCI_PORTSC_PP) : (neutral & ~XHCI_PORTSC_PP);
}

struct xhci_port_range {
    unsigned first;  // 1-based port number; 0: none found
    unsigned count;
};

// The ports of protocol `major` (2: USB 2.0, 3: USB 3.x) from the Supported Protocol capabilities in the extended
// capability list. `read` reads a 32-bit register at a byte offset from the capability base; `window` bounds the
// offsets (the register window's size). The first capability of that major wins (Tegra XUSB: one per protocol).
// A malformed list (a loop, an offset outside the window) ends the walk: at most 64 capabilities are read.
static inline struct xhci_port_range xhci_find_ports(uint32_t (*read)(void *context, uint32_t offset), void *context,
                                                     uint32_t window, unsigned major)
{
    struct xhci_port_range range = {0, 0};
    uint32_t offset = (read(context, XHCI_HCCPARAMS1) >> 16) << 2;
    for (unsigned i = 0; i < 64 && offset && offset + 12 <= window; ++i) {
        const uint32_t head = read(context, offset);
        if ((head & 0xffu) == XHCI_XECP_SUPPORTED_PROT && (head >> 24) == major) {
            const uint32_t ports = read(context, offset + 8);
            range.first = ports & 0xffu;
            range.count = (ports >> 8) & 0xffu;
            return range;
        }
        const uint32_t next = ((head >> 8) & 0xffu) << 2;
        if (!next)
            break;
        offset += next;
    }
    return range;
}

// --- XUSB pad controller, USB2 port 0 (the USB-C port's D+/D-) ------------------------------------------------------
#define XUSB_USB2_PORT_CAP          0x008u
#define XUSB_USB2_OC_MAP            0x010u
#define XUSB_VBUS_OC_MAP            0x018u
#define XUSB_OC_DET                 0x01cu
#define XUSB_CHRG_OTGPAD0_CTL0      0x080u
#define XUSB_CHRG_OTGPAD0_CTL1      0x084u
#define XUSB_USB2_VBUS_ID           0xc60u
#define XUSB_PORT0_CAP_MASK         3u
#define XUSB_PORT0_CAP_DEVICE       2u
#define XUSB_PORT0_CAP_OTG          3u
#define XUSB_VBUS_ID_VBUS_SRC_OVR   (1u << 12)  // VBUS source: the override
#define XUSB_VBUS_ID_VBUS_ON        (1u << 14)  // the VBUS override's value
#define XUSB_VBUS_ID_ID_SRC_OVR     (1u << 16)  // ID source: the override
#define XUSB_VBUS_ID_ID_MASK        (0xfu << 18)
#define XUSB_VBUS_ID_ID_GROUNDED    (0u << 18)  // a host's ID: the XHCI owns the pad
#define XUSB_VBUS_ID_ID_FLOATING    (8u << 18)  // a device's ID: the XUDC owns it
#define XUSB_CHRG_CTL0_PD_CHG       (1u << 0)
#define XUSB_CHRG_CTL0_SRC_SINK     ((1u << 9) | (1u << 10) | (1u << 11) | (1u << 12))   // OP/ON source and sink
#define XUSB_CHRG_CTL1_RPD_RPU_OVRD ((1u << 16) | (1u << 18) | (1u << 20) | (1u << 22))  // D+/D- pull overrides
#define XUSB_OC_MAP_PORT0_MASK      0xfu
#define XUSB_OC_MAP_PORT0_DISABLED  0xfu

// Host: ID and VBUS from their overrides, VBUS override low, ID grounded (tegra210_xusb_padctl_id_override set; Linux
// drops a set VBUS override first and waits 1-2 ms, which the caller does with xusb_vbus_id_drop_vbus).
static inline uint32_t xusb_vbus_id_drop_vbus(uint32_t v)
{
    return (v | XUSB_VBUS_ID_VBUS_SRC_OVR | XUSB_VBUS_ID_ID_SRC_OVR) & ~XUSB_VBUS_ID_VBUS_ON;
}

static inline uint32_t xusb_vbus_id_host(uint32_t v)
{
    return (xusb_vbus_id_drop_vbus(v) & ~XUSB_VBUS_ID_ID_MASK) | XUSB_VBUS_ID_ID_GROUNDED;
}

// Device: ID floating (id_override clear); the VBUS override is the XUDC's to set when it connects.
static inline uint32_t xusb_vbus_id_device(uint32_t v)
{
    return ((v | XUSB_VBUS_ID_VBUS_SRC_OVR | XUSB_VBUS_ID_ID_SRC_OVR) & ~XUSB_VBUS_ID_ID_MASK) |
           XUSB_VBUS_ID_ID_FLOATING;
}

// The charger-detection leftovers a host's D+/D- must not have (a boot stage's BC1.2 detection): the pull-up/down
// overrides off (utmi_pad_charger_detect_off) so the pad's own 15 kOhm host pull-downs apply, and no D+/D- source or
// sink. PD_CHG is left as found (hekate and the XUDC clear it; the sources it powers are off).
static inline uint32_t xusb_chrg_ctl0_host(uint32_t v)
{
    return v & ~XUSB_CHRG_CTL0_SRC_SINK;
}

static inline uint32_t xusb_chrg_ctl1_host(uint32_t v)
{
    return v & ~XUSB_CHRG_CTL1_RPD_RPU_OVRD;
}

// Port 0's over-current pin: none on this board (no nvidia,oc-pin), so detection off; a mapped pin that reads
// asserted would hold PORTSC.OCA and the port unpowered.
static inline uint32_t xusb_oc_map_host(uint32_t v)
{
    return (v & ~XUSB_OC_MAP_PORT0_MASK) | XUSB_OC_MAP_PORT0_DISABLED;
}

static inline const char *xusb_id_name(uint32_t vbus_id)
{
    switch ((vbus_id & XUSB_VBUS_ID_ID_MASK) >> 18) {
    case 0:
        return "grounded";
    case 1:
        return "C";
    case 2:
        return "B";
    case 4:
        return "A";
    case 8:
        return "floating";
    default:
        return "?";
    }
}

// --- The host interrupts' check (SwitchXHCI.cpp) --------------------------------------------------------------------
// INTID 71 (xHCI, DT xusb@70090000 GIC_SPI 39, level high, through the LIC) and INTID 72 (falcon mailbox, xusb_mbox
// GIC_SPI 40) had never fired on this hardware when this was written. Until a line has reached its handler once this
// boot (switch_interrupt_count), a missing delivery is a never-booted path failing: one line and host mode off for the
// boot (device mode is the proven path), no panic. Once it has fired, a lost interrupt panics as for every proven line.
//   - delivered: the INTID's handler count moved since the check was armed;
//   - pending: the interrupter has an event (IMAN.IP, or USBSTS.EINT) that nobody consumed (ERDP unmoved);
//   - first: the check follows the controller's first start with a device on the port, so a port event is due even
//     if nothing is pending (no connect seen, or no interrupt asserted at all);
//   - deliverable: IMAN.IE and USBCMD.INTE are on (a proven line is only judged lost when the controller asks for it).
// Stage 1 runs kCheckMS after arming; anything short of fine is looked at once more 50 ms later (stage 2) before it acts.
#define XHCI_USBCMD_INTE (1u << 2)
#define XHCI_USBCMD_EU3S (1u << 11)
#define XHCI_USBSTS_EINT (1u << 3)
#define XHCI_IMAN_IP     (1u << 0)
#define XHCI_IMAN_IE     (1u << 1)

enum xusb_irq_verdict {
    XUSB_IRQ_FINE,     // delivered, or nothing due
    XUSB_IRQ_CONFIRM,  // looks lost: check again in 50 ms
    XUSB_IRQ_OFF,      // never fired this boot and still not delivered: one line, host mode off for the boot
    XUSB_IRQ_PANIC,    // fired before, now lost
};

struct xusb_irq_sample {
    uint32_t count;    // the INTID's handler count (switch_interrupt_count)
    uint32_t erdp;     // ERDP0 (low word), or for the mailbox anything constant
    bool pending;      // an event waits: IMAN.IP or USBSTS.EINT (xHCI), SMI_INTR (mailbox)
    bool deliverable;  // the source asks for an interrupt: IMAN.IE and USBCMD.INTE (xHCI); always (mailbox)
};

static inline enum xusb_irq_verdict xusb_irq_judge(struct xusb_irq_sample armed, struct xusb_irq_sample now, bool first,
                                                   unsigned stage)
{
    const bool delivered = now.count != armed.count;
    const bool waiting = now.pending && now.erdp == armed.erdp && !delivered;
    enum xusb_irq_verdict act = XUSB_IRQ_CONFIRM;
    if (stage >= 2)
        act = now.count ? XUSB_IRQ_PANIC : XUSB_IRQ_OFF;
    if (now.count)
        return waiting && now.deliverable ? act : XUSB_IRQ_FINE;
    if (waiting || (first && !delivered))
        return act;
    return XUSB_IRQ_FINE;
}

static inline bool xusb_xhci_pending(uint32_t iman, uint32_t usbsts)
{
    return ((iman & XHCI_IMAN_IP) != 0u) || ((usbsts & XHCI_USBSTS_EINT) != 0u);
}

static inline bool xusb_xhci_deliverable(uint32_t iman, uint32_t usbcmd)
{
    return ((iman & XHCI_IMAN_IE) != 0u) && ((usbcmd & XHCI_USBCMD_INTE) != 0u);
}

// --- Save State and Restore State (xHCI 1.2 4.23.2) -----------------------------------------------------------------
// AppleUSBXHCI lowers an idle controller to its Sleep state by halting it and setting USBCMD.CSS, and raises it again
// by rewriting the operational and interrupter registers and setting USBCMD.CRS. Tegra's XUSB firmware answers CSS with
// USBSTS.SRE (boot 1109-2145: USBSTS 0x409), which AppleUSBXHCI takes as hardware exception 4. Outside SC7 the XUSB
// partitions stay powered and the halted falcon keeps every slot, ring and port as they were, so the save is the
// controller itself: CSS and CRS are not sent, and the restore leaves interrupter 0's ERSTBA alone, because writing it
// rewinds the event ring's enqueue pointer to the segment start (xHCI 1.2 5.5.2.3.2) under AppleUSBXHCI's dequeue
// pointer. SC7 takes the falcon's state: a restore after it reports SRE, as an xHC that lost power does, and AppleUSBXHCI
// recovers the controller through a reset. HCRST ends whatever was held.
#define XHCI_USBCMD_HCRST (1u << 1)
#define XHCI_USBCMD_CSS   (1u << 8)
#define XHCI_USBCMD_CRS   (1u << 9)
#define XHCI_USBSTS_SRE   (1u << 10)
#define XHCI_ERSTBA0      0x30u  // from the runtime registers: interrupter 0's ERSTBA (0x20 + 0x10)

struct xusb_hold {
    bool held;  // a Save State is held by the halted controller
    bool lost;  // SC7 took the controller's state since
    bool sre;   // the last Restore State failed; USBSTS.SRE until written 1 or HCRST
};

// What AppleUSBXHCI's USBCMD write becomes, CSS and CRS taken out.
static inline uint32_t xusb_hold_usbcmd(struct xusb_hold *hold, uint32_t value)
{
    if (value & XHCI_USBCMD_HCRST) {
        hold->held = false;
        hold->lost = false;
        hold->sre = false;
    }
    if (value & XHCI_USBCMD_CSS) {
        hold->held = true;
        hold->lost = false;
    }
    if (value & XHCI_USBCMD_CRS) {
        hold->sre = !hold->held || hold->lost;
        hold->held = false;
    }
    return value & ~(XHCI_USBCMD_CSS | XHCI_USBCMD_CRS);
}

static inline uint32_t xusb_hold_usbsts(const struct xusb_hold *hold, uint32_t value)
{
    return hold->sre ? (value | XHCI_USBSTS_SRE) : value;
}

static inline void xusb_hold_usbsts_write(struct xusb_hold *hold, uint32_t value)
{
    if (value & XHCI_USBSTS_SRE)
        hold->sre = false;
}

static inline void xusb_hold_lost(struct xusb_hold *hold)
{
    hold->lost = true;
}

// Whether a write of interrupter 0's ERSTBA (either half) is the restore putting back what the controller still holds.
static inline bool xusb_hold_keeps(const struct xusb_hold *hold, uint64_t value, uint64_t current)
{
    return hold->held && !hold->lost && value == current;
}

#endif
