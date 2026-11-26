// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XUSB power partitions both controllers power (XUSBA, SuperSpeed, with XUSBB for the XUDC or XUSBC for the host):
// each one's PMC partition, its CAR clock and reset (Linux TEGRA210_CLK_*: bank * 32 + bit over banks L H U V W X Y),
// and the memory client whose hot reset its power-up releases (L4T powergate-t21x.c tegra210_pg_mc_info).
#pragma once
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../TegraSMMU/TegraSMMU.h"

namespace xusb {

enum : unsigned {
    PART_XUSBA = 20,
    PART_XUSBB = 21,
    PART_XUSBC = 22,
};

constexpr unsigned kClockXusbHost = 2 * 32 + 25, kClockXusbDev = 2 * 32 + 31, kClockXusbSs = 4 * 32 + 28;

// MC_CLIENT_HOTRESET_CTRL bits 20 (XUSB_DEV) and 19 (XUSB_HOST).
inline constexpr switch_pmc_mc_client kXusbDevClient = {0x200, 1u << 20, tegra_mc_hot_reset};
inline constexpr switch_pmc_mc_client kXusbHostClient = {0x200, 1u << 19, tegra_mc_hot_reset};

}  // namespace xusb
