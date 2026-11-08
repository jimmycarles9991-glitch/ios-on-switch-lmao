// SPDX-License-Identifier: BSD-2-Clause-Patent
// AWDL (Apple Wireless Direct Link) protocol core: the on-air frame format, the availability-window timing model,
// master election, the peer table and the data path. Portable C11 with no OS calls: the kernel (SwitchWLANAwdl.cpp)
// and the host tests (awdl/test) drive it through struct awdl_host and the entry points below.
//
// Written from the published descriptions of the protocol: Stute, Kreitschmann, Hollick, "One Billion Apples' Secret
// Sauce: Recipe for the Apple Wireless Direct Link Ad hoc Protocol" (MobiCom 2018) and the AWDL field definitions of
// Wireshark's dissector, cross-checked against the field lists iPadOS 21H461's own wifip2pd carries in its Swift
// metadata (AWDL-CONTRACT.md §4). What goes on the air (field values, TLV order, the sync counter, the election order)
// follows OWL (seemoo-lab/owl), the implementation shown to interoperate with Apple devices. OWL is GPL-3.0 and this
// project GPL-2.0-only, so none of its code is used: its behaviour is matched and its frame layouts are the host
// tests' reference bytes.
//
// Not thread safe: the caller serialises every call on one lock. No allocation: struct awdl_core is one block the
// caller provides (awdl_core_size()). No floating point (kernel code is -mgeneral-regs-only).
#ifndef AWDL_H
#define AWDL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- constants of the protocol -------------------------------------------------------------------------------------
#define AWDL_TU_US             1024u  // one time unit
#define AWDL_AW_TU             16u    // availability window
#define AWDL_EAW_AWS           4u     // one channel-sequence slot (extended AW) = 4 AWs = 64 TU
#define AWDL_SEQ_SLOTS         16u    // channel sequence length; one sequence = 1024 TU
#define AWDL_EAW_TU            64u    // AWDL_AW_TU * AWDL_EAW_AWS
#define AWDL_PSF_INTERVAL_TU   110u   // period of PSFs
#define AWDL_MAX_PEERS         32u
#define AWDL_PEER_TIMEOUT_US   3000000u  // a peer silent this long is lost
#define AWDL_ELECTION_MAX_HOPS 10u
#define AWDL_TXQ_FRAMES        32u
#define AWDL_MAX_FRAME         1600u

extern const uint8_t awdl_bssid[6];  // 00:25:00:ff:94:73, address 3 of every AWDL frame
extern const uint8_t awdl_oui[3];    // 00:17:f2 (Apple)

// Action frame subtypes and TLV types (Stute et al. §4; Wireshark's awdl dissector)
enum {
    AWDL_SUBTYPE_PSF = 0,
    AWDL_SUBTYPE_MIF = 3
};

enum {
    AWDL_TLV_SERVICE_REQUEST = 1,
    AWDL_TLV_SERVICE_RESPONSE = 2,
    AWDL_TLV_SYNC_PARAMS = 4,
    AWDL_TLV_ELECTION_PARAMS = 5,
    AWDL_TLV_SERVICE_PARAMS = 6,
    AWDL_TLV_HT_CAPS = 7,
    AWDL_TLV_DATA_PATH_STATE = 12,
    AWDL_TLV_ARPA = 16,
    AWDL_TLV_VHT_CAPS = 17,
    AWDL_TLV_CHANNEL_SEQUENCE = 18,
    AWDL_TLV_SYNC_TREE = 20,
    AWDL_TLV_VERSION = 21,
    AWDL_TLV_ELECTION_PARAMS_V2 = 24,
};

static inline uint32_t awdl_tlv_bit(unsigned type)
{
    return 1u << type;
}

// Channel sequence encodings
enum {
    AWDL_CHAN_ENC_SIMPLE = 0,
    AWDL_CHAN_ENC_LEGACY = 1,
    AWDL_CHAN_ENC_OPCLASS = 3
};

// Data path state flags (subset this stack writes and reads)
#define AWDL_DPS_INFRA_INFO         0x0001u
#define AWDL_DPS_INFRA_ADDRESS      0x0002u
#define AWDL_DPS_AWDL_ADDRESS       0x0004u
#define AWDL_DPS_UMI                0x0010u
#define AWDL_DPS_COUNTRY_CODE       0x0100u
#define AWDL_DPS_SOCIAL_CHANNEL_MAP 0x0200u
#define AWDL_DPS_EXTENSION          0x8000u

// One channel: the channel number and the operating class (0x51: 2.4 GHz 20 MHz; 0x73: 5 GHz 20 MHz; 0x80: 5 GHz
// 80 MHz). Apple names an 80 MHz slot by its primary channel ({149, 0x80}, {44, 0x80}); a centre index (155, 42) is read
// as its block's AWDL primary.
struct awdl_chan {
    uint8_t number;
    uint8_t opclass;
};

uint16_t awdl_chan_freq(struct awdl_chan c);  // MHz of the primary 20 MHz channel; 0 for an empty slot
uint8_t awdl_chan_primary(struct awdl_chan c);
uint8_t awdl_chan_centre80(uint8_t primary);  // the 80 MHz block's centre channel of a 5 GHz primary; 0 for 2.4 GHz
// The global operating class (IEEE 802.11 Annex E, table E-4) of a 20 MHz channel: 81 (0x51) for 1-13, 82 for 14,
// 115 (0x73) for 36-48, 118 for 52-64, 121 for 100-144, 124 (0x7c) for 149-161, 125 for 165-177; 0 for anything else.
uint8_t awdl_opclass_20mhz(uint8_t channel);
// The AWDL channel of a frequency at 20 MHz ({0, 0} for 0 or an unknown one).
struct awdl_chan awdl_chan_of_freq(uint16_t mhz);

// ---- decoded TLVs ----------------------------------------------------------------------------------------------------
struct awdl_chanseq {
    uint8_t count;  // number of entries (1..16)
    uint8_t encoding, duplicate_count, step_count;
    uint16_t fill_channel;
    struct awdl_chan chan[AWDL_SEQ_SLOTS];
};

struct awdl_sync_params {
    uint8_t tx_channel;
    uint16_t tx_counter;  // TU until the sender's next EAW starts, 1..64 (wifip2pd: "downCounter")
    uint8_t master_channel, guard_time;
    uint16_t aw_period, af_period, flags, aw_ext_length, aw_cmn_length, aw_remaining;
    uint8_t min_ext, max_ext_mcast, max_ext_ucast, max_ext_af;
    uint8_t master[6];
    uint8_t presence_mode;
    uint16_t aw_seq;  // the sender's current AW sequence number (a multiple of 4 starts each EAW)
    int16_t ap_beacon_delta;
    struct awdl_chanseq seq;
    uint8_t has_seq;
};

struct awdl_election_v1 {
    uint8_t flags;
    uint16_t id;
    uint8_t distance;
    uint8_t master[6];
    uint32_t master_metric, self_metric;
};

struct awdl_election_v2 {
    uint8_t root[6], parent[6];
    uint32_t root_counter, hops, root_metric, self_metric, id, flags, self_counter;
};

struct awdl_data_path_state {
    uint16_t flags;
    char country[3];
    uint16_t social_channel_map;
    uint8_t infra_bssid[6];
    uint16_t infra_channel;
    uint8_t infra_address[6], awdl_address[6];
};

// Everything one action frame carried that the stack uses.
struct awdl_af {
    uint8_t subtype;
    uint8_t version;
    uint32_t phy_tx_time, target_tx_time;
    uint8_t src[6], dst[6];
    uint32_t present;  // bit n: TLV type n was decoded (types < 32)
    struct awdl_sync_params sync;
    struct awdl_election_v1 election;
    struct awdl_election_v2 election2;
    struct awdl_chanseq chanseq;
    struct awdl_data_path_state dps;
    uint16_t service_update_indicator;
    uint8_t awdl_version, device_class;
    char hostname[64];
    uint16_t ht_caps;  // HT TLV: capability info, A-MPDU parameters, receive MCS bitmask (MCS 0-7, 8-15)
    uint8_t ht_ampdu, ht_mcs[2];
};

// Parse an 802.11 frame (header + body, no FCS). 0: an AWDL action frame, decoded into *a. Negative: not AWDL or
// malformed (each TLV is bounds-checked; unknown TLVs are skipped).
int awdl_af_parse(const uint8_t *f, size_t len, struct awdl_af *a);

// ---- the stack -------------------------------------------------------------------------------------------------------
struct awdl_rx_meta {
    uint64_t tsf;  // local TSF at the start of the frame, µs
    uint16_t freq_mhz;
    int8_t rssi;
};

// What the integration provides. All calls are made with the caller's lock held and must not block or re-enter the
// stack; queue work for later if needed.
struct awdl_host {
    void *ctx;
    // One 802.11 frame (no FCS) to send on freq_mhz as soon as the radio can (its timing fields are stamped for the
    // moment it was built); the radio then stays on freq_mhz until local TSF `stay_until` (0: no need) so the peers'
    // frames of that slot reach us. A radio that cannot hop makes each such stay one excursion from its home channel.
    // no_ack: group/action frames. Returns 0 when queued.
    int (*tx)(void *ctx, const uint8_t *frame, size_t len, uint16_t freq_mhz, uint64_t stay_until, int no_ack);
    // The radio runs this sequence of 16 slots of 64 TU, slot 0 starting at anchor_tsf (a multiple of 1024 TU after
    // it). Called when the sequence or the synchronisation changes.
    void (*schedule)(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor_tsf);
    // A peer became reachable for data (present != 0) or was lost.
    void (*peer)(void *ctx, const uint8_t addr[6], int present, int8_t rssi);
    // A received data frame for awdl0, as an Ethernet frame (destination, source, ethertype, payload).
    void (*deliver)(void *ctx, const uint8_t *eth, size_t len);
    // One line of log, already formatted. May be NULL.
    void (*log)(void *ctx, const char *line);
    // Native backend only (the firmware runs AWDL): one firmware variable on firmware interface `ifidx` (0 the station,
    // else the AWDL interface's index from its WLC_E_IF), a set or (get != 0) a get carrying `buf` as its request
    // (interface_create). Called with the lock held: queue it and send it in order from a thread, then report the
    // firmware's answer with awdl_core_fw_result. 0 when queued.
    int (*iovar)(void *ctx, int32_t ifidx, int get, const char *name, const void *buf, size_t len);
    // Native backend only: an Ethernet frame for the firmware's AWDL interface. 0 when queued.
    int (*tx_eth)(void *ctx, const uint8_t *eth, size_t len);
};

struct awdl_config {
    uint8_t addr[6];    // our AWDL address (awdl0's link-layer address)
    char hostname[32];  // for the arpa TLV, without ".local"
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    uint32_t self_metric;  // election metric; low so a nearby Apple device stays master
    uint8_t awdl_version;  // e.g. 0x34 = 3.4
    uint8_t device_class;  // 1 macOS, 2 iOS
    uint8_t ht_streams;    // receive MCS bytes in the HT TLV (the BCM4356 is 2x2)
    char country[3];
    // Who runs the protocol. NATIVE (the default on this chip): the firmware's own AWDL does sync, election, PSF/MIF
    // and the AWDL data encapsulation; the core configures it with iovars, learns peers from the action frames the
    // firmware forwards and adds them for data. SOFT: the core does everything over monitor + injection.
    uint8_t backend;
    // Native: which firmware generation the bring-up probe found (enum bcm_awdl_api) and the probe items it lacks
    // (bit n: bcm_probe_item n answered UNSUPPORTED; those iovars are not sent).
    uint8_t fw_api;
    uint32_t fw_absent;
};

enum {
    AWDL_BACKEND_SOFT = 0,
    AWDL_BACKEND_NATIVE = 1
};

void awdl_config_default(struct awdl_config *c, const uint8_t addr[6]);

enum awdl_role {
    AWDL_ROLE_OFF,
    AWDL_ROLE_ROOT,
    AWDL_ROLE_FOLLOWER
};

struct awdl_peer {
    uint8_t addr[6];
    uint8_t used, reachable, has_v2, has_seq;
    uint64_t first_seen, last_seen;  // local TSF
    int8_t rssi;
    uint8_t awdl_version, device_class;
    struct awdl_election_v2 election;
    struct awdl_sync_params sync;
    struct awdl_chanseq seq;
    uint64_t next_aw_tsf;  // the peer's next AW start, local TSF, from its last sync params
    uint16_t next_aw_seq;
    uint16_t last_data_seq;
    uint8_t has_data_seq;
    char hostname[64];
    uint32_t frames;
    uint8_t has_ht, ht_ampdu, ht_mcs[2];
    uint16_t ht_caps;
};

struct awdl_stats {
    uint32_t af_rx, af_bad, af_tx, psf_tx, mif_tx, data_rx, data_tx, data_bad, data_drop, tx_refused, resyncs;
    int32_t last_sync_error_us;
    uint32_t fw_events, fw_unknown, iovars, iovar_refused, peer_adds, peer_dels, aws;
    uint32_t iovar_skipped, data_not_awdl, roles, syncs_pushed, fw_frames, mode_changes;
    int32_t fw_role;  // the firmware's last WLC_E_AWDL_ROLE status (-1 none yet)
};

struct awdl_core;
size_t awdl_core_size(void);
int awdl_core_init(struct awdl_core *c, const struct awdl_host *host, const struct awdl_config *cfg);
// Start at local TSF `now` (we are root until we hear a better one), stop (every reachable peer reported lost).
void awdl_core_start(struct awdl_core *c, uint64_t now);
void awdl_core_stop(struct awdl_core *c);
int awdl_core_running(const struct awdl_core *c);
void awdl_core_set_addr(struct awdl_core *c, const uint8_t addr[6]);
void awdl_core_set_sequence(struct awdl_core *c, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t now);
// A received 802.11 frame (action or data).
void awdl_core_rx(struct awdl_core *c, const uint8_t *frame, size_t len, const struct awdl_rx_meta *m);
// An Ethernet frame from awdl0. 0: sent or queued for the next availability window; negative: dropped.
int awdl_core_output(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now);
// Run what is due at `now`; returns the local TSF of the next deadline (the caller arms one timer for it).
uint64_t awdl_core_timer(struct awdl_core *c, uint64_t now);
// Native backend: one firmware event (awdl_bcm.h numbers) with its payload; `now` is the host's TSF estimate.
struct awdl_fw_event;
void awdl_core_fw_event(struct awdl_core *c, const struct awdl_fw_event *e, const uint8_t *data, size_t len,
                        uint64_t now);
// Native backend: the firmware's answer to a request the host queued (0, or its BCME code; AWDL_FW_NO_ANSWER when the
// request never reached it). A refused interface creation ends the session (one log line); a refused awdl_payload
// is resent once with the other length form.
#define AWDL_FW_NO_ANSWER (-32768)
void awdl_core_fw_result(struct awdl_core *c, const char *name, int status, uint64_t now);
// Native backend: a frame the firmware delivered on its AWDL interface. AWDL data (802.3 with the AWDL LLC/SNAP
// header) is decapsulated and handed to host.deliver as Ethernet; it refreshes the peer. Anything else is counted.
void awdl_core_rx_eth(struct awdl_core *c, const uint8_t *frame, size_t len, uint64_t now);
// The access point's frequency while the station is associated (0 otherwise). The schedule interleaves it
// (awdl_schedule); a change while running re-sends the schedule (native) or re-installs it (soft).
void awdl_core_set_infra(struct awdl_core *c, uint16_t infra_mhz, uint64_t now);
// The AWDL interface's firmware index once its WLC_E_IF arrived, -1 before.
int awdl_core_fw_ifidx(const struct awdl_core *c);

// Both backends' schedule follows the AWDL states macOS was measured in (Stute, Kreitschmann, Hollick, MobiCom 2018,
// table 2; p the 5 GHz social channel, s channel 6, i the access point's):
//   idle (enabled, discoverable, browsing, no transfer): "Low Power", p in slots 0, 9, 10 and s in 8, the least any
//     Apple device was seen to keep; the other 12 slots are the access point's;
//   transfer while associated: "Data+Infra 75 %", p in 0-5 and 9-13, s in 8; the access point keeps 6, 7, 14, 15
//     (the paper: "at least 25 % of the time is reserved for the AP connection if the node is connected to an AP");
//   transfer while unassociated: "Data", every slot AWDL's.
// Bit n: slot n carries the 5 GHz social channel (slot 8 always carries channel 6).
enum awdl_mode {
    AWDL_MODE_IDLE = 0,
    AWDL_MODE_TRANSFER = 1
};

#define AWDL_SLOTS_IDLE           0x0601u  // 0, 9, 10
#define AWDL_SLOTS_TRANSFER_INFRA 0x3e3fu  // 0-5, 9-13
#define AWDL_SLOTS_TRANSFER_ALL   0xfeffu  // all but 8
#define AWDL_SLOTS_TRACE 0x1e1fu  // the brcmfmac series' slot_bss: 0-4, 9-12 (macOS 15, AirDrop open in Finder)
uint16_t awdl_slot_mask(uint16_t infra_mhz, enum awdl_mode mode);
// The 16-slot schedule for a mode: the mask's slots on the 5 GHz social channel, slot 8 on channel 6, the rest on the
// infra channel while associated (the radio stays with the access point there) and empty otherwise. The 5 GHz social
// channel is the infra channel when that is 44 or 149 (then only slot 8 leaves it), else `social5` (44 or 149, learnt
// from the master's sequence), else both until a master is heard (149 before slot 8, 44 after). 5 GHz social slots
// are 80 MHz ({44, 0x80}, {149, 0x80}); the infra channel is 20 MHz with its global operating class.
void awdl_schedule(struct awdl_chan out[AWDL_SEQ_SLOTS], uint16_t infra_mhz, uint8_t social5, enum awdl_mode mode);
// Slots of a schedule away from the infra channel (unassociated: the slots AWDL uses at all).
unsigned awdl_slots_away(const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint16_t infra_mhz);
// Transfer mode starts on the first unicast TCP frame on awdl0 (AirDrop's TLS), either way, or when wifip2pd
// registers AWDL traffic (AWDL_PEER_TRAFFIC_REGISTRATION), and ends this long after the last such frame (a deadline
// each frame moves, not a poll). One log line per change, with the slots away from infra.
#define AWDL_TRANSFER_HOLD_US 3000000u
void awdl_core_traffic_registered(struct awdl_core *c, const char *who, uint64_t now);
enum awdl_mode awdl_core_mode(const struct awdl_core *c);
// The data path state TLV's social channel map for a schedule: 0x1 channel 6, 0x2 channel 44, 0x4 channel 149.
uint16_t awdl_social_map(const struct awdl_chan seq[AWDL_SEQ_SLOTS]);
// The TLVs our MIFs carry beyond sync and election (service params, HT caps, arpa, data path state, version): the
// "awdl_payload" the firmware appends to its frames in the native backend. Returns the length or 0.
size_t awdl_core_build_payload(struct awdl_core *c, uint8_t *buf, size_t size);

// Inspection
enum awdl_role awdl_core_role(const struct awdl_core *c);
const struct awdl_election_v2 *awdl_core_election(const struct awdl_core *c);
const struct awdl_peer *awdl_core_peer(const struct awdl_core *c, unsigned index);  // NULL past the end / unused
const struct awdl_peer *awdl_core_find(const struct awdl_core *c, const uint8_t addr[6]);
const struct awdl_stats *awdl_core_stats(const struct awdl_core *c);
const struct awdl_chan *awdl_core_sequence(const struct awdl_core *c);  // AWDL_SEQ_SLOTS entries
// The timing model: AW sequence number at `now`, start of AW `seq`, channel at `now`.
uint16_t awdl_core_aw_seq(const struct awdl_core *c, uint64_t now);
uint64_t awdl_core_aw_start(const struct awdl_core *c, uint16_t seq, uint64_t now);
struct awdl_chan awdl_core_channel_at(const struct awdl_core *c, uint64_t now);

// ---- frame building (exposed for tests) ------------------------------------------------------------------------------
// Our PSF or MIF stamped for `now` (OWL's TLV order: sync, election, channel sequence, election v2, service params,
// then for a MIF HT capabilities and arpa, then data path state and version) into buf; its length (0 if no room).
size_t awdl_core_build_af(struct awdl_core *c, uint8_t subtype, uint64_t now, uint8_t *buf, size_t size);

// ---- data path -------------------------------------------------------------------------------------------------------
// Ethernet frame -> AWDL 802.11 data frame (src = our address). Returns the length or 0.
size_t awdl_data_encap(const uint8_t *eth, size_t len, const uint8_t self[6], uint16_t seq, uint8_t *out, size_t size);

// One MSDU of an AWDL data frame, pointing into the frame.
struct awdl_msdu {
    const uint8_t *dst, *src, *payload;
    uint16_t ethertype;
    size_t len;
};

#define AWDL_MAX_MSDUS 8
// An AWDL data frame (data or QoS data, plain or A-MSDU) -> its MSDUs. Returns how many (0: not AWDL data or malformed;
// one bad subframe rejects the frame). *seq gets the first MSDU's AWDL sequence number, *retry the 802.11 retry bit.
size_t awdl_data_msdus(const uint8_t *frame, size_t len, struct awdl_msdu *out, size_t max, uint16_t *seq, int *retry);
size_t awdl_msdu_ether(const struct awdl_msdu *m, uint8_t *out, size_t size);  // Ethernet frame length, 0 if no room
// AWDL 802.11 data frame -> Ethernet frame of its first MSDU. Returns the length or 0.
size_t awdl_data_decap(const uint8_t *frame, size_t len, uint8_t *out, size_t size, uint16_t *seq, int *retry);

// ---- TSF clock: the radio's TSF from the host clock between reads ----------------------------------------------------
struct awdl_tsf_clock {
    uint64_t tsf0, host0_ns;
    int32_t ppm_x16;  // radio TSF rate relative to the host clock, in 1/16 ppm
    uint8_t valid;
};

void awdl_tsf_clock_anchor(struct awdl_tsf_clock *k, uint64_t tsf, uint64_t host_ns);
uint64_t awdl_tsf_clock_now(const struct awdl_tsf_clock *k, uint64_t host_ns);
uint64_t awdl_tsf_clock_host(const struct awdl_tsf_clock *k, uint64_t tsf);  // host ns at which the TSF reaches tsf

#ifdef __cplusplus
}
#endif

#endif
