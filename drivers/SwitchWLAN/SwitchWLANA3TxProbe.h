// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_A3_TX_PROBE_H
#define SWITCH_WLAN_A3_TX_PROBE_H
#if DEVELOPMENT || DEBUG
#include <stddef.h>
#include <stdint.h>
#include "firmware/a3/wire.h"
void switch_wlan_a3_tx_probe_prepare(const uint8_t mac[6], int d11n, uint16_t firstRing, unsigned ringCount);
void switch_wlan_a3_tx_probe_start(void);
bool switch_wlan_a3_tx_probe_authorize(const uint8_t *envelope, size_t length);
void switch_wlan_a3_tx_probe_observe(const uint8_t *frame, size_t length);

struct switch_wlan_a3_probe_ownership {
    unsigned dma_pending;
    bool rings_closed;
};

void switch_wlan_a3_tx_probe_dma(unsigned outstanding, bool closed);

struct switch_wlan_a3_probe_lease {
    uint32_t session = 0, epoch = 0;

    bool current(uint32_t currentSession, uint32_t currentEpoch) const
    {
        return session && epoch && session == currentSession && epoch == currentEpoch;
    }

    bool claim(uint32_t currentSession, uint32_t currentEpoch, bool open)
    {
        if (!currentSession || !currentEpoch || open || current(currentSession, currentEpoch))
            return false;
        session = currentSession;
        epoch = currentEpoch;
        return true;
    }
};

struct switch_wlan_a3_probe_driver_state {
    uint32_t session, epoch;
    bool up, power, gone, halted, open;
};

// The state snapshot and open mutation share the driver's command gate.
static inline bool switch_wlan_a3_probe_mark_open(const switch_wlan_a3_probe_lease &lease,
                                                  const switch_wlan_a3_probe_driver_state &state, bool &open)
{
    if (!state.up || !state.power || state.gone || state.halted || lease.current(state.session, state.epoch))
        return false;
    open = true;
    return true;
}

static inline bool switch_wlan_a3_probe_control_allowed(const switch_wlan_a3_probe_lease &lease,
                                                        const switch_wlan_a3_probe_driver_state &state,
                                                        const switch_a3_control &request)
{
    if (!state.up || !state.power || state.gone || state.halted)
        return false;
    if (request.operation == SWITCH_A3_GET_CLOCK)
        return true;
    return !state.open && lease.current(state.session, state.epoch) &&
           lease.current(request.session, request.clock_epoch);
}

enum {
    SWITCH_A3_PROBE_CLOSE = 1,
    SWITCH_A3_PROBE_RECOVER = 2,
    SWITCH_A3_PROBE_RELEASE = 4
};

int switch_wlan_a3_tx_probe_claim(uint32_t session, uint32_t epoch);
int switch_wlan_a3_tx_probe_transmit(const uint8_t *envelope, size_t length);
int switch_wlan_a3_tx_probe_cleanup(uint32_t session, uint32_t epoch, unsigned flags,
                                    switch_wlan_a3_probe_ownership *ownership);
#endif
#endif
