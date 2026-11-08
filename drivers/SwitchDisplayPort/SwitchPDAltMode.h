// SPDX-License-Identifier: GPL-2.0-only
// The BM92T36's alternate-mode sequence, in plain C for the host tests: DR swap to DFP, Discover Identity, the Nintendo
// dock's own mode, DisplayPort discovery and Configure, and the dock's LED and USB hub. SwitchPDAltMode.cpp performs
// what each step returns on SwitchPD's work loop.
//
// Order, messages and reply checks: CTCaer switch-l4t-kernel-4.9 drivers/misc/bm92txx.c (Copyright (c) 2015-2017,
// NVIDIA CORPORATION; 2020-2022 CTCaer), bm92t_event_handler from PS_RDY_SENT on and bm92t_handle_dp_config_and_hpd.
// The controller runs DisplayPort itself once configured: it answers Attention VDMs by driving the HPD line into
// DPAUX1 (PCC1), so nothing here sees HPD.
#ifndef SWITCH_PD_ALT_MODE_H
#define SWITCH_PD_ALT_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_PD_ALT_ALERT_CMD_DONE     (1u << 2)
#define SWITCH_PD_ALT_ALERT_PLUGPULL     (1u << 3)
#define SWITCH_PD_ALT_ALERT_VDM_RECEIVED (1u << 11)
#define SWITCH_PD_ALT_ALERT_CONTRACT     (1u << 12)
#define SWITCH_PD_ALT_STATUS1_LASTCMD    (7u << 4)
#define SWITCH_PD_ALT_STATUS1_INSERT     (1u << 7)
#define SWITCH_PD_ALT_STATUS1_DR_SHIFT   8u
#define SWITCH_PD_ALT_STATUS1_CSIDE      (1u << 11)
#define SWITCH_PD_ALT_STATUS1_CMD_BUSY   (1u << 13)
#define SWITCH_PD_ALT_DP_STATUS_DP_EN    (1u << 15)
#define SWITCH_PD_ALT_DATA_ROLE_UFP      1u
#define SWITCH_PD_ALT_DATA_ROLE_DFP      2u

#define SWITCH_PD_ALT_CMD_SEND_VDM     0x1111u
#define SWITCH_PD_ALT_CMD_ACCEPT_VDM   0x1616u
#define SWITCH_PD_ALT_CMD_DR_SWAP      0x1818u
#define SWITCH_PD_ALT_CMD_DP_DISCOVER  0x3131u
#define SWITCH_PD_ALT_CMD_DP_CONFIGURE 0x3636u
#define SWITCH_PD_ALT_CMD_DP_EXIT      0x3B3Bu

#define SWITCH_PD_ALT_VDM_MAX      13u  // count byte + 12 bytes: the longest message sent (the dock's LED)
#define SWITCH_PD_ALT_INCOMING_MAX 29u  // count byte + header + six VDOs
#define SWITCH_PD_ALT_REPLY_MS     1500u
#define SWITCH_PD_ALT_HUB_POWER_MS 500u
#define SWITCH_PD_ALT_HUB_RETRY_MS 250u
#define SWITCH_PD_ALT_HUB_RETRIES  10u

#define SWITCH_PD_ALT_PIN_C (1u << 2)
#define SWITCH_PD_ALT_PIN_D (1u << 3)

enum switch_pd_alt_step {
    SWITCH_PD_ALT_IDLE,
    SWITCH_PD_ALT_STALE_EXIT,  // an earlier boot left DisplayPort configured
    SWITCH_PD_ALT_DR_SWAP,
    SWITCH_PD_ALT_DISCOVER_ID,
    SWITCH_PD_ALT_DISCOVER_SVID,
    SWITCH_PD_ALT_DISCOVER_MODE,
    SWITCH_PD_ALT_ENTER_DOCK,
    SWITCH_PD_ALT_DP_DISCOVER,
    SWITCH_PD_ALT_DP_CONFIGURE,
    SWITCH_PD_ALT_DOCK_QUERY,
    SWITCH_PD_ALT_DOCK_LED,
    SWITCH_PD_ALT_HUB_POWER,  // the dock's hub powers up after its LED reply
    SWITCH_PD_ALT_HUB_ENABLE,
    SWITCH_PD_ALT_HUB_RETRY,
    SWITCH_PD_ALT_DONE,
    SWITCH_PD_ALT_FAILED,
};

enum switch_pd_alt_lanes {
    SWITCH_PD_ALT_LANES_PENDING = 0,
    SWITCH_PD_ALT_LANES_FREE = 1,
    SWITCH_PD_ALT_LANES_TAKEN = 2,
};

enum switch_pd_alt_act {
    SWITCH_PD_ALT_NONE,
    SWITCH_PD_ALT_COMMAND,  // write `message` to OUTGOING_VDM when message_len, then `command` to COMMAND
    SWITCH_PD_ALT_READ,     // read INCOMING_VDM and hand it to switch_pd_alt_reply
};

struct switch_pd_alt_out {
    enum switch_pd_alt_act act;
    uint16_t command;
    uint8_t message_len;
    uint8_t message[SWITCH_PD_ALT_VDM_MAX];
    uint32_t timer_ms;  // 0: cancel the step timer
    const char *line;   // one log line for this step, or NULL
};

struct switch_pd_alt {
    enum switch_pd_alt_step step;
    bool want_dp;
    bool dock_power;  // the source's PDOs are the dock's signature (SwitchChargePolicy.h): the only partner swapped to
    bool accepted;    // the step's VDM was answered and ACCEPT went out: its CMD_DONE means the reply is readable
    bool paused;      // a contract alert while a command was out: the step goes again once SwitchPD is at rest
    bool dock, old_dock_firmware, hub_enabled;
    uint8_t pin, dp_lanes, hub_tries;
    uint8_t configure[4];
    enum switch_pd_alt_lanes lanes;
};

static inline void switch_pd_alt_init(struct switch_pd_alt *alt, bool want_dp)
{
    const struct switch_pd_alt fresh = {
        SWITCH_PD_ALT_IDLE,      want_dp, false, false, false, false, false, false, 0, 0, 0, {0, 0, 0, 0},
        SWITCH_PD_ALT_LANES_FREE};
    *alt = fresh;
}

static inline bool switch_pd_alt_active(const struct switch_pd_alt *alt)
{
    return alt->step != SWITCH_PD_ALT_IDLE && alt->step != SWITCH_PD_ALT_DONE && alt->step != SWITCH_PD_ALT_FAILED;
}

static inline struct switch_pd_alt_out switch_pd_alt_none()
{
    struct switch_pd_alt_out out = {SWITCH_PD_ALT_NONE, 0, 0, {0}, 0, NULL};
    return out;
}

static inline struct switch_pd_alt_out switch_pd_alt_command(uint16_t command, uint32_t timer_ms)
{
    struct switch_pd_alt_out out = switch_pd_alt_none();
    out.act = SWITCH_PD_ALT_COMMAND;
    out.command = command;
    out.timer_ms = timer_ms;
    return out;
}

static inline struct switch_pd_alt_out switch_pd_alt_vdm(const uint8_t *bytes, uint8_t len)
{
    struct switch_pd_alt_out out = switch_pd_alt_command(SWITCH_PD_ALT_CMD_SEND_VDM, SWITCH_PD_ALT_REPLY_MS);
    out.message[0] = len;
    for (uint8_t i = 0; i < len; ++i) {
        out.message[1 + i] = bytes[i];
    }
    out.message_len = (uint8_t)(len + 1u);
    return out;
}

// Structured VDM headers: command, structured (| object position), SVID little-endian.
static inline struct switch_pd_alt_out switch_pd_alt_structured(uint8_t command, uint8_t position, uint16_t svid)
{
    const uint8_t bytes[4] = {command, (uint8_t)(0x80u | position), (uint8_t)svid, (uint8_t)(svid >> 8)};
    return switch_pd_alt_vdm(bytes, 4);
}

// The dock's unstructured requests (SVID 057E): read or write, from the host, a command.
static inline struct switch_pd_alt_out switch_pd_alt_dock_request(uint8_t write, uint8_t command)
{
    const uint8_t bytes[8] = {0x00, 0x00, 0x7E, 0x05, write, 1, command, 0x00};
    return switch_pd_alt_vdm(bytes, 8);
}

// The LED as bm92t_usbhub_led_cfg(info, 128, 0, 0, 64) sets it at dock entry: fade 64, off 0, on 0, duty 128.
static inline struct switch_pd_alt_out switch_pd_alt_dock_led()
{
    const uint8_t bytes[12] = {0x00, 0x00, 0x7E, 0x05, 1, 1, 0x01, 0x00, 64, 0, 0, 128};
    return switch_pd_alt_vdm(bytes, 12);
}

struct switch_pd_alt_config {
    uint8_t pin;    // SWITCH_PD_ALT_PIN_C or _D, 0 when nothing usable is offered
    uint8_t lanes;  // 4 for C, 2 for D
    uint8_t vdo[4];
};

// DisplayPort Configure from the partner's capabilities VDO (`caps` = INCOMING_VDM after DP discovery: count, then the
// VDO's bytes). Pin assignment D (two lanes, the other SuperSpeed pair stays USB 3) whenever it is offered, C (four
// lanes) otherwise. The UFP_D assignments are bits 23:16 for a receptacle and 15:8 for a plug. The dock reports a plug
// but answers as a receptacle and ignores where the Configure's assignment is: bm92txx puts the dock's in bits 23:16
// and everyone else's in the standard 15:8, which is what the dock is known to work with.
static inline struct switch_pd_alt_config switch_pd_alt_choose_pins(const uint8_t *caps, size_t len, bool dock)
{
    struct switch_pd_alt_config config = {0, 0, {0x06, 0, 0, 0}};  // UFP_U as UFP_D, DP v1.3 signalling
    if (len < 5 || caps[0] != 4) {
        return config;
    }
    const bool receptacle = dock || (caps[1] & 0x40u) != 0;
    const uint8_t offered = receptacle ? caps[3] : caps[2];
    if ((offered & SWITCH_PD_ALT_PIN_D) != 0) {
        config.pin = SWITCH_PD_ALT_PIN_D;
        config.lanes = 2;
    } else if ((offered & SWITCH_PD_ALT_PIN_C) != 0) {
        config.pin = SWITCH_PD_ALT_PIN_C;
        config.lanes = 4;
    } else {
        return config;
    }
    config.vdo[dock ? 2 : 1] = config.pin;
    return config;
}

static inline bool switch_pd_alt_has_svid(const uint8_t *vdm, size_t len, uint16_t svid)
{
    const size_t count = vdm[0] < len ? vdm[0] : len - 1;
    for (size_t i = 5; i + 1 <= count; i += 2) {
        if ((uint16_t)((unsigned)vdm[i] | (unsigned)vdm[i + 1] << 8U) == svid) {
            return true;
        }
    }
    return false;
}

static inline bool switch_pd_alt_acked(const uint8_t *vdm, uint8_t command)
{
    return vdm[1] == (uint8_t)(0x40u | command);
}

#ifdef __cplusplus
}
#endif

#endif
