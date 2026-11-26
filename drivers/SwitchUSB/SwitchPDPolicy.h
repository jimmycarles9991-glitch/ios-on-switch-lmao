// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD's power-role decisions (SwitchPD.h), in plain C with no kernel dependency, so the host tests
// (test/switch_usb_host_test.cpp) run this same header against the BM92T36 traces r7w recorded. SwitchPD.cpp owns the
// controller, the GPIOs, the charger call and the one-shot windows; this file maps (phase, ALERT_STATUS, STATUS1,
// STATUS2) to the next step and says, for every phase, which outputs are on:
//   - source switches (CONFIG1 SPDSRC): on in Empty, WaitSource and Source when host mode is packaged, off in
//     Sink, Held and Classify;
//   - fixed VBUS rail (v_vdd5v3 on PX3) and BQ24193 boost: on in WaitSource and Source only; off in Empty, Classify,
//     Sink and Held;
//   - VCONN (PK5): on in WaitSource and Source, and in Sink only while STATUS1.INSERT and STATUS2.VCONN_ON are set
//     (e-marked cable still needing VCONN after PR_Swap); off otherwise.
// On the OLED, v_vdd5v3 is the port's VBUS power (L4T nx-platform tegra210-nx-power-fixed.dtsi "VBUS power", and
// tegra210b01-fric-power-tree.dtsi usb2-0 vbus-supply "Actual v_vdd5v3"); the BM92T36's SPDSRC switch connects it to
// the connector once the partner's Rd is seen. Keeping PX3 and PK5 off in Empty prevents back-feeding 5.3 V into a
// dual-role partner while hung or booting, and keeping SPDSRC12_ON armed across Empty -> WaitSource preserves Try.SNK
// timing on DRP monitors without disturbing Rp.
#ifndef SWITCH_PD_POLICY_H
#define SWITCH_PD_POLICY_H

#include <stdbool.h>
#include <stdint.h>

// BM92T36 bits (CTCaer switch-l4t-kernel-4.9 drivers/misc/bm92txx.c).
#define SWITCH_PD_ALERT_SRC_FAULT    (1u << 1)
#define SWITCH_PD_ALERT_PLUGPULL     (1u << 3)
#define SWITCH_PD_ALERT_PR_SWAP      (1u << 4)
#define SWITCH_PD_ALERT_CONTRACT     (1u << 12)
#define SWITCH_PD_ALERT_SRC_PLUGIN   (1u << 13)
#define SWITCH_PD_STATUS1_FAULT_MASK (3u << 0)
#define SWITCH_PD_STATUS1_SPDSRC2    (1u << 3)  // VBUS switch 2 on
#define SWITCH_PD_STATUS1_INSERT     (1u << 7)
#define SWITCH_PD_STATUS1_DR_SHIFT   8
#define SWITCH_PD_STATUS1_VSAFE      (1u << 10)  // VBUS at vSafe5V or a contract
#define SWITCH_PD_STATUS1_SRC_MODE   (1u << 12)  // we are the source (the partner presents Rd)
#define SWITCH_PD_STATUS1_SPDSNK     (1u << 14)
#define SWITCH_PD_STATUS1_SPDSRC1    (1u << 15)  // VBUS switch 1 on
#define SWITCH_PD_STATUS2_VCONN_ON   (1u << 9)
#define SWITCH_PD_STATUS2_OTG_INSERT (1u << 13)
#define SWITCH_PD_DATA_ROLE_DFP      2u
#define SWITCH_PD_ALERT_SNK_FAULT    (1u << 0)
#define SWITCH_PD_STATUS1_LASTCMD    (7u << 4)  // the last command's result: 0 complete
#define SWITCH_PD_DP_STATUS_DP_EN    (1u << 15)

enum switch_pd_phase {
    SWITCH_PD_EMPTY,        // nothing on the port; source switches on only with host mode, rails off
    SWITCH_PD_SINK,         // a partner we do not source: switches off until it is unplugged
    SWITCH_PD_CLASSIFY,     // verified USB host in Source did not swap power role: switches off 1 s to yield
    SWITCH_PD_WAIT_SOURCE,  // partner presented Rd: boost and rails on, SPDSRC12_ON armed, waiting for SRC_PLUGIN
    SWITCH_PD_SOURCE,       // our VBUS on a sink partner
    SWITCH_PD_HELD,         // source error: switches, boost and source rails off, not again until unplugged
};

enum switch_pd_act {
    SWITCH_PD_NONE,        // nothing changes
    SWITCH_PD_SAW_DETACH,  // Classify: the partner left with our switches off (the window decides)
    SWITCH_PD_ENTER,       // go to `next`: its switches and rails, one line with `why`
    SWITCH_PD_START,       // WaitSource: the controller closed onto our VBUS; publish Source
    SWITCH_PD_END,         // leave Source/WaitSource: boost off, then as ENTER
};

struct switch_pd_step {
    enum switch_pd_act act;
    enum switch_pd_phase next;
    bool cancel_window;  // a Classify/WaitSource window is running and this step ends it
    const char *why;
};

static inline const char *switch_pd_phase_name(enum switch_pd_phase phase)
{
    switch (phase) {
    case SWITCH_PD_EMPTY:
        return "empty";
    case SWITCH_PD_SINK:
        return "sink";
    case SWITCH_PD_CLASSIFY:
        return "classify";
    case SWITCH_PD_WAIT_SOURCE:
        return "wait-source";
    case SWITCH_PD_SOURCE:
        return "source";
    case SWITCH_PD_HELD:
        return "held";
    }
    return "?";
}

static inline bool switch_pd_switches_on(enum switch_pd_phase phase, bool allow_source)
{
    return allow_source && phase != SWITCH_PD_SINK && phase != SWITCH_PD_CLASSIFY && phase != SWITCH_PD_HELD;
}

static inline bool switch_pd_vbus_rail_on(enum switch_pd_phase phase)
{
    return phase == SWITCH_PD_WAIT_SOURCE || phase == SWITCH_PD_SOURCE;
}

static inline bool switch_pd_vconn_on(enum switch_pd_phase phase, uint16_t status1, uint16_t status2)
{
    if (phase == SWITCH_PD_WAIT_SOURCE || phase == SWITCH_PD_SOURCE)
        return true;
    return phase == SWITCH_PD_SINK && (status1 & SWITCH_PD_STATUS1_INSERT) != 0 &&
           (status2 & SWITCH_PD_STATUS2_VCONN_ON) != 0;
}

static inline bool switch_pd_boost_on(enum switch_pd_phase phase)
{
    return phase == SWITCH_PD_WAIT_SOURCE || phase == SWITCH_PD_SOURCE;
}

// The FET state alone proves our VBUS only while we own the enabled source supply.
static inline bool switch_pd_vbus_ours(uint16_t status1)
{
    return (status1 & (SWITCH_PD_STATUS1_SPDSRC1 | SWITCH_PD_STATUS1_SPDSRC2)) != 0;
}

static inline struct switch_pd_step switch_pd_step_make(enum switch_pd_act act, enum switch_pd_phase next,
                                                        bool cancel_window, const char *why)
{
    struct switch_pd_step step = {act, next, cancel_window, why};
    return step;
}

// After an ALERT (alert = its ALERT_STATUS) or with the state as it is (alert 0). source_ready includes host mode and
// armed connector switches.
// NOLINTBEGIN(modernize-use-nullptr): C (the host harness) reads this header too; 0 is the step without a reason
static inline struct switch_pd_step switch_pd_decide(enum switch_pd_phase phase, uint16_t alert, uint16_t status1,
                                                     uint16_t status2, bool source_ready)
{
    const bool plugged = (status1 & SWITCH_PD_STATUS1_INSERT) != 0;
    const bool vbus = (status1 & SWITCH_PD_STATUS1_VSAFE) != 0;
    const bool partner_is_sink = plugged && ((status1 & SWITCH_PD_STATUS1_SRC_MODE) != 0u);
    const bool otg_sink = partner_is_sink && ((status2 & SWITCH_PD_STATUS2_OTG_INSERT) != 0u);
    const struct switch_pd_step none = switch_pd_step_make(SWITCH_PD_NONE, phase, false, 0);
    if ((phase == SWITCH_PD_EMPTY || phase == SWITCH_PD_CLASSIFY) &&
        ((alert & SWITCH_PD_ALERT_SRC_FAULT) || (status1 & SWITCH_PD_STATUS1_FAULT_MASK)))
        return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_HELD, phase != SWITCH_PD_EMPTY,
                                   "not sourcing: source fault");
    switch (phase) {
    case SWITCH_PD_EMPTY:
        // Our sink without OTG_INSERT (yet, or an accessory): nothing to decide until the controller says more.
        if (!plugged || (partner_is_sink && !otg_sink))
            return none;
        if (!partner_is_sink) {
            return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_SINK, false,
                                       "sink: the partner is a source (charger, host or dock)");
        }
        if (vbus) {
            return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_SINK, false,
                                       "sink: the partner came up as our sink with VBUS already up");
        }
        if (!source_ready) {
            return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_SINK, false,
                                       "sink: the partner came up as our sink; host mode off");
        }
        return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_WAIT_SOURCE, false,
                                   "waiting for source: the partner came up as our sink; boost and source rails on");
    case SWITCH_PD_CLASSIFY:
        if (!plugged)
            return switch_pd_step_make(SWITCH_PD_SAW_DETACH, phase, false, 0);
        if (!partner_is_sink) {
            return switch_pd_step_make(
                SWITCH_PD_ENTER, SWITCH_PD_SINK, true,
                "dual-role partner: it sources with our source switches off (sink until unplugged)");
        }
        return none;
    case SWITCH_PD_WAIT_SOURCE:
        if ((alert & SWITCH_PD_ALERT_SRC_FAULT) || (status1 & SWITCH_PD_STATUS1_FAULT_MASK))
            return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_HELD, true, "not sourcing: source fault");
        if (!plugged)
            return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_EMPTY, true, "detached: VBUS, v_vdd5v3, VCONN off");
        if (!partner_is_sink || (vbus && !switch_pd_vbus_ours(status1))) {
            return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_SINK, true,
                                       "sink: the partner came back as a source, or with VBUS up that is not ours");
        }
        if (otg_sink && ((alert & SWITCH_PD_ALERT_SRC_PLUGIN) || (alert == 0 && switch_pd_vbus_ours(status1))))
            return switch_pd_step_make(SWITCH_PD_START, SWITCH_PD_SOURCE, true, 0);
        return none;
    case SWITCH_PD_SOURCE:
        if (!plugged)
            return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_EMPTY, false, "detached: VBUS, v_vdd5v3, VCONN off");
        if (!partner_is_sink) {
            return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_SINK, false,
                                       "not sourcing: the partner took the source role (sink until unplugged)");
        }
        if ((alert & SWITCH_PD_ALERT_SRC_FAULT) || (status1 & SWITCH_PD_STATUS1_FAULT_MASK))
            return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_HELD, false, "not sourcing: source fault");
        return none;
    case SWITCH_PD_SINK:
    case SWITCH_PD_HELD:
        if (!plugged)
            return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_EMPTY, false, "detached");
        return none;
    }
    return none;
}

// The data role SwitchUSB takes: USB host for our own sink-only partner (we source it), or for a partner that sources
// and left us the data role DFP (the dock after its data role swap, a hub with power in), as its device otherwise. A
// partner that came up as our sink while we still classify it is neither: it presents Rd (SRC_MODE), we are DFP by
// default, and it may yet turn out to be a dual-role host such as a Mac.
static inline bool switch_pd_partner_sources(uint16_t status1)
{
    const bool plugged = (status1 & SWITCH_PD_STATUS1_INSERT) != 0;
    const bool vbus = (status1 & SWITCH_PD_STATUS1_VSAFE) != 0;
    return plugged && vbus && ((status1 & SWITCH_PD_STATUS1_SRC_MODE) == 0u);
}

static inline bool switch_pd_dfp(uint16_t status1)
{
    return ((status1 & (3u << SWITCH_PD_STATUS1_DR_SHIFT)) >> SWITCH_PD_STATUS1_DR_SHIFT) == SWITCH_PD_DATA_ROLE_DFP;
}

static inline bool switch_pd_usb_host(bool sourcing, uint16_t status1)
{
    return sourcing || (switch_pd_partner_sources(status1) && switch_pd_dfp(status1));
}

// VBUS as the device controller is to see it (Tegra X1 TRM 22.9.11, pp. 1406-1407: the VBUS override on only while
// connected to a host port, off with no cable): a partner that sources VBUS and leaves us UFP, while we source
// nothing. A charger counts: its VBUS is real, and with no host on D+/D- nothing connects.
static inline bool switch_pd_usb_device_vbus(bool sourcing, uint16_t status1)
{
    return !sourcing && switch_pd_partner_sources(status1) && !switch_pd_dfp(status1);
}

// A Classify or WaitSource window ran out (status1/status2 read at its end). Other phases: nothing (a stale timer).
static inline struct switch_pd_step switch_pd_window_end(enum switch_pd_phase phase, bool saw_detach, uint16_t status1,
                                                         uint16_t status2)
{
    (void)saw_detach;
    const bool plugged = (status1 & SWITCH_PD_STATUS1_INSERT) != 0;
    if (phase == SWITCH_PD_CLASSIFY) {
        if (!plugged) {
            return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_EMPTY, false,
                                       "detached: USB host partner left during yield window");
        }
        return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_SINK, false,
                                   "not sourcing: USB host partner did not yield source role");
    }
    if (phase == SWITCH_PD_WAIT_SOURCE) {
        if (plugged && (status1 & SWITCH_PD_STATUS1_SRC_MODE) && (status2 & SWITCH_PD_STATUS2_OTG_INSERT) &&
            switch_pd_vbus_ours(status1)) {
            return switch_pd_step_make(SWITCH_PD_START, SWITCH_PD_SOURCE, false, 0);
        }
        if (plugged && !(status1 & SWITCH_PD_STATUS1_SRC_MODE)) {
            return switch_pd_step_make(SWITCH_PD_ENTER, SWITCH_PD_SINK, false,
                                       "sink: the partner came back as a source");
        }
        return switch_pd_step_make(SWITCH_PD_END, SWITCH_PD_EMPTY, false,
                                   "empty: the sink-only partner did not come back; boost and source rails off");
    }
    return switch_pd_step_make(SWITCH_PD_NONE, phase, false, 0);
}

// bm92txx.c bm92t_system_reset_auto(info, false), from bm92t_extcon_cable_set_init_state: a controller found plugged and
// left as DFP, in DisplayPort mode or with a last command that did not complete gets a SYS_RESET before anything else.
static inline bool switch_pd_reset_at_start(uint16_t status1, uint16_t dp_status)
{
    const bool dfp =
        ((status1 & (3u << SWITCH_PD_STATUS1_DR_SHIFT)) >> SWITCH_PD_STATUS1_DR_SHIFT) == SWITCH_PD_DATA_ROLE_DFP;
    return (status1 & SWITCH_PD_STATUS1_INSERT) &&
           (dfp || (dp_status & SWITCH_PD_DP_STATUS_DP_EN) || (status1 & SWITCH_PD_STATUS1_LASTCMD));
}

// bm92t_event_handler: an internal fault (STATUS1 1:0) ends the event; it resets the controller (SYS_RESET, with VBUS
// already off) when a partner is plugged, the ALERT was a sink fault, or nothing raised it.
static inline bool switch_pd_internal_fault(uint16_t status1)
{
    return (status1 & SWITCH_PD_STATUS1_FAULT_MASK) != 0;
}

static inline bool switch_pd_reset_on_fault(uint16_t alert, uint16_t status1)
{
    return switch_pd_internal_fault(status1) &&
           ((status1 & SWITCH_PD_STATUS1_INSERT) || (alert & SWITCH_PD_ALERT_SNK_FAULT) || alert == 0);
}

// ... and a source fault without one gets a PD hard reset (PD_HARD_RST_CMD).
static inline bool switch_pd_hard_reset(uint16_t alert, uint16_t status1)
{
    return (alert & SWITCH_PD_ALERT_SRC_FAULT) && !switch_pd_internal_fault(status1);
}

// NOLINTEND(modernize-use-nullptr)

#endif
