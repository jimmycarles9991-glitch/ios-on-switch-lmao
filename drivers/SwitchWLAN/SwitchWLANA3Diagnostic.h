/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_WLAN_A3_DIAGNOSTIC_H
#define SWITCH_WLAN_A3_DIAGNOSTIC_H

#if DEVELOPMENT || DEBUG
#include <string.h>
#include "firmware/a3/diagnostic.h"
#include "firmware/a3/wire.h"

struct SwitchWLANA3EchoSnapshot {
    uint32_t lifetime, epoch;
    uint16_t id, sequence;
};

struct SwitchWLANA3Diagnostics {
    SwitchWLANA3EchoSnapshot echo;
    uint64_t requested_ns;
    uint32_t requests;
    uint32_t trial_generation, trial_epoch, trial_lifetime;
    uint16_t trial_channel;
    bool fixed_trial_ended;
    bool fault_requested, fault_pending, trial_requested, trial_pending;
    uint32_t transitions, last_ready, last_epoch;
    int32_t last_error;
    uint32_t rx_records, rx_unready, rx_invalid, rx_stale, rx_unbound, rx_delivered;
    uint32_t tx_records, tx_rejected, tx_sent, tx_expired, tx_cancelled, tx_failed, tx_ack_missing;
    uint32_t tx_last_id, tx_last_status, tx_last_flags;
    uint64_t tx_last_tsf;
};

void switch_wlan_a3_discovery_trial();
void switch_wlan_a3_echo_snapshot(uint32_t session, uint32_t epoch, uint16_t id, uint16_t sequence);
bool switch_wlan_a3_trial_applied(uint16_t channel, const char **blocked = nullptr);
void switch_wlan_a3_trial_boundary(bool started, uint16_t channel);

static inline bool switch_a3_diagnostic_due(const SwitchWLANA3Diagnostics &state, uint64_t now)
{
    return !state.requests || (now >= state.requested_ns && now - state.requested_ns >= 30000000000ULL);
}

static inline bool switch_a3_diagnostic_decode(const void *data, size_t length, const switch_a3_reply &reply,
                                               struct switch_a3_diagnostic &snapshot)
{
    if (!data || length != sizeof snapshot)
        return false;
    memcpy(&snapshot, data, sizeof snapshot);
    return snapshot.magic == SWITCH_A3_DIAGNOSTIC_MAGIC && snapshot.version == SWITCH_A3_DIAGNOSTIC_VERSION &&
           snapshot.bytes == sizeof snapshot && snapshot.session == reply.session &&
           snapshot.clock_epoch == reply.clock_epoch && snapshot.request_id == reply.request_id &&
           snapshot.state_revision == reply.state_revision && snapshot.pm_reason <= SWITCH_A3_PM_OWNERSHIP_LOST;
}
#endif
#endif
