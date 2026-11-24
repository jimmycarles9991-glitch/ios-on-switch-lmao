/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_BEACON_H
#define SWITCH_A3_BEACON_H
#include <stdint.h>

struct switch_a3_beacon_anchor {
    uint64_t radio_tsf, ap_tsf;
    uint8_t bssid[6];
    uint16_t chanspec;
};

#if DEVELOPMENT || DEBUG
const struct switch_a3_beacon_anchor *switch_a3_beacon_anchor(void *wlc);
#endif
int switch_a3_beacon_observe(void *wlc, const void *status, uint32_t status_bytes, const uint8_t *frame,
                             uint32_t frame_bytes, uint64_t received_tsf);
int switch_a3_beacon_bound(void *wlc, uint16_t home_chanspec, uint64_t now, void **bsscfg, uint64_t *return_before_tsf);
void switch_a3_beacon_reset(void *wlc);

#endif
