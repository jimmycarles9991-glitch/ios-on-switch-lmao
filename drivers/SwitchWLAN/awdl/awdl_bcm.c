// SPDX-License-Identifier: BSD-2-Clause-Patent
// Byte-exact encoders and decoders for the Broadcom firmware's AWDL iovars, events and data frames (awdl_bcm.h).
#include "awdl_bcm.h"
#include "awdl_bcm_trace.h"
#include "awdl_internal.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

const uint8_t bcm_awdl_events[] = {WLC_E_ACTION_FRAME_RX,    WLC_E_AWDL_AW,
                                   WLC_E_AWDL_ROLE,          WLC_E_AWDL_EVENT,
                                   WLC_E_AWDL_RX_PRB_RESP,   WLC_E_AWDL_RX_ACT_FRAME,
                                   WLC_E_AWDL_WOWL_NULLPKT,  WLC_E_AWDL_PHYCAL_STATUS,
                                   WLC_E_AWDL_OOB_AF_STATUS, WLC_E_AWDL_SCAN_STATUS,
                                   WLC_E_AWDL_AW_START,      WLC_E_AWDL_AW_END,
                                   WLC_E_AWDL_AW_EXT,        WLC_E_AWDL_PEER_CACHE_CONTROL};
const unsigned bcm_awdl_event_count = sizeof bcm_awdl_events;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- status codes -----------------------------------------------------------------------------------------------------
const char *bcme_name(int status)
{
    static const char *const names[] = {
        "OK",
        "ERROR",
        "BADARG",
        "BADOPTION",
        "NOTUP",
        "NOTDOWN",
        "NOTAP",
        "NOTSTA",
        "BADKEYIDX",
        "RADIOOFF",
        "NOTBANDLOCKED",
        "NOCLK",
        "BADRATESET",
        "BADBAND",
        "BUFTOOSHORT",
        "BUFTOOLONG",
        "BUSY",
        "NOTASSOCIATED",
        "BADSSIDLEN",
        "OUTOFRANGECHAN",
        "BADCHAN",
        "BADADDR",
        "NORESOURCE",
        "UNSUPPORTED",
        "BADLEN",
        "NOTREADY",
        "EPERM",
        "NOMEM",
        "ASSOCIATED",
        "RANGE",
        "NOTFOUND",
        "WME_NOT_ENABLED",
        "TSPEC_NOTFOUND",
        "ACM_NOTSUPPORTED",
        "NOT_WME_ASSOCIATION",
        "SDIO_ERROR",
        "DONGLE_DOWN",
        "VERSION",
        "TXFAIL",
        "RXFAIL",
        "NODEVICE",
        "NMODE_DISABLED",
        "NONRESIDENT",
        "SCANREJECT",
        "USAGE_ERROR",
        "IOCTL_ERROR",
        "SERIAL_PORT_ERR",
        "DISABLED",
    };
    if (status == BCM_PROBE_NO_ANSWER)
        return "no answer";
    if (status > 0 || -status >= (int)(sizeof names / sizeof names[0]))
        return "?";
    return names[-status];
}

// ---- the probe ----------------------------------------------------------------------------------------------------------
const char *const bcm_probe_names[BCM_PROBE_COUNT] = {
    "interface_create", "awdl_if",       "awdl",           "awdl_config",    "slot_bss",           "awdl_sync_params",
    "awdl_chan_seq",    "awdl_peer_op",  "awdl_opmode",    "mchan",          "awdl_election_tree", "awdl_payload",
    "awdl_oob_af_auto", "awdl_aftxmode", "awdl_osoc_chan", "awdl_psf_dwell", "awdl_presencemode",  "interface_remove",
    "event_msgs_ext",   "awdl_if2",
};

size_t bcm_probe_request(unsigned item, uint8_t *out, size_t size)
{
    if (item >= BCM_PROBE_COUNT)
        return 0;
    const char *name = bcm_probe_names[item];
    const size_t n = strlen(name) + 1;
    // brcmf_feat_awdl_check: brcmf_fil_bsscfg_int_query(ifp 0, "interface_create", &ver = 0); for bsscfg 0
    // brcmf_create_bsscfg drops the "bsscfg:" prefix, so it is the name and a le32 0. peer_op: INFO for no address
    // (never ADD, opcode 0, from a zero-filled buffer).
    const size_t params = item == BCM_PROBE_INTERFACE_CREATE ? 4 : item == BCM_PROBE_AWDL_PEER_OP ? 9 : 0;
    if (n + params > size)
        return 0;
    memcpy(out, name, n);
    memset(out + n, 0, params);
    if (item == BCM_PROBE_AWDL_PEER_OP)
        out[n + 1] = BCM_AWDL_PEER_OP_INFO;
    return n + params;
}

const char *const bcm_monitor_probe_names[BCM_MONITOR_PROBE_COUNT] = {"WLC_GET_MONITOR", "monitor_promisc_level",
                                                                      "sta_monitor"};

size_t bcm_monitor_probe_request(unsigned item, uint8_t *out, size_t size)
{
    if (item == BCM_MONITOR_PROBE_GET_MONITOR || item >= BCM_MONITOR_PROBE_COUNT)
        return 0;
    const size_t n = strlen(bcm_monitor_probe_names[item]) + 1;
    if (n > size)
        return 0;
    memcpy(out, bcm_monitor_probe_names[item], n);
    return n;
}

size_t bcm_actframe_request(uint8_t *out, size_t size, const uint8_t *frame, size_t len, uint16_t chanspec,
                            uint32_t dwell_ms, uint32_t packet_id)
{
    static const char name[] = "actframe";
    if (len <= BCM_80211_MGMT_HDR_LEN || len - BCM_80211_MGMT_HDR_LEN > BCM_AF_BODY_MAX)
        return 0;
    const uint16_t fc = (uint16_t)(frame[0] | frame[1] << 8);
    if ((fc & 0x00fc) != 0x00d0)  // type management, subtype action
        return 0;
    const size_t body = len - BCM_80211_MGMT_HDR_LEN, total = sizeof(name) + BCM_AF_PARAMS_HDR_LEN + body;
    if (total > size)
        return 0;
    uint8_t *p = out;
    memcpy(p, name, sizeof(name));
    p += sizeof(name);
    const uint32_t words[2] = {chanspec, dwell_ms};
    for (unsigned w = 0; w < 2; ++w) {
        for (unsigned i = 0; i < 4; ++i)
            *p++ = (uint8_t)(words[w] >> (8 * i));
    }
    memcpy(p, frame + 16, 6);  // addr3, the BSSID
    p += 6;
    *p++ = 0;
    *p++ = 0;
    memcpy(p, frame + 4, 6);  // addr1, the destination
    p += 6;
    *p++ = (uint8_t)body;
    *p++ = (uint8_t)(body >> 8);
    for (unsigned i = 0; i < 4; ++i)
        *p++ = (uint8_t)(packet_id >> (8 * i));
    memcpy(p, frame + BCM_80211_MGMT_HDR_LEN, body);
    return total;
}

void bcm_probe_record(struct bcm_awdl_probe *p, unsigned item, int status, const uint8_t *reply, size_t length)
{
    if (item >= BCM_PROBE_COUNT)
        return;
    p->status[item] = (int16_t)(status < -32768 ? -32768 : status);
    p->returned[item] = (uint16_t)(length > 0xffff ? 0xffff : length);
    if (item == BCM_PROBE_INTERFACE_CREATE)
        p->ifc_version = status == 0 && reply && length >= 4 ? le32(reply) : 0;
}

int bcm_probe_has(const struct bcm_awdl_probe *p, unsigned item)
{
    return p && p->done && item < BCM_PROBE_COUNT && p->status[item] != BCME_UNSUPPORTED &&
           p->status[item] != BCM_PROBE_NO_ANSWER;
}

uint32_t bcm_probe_absent(const struct bcm_awdl_probe *p)
{
    uint32_t absent = 0;
    for (unsigned i = 0; i < BCM_PROBE_COUNT; ++i)
        if (!bcm_probe_has(p, i))
            absent |= 1u << i;
    return absent;
}

enum bcm_awdl_api bcm_awdl_api_of(const struct bcm_awdl_probe *p)
{
    if (!p || !p->done)
        return BCM_AWDL_API_NONE;
    if (p->ifc_version == 3)
        return BCM_AWDL_API_IFC_V3;
    if (bcm_probe_has(p, BCM_PROBE_AWDL_IF) || bcm_probe_has(p, BCM_PROBE_AWDL_IF2))
        return BCM_AWDL_API_AWDL_IF;
    return BCM_AWDL_API_NONE;
}

const char *bcm_awdl_api_name(enum bcm_awdl_api api)
{
    return api == BCM_AWDL_API_IFC_V3 ? "interface_create v3" : api == BCM_AWDL_API_AWDL_IF ? "awdl_if (7.x)" : "none";
}

struct awdl_radio_verdict awdl_radio_verdict_of(int query, int abi_ok, int has_native, int can_monitor, int can_inject,
                                                const struct bcm_awdl_probe *probe)
{
    struct awdl_radio_verdict v = {AWDL_RADIO_OFF, AWDL_OFF_NOT, bcm_awdl_api_of(probe)};
    const int soft = can_monitor && can_inject;
    if (query == -EAGAIN)
        v.mode = AWDL_RADIO_UNDECIDED;
    else if (query < 0)
        v.off = AWDL_OFF_NO_RADIO;
    else if (!abi_ok)
        v.off = AWDL_OFF_ABI;
    else if (!has_native && !soft)
        v.off = AWDL_OFF_NO_BACKEND;
    else if (has_native && v.api == BCM_AWDL_API_NONE && !soft)
        v.off = AWDL_OFF_NO_API;
    else
        v.mode = has_native && v.api != BCM_AWDL_API_NONE ? AWDL_RADIO_NATIVE : AWDL_RADIO_SOFT;
    return v;
}

// ---- interface creation ------------------------------------------------------------------------------------------------
size_t bcm_ifc_v3_awdl(uint8_t *out, const uint8_t mac[6])
{
    memset(out, 0, BCM_IFC_V3_LEN);
    awdl_put16(out, 3);                       // ver
    awdl_put16(out + 2, BCM_IFC_V3_LEN);      // len
    awdl_put16(out + 4, BCM_IFC_V3_LEN);      // fixed_len
    out[6] = 2;                               // iftype WL_INTERFACE_CREATE_AWDL
    out[7] = 0;                               // wlc_index
    awdl_put32(out + 8, 0x02 | 0x08 | 0x10);  // MAC_USE | IF_INDEX_USE | BSSID_USE
    memcpy(out + 12, mac, 6);
    memcpy(out + 18, awdl_bssid, 6);
    out[24] = BCM_AWDL_IFIDX;  // if_index, then pad[3]
    return BCM_IFC_V3_LEN;
}

size_t bcm_awdl_if2(uint8_t *out, int32_t cfg_idx, int up, const uint8_t bssid[6], const uint8_t addr[6])
{
    awdl_put32(out, (uint32_t)cfg_idx);
    awdl_put32(out + 4, up ? 1u : 0u);
    memcpy(out + 8, bssid, 6);
    memcpy(out + 14, addr, 6);
    return BCM_AWDL_IF2_LEN;
}

// ---- iovar payloads ----------------------------------------------------------------------------------------------------
size_t bcm_u32(uint8_t *out, uint32_t v)
{
    awdl_put32(out, v);
    return 4;
}

size_t bcm_awdl_config_encode(uint8_t *out, const struct bcm_awdl_config *c)
{
    memset(out, 0, BCM_AWDL_CONFIG_LEN);
    awdl_put32(out, 1);  // version
    out[4] = c->channel;
    out[5] = c->guard_time;
    awdl_put16(out + 6, c->aw_period);
    awdl_put16(out + 8, c->aw_cmn_length);
    awdl_put16(out + 10, c->action_frame_period);
    awdl_put16(out + 12, c->pkt_lifetime_ms);
    awdl_put16(out + 14, c->max_no_master);
    awdl_put16(out + 16, c->ext_count);
    awdl_put16(out + 18, c->aw_ext_length);
    awdl_put16(out + 20, c->nmode);
    memcpy(out + 22, c->dest, 6);
    return BCM_AWDL_CONFIG_LEN;
}

size_t bcm_awdl_sync_params(uint8_t *out, uint8_t master_channel)
{
    memset(out, 0, BCM_AWDL_SYNC_PARAMS_LEN);
    out[0] = 0;                                     // type
    awdl_put16(out + 1, BCM_AWDL_SYNC_PARAMS_LEN);  // param_len
    out[6] = master_channel;                        // master_chan (tx_chan, tx_counter: the firmware's)
    awdl_put16(out + 8, AWDL_AW_TU);                // aw_period
    awdl_put16(out + 10, AWDL_PSF_INTERVAL_TU);     // action_frame_period
    awdl_put16(out + 14, AWDL_AW_TU);               // aw_ext_length
    awdl_put16(out + 16, AWDL_AW_TU);               // aw_cmn_length
    return BCM_AWDL_SYNC_PARAMS_LEN;
}

size_t bcm_awdl_opmode(uint8_t *out, uint8_t role, const uint8_t master[6])
{
    memset(out, 0, BCM_AWDL_OPMODE_LEN);
    out[0] = BCM_AWDL_OPMODE_FIXED;
    out[1] = role;
    if (master)
        memcpy(out + 4, master, 6);
    return BCM_AWDL_OPMODE_LEN;
}

size_t bcm_awdl_election_tree(uint8_t *out, const uint8_t top_master[6], uint32_t top_metric, uint8_t distance)
{
    memset(out, 0, BCM_AWDL_ELECTION_TREE_LEN);
    awdl_put32(out + 3, BCM_AWDL_SELF_METRIC);  // flags 0, election ID 0, self_metrics
    out[8] = 5;                                 // master_rssi_boost
    out[9] = (uint8_t)(int8_t)-70;              // edge_sync_rssi_thld
    out[12] = 1;                                // max_higher_masters_close_range
    out[13] = 3;                                // max_higher_masters_mid_range
    out[14] = 4;                                // max_tree_depth
    if (top_master)
        memcpy(out + 15, top_master, 6);
    awdl_put32(out + 21, top_metric);
    out[25] = distance;  // current_tree_depth
    return BCM_AWDL_ELECTION_TREE_LEN;
}

size_t bcm_awdl_peer_op(uint8_t *out, size_t size, uint8_t opcode, const uint8_t addr[6], const uint8_t *ies,
                        size_t ies_length)
{
    const int add = opcode == BCM_AWDL_PEER_OP_ADD;
    if (!add)
        ies_length = 0;
    if (ies_length > BCM_AWDL_PEER_IES_MAX)
        return 0;
    size_t n = BCM_AWDL_PEER_OP_HDR_LEN + ies_length;
    if (add && n < BCM_AWDL_PEER_OP_ADD_LEN)
        n = BCM_AWDL_PEER_OP_ADD_LEN;
    if (n > size)
        return 0;
    memset(out, 0, n);
    out[0] = 0;  // AWDL_PEER_OP_CUR_VER
    out[1] = opcode;
    memcpy(out + 2, addr, 6);
    out[8] = (uint8_t)add;
    if (ies_length)
        memcpy(out + BCM_AWDL_PEER_OP_HDR_LEN, ies, ies_length);
    return n;
}

size_t bcm_ht_cap_ie(uint8_t *out, uint16_t cap_info, uint8_t ampdu, const uint8_t mcs[2])
{
    memset(out, 0, BCM_HT_CAP_IE_LEN);
    out[0] = 45;
    out[1] = BCM_HT_CAP_IE_LEN - 2;
    awdl_put16(out + 2, cap_info);
    out[4] = ampdu;
    out[5] = mcs[0];  // supported MCS set: rx bitmask, then highest rate, tx parameters (left 0: "same as rx")
    out[6] = mcs[1];
    return BCM_HT_CAP_IE_LEN;
}

size_t bcm_awdl_payload(uint8_t *out, size_t size, int u16_length, const uint8_t *tlvs, size_t length)
{
    const size_t head = u16_length ? 2 : 4;
    if (head + length > size || length > 0xffff)
        return 0;
    if (u16_length)
        awdl_put16(out, (uint16_t)length);
    else
        awdl_put32(out, (uint32_t)length);
    if (length)
        memcpy(out + head, tlvs, length);
    return head + length;
}

size_t bcm_awdl_oob_auto(uint8_t *out, size_t size, const uint8_t *tlvs, size_t length)
{
    if (BCM_AWDL_OOB_HDR_LEN + 2 + length > size || length > 0xffff)
        return 0;
    memset(out, 0, BCM_AWDL_OOB_HDR_LEN);
    awdl_put32(out, 0xffffffffu);  // tx_chan_map: every channel of the sequence
    awdl_put32(out + 4, 16600);    // tx_aws_offset, us after the AW start
    memcpy(out + 8, awdl_bssid, 6);
    memset(out + 14, 0xff, 6);        // dst_mac
    awdl_put32(out + 28, 0x2 | 0x4);  // flags: AWDL_OOB_AF_FILL_SYNC_PARAMS | AWDL_OOB_AF_FILL_ELECT_PARAMS
    awdl_put32(out + 36, 12);         // tx_rate (500 kb/s units): 6 Mb/s
    awdl_put16(out + BCM_AWDL_OOB_HDR_LEN, (uint16_t)length);
    if (length)
        memcpy(out + BCM_AWDL_OOB_HDR_LEN + 2, tlvs, length);
    return BCM_AWDL_OOB_HDR_LEN + 2 + length;
}

size_t bcm_awdl_psf_dwell(uint8_t *out, int on)
{
    if (on)
        memcpy(out, bcm_trace_psf_dwell, BCM_AWDL_PSF_DWELL_LEN);
    else
        memset(out, 0, BCM_AWDL_PSF_DWELL_LEN);
    return BCM_AWDL_PSF_DWELL_LEN;
}

size_t bcm_awdl_chan_seq(uint8_t *out, size_t size, const struct awdl_chan *seq, unsigned slots)
{
    const size_t n = BCM_AWDL_CHAN_SEQ_FIXED + 2u * slots;
    if (!slots || slots > AWDL_SEQ_SLOTS || n > size)
        return 0;
    out[0] = (uint8_t)(slots - 1);  // as on the air: entries - 1
    out[1] = AWDL_CHAN_ENC_OPCLASS;
    out[2] = 0;
    out[3] = 3;
    awdl_put16(out + 4, 0xffff);
    for (unsigned i = 0; i < slots; ++i) {
        out[6 + 2 * i] = seq[i].number;
        out[7 + 2 * i] = seq[i].opclass;
    }
    return n;
}

// The series' chip without SDB (BCM4364; the BCM4356 has one radio too): byte 0x0c = 9 and every slot in slice 0
// (0x4c), slice 1 (0x78) cleared, a slot of slice 0 empty in the trace taking slice 1's.
size_t bcm_slot_bss(uint8_t *out, uint16_t slots44)
{
    memcpy(out, bcm_trace_slot_bss, BCM_AWDL_SLOT_BSS_LEN);
    out[0x0c] = 9;
    for (unsigned i = 0; i < 16; ++i) {
        const uint16_t cs = i == 8 ? 0x1006 : (slots44 >> i) & 1 ? 0xe22a : 0;  // as traced: 6/20, 44 in 36-48/80
        uint8_t *s0 = out + 0x4c + 2 * i, *s1 = out + 0x78 + 2 * i, *adv = out + 0x1e + 2 * i;
        s0[0] = adv[0] = (uint8_t)(cs >> 8);
        s0[1] = adv[1] = (uint8_t)cs;
        s1[0] = s1[1] = 0;
    }
    return BCM_AWDL_SLOT_BSS_LEN;
}

uint16_t bcm_chanspec_of(struct awdl_chan c)
{
    const uint8_t p = awdl_chan_primary(c);
    if (!p)
        return 0;
    if (p <= 14)
        return (uint16_t)(0x1000 | p);  // 2.4 GHz, 20 MHz
    if (c.opclass == 0x80 && c.number >= 6 && p >= c.number - 6) {
        const unsigned sb = (unsigned)(p - (c.number - 6)) / 4;  // which 20 MHz of the block is primary
        return (uint16_t)(0xc000 | 0x2000 | (sb << 8) | c.number);
    }
    return (uint16_t)(0xc000 | 0x1000 | p);  // 5 GHz, 20 MHz
}

uint8_t bcm_chanspec_primary(uint16_t chanspec)
{
    const uint8_t ch = (uint8_t)(chanspec & 0xff);
    const unsigned sb = (chanspec >> 8) & 7;
    switch (chanspec & 0x3800) {
    case 0x2000:
        return (uint8_t)(ch - 6 + 4 * sb);  // 80 MHz
    case 0x1800:
        return (uint8_t)(ch - 2 + 4 * sb);  // 40 MHz
    default:
        return ch;
    }
}

// ---- data path -----------------------------------------------------------------------------------------------------------
static const uint8_t kAwdlLlc[10] = {0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x03, 0x04};

size_t bcm_awdl_encap(const uint8_t *eth, size_t length, uint16_t seq, uint8_t *out, size_t size)
{
    if (length < 14 || length + BCM_AWDL_DATA_HDR_LEN > size)
        return 0;
    const size_t n = length + BCM_AWDL_DATA_HDR_LEN;
    memmove(out + 14 + BCM_AWDL_DATA_HDR_LEN, eth + 14, length - 14);  // payload first: out may overlap eth's tail
    const uint8_t type0 = eth[12], type1 = eth[13];
    memmove(out, eth, 12);
    out[12] = (uint8_t)((n - 14) >> 8);  // 802.3 length
    out[13] = (uint8_t)(n - 14);
    memcpy(out + 14, kAwdlLlc, sizeof kAwdlLlc);
    awdl_put16(out + 24, seq);
    out[26] = out[27] = 0;
    out[28] = type0;
    out[29] = type1;
    return n;
}

size_t bcm_awdl_decap(const uint8_t *f, size_t length, uint8_t *out, size_t size, uint16_t *seq)
{
    if (length < 14 + BCM_AWDL_DATA_HDR_LEN || be16(f + 12) >= 0x600 || memcmp(f + 14, kAwdlLlc, sizeof kAwdlLlc))
        return 0;
    const size_t n = length - BCM_AWDL_DATA_HDR_LEN;
    if (n > size)
        return 0;
    if (seq)
        *seq = (uint16_t)(f[24] | (f[25] << 8));
    const uint8_t type0 = f[28], type1 = f[29];
    memmove(out + 14, f + 30, length - 30);
    memmove(out, f, 12);
    out[12] = type0;
    out[13] = type1;
    return n;
}

// ---- events -----------------------------------------------------------------------------------------------------------
int bcm_if_event_decode(const uint8_t *d, size_t len, struct bcm_if_event *o)
{
    if (len < 5)
        return -1;
    o->ifidx = d[0];
    o->opcode = d[1];
    o->flags = d[2];
    o->bssidx = d[3];
    o->role = d[4];
    return 0;
}

static int is_awdl_body(const uint8_t *b, size_t len)
{
    return len >= 5 && b[0] == 0x7f && !memcmp(b + 1, awdl_oui, 3) && b[4] == 0x08;
}

static int is_awdl_frame(const uint8_t *f, size_t len)
{
    return len >= 24 + 5 && (f[0] & 0xfc) == 0xd0 && is_awdl_body(f + 24, len - 24);
}

int bcm_awdl_rx_af(const uint8_t *d, size_t len, struct bcm_rx_frame *meta, const uint8_t **body, size_t *body_len)
{
    memset(meta, 0, sizeof *meta);
    if (len >= 2) {
        const uint16_t version = be16(d);
        const size_t hdr = version == 1 ? BCM_RX_MGMT_V1_LEN : version == 2 ? BCM_RX_MGMT_V2_LEN : 0;
        if (hdr && len > hdr) {
            const uint8_t *b = d + hdr;
            size_t n = len - hdr;
            if (is_awdl_frame(b, n)) {  // a whole 802.11 frame after the header
                b += 24;
                n -= 24;
            }
            if (is_awdl_body(b, n)) {
                meta->version = version;
                meta->chanspec = be16(d + 2);
                meta->rssi = (int32_t)be32(d + 4);
                meta->mactime = be32(d + 8);
                meta->rate = be32(d + 12);
                *body = b;
                *body_len = n;
                return 0;
            }
        }
    }
    if (is_awdl_frame(d, len)) {  // no header: the 802.11 frame alone
        *body = d + 24;
        *body_len = len - 24;
        return 0;
    }
    return -1;
}
