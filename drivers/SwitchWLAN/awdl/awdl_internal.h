// SPDX-License-Identifier: BSD-2-Clause-Patent
// Internals shared by the AWDL core's units. Not for the integration (awdl.h is the interface).
#ifndef AWDL_INTERNAL_H
#define AWDL_INTERNAL_H

#include "awdl.h"

#include <stdarg.h>
#include <string.h>
#ifdef KERNEL
extern int vsnprintf(char *, size_t, const char *, va_list) __attribute__((format(printf, 3, 0)));
#else
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AWDL_AW_US             ((uint64_t)AWDL_AW_TU * AWDL_TU_US)            // 16384
#define AWDL_EAW_US            (AWDL_AW_US * AWDL_EAW_AWS)                    // 65536
#define AWDL_SEQ_US            (AWDL_EAW_US * AWDL_SEQ_SLOTS)                 // 1048576
#define AWDL_PSF_US            ((uint64_t)AWDL_PSF_INTERVAL_TU * AWDL_TU_US)  // 112640
#define AWDL_PSF_FOLLOWER_US   ((uint64_t)440 * AWDL_TU_US)  // Apple's other action frame period (Stute et al. §7.4)
#define AWDL_ROOT_COUNTER_US   3140000u  // a root's self counter ticks every π s (OWL's reading of IO80211Family)
#define AWDL_ELECTION_FRESH_US 2000000u  // election parameters older than this are ignored
#define AWDL_RESYNC_US         256       // re-install the radio schedule beyond this error
// An away slot opens this far in: the peers synchronised to the same master switch channel at the slot boundary too,
// and on a radio without hops that first send is what takes it to the slot's channel (host.tx).
#define AWDL_SLOT_GUARD_US 2048u

struct awdl_writer {
    uint8_t *buf;
    size_t size, at, tlv;
    int overflow;
};

void awdl_put16(uint8_t *p, uint16_t v);
void awdl_put32(uint8_t *p, uint32_t v);
int awdl_w_begin(struct awdl_writer *w, uint8_t type);
void awdl_w_end(struct awdl_writer *w);
void awdl_w_bytes(struct awdl_writer *w, const void *p, size_t n);
void awdl_w_u8(struct awdl_writer *w, uint8_t v);
void awdl_w_u16(struct awdl_writer *w, uint16_t v);
void awdl_w_u32(struct awdl_writer *w, uint32_t v);
void awdl_w_chanseq(struct awdl_writer *w, const struct awdl_chan seq[AWDL_SEQ_SLOTS]);

struct awdl_txq_entry {
    uint16_t len;
    uint8_t group, transfer;
    uint8_t dst[6];
    uint8_t frame[AWDL_MAX_FRAME];
};

static inline int awdl_addr_eq(const uint8_t *a, const uint8_t *b)
{
    return !memcmp(a, b, 6);
}

static inline int awdl_is_group(const uint8_t *a)
{
    return a[0] & 1;
}

struct awdl_core {
    struct awdl_host host;
    struct awdl_config cfg;
    int running;
    // timing: AW number base_seq starts at local TSF base_tsf
    uint64_t base_tsf;
    uint16_t base_seq;
    uint64_t next_psf, next_slot, next_mif, last_counter_tick, anchor;
    // election
    enum awdl_role role;
    struct awdl_election_v2 adv;  // what we advertise (root, parent, hops, metrics, counters)
    uint32_t self_counter;
    // peers and transmit queue
    struct awdl_peer peers[AWDL_MAX_PEERS];
    struct awdl_txq_entry txq[AWDL_TXQ_FRAMES];
    unsigned txq_head, txq_count;
    uint16_t data_seq, mgmt_seq, service_update_indicator;
    uint8_t seq_fixed;  // awdl_core_set_sequence named the sequence: no schedule rebuilds
    struct awdl_stats stats;
    // native backend: the firmware's AWDL interface and its bring-up
    uint8_t nstate;        // AWDL_N_*
    uint8_t created_with;  // enum bcm_awdl_api used for the interface being made / made
    uint8_t payload_u16;   // awdl_payload's length form (u16: interface_create v3; u32: 7.x), switched once on refusal
    uint8_t payload_switched;
    uint8_t seq_no_empty;     // the 7.x awdl_chan_seq refused empty slots once: free slots take slot 0's channel
    uint8_t mode;             // enum awdl_mode
    uint64_t transfer_until;  // transfer mode ends here unless more data comes
    uint8_t social5;          // 5 GHz social channel learnt from the master's sequence (44 or 149), 0 before
    int16_t fw_ifidx;         // the AWDL interface's firmware index (WLC_E_IF add), -1 before
    uint8_t fw_bsscfg;
    uint16_t infra_mhz;
    uint64_t create_deadline, aftx_at, settle_at;  // 0: not pending
    uint8_t pushed_role, pushed_parent[6], pushed_root[6];
    uint8_t scratch[AWDL_MAX_FRAME];
};

enum {
    AWDL_N_OFF,
    AWDL_N_CREATING,
    AWDL_N_UP
};

#define AWDL_CREATE_TIMEOUT_US 2000000u  // the series' BRCMF_AWDL_IF_TIMEOUT
#define AWDL_AFTX_DELAY_US     500000u
#define AWDL_SETTLE_DELAY_US   5000000u

void awdl_log(struct awdl_core *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static inline int awdl_native(const struct awdl_core *c)
{
    return c->cfg.backend == AWDL_BACKEND_NATIVE;
}

// awdl_core.c: the timing model and the peer table.
uint64_t awdl_eaw_start(const struct awdl_core *c, uint64_t now);  // start of the EAW containing now
uint8_t awdl_master_chan(const struct awdl_core *c);  // the master's social channel: our parent's word, else ours
void awdl_expire_peers(struct awdl_core *c, uint64_t now);
void awdl_run_election(struct awdl_core *c, uint64_t now);
void awdl_handle_af(struct awdl_core *c, const struct awdl_af *a, const struct awdl_rx_meta *m);

// awdl_tx.c: the TLVs beyond sync and election (OWL's order; `mif` adds HT capabilities and arpa).
void awdl_write_extras(struct awdl_core *c, struct awdl_writer *w, int mif);

// awdl_schedule.c: the sequence for the mode, the infra channel and the learnt social channel, shared by both backends.
void awdl_sched_start(struct awdl_core *c);
void awdl_sched_learn_social(struct awdl_core *c, const struct awdl_peer *p, uint64_t now);
void awdl_schedule_from(struct awdl_chan out[AWDL_SEQ_SLOTS], uint16_t mask, struct awdl_chan free_chan, uint8_t first,
                        uint8_t second);
// Only data that really moved counts: frames out once the radio took them, frames in once delivered.
int awdl_transfer_frame(const uint8_t *eth, size_t len);
void awdl_sched_transfer(struct awdl_core *c, const uint8_t peer[6], int out, uint64_t now);
void awdl_sched_traffic(struct awdl_core *c, const uint8_t *eth, size_t len, int out, uint64_t now);
void awdl_sched_deadline(struct awdl_core *c, uint64_t now);  // transfer mode's end
uint64_t awdl_sched_next(const struct awdl_core *c);          // its deadline, UINT64_MAX when idle
void awdl_sched_log(struct awdl_core *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// awdl_native.c: the firmware's own AWDL.
void awdl_native_build_seq(struct awdl_core *c);
void awdl_native_resend(struct awdl_core *c, uint16_t old_map);
void awdl_native_start(struct awdl_core *c, uint64_t now);
void awdl_native_stop(struct awdl_core *c);
uint64_t awdl_native_timer(struct awdl_core *c, uint64_t now);
void awdl_native_after_af(struct awdl_core *c);
void awdl_native_if_up(struct awdl_core *c, uint8_t ifidx, uint8_t bssidx, uint64_t now);
void awdl_native_fail(struct awdl_core *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void awdl_native_peer(struct awdl_core *c, const struct awdl_peer *p, int add);
int awdl_native_output(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now);

// awdl_data.c: the soft backend's data path.
void awdl_data_flush(struct awdl_core *c, uint64_t now);
void awdl_data_rx(struct awdl_core *c, const uint8_t *f, size_t len, const struct awdl_rx_meta *m);

// awdl_core.c: the soft backend's sequence changed (mode, infra, social channel): install it on the radio.
void awdl_soft_install(struct awdl_core *c, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif
