// SPDX-License-Identifier: BSD-2-Clause-Patent
// The radio services the AWDL stack (SwitchWLANAwdl.cpp) uses: SwitchWLAN.cpp's for the firmware's own AWDL (§6), and
// SwitchWLANAwdlSoft.cpp's for the soft backend (§2-§5).
// Plain C types only. Every function: any thread, never blocks; completions come through the callbacks. The
// exception waits for the firmware (swlan_awdl_var): the stack calls it only from its own threads, with its lock
// dropped, never from a callback of this API. The registration calls
// (swlan_awdl_rx_start, swlan_awdl_events, swlan_awdl_fw_events, swlan_awdl_eth_rx) only store the callback: the
// stack calls them with its lock held. Whatever the firmware needs for them (event_msgs bits, monitor) is set up by
// swlan_awdl_open, which the stack calls without its lock.
#ifndef SWITCH_WLAN_AWDL_RADIO_H
#define SWITCH_WLAN_AWDL_RADIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 2: per-interface variables (swlan_awdl_var) and the bring-up probe in the caps; 3: action frames with a stay
// (can_tx_action, swlan_tx_req.stay_us) in place of timed injection
#define SWLAN_AWDL_ABI_VERSION 3

struct bcm_awdl_probe;  // awdl/awdl_bcm.h

struct swlan_awdl_caps {
    uint32_t abi_version;        // SWLAN_AWDL_ABI_VERSION
    uint8_t can_monitor;         // rx of 802.11 frames, AWDL data included (§2)
    uint8_t can_inject;          // tx of caller-built 802.11 data frames (§3)
    uint8_t can_tx_action;       // tx of action frames on a chosen channel, then a stay there (§3, actframe)
    uint8_t can_scheduled_hops;  // runs a TSF-locked channel sequence (§4)
    uint8_t can_stay_assoc;      // hops while en0 stays associated (mchan); else time-shared
    uint8_t has_native_awdl;     // the firmware's own awdl iovars (§6)
    uint8_t soft_trial;          // debug builds: the soft backend runs although it cannot carry data (switch-awdl-soft)
    uint8_t self_mac[6];
    uint16_t max_inject_len;
    // What the firmware's iovar table answered at bring-up (SwitchWLAN.cpp's probe, once a boot): the stack picks the
    // firmware generation's bring-up from it (bcm_awdl_api_of). NULL when the probe did not run.
    const struct bcm_awdl_probe *probe;
};

// <0 (negative errno): AWDL unavailable; the stack says so to wifip2pd and does not retry. Sets the firmware up for
// AWDL (event_msgs): the stack calls it only once wifip2pd enables AWDL on awdl0.
int swlan_awdl_open(const struct swlan_awdl_caps **caps_out);
// What swlan_awdl_open would answer, without touching the firmware (en0's CARD_CAPABILITIES asks this). -EAGAIN while
// the radio has not got that far (capability list not read, bring-up probe not run yet: ask again later); another
// negative errno when it never will (no "awdl" in the capability list, no AWDL flow rings).
int swlan_awdl_query(const struct swlan_awdl_caps **caps_out);
void swlan_awdl_close(void);

struct swlan_rx_meta {
    uint64_t tsf;  // local TSF at rx, us (0: not stamped)
    uint16_t freq_mhz;
    int8_t rssi_dbm;
    uint8_t flags;  // bit0 FCS_OK, bit1 FCS_PRESENT, bit2 AMPDU, bit3 SHORT_GI
    uint8_t mcs;
    uint8_t _pad[3];
};

enum {
    SWLAN_RX_FCS_OK = 1,
    SWLAN_RX_FCS_PRESENT = 2
};

typedef void (*swlan_rx_fn)(void *ctx, const uint8_t *frame, size_t len, const struct swlan_rx_meta *meta);
int swlan_awdl_rx_start(swlan_rx_fn fn, void *ctx);
int swlan_awdl_rx_stop(void);
int swlan_awdl_rx_filter(const uint8_t dst[6], const uint8_t bssid[6], uint16_t ethertype_or_0);

// Sent as soon as the radio can. A radio without hops leaves its home channel for a frame on another one and comes back
// `stay_us` after the request (an excursion: the frame's slot); on its home channel it never moves.
struct swlan_tx_req {
    const uint8_t *frame;  // whole 802.11 frame, no FCS
    size_t len;
    uint16_t freq_mhz;
    uint8_t rate_mcs;  // 0xff: firmware default
    uint8_t flags;     // bit0 NO_ACK, bit1 fail-if-off-channel
    uint32_t stay_us;  // 0: no need to stay
};

enum {
    SWLAN_TX_NO_ACK = 1,
    SWLAN_TX_ON_CHANNEL_ONLY = 2
};

int swlan_awdl_tx(const struct swlan_tx_req *req);  // >= 0 request id, < 0 errno

struct swlan_chan_slot {
    uint16_t freq_mhz;
    uint16_t dwell_us;
};

struct swlan_chan_seq {
    uint64_t anchor_tsf;
    uint32_t period_us;
    uint16_t n_slots;
    uint16_t infra_freq_mhz;
    const struct swlan_chan_slot *slots;
};

int swlan_awdl_chanseq_set(const struct swlan_chan_seq *seq);
int swlan_awdl_chanseq_clear(void);

enum swlan_awdl_evt {
    SWLAN_AWDL_TXDONE,
    SWLAN_AWDL_SEQ_WRAP,
    SWLAN_AWDL_CHIP_LOST,
    SWLAN_AWDL_OFFCHAN_DROP
};

struct swlan_awdl_event {
    uint32_t type;
    uint32_t id;
    uint16_t status;
    uint64_t tsf;
};

typedef void (*swlan_evt_fn)(void *ctx, const struct swlan_awdl_event *e);
int swlan_awdl_events(swlan_evt_fn fn, void *ctx);

// ---- §6 native AWDL: the firmware's own AWDL (cap "awdl", r7p8) ----------------------------------------------------------
// One firmware variable on firmware interface `ifidx` (the msgbuf ioctl's interface: 0 the station; the AWDL
// interface's index from its WLC_E_IF add for its awdl_* variables, as brcmfmac's brcmf_fil_iovar_data_set on the AWDL
// ifp). set: "<name>\0<in>"; get: the same request, answered into `out` (a get may carry a request:
// interface_create). Thread context only (never from a callback): waits for the firmware's answer, bounded like
// every SwitchWLAN ioctl. Returns 0, SWLAN_FWERR(code) when the firmware answered with a BCME code, or a negative
// errno when the request did not reach it (radio down, no answer).
#define SWLAN_FWERR_BASE  1000
#define SWLAN_FWERR(bcme) (-SWLAN_FWERR_BASE + (bcme))

static inline bool swlan_is_fwerr(int e)
{
    return e < -SWLAN_FWERR_BASE && e > -2 * SWLAN_FWERR_BASE;  // BCME codes are -1 .. -60
}

static inline int swlan_fwerr_code(int e)
{
    return e + SWLAN_FWERR_BASE;
}

int swlan_awdl_var(int set, uint8_t ifidx, const char *name, const void *in, size_t in_len, void *out, size_t out_len,
                   size_t *returned);

// The firmware events of an AWDL session (WLC_E_IF for the AWDL interface, WLC_E_ACTION_FRAME_RX (75) and
// WLC_E 96-98, 111-120 while AWDL is open; awdl_bcm.h):
// enabled in "event_msgs" by swlan_awdl_open; this call only stores the callback. Delivered from the work loop,
// payload after the wl_event_msg_t.
struct swlan_fw_event {
    uint32_t type, status, reason;
    uint8_t addr[6];
    uint8_t ifidx, bsscfgidx;
};

typedef void (*swlan_fw_event_fn)(void *ctx, const struct swlan_fw_event *e, const uint8_t *data, size_t len);
int swlan_awdl_fw_events(swlan_fw_event_fn fn, void *ctx);
// Frames on the firmware's AWDL interface (the ifidx its WLC_E_IF ADD names; one msgbuf flow ring per destination):
// the 802.3 frames with the AWDL data header the stack builds (bcm_awdl_encap), and what the firmware delivers there.
int swlan_awdl_eth_tx(const uint8_t *eth, size_t len);  // 0 queued
typedef void (*swlan_eth_rx_fn)(void *ctx, const uint8_t *eth, size_t len);
int swlan_awdl_eth_rx(swlan_eth_rx_fn fn, void *ctx);
// The access point's channel (MHz) while the station is associated, 0 otherwise. Lock-free; SwitchWLANAwdl.cpp gives
// it to the stack, whose schedule puts the free slots on it (awdl_schedule: AWDL and the station share one radio).
uint16_t swlan_awdl_infra_freq(void);

// ---- SwitchWLANAwdl.cpp (r12-awdl-stack) -> SwitchWLANNet.cpp ---------------------------------------------------
// Whether en0 advertises AWDL (CARD_CAPABILITIES bit 32): the radio's bring-up probe found a way to make the
// firmware's AWDL interface (interface_create v3 or awdl_if) and the stack's memory is there. Decided once, as soon as
// the radio can answer (swlan_awdl_query); 0 before that; cheap after. Touches no firmware state.
int switch_wlan_awdl_capable(void);
// en0's VIRTUAL_IF_CREATE (94) / VIRTUAL_IF_DELETE (95), the payload already copied in; create may write the new
// interface's name back into it (the caller copies it out). 0 or an errno.
int switch_wlan_awdl_vif_create(void *payload, size_t length);
int switch_wlan_awdl_vif_delete(const void *payload, size_t length);

#ifdef __cplusplus
}
#endif

#endif
