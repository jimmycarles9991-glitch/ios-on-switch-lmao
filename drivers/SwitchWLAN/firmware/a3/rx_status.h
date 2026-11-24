/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_RX_STATUS_H
#define SWITCH_A3_RX_STATUS_H

#include <stdint.h>

static __attribute__((noinline, noclone)) int switch_a3_rx_phy_valid(const void *wlc, const uint8_t *status)
{
    const uint8_t *band = *(const uint8_t *const *)((const uint8_t *)wlc + 32);
    if (!band)
        return 0;
    const uint16_t phy_type = (uint16_t)(band[8] | (uint16_t)band[9] << 8);
    const uint16_t phy = (uint16_t)(status[4] | (uint16_t)status[5] << 8);
    const uint16_t errors = phy_type == 8 || phy_type == 10 || phy_type == 12 ? 0x210u : 0x310u;
    return !(phy & errors) && !(status[16] & 1u);
}
#endif
