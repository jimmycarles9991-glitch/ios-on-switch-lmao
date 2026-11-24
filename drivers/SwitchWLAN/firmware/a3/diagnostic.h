/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_DIAGNOSTIC_H
#define SWITCH_A3_DIAGNOSTIC_H

#include <stddef.h>
#include <stdint.h>
#include "fault.h"
#include "beacon.h"

#define SWITCH_A3_DIAGNOSTIC_MAGIC 0x33445753u
#define SWITCH_A3_DIAGNOSTIC_VERSION 4u
#define SWITCH_A3_PM_STATUS_UNAVAILABLE 0xffu

enum switch_a3_pm_reason {
    SWITCH_A3_PM_NONE,
    SWITCH_A3_PM_PENDING,
    SWITCH_A3_PM_ACKED,
    SWITCH_A3_PM_STATUS_REJECTED,
    SWITCH_A3_PM_NO_PACKET,
    SWITCH_A3_PM_SUBMIT_REFUSED,
    SWITCH_A3_PM_INVALIDATED,
    SWITCH_A3_PM_CANCELLED,
    SWITCH_A3_PM_OWNERSHIP_LOST
};

enum switch_a3_diagnostic_counter {
    SWITCH_A3_DIAG_TX_INGRESS,
    SWITCH_A3_DIAG_TX_NATIVE,
    SWITCH_A3_DIAG_TX_STATUS,
    SWITCH_A3_DIAG_RX_HEADER,
    SWITCH_A3_DIAG_RX_PHY,
    SWITCH_A3_DIAG_RX_ACTION,
    SWITCH_A3_DIAG_RX_BSSID,
    SWITCH_A3_DIAG_RX_DEST,
    SWITCH_A3_DIAG_SCHED_BLOCK,
    SWITCH_A3_DIAG_RX_PSF,
    SWITCH_A3_DIAG_GATE_ALLOW,
    SWITCH_A3_DIAG_GATE_DENY,
    SWITCH_A3_DIAG_EXCURSION_START,
    SWITCH_A3_DIAG_EXCURSION_ENTER,
    SWITCH_A3_DIAG_EXCURSION_RETURN,
    SWITCH_A3_DIAG_RX_HOOK,
    SWITCH_A3_DIAG_RX_MIF,
    SWITCH_A3_DIAG_RX_MATCH,
    SWITCH_A3_DIAG_RX_SPLIT,
    SWITCH_A3_DIAG_RX_EXPORT,
    SWITCH_A3_DIAG_RX_DROP,
    SWITCH_A3_DIAG_TX_STATUS_EXPORT_DROP,
    SWITCH_A3_DIAG_SCHED_FAULT,
    SWITCH_A3_DIAG_RX_FETCH_FAILED,
    SWITCH_A3_DIAG_COUNTERS,
};

struct switch_a3_fault_record {
    uint64_t observed_tsf, deadline_tsf, actual_tsf;
    uint32_t sequence, session, epoch, config;
    uint16_t reason;
    int16_t native_error;
    uint16_t target_chanspec, actual_chanspec;
};

struct switch_a3_diagnostic {
    uint32_t magic;
    uint16_t version, bytes;
    uint32_t session, clock_epoch, request_id, state_revision;
    struct switch_a3_beacon_anchor beacon;
    uint32_t counters[SWITCH_A3_DIAG_COUNTERS];
    struct switch_a3_fault_record first_fault, latest_fault;
    uint64_t dwell_entered, dwell_returned;
    struct switch_a3_owner dwell_owner;
    uint16_t dwell_channel;
    uint8_t pm_status, pm_reason;
    uint32_t pm_requests, pm_packets, pm_acks, pm_failed;
};

#ifdef __cplusplus
#define SWITCH_A3_DIAG_ASSERT static_assert
#else
#define SWITCH_A3_DIAG_ASSERT _Static_assert
#endif
SWITCH_A3_DIAG_ASSERT(sizeof(struct switch_a3_diagnostic) == 288, "A3 diagnostic size");
SWITCH_A3_DIAG_ASSERT(sizeof(struct switch_a3_fault_record) == 48, "A3 fault record size");
SWITCH_A3_DIAG_ASSERT(offsetof(struct switch_a3_diagnostic, counters) == 48, "A3 diagnostic counters");
SWITCH_A3_DIAG_ASSERT(offsetof(struct switch_a3_diagnostic, first_fault) == 144, "A3 diagnostic first fault");
SWITCH_A3_DIAG_ASSERT(offsetof(struct switch_a3_diagnostic, latest_fault) == 192, "A3 diagnostic latest fault");
SWITCH_A3_DIAG_ASSERT(offsetof(struct switch_a3_diagnostic, dwell_entered) == 240, "A3 diagnostic dwell");
SWITCH_A3_DIAG_ASSERT(offsetof(struct switch_a3_diagnostic, pm_status) == 270, "A3 diagnostic PM status");
#undef SWITCH_A3_DIAG_ASSERT

#if DEVELOPMENT || DEBUG
extern struct switch_a3_diagnostic switch_a3_diagnostic;
void switch_a3_diagnostic_count(uint32_t offset);
void switch_a3_diagnostic_pm(uint32_t value);
#define SWITCH_A3_DIAG_INDEX(index) \
    switch_a3_diagnostic_count((offsetof(struct switch_a3_diagnostic, counters) >> 2) + (index))
#define SWITCH_A3_DIAG_FIELD(name) switch_a3_diagnostic_count(offsetof(struct switch_a3_diagnostic, name) >> 2)
#define SWITCH_A3_DIAG_COUNT(name) SWITCH_A3_DIAG_INDEX(SWITCH_A3_DIAG_##name)
#define SWITCH_A3_DIAG(statement) do { statement; } while (0)
#else
#define SWITCH_A3_DIAG_COUNT(name) ((void)0)
#define SWITCH_A3_DIAG(statement) ((void)0)
#endif

#define SWITCH_A3_DIAG_PM(reason, status) SWITCH_A3_DIAG(switch_a3_diagnostic_pm((SWITCH_A3_PM_##reason << 8) | (uint8_t)(status)))

#endif
