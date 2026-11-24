/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_CHANNEL_H
#define SWITCH_A3_CHANNEL_H
#include <stdint.h>

struct switch_a3_channel_binding {
    void *wlc, *bsscfg;
    uintptr_t completion, entered;
    void *completion_context, *entered_context;
    uint64_t expiry_tsf;
    uint32_t session, clock_epoch, generation;
    uint16_t home_chanspec, target_chanspec;
};

enum switch_a3_channel_result {
    SWITCH_A3_CHANNEL_OK,
    SWITCH_A3_CHANNEL_NO_PM_ACK,
    SWITCH_A3_CHANNEL_EXPIRED,
    SWITCH_A3_CHANNEL_CANCELLED,
    SWITCH_A3_CHANNEL_OWNERSHIP_LOST,
    SWITCH_A3_CHANNEL_STATION_CHANGED,
    SWITCH_A3_CHANNEL_CLOCK_FAILED,
    SWITCH_A3_CHANNEL_RESTRICTED
};

int switch_a3_channel_claim(const struct switch_a3_channel_binding *binding);
void *switch_a3_station_bsscfg(void *wlc);
uint16_t switch_a3_channel_control(uint32_t chanspec);
int switch_a3_channel_usable(void *wlc, uint16_t chanspec);
uint16_t switch_a3_channel_socials(void *wlc);
uint16_t switch_a3_channel_home(void *wlc);
const struct switch_a3_channel_binding *switch_a3_channel_current(void);
int switch_a3_channel_owned(void);
int switch_a3_channel_abort(void);
uint32_t switch_a3_channel_complete(void);
int switch_a3_channel_may_leave(void *wlc, uint32_t chanspec);
void switch_a3_channel_pm_status(void *wlc, void *bsscfg, void *packet, uint32_t original_fc, uint32_t compact_status,
                                 const uint8_t *header);
int switch_a3_channel_pm_prepare(void *bsscfg);
uintptr_t switch_a3_channel_pm_capture(void *wlc, void *packet, void *scb);
void switch_a3_channel_pm_submitted(uintptr_t token, int accepted);
void switch_a3_channel_pm_set(void *bsscfg, uint32_t enabled);
void switch_a3_channel_gate(void *wlc, uint32_t chanspec);

#endif
