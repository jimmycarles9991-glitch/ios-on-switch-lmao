// SPDX-License-Identifier: BSD-2-Clause-Patent
// awdl0 as iPadOS 21H461's wifip2pd sees it: the Apple80211 request payloads it sends for AWDL, the events it
// reads back, and the IO80211 user-client event ring they travel through. Portable C (host tests run it); the kernel
// side (SwitchWLANAwdl.cpp) does the copyin/copyout, the IOUserClient and the ifnet. Layouts and numbers are from the
// 21H461 binaries (AWDL-CONTRACT.md): natural C alignment, little endian.
#ifndef AWDL_APPLE80211_H
#define AWDL_APPLE80211_H

#include "awdl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- request and event numbers (IO80211.framework gAppleIoucIndexToString / gAppleMIndexToString) -----------------
enum {
    A80211_IOC_SSID = 1,
    A80211_IOC_CHANNEL = 4,
    A80211_IOC_BSSID = 9,
    A80211_IOC_CARD_CAPABILITIES = 12,
    A80211_IOC_STATE = 13,
    A80211_IOC_OP_MODE = 15,
    A80211_IOC_POWER = 19,
    A80211_IOC_DRIVER_VERSION = 43,
    A80211_IOC_HARDWARE_VERSION = 44,
    A80211_IOC_COUNTRY_CODE = 51,
    A80211_IOC_VIRTUAL_IF_CREATE = 94,
    A80211_IOC_VIRTUAL_IF_DELETE = 95,
    A80211_IOC_VIRTUAL_IF_ROLE = 96,
    A80211_IOC_VIRTUAL_IF_PARENT = 97,
    A80211_IOC_AWDL_SYNC_ENABLED = 117,
    A80211_IOC_AWDL_PEER_SERVICE_REQUEST = 120,
    A80211_IOC_AWDL_SYNC_CHANNEL_SEQUENCE = 129,
    A80211_IOC_AWDL_STRATEGY = 148,
    A80211_IOC_AWDL_CTL = 162,
    A80211_IOC_AWDL_SOCIAL_TIME_SLOTS = 163,
    A80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION = 164,
    A80211_IOC_CHANNELS_INFO = 207,
    A80211_IOC_AWDL_OPER_MODE = 217,
    A80211_IOC_AWDL_STATISTICS = 223,
    A80211_IOC_AWDL_ADVERTISERS = 229,
    A80211_IOC_AWDL_PREFERRED_CHANNELS = 244,
    A80211_IOC_COMPANION_SKYWALK_IF_CREATE = 293,
    A80211_IOC_AWDL_LOW_LATENCY_STATISTICS = 343,
    A80211_IOC_LOWLATENCY_STATISTICS = 364,
    A80211_IOC_USERSPACE_P2P_OPTIONS = 389,
    A80211_IOC_POST_PRESENCE_DONE = 415,
    A80211_IOC_AWDL_UMI_TX_PEER = 417,
    A80211_IOC_GATEWAY_ADDRESS = 450,
    A80211_IOC_UNICAST_INFRA_PEER_ADDRESS = 451,
};

enum {
    A80211_M_LINK_CHANGED = 4,
    A80211_M_PEER_STATE = 40,
    A80211_M_AWDL_SYNC_STATE_CHANGED = 46,
    A80211_M_PEER_PRESENCE = 47,
    A80211_M_INTERFACE_STATE = 58,
    A80211_M_AWDL_CHAN_SEQ_CHANGED = 195,
};

enum {
    A80211_CAP_AWDL = 32,
    A80211_CAP_NAN = 72
};  // CARD_CAPABILITIES bit numbers wifip2pd tests

enum {
    A80211_VIF_ROLE_AWDL = 6,
    A80211_VIF_ROLE_NAN = 9,
    A80211_VIF_ROLE_NAN_DATA = 10
};

// apple80211_peer_presence_indication.state: which value means "present" is not settled by the binary (STATUS.md)
enum {
    A80211_PEER_ABSENT = 0,
    A80211_PEER_PRESENT = 1
};

// ---- payloads (wifip2pd's Swift metadata names; sizes checked against its call sites) ------------------------------
struct a80211_channel {
    uint32_t version, channel, flags;
};

struct a80211_virt_if_create {  // 32
    uint32_t unit;
    uint8_t mac[6];
    uint16_t reserved;
    uint32_t role;
    char bsd_name[16];
};

struct a80211_virt_if_delete {  // 20
    uint32_t version;
    char name[16];
};

struct a80211_awdl_sync_enabled {  // 12
    uint32_t version, flags, enabled;
};

struct a80211_awdl_strategy {  // 24
    uint32_t version, strategy, param;
    uint64_t last_state_change;
};

struct a80211_awdl_oper_mode {
    uint32_t version, mode;
};

struct a80211_awdl_social_time_slots {
    uint32_t version;
    uint8_t enabled;
};

struct a80211_awdl_preferred_channels {
    uint32_t version;
    uint16_t preferred, master, secondary_preferred;
};

struct a80211_awdl_sync_channel_sequence {  // 400
    uint32_t version;
    uint8_t flags, length, encoding, step_count, duplicate_count, fill_chan_num;
    struct a80211_channel channel[32];
    uint32_t strategy;
};

struct a80211_awdl_peer_traffic_registration {  // 184
    uint32_t version;
    uint8_t address[6];
    uint32_t unique_id_length;
    uint8_t unique_id[128];
    uint32_t desired_latency, desired_bandwidth;
    char ifname[16];
    uint32_t active, rt_peer_indication;
    uint16_t desired_channel, desired_sec_channel, port;
};

struct a80211_peer_service_request {  // 20 + data
    uint32_t version;
    uint8_t peer[6];
    uint8_t delete_request, aw_offset;
    uint32_t token;
    uint16_t data_len, expect_fragmented_response;
};

struct a80211_awdl_peer_advert {  // 44: awdl_peer_advert_add
    uint16_t aw_counter, tx_counter, tx_delay, period_tu, aw_cmn_length, aw_ext_length, aw_period;
    int8_t rssi;
    uint8_t guard_time, presence_mode, age, dist_top;
    uint32_t ms, self_metrics, top_master_metrics;
    uint8_t top_master[6], addr[6];
};

struct a80211_userspace_p2p_options {  // 16
    uint32_t version;
    uint64_t flags;
};

struct a80211_ppp_done {  // 12
    uint32_t version;
    uint8_t peer[6];
};

struct a80211_umi_data {  // 16
    uint32_t version, option;
    uint8_t peer[6];
};

struct a80211_peer_presence {  // 68
    uint8_t address[6];
    uint16_t handle;
    uint32_t state;
    int32_t rssi, link_quality;
    uint8_t opaque[48];
};

// ---- the IO80211 user-client event ring (IO80211 _Apple80211EventMonitoringHelper) ---------------------------------
#define A80211_RING_MAGIC_EVENT 0x57u
#define A80211_RING_MAGIC_PAD   0x50u
#define A80211_RING_HEADER      0x30u
#define A80211_RING_MAX_PAYLOAD 0xff0u

struct a80211_ring_header {
    uint32_t magic;
    uint8_t reserved[0x14];
    uint16_t payload_len, pad_len;
    uint32_t event;
    char ifname[16];
};

struct a80211_ring {
    uint8_t *buf;              // the region userland maps read-only (memory type 1)
    uint32_t size;             // a multiple of 4
    volatile uint32_t *state;  // the region userland maps read-write (type 2): [0] read offset, [1] write offset
    uint32_t posted, dropped;
};

int a80211_ring_init(struct a80211_ring *r, void *buf, uint32_t size, volatile uint32_t *state);
// Append one event. 0, or -1 when it does not fit (the event is dropped and counted: userland is not reading).
int a80211_ring_post(struct a80211_ring *r, uint32_t event, const char *ifname, const void *payload, uint32_t len);
// The consumer, as IO80211 implements it (for tests): 1 and one event, 0 when empty, -1 on a malformed ring.
int a80211_ring_take(struct a80211_ring *r, uint32_t *event, char ifname[16], void *payload, uint32_t *len,
                     uint32_t capacity);

// ---- awdl0's request handler ------------------------------------------------------------------------------------------
struct a80211_awdl_ops {
    void *ctx;
    int (*start)(void *ctx);  // AWDL_SYNC_ENABLED on: start the stack (radio open, schedule); 0 or an errno
    void (*stop)(void *ctx);
    // an event for awdl0's ring (the integration adds the interface name and wakes the reader)
    void (*post)(void *ctx, uint32_t event, const void *payload, uint32_t len);
    void (*log)(void *ctx, const char *line);
};

struct a80211_awdl {
    struct awdl_core *core;
    struct a80211_awdl_ops ops;
    char ifname[16], parent[16];
    uint8_t caps[12];  // CARD_CAPABILITIES bitmap as en0 answers it (plus bit 32)
    uint32_t sync_flags, strategy, strategy_param, oper_mode, social_time_slots;
    uint16_t preferred, master_channel, secondary_preferred;
    uint64_t userspace_p2p_flags;
    uint32_t traffic_registrations;
    uint8_t enabled;
    uint16_t presence_handle;
    uint32_t unknown_seen[16];  // request types answered EOPNOTSUPP, logged once each
};

void a80211_awdl_init(struct a80211_awdl *f, struct awdl_core *core, const struct a80211_awdl_ops *ops,
                      const char *ifname, const char *parent, const uint8_t caps[12]);
// One request. `data` holds `*len` bytes (copied in for a set; room for `capacity` bytes for a get, *len updated);
// *val is req_val. Returns 0 or an errno (EOPNOTSUPP for what awdl0 does not do).
int a80211_awdl_request(struct a80211_awdl *f, int set, int32_t type, int32_t *val, void *data, uint32_t *len,
                        uint32_t capacity, uint64_t now);
// The core's peer callback lands here: posts PEER_PRESENCE to wifip2pd.
void a80211_awdl_peer(struct a80211_awdl *f, const uint8_t addr[6], int present, int8_t rssi);

// awdl0 on a radio that cannot run AWDL. 21H461's sharingd force-unwraps nw_interface_create_with_name("awdl0") when
// it starts its AirDrop Bonjour server and its temporary-pairing server, and traps when awdl0 is missing, so the
// interface has to exist. It is never brought up and its link never reads active, as on an iPad whose AWDL is down;
// NECP's listener scan skips an interface without IFF_UP | IFF_RUNNING.
// BSD <net/if.h> and <net/if_media.h> values (this file builds without them); SwitchWLANAwdl.cpp asserts they match.
#define A80211_IFF_BROADCAST 0x2u
#define A80211_IFF_SIMPLEX   0x800u
#define A80211_IFF_MULTICAST 0x8000u
#define A80211_IFM_AVALID    0x1u

struct a80211_awdl_down_if {
    uint16_t if_flags;
    uint32_t mtu;
    uint8_t lladdr[6];
};

// 0, or -1 when `station` is not a unicast address. The link-layer address is the station's made locally
// administered, never equal to the station's.
int a80211_awdl_down_if(const uint8_t station[6], struct a80211_awdl_down_if *out);
int a80211_awdl_down_media_status(void);

#ifdef __cplusplus
}
#endif

#endif
