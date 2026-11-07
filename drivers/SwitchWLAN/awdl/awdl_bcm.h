// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Broadcom FullMAC firmware's own AWDL (the "awdl" capability the BCM4356A3's 7.35.252.108 reports): the iovars a
// host driver sets, the events it receives and the data frames on the AWDL interface, as byte-exact encoders and
// decoders. Little endian; "packed" where Broadcom packs the struct, natural alignment where it does not.
//
// Two firmware generations, told apart at bring-up by the probe below (SwitchWLAN.cpp, once a boot):
//   - interface_create version 3 (the Apple firmware of the BCM4364/BCM4377): the AWDL interface is an interface of
//     type AWDL made by GET interface_create, configured on its own firmware interface (the msgbuf ioctl's ifidx), in
//     the order and with the values Andre Eikmeyer's brcmfmac series traced from macOS 15.4.1 (LKML 2026-11-04,
//     "[PATCH 0/7] wifi: brcmfmac: AWDL for AirDrop on Apple T2 Macs"; the traced bytes are in awdl_bcm_trace.c with
//     that series' notice);
//   - the 7.x generation (Broadcom wlioctl.h $Id 499158 2014-08-27, permissive notice, excerpt in
//     out/agent-scratch/r12-awdl-stack/sources/): awdl_if makes the AWDL bsscfg, awdl_chan_seq carries the sequence,
//     awdl_config is the 28-byte awdl_config_params_t and awdl_payload's length is a u32.
// What both share has the same field offsets: the series' awdl_sync_params, awdl_opmode, awdl_election_tree,
// awdl_peer_op and awdl_oob_af_auto bytes are the 2014 structures filled in (tests/test_native.c overlays them), so one
// encoder serves both, with the series' lengths (a superset: a 7.x handler reads its shorter struct from the front).
// Event numbers: the series' (fweh.h) and Infineon's Apache-2.0 WHD.
#ifndef AWDL_BCM_H
#define AWDL_BCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- firmware status codes (bcmutils.h BCME_*) ---------------------------------------------------------------------
enum {
    BCME_OK = 0,
    BCME_BADARG = -2,
    BCME_NOTUP = -4,
    BCME_BUFTOOSHORT = -14,
    BCME_BUFTOOLONG = -15,
    BCME_OUTOFRANGECHAN = -19,
    BCME_BADCHAN = -20,
    BCME_UNSUPPORTED = -23,
    BCME_BADLEN = -24,
    BCME_NOTFOUND = -30,
};

// "UNSUPPORTED" and so on for a BCME code (0 "OK"); "?" past the table.
const char *bcme_name(int status);

// ---- firmware events (wl_event_msg_t.event_type) ---------------------------------------------------------------------
enum {
    WLC_E_IF = 54,
    WLC_E_ACTION_FRAME_RX = 75,  // received action frames on the AWDL interface (the series' AWDL frames)
    WLC_E_AWDL_AW = 96,
    WLC_E_AWDL_ROLE = 97,   // the role is the event's status: 0 slave, 1 non-election master, 2 master
    WLC_E_AWDL_EVENT = 98,  // carries an AWDL action frame with some of its status values
    WLC_E_AWDL_RX_PRB_RESP = 111,
    WLC_E_AWDL_RX_ACT_FRAME = 112,  // WHD's number for received AWDL action frames
    WLC_E_AWDL_WOWL_NULLPKT = 113,
    WLC_E_AWDL_PHYCAL_STATUS = 114,
    WLC_E_AWDL_OOB_AF_STATUS = 115,
    WLC_E_AWDL_SCAN_STATUS = 116,
    WLC_E_AWDL_AW_START = 117,
    WLC_E_AWDL_AW_END = 118,
    WLC_E_AWDL_AW_EXT = 119,
    WLC_E_AWDL_PEER_CACHE_CONTROL = 120,
};

// The event mask bits the host sets in "event_msgs" while AWDL is open (WLC_E_IF is always on). All below 128: the
// series' 150/152 (AWDL_STATS, SDB_STATE) need event_msgs_ext and the series drops them anyway.
extern const uint8_t bcm_awdl_events[];
extern const unsigned bcm_awdl_event_count;

// One firmware event as the radio side hands it over (the fields of wl_event_msg_t the stack uses).
struct awdl_fw_event {
    uint32_t type, status, reason;
    uint8_t addr[6];
    uint8_t ifidx, bsscfgidx;
};

// ---- the bring-up probe: which AWDL iovars this firmware's table has --------------------------------------------------
// GET of each, once a boot (no side effects: peer_op asks INFO for no one, interface_create its version). A name
// the table lacks answers UNSUPPORTED; anything else (OK, BADARG, NOTFOUND, BUFTOOSHORT, ...) means the handler exists.
enum bcm_probe_item {
    BCM_PROBE_INTERFACE_CREATE,  // "bsscfg:interface_create" version query, as brcmf_feat_awdl_check
    BCM_PROBE_AWDL_IF,
    BCM_PROBE_AWDL,
    BCM_PROBE_AWDL_CONFIG,
    BCM_PROBE_SLOT_BSS,
    BCM_PROBE_AWDL_SYNC_PARAMS,
    BCM_PROBE_AWDL_CHAN_SEQ,
    BCM_PROBE_AWDL_PEER_OP,
    BCM_PROBE_AWDL_OPMODE,
    BCM_PROBE_MCHAN,
    // the rest of what either bring-up sets
    BCM_PROBE_AWDL_ELECTION_TREE,
    BCM_PROBE_AWDL_PAYLOAD,
    BCM_PROBE_AWDL_OOB_AF_AUTO,
    BCM_PROBE_AWDL_AFTXMODE,
    BCM_PROBE_AWDL_OSOC_CHAN,
    BCM_PROBE_AWDL_PSF_DWELL,
    BCM_PROBE_AWDL_PRESENCEMODE,
    BCM_PROBE_INTERFACE_REMOVE,
    BCM_PROBE_EVENT_MSGS_EXT,
    BCM_PROBE_AWDL_IF2,  // the struct's name (wl_awdl_if2_t), in case this build names the variable after it
    BCM_PROBE_COUNT
};

#define BCM_PROBE_NO_ANSWER (-32768)  // the request did not reach the firmware (no answer, chip down)
extern const char *const bcm_probe_names[BCM_PROBE_COUNT];
// The GET_VAR request for one item: name, NUL, parameters. Returns its length (0 if `size` is too small).
size_t bcm_probe_request(unsigned item, uint8_t *out, size_t size);

struct bcm_awdl_probe {
    int16_t status[BCM_PROBE_COUNT];  // BCME code, or BCM_PROBE_NO_ANSWER
    uint16_t returned[BCM_PROBE_COUNT];
    uint32_t ifc_version;  // interface_create's answer (0 when not OK)
    uint8_t done;
};

void bcm_probe_record(struct bcm_awdl_probe *p, unsigned item, int status, const uint8_t *reply, size_t length);
// The handler exists (the probe ran and the firmware answered with anything but UNSUPPORTED).
int bcm_probe_has(const struct bcm_awdl_probe *p, unsigned item);
// Bit `item` set for every probe item this firmware lacks (the bring-up skips those iovars).
uint32_t bcm_probe_absent(const struct bcm_awdl_probe *p);

enum bcm_awdl_api {
    BCM_AWDL_API_NONE = 0,     // no way to make an AWDL interface: neither interface_create v3 nor awdl_if
    BCM_AWDL_API_AWDL_IF = 1,  // 7.x: awdl_if, awdl_chan_seq, 28-byte awdl_config, u32 payload length
    BCM_AWDL_API_IFC_V3 = 2,   // interface_create v3 (the series)
};
enum bcm_awdl_api bcm_awdl_api_of(const struct bcm_awdl_probe *p);
const char *bcm_awdl_api_name(enum bcm_awdl_api api);

// Monitor mode, the other way to AirDrop (monitor + injection, the soft backend). 7.35.252.108 lists neither "monitor"
// nor "rtap" in cap, yet its RAM image registers monitor_promisc_level and sta_monitor; the chip's ROM holds the rest
// of the ioctl and iovar tables. GET only, like the AWDL probe. sta_monitor watches given stations while associated,
// so it is not a monitor mode itself.
enum bcm_monitor_probe_item {
    BCM_MONITOR_PROBE_GET_MONITOR,  // WLC_GET_MONITOR, an ioctl rather than a variable
    BCM_MONITOR_PROBE_PROMISC_LEVEL,
    BCM_MONITOR_PROBE_STA_MONITOR,
    BCM_MONITOR_PROBE_COUNT
};

enum {
    BCM_WLC_GET_MONITOR = 107,
    BCM_WLC_SET_MONITOR = 108,  // 1: the radio delivers 802.11 frames (msgbuf rx flag BCM_MSGBUF_RX_FRAME_802_11)
};

// A msgbuf rx completion's flags (brcmfmac msgbuf.c BRCMF_MSGBUF_PKT_FLAGS_*): the frame is 802.11, not 802.3.
enum {
    BCM_MSGBUF_RX_FRAME_802_11 = 0x01,
    BCM_MSGBUF_RX_FRAME_MASK = 0x07,
};

// The soft backend's transmit path on firmware without injection: one action frame through the "actframe" iovar on a
// channel for a dwell (brcmfmac p2p.c brcmf_p2p_tx_action_frame, fwil_types.h brcmf_fil_af_params_le: channel and
// dwell as le32, BSSID, 2 pad, then brcmf_fil_action_frame_le {DA, le16 length, le32 packet id, body}). The body is
// the frame after its 24-byte management header, from the category byte: AWDL's vendor-specific action (127, Apple
// OUI 00:17:f2, type 8). `frame` is the whole 802.11 action frame the stack built (swlan_tx_req.frame).
enum {
    BCM_AF_PARAMS_HDR_LEN = 28,  // channel, dwell, BSSID, pad, DA, length, packet id
    BCM_AF_BODY_MAX = 1800,      // BRCMF_FIL_ACTION_FRAME_SIZE
    BCM_80211_MGMT_HDR_LEN = 24,
};

// The iovar request "actframe\0<params>" into `out`: its length, or 0 when the frame is not a management action frame,
// its body is over BCM_AF_BODY_MAX, or `size` is too small. `chanspec` is the channel to send on (bcm_chanspec_of),
// `dwell_ms` how long the radio stays there for an answer (0 for none).
size_t bcm_actframe_request(uint8_t *out, size_t size, const uint8_t *frame, size_t len, uint16_t chanspec,
                            uint32_t dwell_ms, uint32_t packet_id);

extern const char *const bcm_monitor_probe_names[BCM_MONITOR_PROBE_COUNT];
// The GET_VAR request for a variable item; 0 for the ioctl item, past the table or when `size` is too small.
size_t bcm_monitor_probe_request(unsigned item, uint8_t *out, size_t size);

enum awdl_radio_mode {
    AWDL_RADIO_UNDECIDED = -1,  // the radio has not read its capability list or run the probe yet: ask again
    AWDL_RADIO_OFF = 0,
    AWDL_RADIO_NATIVE = 1,
    AWDL_RADIO_SOFT = 2,
};

enum awdl_radio_off {
    AWDL_OFF_NOT = 0,
    AWDL_OFF_NO_RADIO,
    AWDL_OFF_ABI,
    AWDL_OFF_NO_BACKEND,
    // "awdl" is in the capability list but the iovar table cannot make an AWDL interface. Every 7.35 image of the
    // BCM4350/4356 family lists it (its cap table is generic) and none registers an awdl_* iovar.
    AWDL_OFF_NO_API,
};

struct awdl_radio_verdict {
    enum awdl_radio_mode mode;
    enum awdl_radio_off off;
    enum bcm_awdl_api api;
};
// `query` is swlan_awdl_query's return with a missing caps pointer counted as refused: -EAGAIN is undecided, any other
// negative value is off for good.
struct awdl_radio_verdict awdl_radio_verdict_of(int query, int abi_ok, int has_native, int can_monitor, int can_inject,
                                                const struct bcm_awdl_probe *probe);

// ---- interface creation -------------------------------------------------------------------------------------------
enum {
    BCM_IFC_V3_LEN = 28,  // wl_interface_create_v3 (natural alignment)
    BCM_AWDL_IF2_LEN = 20,
    BCM_AWDL_IFIDX = 2,  // the firmware interface index the host asks for (the series' BRCMF_AWDL_IFIDX)
};

// interface_create v3 for the AWDL interface: iftype 2, flags MAC_USE|IF_INDEX_USE|BSSID_USE (0x1a), our address,
// the AWDL BSSID, if_index 2. Sent as a GET on the station's interface (brcmf_fil_iovar_data_get).
size_t bcm_ifc_v3_awdl(uint8_t *out, const uint8_t mac[6]);
// wl_awdl_if2_t {s32 cfg_idx; s32 up; ea bssid; ea if_addr}
size_t bcm_awdl_if2(uint8_t *out, int32_t cfg_idx, int up, const uint8_t bssid[6], const uint8_t addr[6]);

// ---- iovar payloads -------------------------------------------------------------------------------------------------
enum {
    BCM_AWDL_CONFIG_LEN = 28,         // awdl_config_params_t (7.x)
    BCM_AWDL_CONFIG_START = 0x83,     // the series' u32 awdl_config at start ...
    BCM_AWDL_CONFIG_SETTLED = 0x73,   // ... and 5 s later
    BCM_AWDL_SYNC_PARAMS_LEN = 36,    // awdl_sync_params_t (packed)
    BCM_AWDL_OPMODE_LEN = 28,         // awdl_opmode_t (12, packed) as the series sends it
    BCM_AWDL_ELECTION_TREE_LEN = 42,  // awdl_election_tree_info_t (packed)
    BCM_AWDL_PEER_OP_HDR_LEN = 9,     // awdl_peer_op_t {u8 version, opcode; ea addr; u8 mode/flags}
    BCM_AWDL_PEER_OP_ADD_LEN = 0x25,  // an add is at least this long (room for one HT capabilities element)
    BCM_AWDL_PEER_IES_MAX = 128,
    BCM_AWDL_OOB_HDR_LEN = 44,  // awdl_oob_af_params_auto_t before payload_len
    BCM_AWDL_PSF_DWELL_LEN = 8,
    BCM_AWDL_SLOT_BSS_LEN = 158,
    BCM_AWDL_CHAN_SEQ_FIXED = 6,
    BCM_AWDL_PRESENCEMODE = 4,
    BCM_AWDL_OSOC_CHAN = 44,  // the series' awdl_osoc_chan 500 ms after the start
    BCM_RX_MGMT_V1_LEN = 16,  // brcmf_rx_mgmt_data (big endian)
    BCM_RX_MGMT_V2_LEN = 24,
    BCM_HT_CAP_IE_LEN = 28,  // element id 45, length 26
};

enum {
    BCM_AWDL_PEER_OP_ADD = 0,
    BCM_AWDL_PEER_OP_DEL = 1,
    BCM_AWDL_PEER_OP_INFO = 2
};

enum {
    BCM_AWDL_ROLE_SLAVE = 0,
    BCM_AWDL_ROLE_NE_MASTER = 1,
    BCM_AWDL_ROLE_MASTER = 2
};

enum {
    BCM_AWDL_OPMODE_AUTO = 0,
    BCM_AWDL_OPMODE_FIXED = 1
};

// 7.x awdl_config_params_t
struct bcm_awdl_config {
    uint8_t channel, guard_time;
    uint16_t aw_period, aw_cmn_length, action_frame_period, pkt_lifetime_ms, max_no_master, ext_count, aw_ext_length;
    uint16_t nmode;   // 0 legacy, 1 11n rates only, 2 11n + A-MPDU
    uint8_t dest[6];  // where the firmware sends PSF/MIF (broadcast)
};

size_t bcm_awdl_config_encode(uint8_t *out, const struct bcm_awdl_config *c);
size_t bcm_u32(uint8_t *out, uint32_t v);
// awdl_sync_params_t: type 0, param_len 36, master channel, AW period 16 TU, action frame period 110 TU, AW extension
// and common lengths 16 TU (the series' bytes with master_channel 6).
size_t bcm_awdl_sync_params(uint8_t *out, uint8_t master_channel);
// awdl_opmode_t, fixed mode (the host elects): role master with our address, or slave with the master's.
size_t bcm_awdl_opmode(uint8_t *out, uint8_t role, const uint8_t master[6]);
// awdl_election_tree_info_t with the series' thresholds (self metric 65, master RSSI boost 5, edge sync -70 dBm,
// higher masters 1/3, depth 4) and the top master, its metric and our distance to it written in the "read-only" tail.
size_t bcm_awdl_election_tree(uint8_t *out, const uint8_t top_master[6], uint32_t top_metric, uint8_t distance);
#define BCM_AWDL_SELF_METRIC 65u
// awdl_peer_op: add (flags byte 1, then the peer's HT/VHT capability elements, padded to 0x25) or delete (9 bytes).
size_t bcm_awdl_peer_op(uint8_t *out, size_t size, uint8_t opcode, const uint8_t addr[6], const uint8_t *ies,
                        size_t ies_length);
// An HT capabilities element (45) from what a peer's AWDL HT TLV carries: capability info, A-MPDU parameters and its
// receive MCS bitmask (MCS 0-7, 8-15).
size_t bcm_ht_cap_ie(uint8_t *out, uint16_t cap_info, uint8_t ampdu, const uint8_t mcs[2]);
// awdl_payload: the series' u16 length (interface_create v3) or the 7.x u32, then our TLVs.
size_t bcm_awdl_payload(uint8_t *out, size_t size, int u16_length, const uint8_t *tlvs, size_t length);
// awdl_oob_af_auto: awdl_oob_af_params_auto_t {tx_chan_map all, tx_aws_offset 16600 us, AWDL BSSID, broadcast,
// channel 0, dwell 0, flags FILL_SYNC|FILL_ELECT, lifetime 0, rate 12 (6 Mb/s), retries 0}, u16 length, our TLVs.
size_t bcm_awdl_oob_auto(uint8_t *out, size_t size, const uint8_t *tlvs, size_t length);
// awdl_psf_dwell: on (the start) or all zero (5 s later).
size_t bcm_awdl_psf_dwell(uint8_t *out, int on);
// 7.x awdl_channel_sequence_t: entries - 1 (as on the air), operating-class encoding, step 3, fill 0xffff.
struct awdl_chan;
size_t bcm_awdl_chan_seq(uint8_t *out, size_t size, const struct awdl_chan *seq, unsigned slots);
// slot_bss (interface_create v3 only): the series' bytes for a chip without SDB (byte 0x0c = 9, every slot in slice 0,
// as it sends the BCM4364). Its slots name chanspecs high byte first (0xe22a = 44 in its 80 MHz block, 0x1006 = 6/20)
// and leave the infra slots empty; the firmware fills them with the station's channel. `slots44`: bit n puts 44 in
// slot n (slot 8 is always 6), in slice 0 and in the advertised sequence at 0x1e; AWDL_SLOTS_TRACE gives the trace.
size_t bcm_slot_bss(uint8_t *out, uint16_t slots44);
// The d11ac chanspec of an AWDL channel (0 for an empty slot): 2.4 GHz 20 MHz, 5 GHz 20 or 80 MHz.
uint16_t bcm_chanspec_of(struct awdl_chan c);
// The primary channel number of a Broadcom chanspec (bits 0-7 for 20 MHz; the primary inside a 40/80 MHz block).
uint8_t bcm_chanspec_primary(uint16_t chanspec);

// ---- the data path on the AWDL interface -------------------------------------------------------------------------
// The firmware carries AWDL data as 802.3 frames: DA, SA, length, then LLC/SNAP aa aa 03 00 17 f2 08 00, the AWDL
// data header 03 04, le16 sequence, 00 00, and the original ethertype (be16).
enum {
    BCM_AWDL_DATA_HDR_LEN = 16
};

// Ethernet frame -> the 802.3 frame for the firmware. Returns its length (0 if malformed or `size` is too small).
size_t bcm_awdl_encap(const uint8_t *eth, size_t length, uint16_t seq, uint8_t *out, size_t size);
// The 802.3 frame from the firmware -> Ethernet. Returns its length, 0 for frames that are not AWDL data.
size_t bcm_awdl_decap(const uint8_t *frame, size_t length, uint8_t *out, size_t size, uint16_t *seq);

// ---- event payloads ----------------------------------------------------------------------------------------------------
struct bcm_if_event {  // WLC_E_IF: wl_event_data_if_t {ifidx, opcode (1 add, 2 del, 3 change), flags, bssidx, role}
    uint8_t ifidx, opcode, flags, bssidx, role;
};

int bcm_if_event_decode(const uint8_t *data, size_t len, struct bcm_if_event *out);

struct bcm_rx_frame {  // brcmf_rx_mgmt_data, big endian: version, chanspec, rssi, mactime, rate
    uint16_t version, chanspec;
    int32_t rssi;
    uint32_t mactime, rate;
};

// A received AWDL action frame (WLC_E_ACTION_FRAME_RX, WLC_E_AWDL_EVENT, WHD's WLC_E_AWDL_RX_ACT_FRAME): the header
// (be16 version 1: 16 bytes, 2: 24), then the frame body from the category (7f 00 17 f2 08 ...). A whole 802.11
// frame after the header, or alone, is accepted too (*body then skips its 24 bytes). *body points into data.
// 0, or -1 when this is not an AWDL action frame.
int bcm_awdl_rx_af(const uint8_t *data, size_t len, struct bcm_rx_frame *meta, const uint8_t **body, size_t *body_len);

#ifdef __cplusplus
}
#endif

#endif
