// SPDX-License-Identifier: BSD-2-Clause-Patent
// The registers XusbHost.cpp (firmware, ports, mailbox) and XusbHostPower.cpp (clocks, partitions, pad, FPCI/IPFS)
// both program.
#pragma once
#include <stdint.h>

namespace xhost {

// Physical bases: Tegra X1 TRM, hekate bdk/soc/t210.h, the OLED DTB.
inline constexpr uint64_t kCarPA = 0x60006000, kFusePagePA = 0x7000F000, kMcPA = 0x70019000, kPadctlPA = 0x7009F000,
                          kFpciPA = 0x70098000, kIpfsPA = 0x70099000;

enum : uint32_t {  // clock and reset controller
    RST_DEV_U_SET = 0x310,
    RST_DEV_U_CLR = 0x314,
    CLK_ENB_U_SET = 0x330,
    RST_DEV_W_SET = 0x438,
    RST_DEV_W_CLR = 0x43c,
    CLK_ENB_W_SET = 0x448,
    PLLU_BASE = 0x0c0,
    UTMIPLL_HW_PWRDN_CFG0 = 0x52c,
    CLK_SOURCE_XUSB_CORE_HOST = 0x600,
    CLK_SOURCE_XUSB_FALCON = 0x604,
    CLK_SOURCE_XUSB_FS = 0x608,
    CLK_SOURCE_XUSB_SS = 0x610,
};

// USB2_PORT_CAP port 0: CAP 1:0, INTERNAL 2, REVERSE_ID 3; OTG with neither, as the Tegra X1 TRM's OTG role has it
// ("PORTN_CAP = OTG_CAP, REVERSE_ID = NO", 22.9.11.1).
inline constexpr uint32_t kPort0CapFields = 0xfu;

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
};

enum : uint32_t {  // FPCI configuration space of the host (xhci-tegra.c)
    XUSB_CFG_1 = 0x004,
    XUSB_CFG_4 = 0x010,
    XUSB_CFG_16 = 0x040,
    XUSB_CFG_24 = 0x060,
    XUSB_CFG_AXI_CFG = 0x0f8,
    XUSB_CFG_ARU_CONTEXT = 0x43c,
    XUSB_CFG_ARU_CONTEXT_HS_PLS = 0x478,
    XUSB_CFG_ARU_CONTEXT_FS_PLS = 0x47c,
    XUSB_CFG_ARU_CONTEXT_HSFS_SPEED = 0x480,
    XUSB_CFG_ARU_CONTEXT_HSFS_PP = 0x484,
    XUSB_CFG_ARU_MBOX_CMD = 0x0e4,
    XUSB_CFG_ARU_MBOX_DATA_IN = 0x0e8,
    XUSB_CFG_ARU_MBOX_DATA_OUT = 0x0ec,
    XUSB_CFG_ARU_MBOX_OWNER = 0x0f0,
    XUSB_CFG_ARU_C11PAGESEL = 0x404,
    XUSB_CFG_ARU_C11_CSBRANGE = 0x41c,
    XUSB_CFG_ARU_SMI_INTR = 0x428,
    XUSB_CFG_HSPX_CORE_CTRL = 0x600,
    XUSB_CFG_CSB_BASE_ADDR = 0x800,
    CFG_1_ENABLES = 6,  // memory space, bus master (Tegra X1 TRM 22.8.4 step 9; no IO space)
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
    IPFS_FPCI_BAR0 = 0x080,
    FPCI_BAR0_ACCESS_TYPE = 1u,  // 0: memory-mapped, posted writes; 1 (reset): I/O or config, non-posted
    IPFS_MSI_BAR_SZ = 0x0c0,
    IPFS_MSI_AXI_BAR_ST = 0x0c4,
    IPFS_MSI_FPCI_BAR_ST = 0x0c8,
    IPFS_MSI_VEC0 = 0x100,
    IPFS_MSI_EN_VEC0 = 0x140,
    IPFS_CONFIGURATION = 0x180,
    IPFS_FPCI_ERROR_MASKS = 0x184,
    IPFS_INTR_MASK = 0x188,
    IPFS_INTR_ENABLE = 0x198,  // xhci-tegra.c XUSB_HOST_IPFS_INTR_ENABLE_0: saved and restored, never set
    IPFS_UFPCI_CONFIG = 0x19c,
    IPFS_CLKGATE_HYSTERESIS = 0x1bc,
    IPFS_MCCIF_FIFOCTRL = 0x1dc,
    IPFS_EN_FPCI = 1u << 0,
    IPFS_IP_INT_MASK = 1u << 16,
};

enum : uint32_t {  // XUSB pad controller (xusb-tegra210.c); port 0's role registers are XusbPorts.h's
    USB2_PAD_MUX = 0x004,
    SS_PORT_MAP = 0x014,
    ELPG_PROGRAM_0 = 0x020,
    ELPG_PROGRAM_1 = 0x024,
    OTG_PAD0_CTL_0 = 0x088,
    OTG_PAD0_CTL_1 = 0x08c,
    BIAS_PAD_CTL_0 = 0x284,
    OTG_PAD_PD = 1u << 26,
    OTG_PAD_PD_ZI = 1u << 29,
};

}  // namespace xhost
