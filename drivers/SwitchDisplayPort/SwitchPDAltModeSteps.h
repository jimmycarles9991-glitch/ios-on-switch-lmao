// SPDX-License-Identifier: GPL-2.0-only
// The alternate-mode steps (SwitchPDAltMode.h has the messages): what each ALERT, reply and timer means in each step,
// and the command that follows. bm92txx.c bm92t_event_handler's states from PS_RDY_SENT on, with three differences:
// a stale DisplayPort configuration from an earlier boot is exited instead of resetting the controller (which would
// drop the contract and the charging with it), every wait for the partner has a timeout, and a contract alert while a
// command is out pauses the step until SwitchPD's own request is done, then sends it again.
#ifndef SWITCH_PD_ALT_MODE_STEPS_H
#define SWITCH_PD_ALT_MODE_STEPS_H

#include "SwitchPDAltMode.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline unsigned switch_pd_alt_last_command(uint16_t status1)
{
    return (status1 & SWITCH_PD_ALT_STATUS1_LASTCMD) >> 4;
}

static inline unsigned switch_pd_alt_data_role(uint16_t status1)
{
    return ((unsigned)status1 >> SWITCH_PD_ALT_STATUS1_DR_SHIFT) & 3u;
}

// The step's opening command: what a transition into it sends, and what a paused step sends again.
static inline struct switch_pd_alt_out switch_pd_alt_send(struct switch_pd_alt *alt)
{
    alt->accepted = false;
    switch (alt->step) {
    case SWITCH_PD_ALT_DRAIN_VDM:
        return switch_pd_alt_command(SWITCH_PD_ALT_CMD_ACCEPT_VDM, SWITCH_PD_ALT_DRAIN_MS);
    case SWITCH_PD_ALT_STALE_EXIT:
        return switch_pd_alt_command(SWITCH_PD_ALT_CMD_DP_EXIT, SWITCH_PD_ALT_REPLY_MS);
    case SWITCH_PD_ALT_DR_SWAP:
    case SWITCH_PD_ALT_DR_SWAP_TO_UFP:
        return switch_pd_alt_command(SWITCH_PD_ALT_CMD_DR_SWAP, SWITCH_PD_ALT_REPLY_MS);
    case SWITCH_PD_ALT_DISCOVER_ID: {
        struct switch_pd_alt_out out = switch_pd_alt_structured(0x01, 0, 0xFF00);
        out.timer_ms = alt->vdm_retried ? SWITCH_PD_ALT_DISCOVER_FINAL_MS : SWITCH_PD_ALT_DISCOVER_RETRY_MS;
        return out;
    }
    case SWITCH_PD_ALT_DISCOVER_SVID:
        return switch_pd_alt_structured(0x02, 0, 0xFF00);
    case SWITCH_PD_ALT_DISCOVER_MODE:
        return switch_pd_alt_structured(0x03, 0, 0xFF01);
    case SWITCH_PD_ALT_ENTER_DOCK:
        return switch_pd_alt_structured(0x04, 1, 0x057E);
    case SWITCH_PD_ALT_DP_DISCOVER:
        return switch_pd_alt_command(SWITCH_PD_ALT_CMD_DP_DISCOVER, SWITCH_PD_ALT_REPLY_MS);
    case SWITCH_PD_ALT_DP_CONFIGURE: {
        struct switch_pd_alt_out out = switch_pd_alt_vdm(alt->configure, 4);
        out.command = SWITCH_PD_ALT_CMD_DP_CONFIGURE;
        return out;
    }
    case SWITCH_PD_ALT_DOCK_DP_SLEEP:
    case SWITCH_PD_ALT_DOCK_DP_WAKE:
        return switch_pd_alt_dock_request(alt->step == SWITCH_PD_ALT_DOCK_DP_SLEEP, 0x1C);
    case SWITCH_PD_ALT_DOCK_QUERY:
        return switch_pd_alt_dock_request(0, 0x16);
    case SWITCH_PD_ALT_DOCK_LED:
        return switch_pd_alt_dock_led();
    case SWITCH_PD_ALT_HUB_ENABLE:
        return switch_pd_alt_dock_request(1, 0x20);
    case SWITCH_PD_ALT_HUB_POWER: {
        struct switch_pd_alt_out out = switch_pd_alt_none();
        out.timer_ms = SWITCH_PD_ALT_HUB_POWER_MS;
        return out;
    }
    case SWITCH_PD_ALT_HUB_RETRY: {
        struct switch_pd_alt_out out = switch_pd_alt_none();
        out.timer_ms = SWITCH_PD_ALT_HUB_RETRY_MS;
        return out;
    }
    case SWITCH_PD_ALT_IDLE:
    case SWITCH_PD_ALT_DONE:
    case SWITCH_PD_ALT_FAILED:
        break;
    }
    return switch_pd_alt_none();
}

static inline struct switch_pd_alt_out switch_pd_alt_go(struct switch_pd_alt *alt, enum switch_pd_alt_step step)
{
    alt->step = step;
    alt->vdm_retried = false;
    return switch_pd_alt_send(alt);
}

static inline struct switch_pd_alt_out switch_pd_alt_finish(struct switch_pd_alt *alt, enum switch_pd_alt_step step,
                                                            const char *line)
{
    alt->step = step;
    if (alt->dp_lanes == 0) {
        alt->lanes = SWITCH_PD_ALT_LANES_FREE;
    }
    struct switch_pd_alt_out out = switch_pd_alt_none();
    out.line = line;
    return out;
}

// After the DisplayPort steps (configured, refused or not offered): the dock goes on to its LED and hub.
static inline struct switch_pd_alt_out switch_pd_alt_after_dp(struct switch_pd_alt *alt, const char *line)
{
    if (alt->dp_lanes == 0) {
        alt->lanes = SWITCH_PD_ALT_LANES_FREE;
    }
    if (!alt->dock) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, line);
    }
    struct switch_pd_alt_out out = switch_pd_alt_go(alt, SWITCH_PD_ALT_DOCK_QUERY);
    out.line = line;
    return out;
}

static inline struct switch_pd_alt_out switch_pd_alt_dp_or_dock(struct switch_pd_alt *alt)
{
    if (alt->want_dp) {
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DP_DISCOVER);
    }
    return switch_pd_alt_after_dp(alt, NULL);
}

static inline struct switch_pd_alt_out switch_pd_alt_discover(struct switch_pd_alt *alt)
{
    alt->as_ufp = false;
    if (alt->dp_lanes == 0) {
        alt->lanes = alt->want_dp ? SWITCH_PD_ALT_LANES_PENDING : SWITCH_PD_ALT_LANES_FREE;
    }
    return switch_pd_alt_go(alt, SWITCH_PD_ALT_DISCOVER_ID);
}

// The data role decides the first step: DFP discovers, UFP swaps only for the dock's power signature (a Mac that
// accepted DR_Swap would stop being the Switch's USB host; hubs and monitors that want us as DFP send DR_Swap, which
// SwitchPDSettings accepts and the partner-swap branch below picks up).
static inline struct switch_pd_alt_out switch_pd_alt_begin(struct switch_pd_alt *alt, uint16_t status1)
{
    const unsigned role = switch_pd_alt_data_role(status1);
    alt->as_ufp = role == SWITCH_PD_ALT_DATA_ROLE_UFP;
    if (role == SWITCH_PD_ALT_DATA_ROLE_DFP) {
        return switch_pd_alt_discover(alt);
    }
    if (role == SWITCH_PD_ALT_DATA_ROLE_UFP && alt->dock_power) {
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DR_SWAP);
    }
    if (role == SWITCH_PD_ALT_DATA_ROLE_UFP) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, NULL);
    }
    return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "no data role to swap from");
}

// Just before DR_SWAP goes to COMMAND, with STATUS1 read again: avoid swapping back if the partner already swapped us.
static inline struct switch_pd_alt_out switch_pd_alt_before_swap(struct switch_pd_alt *alt,
                                                                 struct switch_pd_alt_out out, uint16_t status1)
{
    if (out.act != SWITCH_PD_ALT_COMMAND || out.command != SWITCH_PD_ALT_CMD_DR_SWAP) {
        return out;
    }
    const unsigned role = switch_pd_alt_data_role(status1);
    if (alt->step == SWITCH_PD_ALT_DR_SWAP && role == SWITCH_PD_ALT_DATA_ROLE_DFP) {
        struct switch_pd_alt_out discover = switch_pd_alt_discover(alt);
        discover.line = "the partner swapped the data roles first: Discover Identity as DFP";
        return discover;
    }
    if (alt->step == SWITCH_PD_ALT_DR_SWAP_TO_UFP && role == SWITCH_PD_ALT_DATA_ROLE_UFP) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "yielded data role to USB host");
    }
    return out;
}

// The partner's answer to Discover Identity as DFP: the dock goes into its own mode, a USB host gets our data role
// (or triggers SwitchPD's yield if we are still sourcing), anyone else goes on to DisplayPort.
static inline struct switch_pd_alt_out switch_pd_alt_partner_identified(struct switch_pd_alt *alt, const uint8_t *vdm,
                                                                        size_t len)
{
    if (!switch_pd_alt_identity_response(vdm, len)) {
        if (!alt->vdm_retried) {
            alt->vdm_retried = true;
            return switch_pd_alt_send(alt);
        }
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no alternate modes: Discover Identity not acked");
    }
    if (!switch_pd_alt_acked(vdm, 0x01) || len < 17) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no alternate modes: Discover Identity not acked");
    }
    const uint32_t id_header = (uint32_t)vdm[5] | ((uint32_t)vdm[6] << 8U) | ((uint32_t)vdm[7] << 16U) |
                               ((uint32_t)vdm[8] << 24U);
    const bool host_capable = (id_header & (1u << 31)) != 0;
    const unsigned ufp_type = (id_header >> 27U) & 7U;
    const unsigned dfp_type = (id_header >> 23U) & 7U;
    const unsigned vid = (unsigned)(id_header & 0xFFFFu);
    const unsigned pid = (unsigned)vdm[15] | (unsigned)vdm[16] << 8U;
    alt->dock = ufp_type == 5 && vid == 0x057E && pid == 0x2003;
    if (alt->dock) {
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_ENTER_DOCK);
    }
    if (host_capable && ufp_type != 5 && (dfp_type == 2 || vid == 0x05AC)) {
        alt->partner_is_usb_host = true;
        if (!alt->is_source) {
            return switch_pd_alt_go(alt, SWITCH_PD_ALT_DR_SWAP_TO_UFP);
        }
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "USB host partner while sourcing");
    }
    if (!alt->want_dp) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, NULL);
    }
    return switch_pd_alt_go(alt, SWITCH_PD_ALT_DISCOVER_SVID);
}

struct switch_pd_alt_in {
    uint16_t alert, status1, dp_status;
    bool port_session;  // explicit SOP contract in the current attachment
    bool at_rest;       // SwitchPD's own USB-PD request is not using COMMAND (published, settling or given up)
    bool dock_power;    // the source's PDOs carry the dock's signature
    bool is_source;     // we are currently the power source
};

static inline struct switch_pd_alt_out switch_pd_alt_start(struct switch_pd_alt *alt, const struct switch_pd_alt_in *in)
{
    alt->dock_power = in->dock_power;
    alt->lanes = alt->want_dp ? SWITCH_PD_ALT_LANES_PENDING : SWITCH_PD_ALT_LANES_FREE;
    if ((in->dp_status & SWITCH_PD_ALT_DP_STATUS_DP_EN) != 0) {
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_STALE_EXIT);
    }
    return switch_pd_alt_begin(alt, in->status1);
}

static inline struct switch_pd_alt_out switch_pd_alt_begin_drain(struct switch_pd_alt *alt,
                                                                 enum switch_pd_alt_step resume, uint16_t status1,
                                                                 uint16_t dp_status, bool dock_power)
{
    alt->drain_resume = resume;
    alt->drain_status1 = status1;
    alt->drain_dp_status = dp_status;
    alt->drain_dock_power = dock_power;
    if (resume == SWITCH_PD_ALT_IDLE) {
        alt->dock_power = dock_power;
    }
    alt->vdm_pending = false;
    ++alt->drain_count;
    alt->step = SWITCH_PD_ALT_DRAIN_VDM;
    alt->drain_retried = false;
    alt->paused = false;
    alt->accepted = false;
    return switch_pd_alt_command(SWITCH_PD_ALT_CMD_ACCEPT_VDM, SWITCH_PD_ALT_DRAIN_MS);
}

static inline struct switch_pd_alt_out switch_pd_alt_finish_drain(struct switch_pd_alt *alt)
{
    if (alt->vdm_pending && alt->drain_count < SWITCH_PD_ALT_DRAIN_MAX) {
        alt->vdm_pending = false;
        ++alt->drain_count;
        alt->drain_retried = false;
        alt->paused = false;
        alt->accepted = false;
        return switch_pd_alt_command(SWITCH_PD_ALT_CMD_ACCEPT_VDM, SWITCH_PD_ALT_DRAIN_MS);
    }
    const enum switch_pd_alt_step resume = alt->drain_resume;
    alt->drain_resume = SWITCH_PD_ALT_IDLE;
    alt->paused = false;
    alt->accepted = false;
    if (resume == SWITCH_PD_ALT_IDLE) {
        alt->step = SWITCH_PD_ALT_IDLE;
        const struct switch_pd_alt_in saved = {
            0, alt->drain_status1, alt->drain_dp_status, true, true, alt->drain_dock_power, alt->is_source};
        return switch_pd_alt_start(alt, &saved);
    }
    if (resume == SWITCH_PD_ALT_DONE) {
        alt->step = SWITCH_PD_ALT_DONE;
        if (alt->as_ufp && !alt->partner_is_usb_host &&
            switch_pd_alt_data_role(alt->drain_status1) == SWITCH_PD_ALT_DATA_ROLE_DFP) {
            struct switch_pd_alt_out out = switch_pd_alt_discover(alt);
            out.line = "the partner swapped the data roles: Discover Identity as DFP";
            return out;
        }
        return switch_pd_alt_none();
    }
    alt->step = resume;
    return switch_pd_alt_send(alt);
}

// INCOMING_VDM after a step's reply was accepted (or the DisplayPort capabilities after DP discovery).
static inline struct switch_pd_alt_out switch_pd_alt_reply(struct switch_pd_alt *alt, const uint8_t *vdm, size_t len)
{
    if (alt->step == SWITCH_PD_ALT_DRAIN_VDM) {
        return switch_pd_alt_finish_drain(alt);
    }
    if (len < 5 || vdm[0] < 4 || vdm[0] > SWITCH_PD_ALT_INCOMING_MAX - 1u || (vdm[0] & 3u) ||
        vdm[0] >= len) {
        if (alt->step == SWITCH_PD_ALT_DOCK_DP_SLEEP)
            return switch_pd_alt_go(alt, SWITCH_PD_ALT_DOCK_DP_WAKE);
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "invalid INCOMING_VDM count");
    }
    len = 1u + vdm[0];
    switch (alt->step) {
    case SWITCH_PD_ALT_DISCOVER_ID:
        return switch_pd_alt_partner_identified(alt, vdm, len);
    case SWITCH_PD_ALT_DISCOVER_SVID:
        if (!switch_pd_alt_acked(vdm, 0x02) || (vdm[2] & 0x87u) != 0x80 || vdm[3] != 0 || vdm[4] != 0xFF ||
            !switch_pd_alt_has_svid(vdm, len, 0xFF01)) {
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no DisplayPort: SVID FF01 not offered");
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DISCOVER_MODE);
    case SWITCH_PD_ALT_DISCOVER_MODE:
        if (vdm[0] <= 4 || !switch_pd_alt_acked(vdm, 0x03) || (vdm[2] & 0x87u) != 0x80 || vdm[3] != 0x01 || vdm[4] != 0xFF ||
            len < 6 || (vdm[5] & 0x01u) == 0 || (vdm[5] & 0x04u) == 0) {
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no DisplayPort: no UFP_D mode");
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DP_DISCOVER);
    case SWITCH_PD_ALT_ENTER_DOCK:
        // A dock already in Nintendo mode may NAK a second Enter Mode.
        if ((vdm[1] != 0x44 && vdm[1] != 0x84) || (vdm[2] & 0x87u) != 0x81 || vdm[3] != 0x7E || vdm[4] != 0x05)
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "unexpected Enter Mode response");
        return switch_pd_alt_dp_or_dock(alt);
    case SWITCH_PD_ALT_DP_DISCOVER: {
        const struct switch_pd_alt_config config = switch_pd_alt_choose_pins(vdm, len, alt->dock);
        if (config.pin == 0) {
            return switch_pd_alt_after_dp(alt, "no DisplayPort: neither pin assignment C nor D offered");
        }
        alt->offered_pins = config.offered;
        alt->pin = config.pin;
        for (unsigned i = 0; i < 4; ++i) {
            alt->configure[i] = config.vdo[i];
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DP_CONFIGURE);
    }
    case SWITCH_PD_ALT_DOCK_QUERY:
        if (!switch_pd_alt_dock_answer(vdm, len, 0x17) || len < 13)
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "unrecognized dock state reply");
        alt->old_dock_firmware = (vdm[11] & 0x02u) != 0;
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DOCK_LED);
    case SWITCH_PD_ALT_DOCK_DP_SLEEP:
        // Restore the signal even when the dock failed to acknowledge disabling it.
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DOCK_DP_WAKE);
    case SWITCH_PD_ALT_DOCK_DP_WAKE:
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE,
                                    switch_pd_alt_dock_answer(vdm, len, 0x1D) && vdm[5] == 0 ? NULL :
                                    "the dock did not acknowledge restoring the DP signal");
    case SWITCH_PD_ALT_DOCK_LED:
        if (!switch_pd_alt_dock_answer(vdm, len, 0x02))
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "unrecognized dock LED reply");
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_HUB_POWER);
    case SWITCH_PD_ALT_HUB_ENABLE: {
        const bool answer = switch_pd_alt_dock_answer(vdm, len, 0x21);
        if (answer && vdm[5] == 1 && alt->hub_tries < SWITCH_PD_ALT_HUB_RETRIES) {
            ++alt->hub_tries;
            return switch_pd_alt_go(alt, SWITCH_PD_ALT_HUB_RETRY);
        }
        alt->hub_enabled = answer && vdm[5] == 0;
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE,
                                    alt->hub_enabled ? NULL : "the dock did not enable its USB hub");
    }
    default:
        break;
    }
    return switch_pd_alt_none();
}

static inline bool switch_pd_alt_waiting_on_chip(const struct switch_pd_alt *alt)
{
    return switch_pd_alt_active(alt) && alt->step != SWITCH_PD_ALT_HUB_POWER && alt->step != SWITCH_PD_ALT_HUB_RETRY;
}

static inline struct switch_pd_alt_out switch_pd_alt_reset(struct switch_pd_alt *alt)
{
    const bool was = alt->step != SWITCH_PD_ALT_IDLE;
    switch_pd_alt_init(alt, alt->want_dp);
    struct switch_pd_alt_out out = switch_pd_alt_none();
    out.line = was ? "ended: the partner left" : NULL;
    return out;
}

// A command's completion in the step that sent it (not a VDM reply).
static inline struct switch_pd_alt_out switch_pd_alt_done(struct switch_pd_alt *alt, uint16_t status1)
{
    const unsigned last = switch_pd_alt_last_command(status1);
    switch (alt->step) {
    case SWITCH_PD_ALT_STALE_EXIT:
        return switch_pd_alt_begin(alt, status1);
    case SWITCH_PD_ALT_DR_SWAP:
        if (last != 0 || switch_pd_alt_data_role(status1) != SWITCH_PD_ALT_DATA_ROLE_DFP) {
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "the partner refused the data role swap");
        }
        return switch_pd_alt_discover(alt);
    case SWITCH_PD_ALT_DR_SWAP_TO_UFP:
        return switch_pd_alt_finish(
            alt, SWITCH_PD_ALT_DONE,
            last == 0 && switch_pd_alt_data_role(status1) == SWITCH_PD_ALT_DATA_ROLE_UFP
                ? "yielded data role to USB host"
                : "USB host partner did not accept data role swap to UFP");
    case SWITCH_PD_ALT_DP_DISCOVER: {
        if (last != 0) {
            return switch_pd_alt_after_dp(alt, "no DisplayPort: discovery failed");
        }
        struct switch_pd_alt_out out = switch_pd_alt_none();
        out.act = SWITCH_PD_ALT_READ;
        return out;
    }
    case SWITCH_PD_ALT_DP_CONFIGURE:
        if (last != 0) {
            if (alt->pin == SWITCH_PD_ALT_PIN_D && (alt->offered_pins & SWITCH_PD_ALT_PIN_C) != 0) {
                alt->pin = SWITCH_PD_ALT_PIN_C;
                alt->configure[alt->dock ? 2 : 1] = SWITCH_PD_ALT_PIN_C;
                return switch_pd_alt_go(alt, SWITCH_PD_ALT_DP_CONFIGURE);
            }
            return switch_pd_alt_after_dp(alt, "no DisplayPort: Configure refused");
        }
        alt->dp_lanes = alt->pin == SWITCH_PD_ALT_PIN_D ? 2 : 4;
        alt->lanes = alt->pin == SWITCH_PD_ALT_PIN_D ? SWITCH_PD_ALT_LANES_FREE : SWITCH_PD_ALT_LANES_TAKEN;
        return switch_pd_alt_after_dp(alt, NULL);
    default:
        break;
    }
    return switch_pd_alt_none();
}

static inline bool switch_pd_alt_vdm_step(enum switch_pd_alt_step step)
{
    switch (step) {
    case SWITCH_PD_ALT_DISCOVER_ID:
    case SWITCH_PD_ALT_DISCOVER_SVID:
    case SWITCH_PD_ALT_DISCOVER_MODE:
    case SWITCH_PD_ALT_ENTER_DOCK:
    case SWITCH_PD_ALT_DOCK_QUERY:
    case SWITCH_PD_ALT_DOCK_LED:
    case SWITCH_PD_ALT_HUB_ENABLE:
    case SWITCH_PD_ALT_DOCK_DP_SLEEP:
    case SWITCH_PD_ALT_DOCK_DP_WAKE:
        return true;
    default:
        return false;
    }
}

static inline struct switch_pd_alt_out switch_pd_alt_vdm_failed(struct switch_pd_alt *alt, const char *why)
{
    if (alt->step == SWITCH_PD_ALT_DOCK_DP_SLEEP)
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DOCK_DP_WAKE);
    if (alt->step == SWITCH_PD_ALT_DOCK_DP_WAKE)
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "VDM command failed restoring the dock DP signal");
    if (alt->step == SWITCH_PD_ALT_DOCK_QUERY || alt->step == SWITCH_PD_ALT_DOCK_LED ||
        alt->step == SWITCH_PD_ALT_HUB_ENABLE) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "dock VDM command failed (its hub stays off)");
    }
    return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, why);
}

// Every ALERT (and SwitchPD's first look at the port), on SwitchPD's work loop after its own handling.
static inline struct switch_pd_alt_out switch_pd_alt_event(struct switch_pd_alt *alt, const struct switch_pd_alt_in *in)
{
    const bool plugged = (in->status1 & SWITCH_PD_ALT_STATUS1_INSERT) != 0;
    if (!plugged || (alt->step != SWITCH_PD_ALT_IDLE && (in->alert & SWITCH_PD_ALT_ALERT_PLUGPULL) != 0)) {
        return switch_pd_alt_reset(alt);
    }
    if (!in->port_session) {
        const bool pending = alt->vdm_pending || (in->alert & SWITCH_PD_ALT_ALERT_VDM_RECEIVED) != 0;
        struct switch_pd_alt_out out =
            alt->step != SWITCH_PD_ALT_IDLE ? switch_pd_alt_reset(alt) : switch_pd_alt_none();
        alt->vdm_pending = pending;
        return out;
    }
    alt->is_source = in->is_source;
    if ((in->alert & SWITCH_PD_ALT_ALERT_VDM_RECEIVED) != 0 &&
        !(switch_pd_alt_vdm_step(alt->step) && !alt->accepted)) {
        alt->vdm_pending = true;
    }
    if ((in->alert & (SWITCH_PD_ALT_ALERT_CONTRACT | SWITCH_PD_ALT_ALERT_PR_SWAP)) != 0 &&
        switch_pd_alt_waiting_on_chip(alt)) {
        alt->paused = true;
    }
    if (!in->at_rest) {
        return switch_pd_alt_none();
    }
    if (alt->vdm_pending && alt->drain_count < SWITCH_PD_ALT_DRAIN_MAX && alt->step != SWITCH_PD_ALT_DRAIN_VDM &&
        (alt->step == SWITCH_PD_ALT_IDLE || alt->step == SWITCH_PD_ALT_DONE || alt->paused)) {
        if ((in->status1 & SWITCH_PD_ALT_STATUS1_CMD_BUSY) != 0) {
            return switch_pd_alt_none();
        }
        return switch_pd_alt_begin_drain(alt, alt->step, in->status1, in->dp_status, in->dock_power);
    }
    if (alt->step == SWITCH_PD_ALT_IDLE) {
        if ((in->status1 & SWITCH_PD_ALT_STATUS1_CMD_BUSY) != 0) {
            return switch_pd_alt_none();
        }
        return switch_pd_alt_start(alt, in);
    }
    if (alt->as_ufp && !alt->partner_is_usb_host && alt->step != SWITCH_PD_ALT_DR_SWAP &&
        alt->step != SWITCH_PD_ALT_DRAIN_VDM &&
        switch_pd_alt_data_role(in->status1) == SWITCH_PD_ALT_DATA_ROLE_DFP &&
        (in->status1 & SWITCH_PD_ALT_STATUS1_CMD_BUSY) == 0) {
        alt->paused = false;
        struct switch_pd_alt_out out = switch_pd_alt_discover(alt);
        out.line = "the partner swapped the data roles: Discover Identity as DFP";
        return out;
    }
    if (alt->paused) {
        if ((in->status1 & SWITCH_PD_ALT_STATUS1_CMD_BUSY) != 0) {
            return switch_pd_alt_none();
        }
        alt->paused = false;
        return switch_pd_alt_send(alt);
    }
    if (alt->step == SWITCH_PD_ALT_DRAIN_VDM) {
        alt->drain_status1 = in->status1;
        if ((in->alert & SWITCH_PD_ALT_ALERT_CMD_DONE) != 0) {
            const unsigned last = switch_pd_alt_last_command(in->status1);
            if (last == 0u) {
                struct switch_pd_alt_out out = switch_pd_alt_none();
                out.act = SWITCH_PD_ALT_READ;
                return out;
            }
            if (last == 2u && !alt->drain_retried) {
                alt->drain_retried = true;
                alt->paused = true;
                struct switch_pd_alt_out out = switch_pd_alt_none();
                out.timer_ms = 100u;
                return out;
            }
            return switch_pd_alt_finish_drain(alt);
        }
        return switch_pd_alt_none();
    }
    if (switch_pd_alt_vdm_step(alt->step)) {
        if (!alt->accepted && (in->alert & SWITCH_PD_ALT_ALERT_VDM_RECEIVED) != 0) {
            alt->accepted = true;
            return switch_pd_alt_command(SWITCH_PD_ALT_CMD_ACCEPT_VDM, SWITCH_PD_ALT_REPLY_MS);
        }
        if (!alt->accepted && (in->alert & SWITCH_PD_ALT_ALERT_CMD_DONE) != 0) {
            const unsigned last = switch_pd_alt_last_command(in->status1);
            if (last == 2u && !alt->vdm_retried) {
                alt->vdm_retried = true;
                alt->paused = true;
                struct switch_pd_alt_out out = switch_pd_alt_none();
                out.timer_ms = 100u;
                return out;
            }
            if (last != 0u) {
                return switch_pd_alt_vdm_failed(alt, "VDM command failed");
            }
        }
        if (alt->accepted && (in->alert & SWITCH_PD_ALT_ALERT_CMD_DONE) != 0) {
            if (switch_pd_alt_last_command(in->status1) != 0u) {
                return switch_pd_alt_vdm_failed(alt, "the partner did not accept the VDM");
            }
            struct switch_pd_alt_out out = switch_pd_alt_none();
            out.act = SWITCH_PD_ALT_READ;
            return out;
        }
        return switch_pd_alt_none();
    }
    if ((in->alert & SWITCH_PD_ALT_ALERT_CMD_DONE) != 0) {
        return switch_pd_alt_done(alt, in->status1);
    }
    return switch_pd_alt_none();
}

// The step timer: a delay ran out (the hub's power-up, a busy hub's retry) or a reply did not come.
static inline struct switch_pd_alt_out switch_pd_alt_timer(struct switch_pd_alt *alt, bool at_rest)
{
    if (alt->step == SWITCH_PD_ALT_HUB_POWER || alt->step == SWITCH_PD_ALT_HUB_RETRY) {
        alt->step = SWITCH_PD_ALT_HUB_ENABLE;
        if (!at_rest) {
            alt->paused = true;
            return switch_pd_alt_none();
        }
        return switch_pd_alt_send(alt);
    }
    if (!switch_pd_alt_active(alt)) {
        return switch_pd_alt_none();
    }
    if (alt->paused) {
        if (!at_rest)
            return switch_pd_alt_none();
        alt->paused = false;
        return switch_pd_alt_send(alt);
    }
    if (alt->step == SWITCH_PD_ALT_DRAIN_VDM) {
        if (!at_rest) {
            const enum switch_pd_alt_step resume = alt->drain_resume;
            alt->drain_resume = SWITCH_PD_ALT_IDLE;
            alt->step = resume;
            alt->paused = resume != SWITCH_PD_ALT_IDLE && resume != SWITCH_PD_ALT_DONE;
            return switch_pd_alt_none();
        }
        return switch_pd_alt_finish_drain(alt);
    }
    if (alt->step == SWITCH_PD_ALT_DISCOVER_ID && !alt->accepted && !alt->vdm_retried) {
        alt->vdm_retried = true;
        if (!at_rest) {
            alt->paused = true;
            return switch_pd_alt_none();
        }
        if (alt->vdm_pending && alt->drain_count < SWITCH_PD_ALT_DRAIN_MAX) {
            return switch_pd_alt_begin_drain(alt, SWITCH_PD_ALT_DISCOVER_ID, alt->drain_status1,
                                             alt->drain_dp_status, alt->drain_dock_power);
        }
        return switch_pd_alt_send(alt);
    }
    if (alt->step == SWITCH_PD_ALT_DP_DISCOVER || alt->step == SWITCH_PD_ALT_DP_CONFIGURE) {
        return switch_pd_alt_after_dp(alt, "no DisplayPort: no answer within 1.5 s");
    }
    if (alt->step == SWITCH_PD_ALT_DOCK_DP_SLEEP)
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DOCK_DP_WAKE);
    if (alt->step == SWITCH_PD_ALT_DOCK_DP_WAKE)
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no reply to restoring the dock DP signal");
    if (alt->step == SWITCH_PD_ALT_DOCK_QUERY || alt->step == SWITCH_PD_ALT_DOCK_LED ||
        alt->step == SWITCH_PD_ALT_HUB_ENABLE) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE,
                                    "the dock did not answer within 1.5 s (its hub stays off)");
    }
    return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "the partner did not answer within 1.5 s");
}

static inline const char *switch_pd_alt_step_name(enum switch_pd_alt_step step)
{
    static const char *const names[] = {"idle",
                                        "drain incoming VDM",
                                        "stale DisplayPort exit",
                                        "data role swap",
                                        "data role swap to UFP",
                                        "Discover Identity",
                                        "Discover SVIDs",
                                        "Discover Modes",
                                        "dock mode entry",
                                        "DisplayPort discovery",
                                        "DisplayPort Configure",
                                        "dock state",
                                        "dock LED",
                                        "dock hub power-up",
                                        "dock hub enable",
                                        "dock hub retry",
                                        "dock DP signal disable",
                                        "dock DP signal restore",
                                        "done",
                                        "failed"};
    return (unsigned)step < sizeof(names) / sizeof(names[0]) ? names[step] : "?";
}

#ifdef __cplusplus
}
#endif

#endif
