// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD: the one owner of the BM92T36 USB-PD controller (I2C1 0x18), its ALERT line (PK4) and the port's power
// role.
//
// The ALERT line is level-low and released by reading ALERT_STATUS, which clears on read (Linux bm92txx.c
// bm92t_probe: IRQF_TRIGGER_LOW). So exactly one driver may take the pin and read that register: this one. On every
// ALERT it reads ALERT_STATUS once, then STATUS1/STATUS2, decides the power role and hands the state to its listeners
// on its own work loop: SwitchBattery (which re-reads the charger and gauge 250 ms later and programs the BQ24193's
// input limit from the contract below) and the USB role manager (SwitchUSB.cpp, which only follows `role` with the
// data role).
//
// USB-PD request (sink sessions with a source partner only; the power role logic below is untouched). The controller
// negotiates a first contract on its own (CONFIG1 AUTONGTSNK, left as the firmware has it, as Linux bm92txx leaves it);
// on that contract (ALERT_CONTRACT, or at start when it is already sinking) SwitchPD reads the source's PDOs, picks one
// with charge::choosePdo (SwitchBattery/SwitchChargePolicy.h: Linux bm92t_check_pdo, at most 15 V and 3 A), requests
// it (SET_RDO + SEND_RDO, operating current = the input limit the BQ24193 will get), verifies the new contract against
// the request (CURRENT_PDO/CURRENT_RDO), sends PS_RDY and publishes it 2 s after PS_RDY completes (bm92t_power_work)
// with one line, "SwitchPD: contract <V> V <A> A (<the source's PDOs, * = ours>)". A source that offers nothing usable,
// rejects the request, or a step that is not answered within 1.5 s: one line, and the controller's own contract stays
// with nothing published (the charger then gets 500 mA). A later unsolicited contract (a multi-port charger
// re-balancing, a source reset) withdraws the published one and starts over; three failed requests in one session end
// the attempts until the cable is pulled. Every step is a reply to an ALERT or a one-shot timer.
//
// Fixed VBUS rail (v_vdd5v3 on PX3) and BQ24193 boost stay off in Empty, Classify, Sink and Held, and turn on only in
// WaitSource and Source. VCONN (PK5) stays on in WaitSource and Source, and in Sink only while STATUS2.VCONN_ON holds
// (e-marked cable across PR_Swap). Keeping SPDSRC12_ON armed across Empty -> WaitSource lets Try.SNK monitors settle
// without dropping Rp; once Source begins, a PR_Swap probe asks dual-role partners to take back VBUS, and verified USB
// hosts that refuse drop to Classify (source switches off 1 s). External VBUS blocks boost unless our own enabled source
// switch accounts for it. Faults and host shutdown isolate the source.
// One kernel log line per decision, with the controller's bits. The decisions are SwitchPDPolicy.h (host-tested).
//
// Interrupt only: a pin that cannot be attached panics; a pin that keeps firing (stuck low: more than 50 ALERTs in a
// second) is masked for the boot with one line and the port stays sink-only (no storm, no panic). The classify windows
// are one-shot timers on the PD work loop, armed only by a new attach: nothing re-arms on its own.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SwitchPDRole {
    SWITCH_PD_ROLE_NONE,    // nothing on the port
    SWITCH_PD_ROLE_SINK,    // something is: we take power or none (a charger, a host, a dock, or not yet classified)
    SWITCH_PD_ROLE_SOURCE,  // a sink-only partner and our VBUS on it: USB host
};

struct SwitchPDState {
    uint16_t alert;          // ALERT_STATUS bits of this event (0 for a state not caused by an ALERT)
    uint16_t status1;        // STATUS1
    uint16_t status2;        // STATUS2
    bool plugged;            // STATUS1.INSERT: a cable is in
    bool vbusPresent;        // STATUS1.VSAFE: VBUS at vSafe5V or a contract (ours when role is SOURCE)
    bool dataHost;           // STATUS1 data role DFP
    enum SwitchPDRole role;  // the power role decided (above)
    bool settled;            // false while WaitSource, Classify, Held, or Source PR_Swap probe is in flight
    // USB-PD request (above), sink sessions only; all 0/false outside one.
    bool pdSource;                 // the partner sent USB-PD source capabilities
    uint16_t contractMillivolts;   // the published (requested and verified) contract, 0 = none
    uint16_t inputLimitMilliamps;  // its operating current: the BQ24193 input current limit for it, 0 = none
};

// Adds a listener (at most 4; for the life of the boot) and starts the PD service on first use (may sleep: I2C,
// GPIO, GIC). The listener runs on the PD work loop after every ALERT and role change, and once right away with the
// current state; keep it short. Returns 0, or -1 when the controller is not there (the listener is then never called).
int switch_pd_add_listener(void (*listener)(void *context, const struct SwitchPDState *state), void *context);

// Host mode packaged and working (true) or gone for this boot (false: its bring-up failed). Without it the port is
// sink-only. Thread context; 0, or -1 without the controller.
int switch_pd_allow_source(bool allow);

// SC7, from switch_usb_suspend/resume: an empty port is sink-only while asleep, so attaching a sink (a keyboard) does
// not wake the console and attaching a source (a charger, a dock, a powered display) does. Thread context.
void switch_pd_sleep(bool entering);

// One line for the role report (SwitchUSB.cpp): the phase, STATUS1/STATUS2/CONFIG1/VENDOR_CONFIG as read now, and the
// GPIOs behind VBUS: v_vdd5v3 (PX3), VCONN (PK5) and the BQ24193's PSEL/OTG inputs (PJ5, PL0; read, never driven).
// Thread context (I2C on the PD work loop); 0, or -1 without the controller (the buffer then says so).
int switch_pd_describe(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif
