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
    case SWITCH_PD_ALT_STALE_EXIT:
        return switch_pd_alt_command(SWITCH_PD_ALT_CMD_DP_EXIT, SWITCH_PD_ALT_REPLY_MS);
    case SWITCH_PD_ALT_DR_SWAP:
        return switch_pd_alt_command(SWITCH_PD_ALT_CMD_DR_SWAP, SWITCH_PD_ALT_REPLY_MS);
    case SWITCH_PD_ALT_DISCOVER_ID:
        return switch_pd_alt_structured(0x01, 0, 0xFF00);
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

// The data role decides the first step: DFP discovers, UFP swaps first (a source starts as DFP). Only a source with the
// dock's power signature is asked to swap: bm92txx swaps with any dual-role source, and a Mac that accepted would stop
// being the Switch's USB host (usbmux, Xcode, idevicesyslog).
static inline struct switch_pd_alt_out switch_pd_alt_begin(struct switch_pd_alt *alt, uint16_t status1)
{
    const unsigned role = switch_pd_alt_data_role(status1);
    if (role == SWITCH_PD_ALT_DATA_ROLE_DFP) {
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DISCOVER_ID);
    }
    if (role == SWITCH_PD_ALT_DATA_ROLE_UFP && !alt->dock_power) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, NULL);
    }
    if (role == SWITCH_PD_ALT_DATA_ROLE_UFP) {
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DR_SWAP);
    }
    return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "no data role to swap from");
}

// INCOMING_VDM after a step's reply was accepted (or the DisplayPort capabilities after DP discovery).
static inline struct switch_pd_alt_out switch_pd_alt_reply(struct switch_pd_alt *alt, const uint8_t *vdm, size_t len)
{
    if (len < 5) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "short INCOMING_VDM read");
    }
    switch (alt->step) {
    case SWITCH_PD_ALT_DISCOVER_ID: {
        if (!switch_pd_alt_acked(vdm, 0x01) || len < 17) {
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no alternate modes: Discover Identity not acked");
        }
        const unsigned vid = (unsigned)vdm[5] | (unsigned)vdm[6] << 8U;
        const unsigned type = ((unsigned)vdm[8] >> 3U) & 7U;
        const unsigned pid = (unsigned)vdm[15] | (unsigned)vdm[16] << 8U;
        alt->dock = type == 5 && vid == 0x057E && pid == 0x2003;
        if (alt->dock) {
            return switch_pd_alt_go(alt, SWITCH_PD_ALT_ENTER_DOCK);
        }
        if (!alt->want_dp) {
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, NULL);
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DISCOVER_SVID);
    }
    case SWITCH_PD_ALT_DISCOVER_SVID:
        if (!switch_pd_alt_acked(vdm, 0x02) || !switch_pd_alt_has_svid(vdm, len, 0xFF01)) {
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no DisplayPort: SVID FF01 not offered");
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DISCOVER_MODE);
    case SWITCH_PD_ALT_DISCOVER_MODE:
        if (vdm[0] <= 4 || !switch_pd_alt_acked(vdm, 0x03) || vdm[2] != 0x80 || vdm[3] != 0x01 || vdm[4] != 0xFF ||
            len < 6 || (vdm[5] & 0x01u) == 0 || (vdm[5] & 0x04u) == 0) {
            return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE, "no DisplayPort: no UFP_D mode");
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DP_DISCOVER);
    case SWITCH_PD_ALT_ENTER_DOCK:
        // A dock still in its mode from an earlier session NAKs a second Enter Mode; it goes on all the same.
        return switch_pd_alt_dp_or_dock(alt);
    case SWITCH_PD_ALT_DP_DISCOVER: {
        const struct switch_pd_alt_config config = switch_pd_alt_choose_pins(vdm, len, alt->dock);
        if (config.pin == 0) {
            return switch_pd_alt_after_dp(alt, "no DisplayPort: neither pin assignment C nor D offered");
        }
        alt->pin = config.pin;
        for (unsigned i = 0; i < 4; ++i) {
            alt->configure[i] = config.vdo[i];
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DP_CONFIGURE);
    }
    case SWITCH_PD_ALT_DOCK_QUERY:
        alt->old_dock_firmware = vdm[6] == 2 && vdm[7] == 0x17 && len > 11 && (vdm[11] & 0x02u) != 0;
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DOCK_LED);
    case SWITCH_PD_ALT_DOCK_LED:
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_HUB_POWER);
    case SWITCH_PD_ALT_HUB_ENABLE: {
        const bool answer = vdm[6] == 2 && vdm[7] == 0x21;
        if (answer && (vdm[5] & 0x01u) != 0 && alt->hub_tries < SWITCH_PD_ALT_HUB_RETRIES) {
            ++alt->hub_tries;
            return switch_pd_alt_go(alt, SWITCH_PD_ALT_HUB_RETRY);
        }
        alt->hub_enabled = answer && (vdm[5] & 0x01u) == 0;
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE,
                                    alt->hub_enabled ? NULL : "the dock did not enable its USB hub");
    }
    default:
        break;
    }
    return switch_pd_alt_none();
}

struct switch_pd_alt_in {
    uint16_t alert, status1, dp_status;
    bool sink_session;  // SwitchPD's sink phase with the partner as source (a dock, a charger)
    bool at_rest;       // SwitchPD's own USB-PD request is not using COMMAND (published, settling or given up)
    bool dock_power;    // the source's PDOs carry the dock's signature
};

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

static inline struct switch_pd_alt_out switch_pd_alt_start(struct switch_pd_alt *alt, const struct switch_pd_alt_in *in)
{
    alt->dock_power = in->dock_power;
    alt->lanes = alt->want_dp ? SWITCH_PD_ALT_LANES_PENDING : SWITCH_PD_ALT_LANES_FREE;
    if ((in->dp_status & SWITCH_PD_ALT_DP_STATUS_DP_EN) != 0) {
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_STALE_EXIT);
    }
    return switch_pd_alt_begin(alt, in->status1);
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
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_DISCOVER_ID);
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
        return true;
    default:
        return false;
    }
}

// Every ALERT (and SwitchPD's first look at the port), on SwitchPD's work loop after its own handling.
static inline struct switch_pd_alt_out switch_pd_alt_event(struct switch_pd_alt *alt, const struct switch_pd_alt_in *in)
{
    const bool plugged = (in->status1 & SWITCH_PD_ALT_STATUS1_INSERT) != 0;
    if (!plugged || !in->sink_session ||
        (alt->step != SWITCH_PD_ALT_IDLE && (in->alert & SWITCH_PD_ALT_ALERT_PLUGPULL) != 0)) {
        return switch_pd_alt_reset(alt);
    }
    if ((in->alert & SWITCH_PD_ALT_ALERT_CONTRACT) != 0 && switch_pd_alt_waiting_on_chip(alt)) {
        alt->paused = true;
    }
    if (!in->at_rest) {
        return switch_pd_alt_none();
    }
    if (alt->step == SWITCH_PD_ALT_IDLE) {
        if ((in->status1 & SWITCH_PD_ALT_STATUS1_CMD_BUSY) != 0) {
            return switch_pd_alt_none();
        }
        return switch_pd_alt_start(alt, in);
    }
    if (alt->paused) {
        alt->paused = false;
        return switch_pd_alt_send(alt);
    }
    if (switch_pd_alt_vdm_step(alt->step)) {
        if (!alt->accepted && (in->alert & SWITCH_PD_ALT_ALERT_VDM_RECEIVED) != 0) {
            alt->accepted = true;
            return switch_pd_alt_command(SWITCH_PD_ALT_CMD_ACCEPT_VDM, SWITCH_PD_ALT_REPLY_MS);
        }
        if (alt->accepted && (in->alert & SWITCH_PD_ALT_ALERT_CMD_DONE) != 0) {
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
        if (!at_rest) {
            return switch_pd_alt_send(alt);
        }
        return switch_pd_alt_go(alt, SWITCH_PD_ALT_HUB_ENABLE);
    }
    if (!switch_pd_alt_active(alt)) {
        return switch_pd_alt_none();
    }
    if (alt->paused) {
        if (!at_rest) {
            struct switch_pd_alt_out out = switch_pd_alt_none();
            out.timer_ms = SWITCH_PD_ALT_REPLY_MS;
            return out;
        }
        alt->paused = false;
        return switch_pd_alt_send(alt);
    }
    if (alt->step == SWITCH_PD_ALT_DP_DISCOVER || alt->step == SWITCH_PD_ALT_DP_CONFIGURE) {
        return switch_pd_alt_after_dp(alt, "no DisplayPort: no answer within 1.5 s");
    }
    if (alt->step == SWITCH_PD_ALT_DOCK_QUERY || alt->step == SWITCH_PD_ALT_DOCK_LED ||
        alt->step == SWITCH_PD_ALT_HUB_ENABLE) {
        return switch_pd_alt_finish(alt, SWITCH_PD_ALT_DONE,
                                    "the dock did not answer within 1.5 s (its hub stays off)");
    }
    return switch_pd_alt_finish(alt, SWITCH_PD_ALT_FAILED, "the partner did not answer within 1.5 s");
}

static inline const char *switch_pd_alt_step_name(enum switch_pd_alt_step step)
{
    static const char *const names[] = {
        "idle",           "stale DisplayPort exit", "data role swap",        "Discover Identity",     "Discover SVIDs",
        "Discover Modes", "dock mode entry",        "DisplayPort discovery", "DisplayPort Configure", "dock state",
        "dock LED",       "dock hub power-up",      "dock hub enable",       "dock hub retry",        "done",
        "failed"};
    return (unsigned)step < sizeof(names) / sizeof(names[0]) ? names[step] : "?";
}

#ifdef __cplusplus
}
#endif

#endif
