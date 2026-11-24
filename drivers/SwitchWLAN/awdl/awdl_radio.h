/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_AWDL_RADIO_H
#define SWITCH_AWDL_RADIO_H

#include "awdl.h"

#define SWITCH_AWDL_CLOCK_UNCERTAINTY_NS 1000000u
/* Maximum assumed TSF drift between correlated samples. */
#define SWITCH_AWDL_CLOCK_DRIFT_PPM 500u
#define SWITCH_AWDL_CLOCK_JUMP_US   3000u

struct switch_awdl_radio_clock {
    struct awdl_tsf_clock affine;
    uint32_t epoch, uncertainty_ns, invalid_epoch;
    uint8_t renewal_attempted;
};

struct switch_awdl_radio_clock_request {
    uint8_t pending, blocked;
};

#ifdef __cplusplus
extern "C" {
#endif

/* 1: a new epoch or discontinuity; 0: same clock; -1: rejected sample. */
int switch_awdl_radio_clock_update(struct switch_awdl_radio_clock *clock, uint64_t tsf, uint64_t host_ns,
                                   uint32_t uncertainty_ns, uint32_t epoch, uint64_t now_ns);
int switch_awdl_radio_clock_valid(const struct switch_awdl_radio_clock *clock, uint64_t now_ns);
uint64_t switch_awdl_radio_clock_expires(const struct switch_awdl_radio_clock *clock);
uint64_t switch_awdl_radio_clock_deadline(const struct switch_awdl_radio_clock *clock);
int switch_awdl_radio_clock_try_renew(struct switch_awdl_radio_clock *clock, uint64_t now_ns);
void switch_awdl_radio_clock_clear(struct switch_awdl_radio_clock *clock);
uint64_t switch_awdl_radio_clock_tolerance_us(uint32_t previous_uncertainty_ns, uint32_t uncertainty_ns,
                                             uint64_t elapsed_ns);
int switch_awdl_radio_clock_request_begin(struct switch_awdl_radio_clock_request *request, int independent);
void switch_awdl_radio_clock_request_complete(struct switch_awdl_radio_clock_request *request, int error);

enum switch_awdl_tx_outcome {
    SWITCH_AWDL_TX_ACKED,
    SWITCH_AWDL_TX_UNACKED,
    SWITCH_AWDL_TX_EXPIRED,
    SWITCH_AWDL_TX_CANCELLED,
    SWITCH_AWDL_TX_FAILED
};

/* A unicast data frame the core built: its AWDL data sequence number, which names it to switch_awdl_core_tx_done. */
int switch_awdl_data_token(const uint8_t *frame, size_t len, uint16_t *token);
/* The radio's outcome for a unicast data frame the core handed it. A frame that never went on air (expired, cancelled)
 * waits in its place in the queue again while its lifetime lasts; any other outcome retires it. */
void switch_awdl_core_tx_done(struct awdl_core *c, uint16_t token, enum switch_awdl_tx_outcome outcome, uint64_t tsf,
                              uint64_t now);

#if DEVELOPMENT || DEBUG
struct switch_awdl_data_probe {
    uint8_t addr[6];
    uint32_t rx_frames[4]; /* unicast data frames on channel 6, 44, 149, other */
    uint32_t rx_retry, rx_dup, rx_delivered;
    uint32_t tx_queued, tx_handed, tx_acked, tx_unacked, tx_expired, tx_cancelled, tx_failed, tx_requeued;
    uint64_t max_wait_us, max_done_us;
};

/* The data counters of peer slot `index` since the previous call, then zeroed; 0 when it had no data traffic. */
int switch_awdl_core_data_probe(struct awdl_core *c, unsigned index, struct switch_awdl_data_probe *out);
#endif

#ifdef __cplusplus
}
#endif
#endif
