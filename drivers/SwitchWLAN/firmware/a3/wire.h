/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_A3_WIRE_H
#define SWITCH_A3_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define SWITCH_A3_CONTROL_IOVAR   "switch_awdl_ctl"
#define SWITCH_A3_DEADLINE_IOVAR  "switch_awdl_deadline"
#define SWITCH_A3_WIRE_VERSION    2u
#define SWITCH_A3_CONTROL_MAGIC   0x43415753u
#define SWITCH_A3_REPLY_MAGIC     0x52515753u
#define SWITCH_A3_STATE_MAGIC     0x53515753u
#define SWITCH_A3_TX_MAGIC        0x58545753u
#define SWITCH_A3_TX_STATUS_MAGIC 0x43545753u
#define SWITCH_A3_RAW_RX_MAGIC    0x58525753u
#define SWITCH_A3_RAW_RX_VERSION  2u
#define SWITCH_A3_RX_FCS_PRESENT  1u
#define SWITCH_A3_MAX_SLOTS       16u
#define SWITCH_A3_MAX_FRAME       1800u
#define SWITCH_A3_CLOCK_STALE     (-35)
/* The inline Ethernet source carries the request token even when DMA fetch fails. */
#define SWITCH_A3_TX_MARKER_BYTES {0x02, 0x53, 0x57, 0x41, 0x44, 0x4c}

enum switch_a3_operation {
    SWITCH_A3_CONFIGURE = 1,
    SWITCH_A3_INSTALL_SCHEDULE,
    SWITCH_A3_BIND_RINGS,
    SWITCH_A3_START,
    SWITCH_A3_STOP,
    SWITCH_A3_GET_CLOCK,
    SWITCH_A3_GET_STATE,
    SWITCH_A3_TEST_DEADLINE,
    SWITCH_A3_TEST_RECEIVE,
    SWITCH_A3_RESET_CLOCK
};

enum switch_a3_feature {
    SWITCH_A3_FEATURE_RX = 1u << 0,
    SWITCH_A3_FEATURE_ACK = 1u << 1,
    SWITCH_A3_FEATURE_TX = 1u << 2,
    SWITCH_A3_FEATURE_SCHEDULE = 1u << 3,
    SWITCH_A3_FEATURE_COEX = 1u << 4,
    SWITCH_A3_FEATURE_CLOCK = 1u << 5,
    SWITCH_A3_FEATURE_REQUIRED = (1u << 6) - 1u
};

enum switch_a3_run_state {
    SWITCH_A3_STOPPED,
    SWITCH_A3_CONFIGURING,
    SWITCH_A3_RUNNING,
    SWITCH_A3_FAULTED
};

enum switch_a3_social_channel {
    SWITCH_A3_SOCIAL_6 = 1u << 0,
    SWITCH_A3_SOCIAL_44 = 1u << 1,
    SWITCH_A3_SOCIAL_149 = 1u << 2
};

enum switch_a3_config_flag {
    SWITCH_A3_CONFIG_RX = 1u << 0,
    SWITCH_A3_CONFIG_ACK = 1u << 1
};

struct switch_a3_control {
    uint32_t magic;
    uint16_t version, header_bytes;
    uint32_t total_bytes, request_id, session, clock_epoch, config_generation, operation;
};

struct switch_a3_reply {
    uint32_t magic;
    uint16_t version, header_bytes;
    uint32_t total_bytes, request_id, session, clock_epoch, applied_generation;
    int32_t status;
    uint32_t features, state, tx_credits, pending_rx;
    uint64_t tsf;
    uint32_t firmware_generation, reserved;
    uint32_t state_generation, state_revision;
    int32_t fault_status;
    uint16_t legal_social_channels, home_chanspec;
};

struct switch_a3_configuration {
    uint8_t address[6], bssid[6];
    uint16_t ethertype, chanspec;
    uint32_t flags;
};

struct switch_a3_ring_binding {
    uint16_t first_ring, ring_count, max_frame, reserved;
    uint32_t credits, reserved2;
};

struct switch_a3_slot {
    uint16_t chanspec, reserved;
    uint32_t dwell_us;
};

struct switch_a3_schedule {
    uint64_t anchor_tsf;
    uint32_t period_us;
    uint16_t infra_chanspec, slot_count;
    struct switch_a3_slot slots[SWITCH_A3_MAX_SLOTS];
};

enum switch_a3_tx_flag {
    SWITCH_A3_TX_NO_ACK = 1u << 0,
    SWITCH_A3_TX_ON_CHANNEL_ONLY = 1u << 1
};

struct switch_a3_tx_header {
    uint32_t magic;
    uint16_t version, header_bytes;
    uint32_t total_bytes, session, clock_epoch, schedule_generation, request_id;
    uint16_t frame_bytes, chanspec;
    uint32_t flags, rate;
    uint64_t earliest_tsf, expiry_tsf;
    uint32_t firmware_generation, reserved;
};

enum switch_a3_tx_result {
    SWITCH_A3_TX_SENT,
    SWITCH_A3_TX_EXPIRED,
    SWITCH_A3_TX_CANCELLED,
    SWITCH_A3_TX_FAILED
};

enum switch_a3_tx_status_flag {
    SWITCH_A3_TX_ACK_VALID = 1u << 0,
    SWITCH_A3_TX_ACKED = 1u << 1,
    SWITCH_A3_TX_TSF_VALID = 1u << 2
};

struct switch_a3_tx_status {
    uint32_t magic;
    uint16_t version, header_bytes;
    uint32_t total_bytes, session, clock_epoch, schedule_generation, request_id;
    int32_t status;
    uint32_t flags, credits;
    uint64_t tsf;
};

struct switch_a3_raw_rx {
    uint32_t magic;
    uint16_t version, header_bytes, frame_bytes, chanspec;
    int8_t rssi;
    uint8_t flags;
    uint16_t reserved;
    uint64_t tsf;
    uint32_t session, clock_epoch, config_generation, reserved2;
};

struct switch_a3_deadline_snapshot {
    uint64_t requested_tsf, actual_tsf;
    uint32_t session_generation, clock_generation, fired, armed;
    int32_t status;
    uint32_t reserved;
};

#ifdef __cplusplus
#define SWITCH_A3_WIRE_ASSERT static_assert
#else
#define SWITCH_A3_WIRE_ASSERT _Static_assert
#endif
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_control) == 32, "A3 control size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_reply) == 80, "A3 reply size");
SWITCH_A3_WIRE_ASSERT(offsetof(struct switch_a3_reply, state_generation) == 64, "A3 state identity");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_configuration) == 20, "A3 configuration size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_ring_binding) == 16, "A3 ring binding size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_slot) == 8, "A3 slot size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_schedule) == 144, "A3 schedule size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_tx_header) == 64, "A3 TX header size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_tx_status) == 48, "A3 TX completion size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_raw_rx) == 40, "A3 RX header size");
SWITCH_A3_WIRE_ASSERT(sizeof(struct switch_a3_deadline_snapshot) == 40, "A3 deadline snapshot size");
#undef SWITCH_A3_WIRE_ASSERT
#endif
