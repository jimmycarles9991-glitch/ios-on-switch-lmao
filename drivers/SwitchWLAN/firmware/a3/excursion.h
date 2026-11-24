/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_EXCURSION_H
#define SWITCH_A3_EXCURSION_H
#include <stdint.h>
#include "fault.h"

struct switch_a3_excursion_request {
    void *wlc, *bsscfg;
    uint64_t expiry_tsf, station_return_tsf;
    uint32_t session, clock_epoch, generation;
    uint16_t home_chanspec, target_chanspec;
    uint32_t station_bound_verified;
};

enum switch_a3_excursion_event {
    SWITCH_A3_EXCURSION_ENTERED,
    SWITCH_A3_EXCURSION_COMPLETED
};

enum switch_a3_excursion_reason {
    SWITCH_A3_EXCURSION_OK,
    SWITCH_A3_EXCURSION_CANCELLED,
    SWITCH_A3_EXCURSION_EXPIRED,
    SWITCH_A3_EXCURSION_NATIVE_FAILED,
    SWITCH_A3_EXCURSION_ENTRY_FAILED,
    SWITCH_A3_EXCURSION_RETURN_FAILED,
    SWITCH_A3_EXCURSION_OWNERSHIP_LOST,
    SWITCH_A3_EXCURSION_CLOCK_FAILED,
    SWITCH_A3_EXCURSION_CLOCK_BACKWARD,
    SWITCH_A3_EXCURSION_TIMER_FAILED
};

struct switch_a3_excursion_result {
    uint64_t returned_tsf;
#if DEVELOPMENT || DEBUG
    uint64_t rx_ready_tsf;
#endif
    union {
        struct switch_a3_owner owner;
        struct { uint32_t session, clock_epoch, generation; };
    };
    uint32_t reason, channel_result;
    int32_t native_status;
};

typedef void (*switch_a3_excursion_callback)(void *, uint32_t, const struct switch_a3_excursion_result *);

int switch_a3_excursion_start(const struct switch_a3_excursion_request *request, switch_a3_excursion_callback callback,
                              void *context);
int switch_a3_excursion_cancel(void *wlc, uint32_t session, uint32_t epoch, uint32_t generation);
uint32_t switch_a3_excursion_pending(void);
#if DEVELOPMENT || DEBUG
void switch_a3_excursion_rx_ready(void *wlc);
#endif

#endif
