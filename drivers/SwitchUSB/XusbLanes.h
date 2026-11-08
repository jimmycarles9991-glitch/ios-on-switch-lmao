// SPDX-License-Identifier: GPL-2.0-only
// SuperSpeed on the USB-C port in plain C: when SS port 0's UPHY lane is up, the register values that bring it up and
// down, what its root port does when the lane comes and goes, what the falcon's SuperSpeed mailbox messages ask for,
// and the xHCI register writes SwitchXHCI adjusts. test/switch_usb_lanes_test.cpp checks this same header (C11,
// -Wconversion).
//
// The port's two SuperSpeed pairs are shared with DisplayPort alt mode (drivers/SwitchDisplayPort, which calls
// switch_usb_set_alt_lanes): USB3 runs on one pair without alt mode or in pin assignment D, and has none in pin C. The
// board's mux follows the PD controller's plug orientation in hardware, so SS port 0 is UPHY lane pcie-5 in both
// orientations. Taking the lane up or down touches only lane 5's pad and SS port 0's clamps: USB2 pad 0, its port and
// a hub's USB2 half are never disturbed, so entering or leaving DisplayPort costs only the SuperSpeed connection.
//
// Sources (CTCaer switch-l4t-kernel-4.9): drivers/phy/tegra/xusb-tegra210.c tegra210b01_usb3_map (SS port 0 = pcie-5),
// usb3_lane_data (the B01 xusb lane defaults through UPHY_MISC_PAD_P5_CTL8), tegra210_pcie_phy_power_on/off for an
// xusb lane (SS_PORT_MAP, ECTL1-6, ELPG_PROGRAM_1's VCORE_DOWN/CLAMP_EN_EARLY/CLAMP_EN order and delays),
// tegra210_uphy_init (FORCE_PCIE_PAD_IDDQ_DISABLE), tegra210_usb3_set_lfps_detect; drivers/usb/host/xhci-tegra.c
// tegra210b01_soc (scale_ss_clock false, lpm_support false), tegra_xusb_mbox_handle (LFPS ports from bit 1, no DFE/CTLE
// context save on T210: NAK) and tegra_xhci_set_host_mode (T210's OTG SS port: PP off, RESET_SSPI, PP on, before the
// USB2 port); CTCaer l4t-hardware-soc-t210 tegra210b01-soc-prods.dtsi prod_c_ss0 (ECTL1 asic and char);
// switch-l4t-platform-t210-nx tegra210b01-nx-common.dtsi (usb3-0: lane pcie-5, companion usb2-0).
#ifndef XUSB_LANES_H
#define XUSB_LANES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What DisplayPort alt mode leaves of the SuperSpeed pairs (r24-dpalt's sequencer, on the PD work loop).
enum switch_usb3_lanes {
    SWITCH_USB3_LANES_PENDING = 0,  // a partner whose alt mode is not decided yet: USB3 stays off its pairs
    SWITCH_USB3_LANES_FREE = 1,     // no DisplayPort, or pin assignment D: one pair is USB3's
    SWITCH_USB3_LANES_TAKEN = 2,    // pin assignment C: all four are DisplayPort's
};

// SwitchUSB.cpp. Returns at once (the lane follows on the role thread). dock_hub_enabled: the Nintendo dock's hub has
// been switched on (HUB_CONTROL acknowledged).
void switch_usb_set_alt_lanes(enum switch_usb3_lanes lanes, bool dock_hub_enabled);

// The lane goes up for a host whose pair is free once the UPHY PLL runs. PLLE and PLL0 belong to the PCIe controller
// (drivers/SwitchWLAN TegraPCIe calibrates them at boot and after SC7, kits/SwitchCAR/SwitchUPHY.h); this never
// programs or powers them down.
static inline bool xusb_ss_lane_wanted(bool host, enum switch_usb3_lanes lanes, bool uphy_ready)
{
    return host && lanes == SWITCH_USB3_LANES_FREE && uphy_ready;
}

static inline const char *xusb_lanes_name(enum switch_usb3_lanes lanes)
{
    switch (lanes) {
    case SWITCH_USB3_LANES_PENDING:
        return "alt mode pending";
    case SWITCH_USB3_LANES_FREE:
        return "free";
    case SWITCH_USB3_LANES_TAKEN:
        return "DisplayPort's";
    }
    return "?";
}

// --- XUSB pad controller ---------------------------------------------------------------------------------------------
#define XUSB_SS_PORT                  0u  // the USB-C port's SuperSpeed port (usb3-0)
#define XUSB_SS_LANE                  5u  // its UPHY lane, pcie-5
#define XUSB_PADCTL_SS_PORT_MAP       0x014u
#define XUSB_PADCTL_ELPG_PROGRAM_1    0x024u
#define XUSB_PADCTL_USB3_PAD_MUX      0x028u
#define XUSB_PADCTL_UPHY_PLL_P0_CTL1  0x360u
#define XUSB_UPHY_PLL_LOCKDET         (1u << 15)
#define XUSB_PADCTL_UPHY_MISC_CTL1(l) (0x460u + 0x40u * (l))
#define XUSB_PADCTL_UPHY_MISC_CTL8(l) (0x47cu + 0x40u * (l))
#define XUSB_PADCTL_USB3_ECTL(p, n)   (0xa60u + 0x40u * (p) + 4u * ((n) - 1u))  // ECTL_1 .. ECTL_6

#define XUSB_ELPG_CLAMP_EN(p)       (1u << (3u * (p)))
#define XUSB_ELPG_CLAMP_EN_EARLY(p) (1u << (3u * (p) + 1u))
#define XUSB_ELPG_VCORE_DOWN(p)     (1u << (3u * (p) + 2u))

#define XUSB_PAD_MUX_SHIFT(l)        (12u + 2u * (l))
#define XUSB_PAD_MUX_FUNCTION_XUSB   1u
#define XUSB_PAD_MUX_IDDQ_DISABLE(l) (1u << (1u + (l)))

#define XUSB_SS_PORT_MAP_MASK(p)  (7u << (5u * (p)))
#define XUSB_SS_PORT_MAP(p, usb2) ((usb2) << (5u * (p)))

#define XUSB_AUX_RX_MODE_OVRD    (1u << 13)
#define XUSB_AUX_RX_TERM_EN      (1u << 18)
#define XUSB_AUX_RX_IDLE_MODE(x) (((x) & 3u) << 20)

// The lane's register-file writes (UPHY_MISC_PAD_PX_CTL8: address, data, reset, write strobe). Linux writes them
// whenever it calibrates the PLL (tegra210_pex_uphy_enable), so they go in at every lane-up, with the lane clamped.
static inline uint32_t xusb_uphy_cfg(uint32_t address, uint32_t data)
{
    return ((address & 0xffu) << 16) | (data & 0xffffu) | (1u << 27) | (1u << 24);
}

// usb3_lane_data: {address, data} for a B01 lane carrying xusb.
static const uint16_t xusb_b01_usb3_lane_data[][2] = {
    {0x01, 0x0002}, {0x04, 0x0032}, {0x07, 0x0022}, {0x35, 0x2587}, {0x49, 0x0fc7}, {0x52, 0x0001},
    {0x53, 0x3c0f}, {0x56, 0xc00f}, {0x5d, 0xff07}, {0x5e, 0x141a}, {0x97, 0x0080},
};
#define XUSB_B01_USB3_LANE_WRITES (sizeof(xusb_b01_usb3_lane_data) / sizeof(xusb_b01_usb3_lane_data[0]))

// Lane 5 to the XUSB, out of IDDQ.
static inline uint32_t xusb_pad_mux_ss(uint32_t v)
{
    return (v & ~(3u << XUSB_PAD_MUX_SHIFT(XUSB_SS_LANE))) |
           (XUSB_PAD_MUX_FUNCTION_XUSB << XUSB_PAD_MUX_SHIFT(XUSB_SS_LANE)) | XUSB_PAD_MUX_IDDQ_DISABLE(XUSB_SS_LANE);
}

// SS port 0 paired with USB2 port 0.
static inline uint32_t xusb_ss_port_map(uint32_t v)
{
    return (v & ~XUSB_SS_PORT_MAP_MASK(XUSB_SS_PORT)) | XUSB_SS_PORT_MAP(XUSB_SS_PORT, 0u);
}

// The SS pad's equalisation (tegra210_pcie_phy_power_on, then prod_c_ss0 over ECTL_1).
static inline uint32_t xusb_ectl1(uint32_t v)
{
    return (v & ~((3u << 16) | 0x3fu)) | (2u << 16) | 0x27u;
}

static inline uint32_t xusb_ectl2(uint32_t v)
{
    return (v & ~0xffffu) | 0x00fcu;
}

#define XUSB_ECTL3 0xc0077f1fu

static inline uint32_t xusb_ectl4(uint32_t v)
{
    return (v & ~(0xffffu << 16)) | (0x01c7u << 16);
}

#define XUSB_ECTL6 0xfcf01368u

// ELPG_PROGRAM_1 steps: up clears VCORE_DOWN, then CLAMP_EN_EARLY, then CLAMP_EN, 100 us apart; down sets CLAMP_EN_EARLY,
// CLAMP_EN (250 us after), then VCORE_DOWN.
struct xusb_elpg_step {
    uint32_t bit;
    bool set;
    uint32_t delay_us;  // after the write
};

static const struct xusb_elpg_step xusb_ss_up_steps[] = {
    {XUSB_ELPG_VCORE_DOWN(XUSB_SS_PORT), false, 100},
    {XUSB_ELPG_CLAMP_EN_EARLY(XUSB_SS_PORT), false, 100},
    {XUSB_ELPG_CLAMP_EN(XUSB_SS_PORT), false, 0},
};
static const struct xusb_elpg_step xusb_ss_down_steps[] = {
    {XUSB_ELPG_CLAMP_EN_EARLY(XUSB_SS_PORT), true, 100},
    {XUSB_ELPG_CLAMP_EN(XUSB_SS_PORT), true, 250},
    {XUSB_ELPG_VCORE_DOWN(XUSB_SS_PORT), true, 0},
};
#define XUSB_SS_STEPS 3u

static inline uint32_t xusb_elpg_apply(uint32_t v, struct xusb_elpg_step step)
{
    return step.set ? v | step.bit : v & ~step.bit;
}

static inline bool xusb_ss_clamped(uint32_t elpg1)
{
    const uint32_t all =
        XUSB_ELPG_CLAMP_EN(XUSB_SS_PORT) | XUSB_ELPG_CLAMP_EN_EARLY(XUSB_SS_PORT) | XUSB_ELPG_VCORE_DOWN(XUSB_SS_PORT);
    return (elpg1 & all) != 0;
}

// LFPS detection on the lane: on is the pad's own receiver; off holds it idle with termination (U3 entry).
static inline uint32_t xusb_lfps_detect(uint32_t ctl1, bool enable)
{
    const uint32_t v = ctl1 & ~(XUSB_AUX_RX_IDLE_MODE(3u) | XUSB_AUX_RX_TERM_EN | XUSB_AUX_RX_MODE_OVRD);
    return enable ? v : v | XUSB_AUX_RX_IDLE_MODE(1u) | XUSB_AUX_RX_TERM_EN | XUSB_AUX_RX_MODE_OVRD;
}

// --- SS port 0's root port --------------------------------------------------------------------------------------------
// The xHCI port of SS port 0: the first USB 3 port of the Supported Protocol capabilities (Tegra XUSB: 1), 0 when none.
static inline unsigned xusb_ss_xhci_port(unsigned usb3_first, unsigned usb3_count)
{
    return usb3_count > XUSB_SS_PORT ? usb3_first + XUSB_SS_PORT : 0u;
}

// What the root port does as the lane and the cable come and go (SwitchXHCIEvents.cpp). It is held On only with the
// cable in and the lane up. Going On it is reset first as tegra_xhci_set_host_mode does on T210: PP off, the falcon's
// RESET_SSPI, PP on. A USB 3 device that came up on its USB 2.0 pair while the lane was down (after SC7, or a lane that
// waited for PCIe's PLL) has given up on SuperSpeed (its upstream port sits in eSS.Disabled) and tries again only after
// a USB 2.0 bus reset, so its USB2 port's power is cycled and AppleUSBXHCI resets it when it comes back.
struct xusb_ss_port_step {
    bool power_on;    // PP off, RESET_SSPI, PP on, then held On (UsbForcePower)
    bool power_off;   // released Off (UsbForcePower)
    bool usb2_cycle;  // after power_on: the USB2 port's PP off and on
};

static inline struct xusb_ss_port_step xusb_ss_port_follow(bool cable, bool lane_up, bool held_on, bool usb2_connected)
{
    const bool want = cable && lane_up;
    struct xusb_ss_port_step step = {want && !held_on, !want && held_on, false};
    step.usb2_cycle = step.power_on && usb2_connected;
    return step;
}

// --- XUSB_HOST through the SMMU ---------------------------------------------------------------------------------------
// The swgroup's ASID register (MC 0x288, ENABLE 31 | ASID 6:0) as the host's bring-up may find it: in bypass, or
// translated by the host's own mapper (UsbSmmuMapper.cpp, attached at the first start and restored by TegraSMMU after
// SC7). Translation by any other ASID would send the falcon's firmware fetch to someone else's tables.
static inline bool xusb_host_asid_usable(uint32_t asid_reg, uint32_t ours)
{
    return ((asid_reg & 0x80000000u) == 0u) || (asid_reg & 0x7fu) == ours;
}

// --- Falcon mailbox: the SuperSpeed messages ---------------------------------------------------------------------------
#define XUSB_MBOX_SAVE_DFE_CTLE_CTX         9u
#define XUSB_MBOX_RESET_SSPI                16u
#define XUSB_MBOX_DISABLE_SS_LFPS_DETECTION 17u
#define XUSB_MBOX_ENABLE_SS_LFPS_DETECTION  18u
#define XUSB_MBOX_ACK                       128u
#define XUSB_MBOX_NAK                       129u

// RESET_SSPI's data: the SS port, counted from 1 (xhci-tegra.c usb3_otg_port_base_1).
#define XUSB_MBOX_RESET_SSPI_DATA (XUSB_SS_PORT + 1u)

struct xusb_mbox_ss {
    bool handled;    // a SuperSpeed message this decides
    uint32_t reply;  // ACK or NAK
    uint32_t data;   // the reply's data
    bool lfps;       // set SS port 0's LFPS detection to lfps_enable
    bool lfps_enable;
    bool lfps_settle;  // T210: 0.5-1 ms after disabling, before the reply (xhci-tegra, U3 entry stability)
};

static inline struct xusb_mbox_ss xusb_mbox_ss_decide(uint32_t cmd, uint32_t data)
{
    struct xusb_mbox_ss d = {false, XUSB_MBOX_ACK, data, false, false, false};
    if (cmd == XUSB_MBOX_DISABLE_SS_LFPS_DETECTION || cmd == XUSB_MBOX_ENABLE_SS_LFPS_DETECTION) {
        const uint32_t ports = (data >> 1) & 0xfu;
        d.handled = true;
        d.lfps = (((ports >> XUSB_SS_PORT) & 1u) != 0u);
        d.lfps_enable = cmd == XUSB_MBOX_ENABLE_SS_LFPS_DETECTION;
        d.lfps_settle = d.lfps && !d.lfps_enable;
    } else if (cmd == XUSB_MBOX_SAVE_DFE_CTLE_CTX) {
        d.handled = true;
        d.reply = XUSB_MBOX_NAK;
    }
    return d;
}

// --- xHCI writes SwitchXHCI adjusts -------------------------------------------------------------------------------------
// U1/U2 off on SuperSpeed ports, as NVIDIA's own driver leaves them on T210B01 (lpm_support false): a zero U1 and U2
// timeout in PORTPMSC makes the port reject both. U3 (suspend) is untouched.
#define XHCI_PORTPMSC_U1_U2_TIMEOUTS 0xffffu

static inline uint32_t xhci_portpmsc_ss(uint32_t value)
{
    return value & ~XHCI_PORTPMSC_U1_U2_TIMEOUTS;
}

// Interrupt moderation: at least this interval (250 ns units, 40 us) between an interrupter's interrupts. A burst of
// SuperSpeed completions is then one interrupt; a lone event is not delayed.
#define XHCI_IMOD_MIN_INTERVAL 160u

static inline uint32_t xhci_imod_floor(uint32_t imod)
{
    return (imod & 0xffffu) >= XHCI_IMOD_MIN_INTERVAL ? imod : (imod & ~0xffffu) | XHCI_IMOD_MIN_INTERVAL;
}

// Which register an AppleUSBXHCI write at `offset` from the capability base is: an SS port's PORTPMSC, an interrupter's
// IMOD, or neither.
enum xhci_write_kind {
    XHCI_WRITE_OTHER,
    XHCI_WRITE_SS_PORTPMSC,
    XHCI_WRITE_IMOD
};

static inline enum xhci_write_kind xhci_write_kind(uint32_t offset, uint32_t cap_length, uint32_t runtime,
                                                   unsigned ss_first, unsigned ss_count, unsigned interrupters)
{
    const uint32_t ports = cap_length + 0x400u;
    if (ss_count && offset >= ports + 0x10u * (ss_first - 1u) && offset < ports + 0x10u * (ss_first - 1u + ss_count) &&
        (offset - ports) % 0x10u == 4u)
        return XHCI_WRITE_SS_PORTPMSC;
    if (offset >= runtime + 0x20u && offset < runtime + 0x20u + 32u * interrupters &&
        (offset - runtime - 0x20u) % 32u == 4u)
        return XHCI_WRITE_IMOD;
    return XHCI_WRITE_OTHER;
}

#ifdef __cplusplus
}
#endif

#endif
