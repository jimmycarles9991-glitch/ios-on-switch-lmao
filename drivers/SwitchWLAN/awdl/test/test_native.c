// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the native backend (the firmware runs AWDL):
//   - every iovar payload byte for byte: against the bytes the brcmfmac AWDL series traced from macOS (awdl_bcm_trace.c)
//     and against Broadcom's 2014 wlioctl.h layouts (the same field offsets);
//   - the probe's requests and its choice of firmware generation;
//   - the data path's 16-byte AWDL header and the received-frame header (big endian, v1/v2);
//   - the schedule (operating classes, the social channels, the infra channel interleaved);
//   - both bring-ups end to end against a mock firmware: interface_create v3 in the series' exact order on the AWDL
//     interface's ifidx, and the 7.x awdl_if form; WLC_E_IF, the 500 ms / 5 s steps, peers from WLC_E_ACTION_FRAME_RX,
//     awdl_peer_op with the peer's HT element, the host's election told to the firmware, data in and out, teardown;
//   - a refused or unanswered creation turning AWDL off with one line.
#include "../awdl.h"
#include "../awdl_apple80211.h"
#include "../awdl_bcm.h"
#include "../awdl_bcm_trace.h"
#include <errno.h>
#include <net/if.h>
#include <net/if_media.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
#define NCHECK(x)                                                        \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            test_failures++;                                             \
        }                                                                \
    } while (0)

static int bytes_eq(const uint8_t *got, size_t n, const char *hex)
{
    uint8_t want[512];
    size_t k = 0;
    while (*hex) {
        if (*hex == ' ') {
            hex++;
            continue;
        }
        unsigned v;
        if (k == sizeof want || sscanf(hex, "%2x", &v) != 1)
            return 0;
        want[k++] = (uint8_t)v;
        hex += 2;
    }
    if (k != n || memcmp(got, want, n)) {
        fprintf(stderr, "  got:");
        for (size_t i = 0; i < n; ++i)
            fprintf(stderr, " %02x", got[i]);
        fprintf(stderr, "\n");
        return 0;
    }
    return 1;
}

static int zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (p[i])
            return 0;
    return 1;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- mock firmware -----------------------------------------------------------------------------------------------------
struct fw_call {
    char name[32];
    int32_t ifidx;
    int get;
    uint8_t buf[512];
    size_t len;
};
static struct fw_call calls[96];
static int ncalls, presents, absents, eths, delivered;
static uint8_t last_presence[6], last_eth[1600], last_rx[1600];
static size_t last_eth_len, last_rx_len;

static int mock_iovar(void *ctx, int32_t ifidx, int get, const char *name, const void *buf, size_t len)
{
    (void)ctx;
    if (ncalls == 96 || len > sizeof calls[0].buf)
        return -1;
    struct fw_call *c = &calls[ncalls++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->ifidx = ifidx;
    c->get = get;
    if (len)
        memcpy(c->buf, buf, len);
    c->len = len;
    return 0;
}

static int mock_tx(void *ctx, const uint8_t *f, size_t n, uint16_t freq, uint64_t tsf, int no_ack)
{
    (void)ctx, (void)f, (void)n, (void)freq, (void)tsf, (void)no_ack;
    return -1;  // the native backend never injects
}

static int mock_tx_eth(void *ctx, const uint8_t *eth, size_t len)
{
    (void)ctx;
    eths++;
    memcpy(last_eth, eth, len);
    last_eth_len = len;
    return 0;
}

static void mock_deliver(void *ctx, const uint8_t *eth, size_t len)
{
    (void)ctx;
    delivered++;
    memcpy(last_rx, eth, len);
    last_rx_len = len;
}
static struct a80211_awdl *facade;

static void mock_peer(void *ctx, const uint8_t addr[6], int present, int8_t rssi)
{
    (void)ctx;
    if (present)
        presents++;
    else
        absents++;
    memcpy(last_presence, addr, 6);
    if (facade)
        a80211_awdl_peer(facade, addr, present, rssi);
}

static const struct fw_call *find_call(const char *name, int nth)
{
    for (int i = 0; i < ncalls; ++i)
        if (!strcmp(calls[i].name, name) && nth-- == 0)
            return &calls[i];
    return NULL;
}

static void reset_calls(void)
{
    ncalls = presents = absents = eths = delivered = 0;
}

// ---- encoders against the series' traced bytes and the 2014 layouts ------------------------------------------------------
static void test_encoders(void)
{
    uint8_t b[512];
    const uint8_t me[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};

    // awdl_sync_params: the trace, and the 2014 awdl_sync_params_t offsets (type, param_len, master_chan, AW period,
    // AF period, ext length, common length)
    NCHECK(bcm_awdl_sync_params(b, 6) == 36 && !memcmp(b, bcm_trace_sync_params, 36));
    NCHECK(b[0] == 0 && b[1] == 36 && b[6] == 6 && b[8] == 16 && b[10] == 110 && b[14] == 16 && b[16] == 16);
    // awdl_opmode: fixed mode (byte 0 = AWDL_OPMODE_FIXED), role master (2014 awdl_opmode_t.role), master at +4
    NCHECK(bcm_awdl_opmode(b, BCM_AWDL_ROLE_MASTER, NULL) == 28 && !memcmp(b, bcm_trace_opmode, 28));
    NCHECK(bcm_awdl_opmode(b, BCM_AWDL_ROLE_SLAVE, me) == 28 && bytes_eq(b, 12, "01 00 0000 02aabbccddee 0000") &&
           zero(b + 12, 16));
    // awdl_election_tree: the trace (self metric 65, boost 5, edge -70, higher masters 1/3, depth 4, top master metric
    // 65 at 0x15), and the 2014 awdl_election_tree_info_t tail: top_master @0x0f, its metric @0x15, depth @0x19
    NCHECK(bcm_awdl_election_tree(b, NULL, BCM_AWDL_SELF_METRIC, 0) == 42 && !memcmp(b, bcm_trace_election_tree, 42));
    const uint8_t phone[6] = {0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f};
    bcm_awdl_election_tree(b, phone, 200, 1);
    NCHECK(!memcmp(b + 0x0f, phone, 6) && rd32(b + 0x15) == 200 && b[0x19] == 1 && rd32(b + 3) == 65);
    // awdl_oob_af_auto: the trace's 44-byte header = awdl_oob_af_params_auto_t (chan map, AW offset, BSSID, dst,
    // channel, dwell, flags FILL_SYNC|FILL_ELECT, lifetime, rate 12, retries), then u16 length and the TLVs
    const uint8_t tlv[] = {0x15, 0x02, 0x00, 0x34, 0x02};
    NCHECK(bcm_awdl_oob_auto(b, sizeof b, tlv, 5) == 51 && !memcmp(b, bcm_trace_oob_hdr, 44) &&
           bytes_eq(b + 44, 7, "0500 15 0200 34 02"));
    NCHECK(rd32(b) == 0xffffffffu && rd32(b + 4) == 16600 && !memcmp(b + 8, awdl_bssid, 6) && rd32(b + 28) == 6 &&
           rd32(b + 36) == 12);
    // awdl_psf_dwell on and off
    NCHECK(bcm_awdl_psf_dwell(b, 1) == 8 && !memcmp(b, bcm_trace_psf_dwell, 8));
    NCHECK(bcm_awdl_psf_dwell(b, 0) == 8 && zero(b, 8));
    // awdl_payload: the series' u16 length (2 + TLVs) and the 7.x u32 (awdl_payload_t)
    NCHECK(bcm_awdl_payload(b, sizeof b, 1, tlv, 5) == 7 && bytes_eq(b, 7, "0500 15 0200 34 02"));
    NCHECK(bcm_awdl_payload(b, sizeof b, 0, tlv, 5) == 9 && bytes_eq(b, 9, "05000000 15 0200 34 02"));
    NCHECK(bcm_awdl_payload(b, 6, 1, tlv, 5) == 0);
    NCHECK(bcm_u32(b, BCM_AWDL_CONFIG_START) == 4 && bytes_eq(b, 4, "83000000"));

    // interface_create v3 (wl_interface_create_v3, natural alignment: flags @8, mac @12, bssid @18, if_index @24)
    NCHECK(bcm_ifc_v3_awdl(b, me) == 28 &&
           bytes_eq(b, 28, "0300 1c00 1c00 02 00 1a000000 02aabbccddee 002500ff9473 02 000000"));
    // awdl_if (wl_awdl_if2_t)
    NCHECK(bcm_awdl_if2(b, 2, 1, awdl_bssid, me) == 20 &&
           bytes_eq(b, 20, "02000000 01000000 002500ff9473 02aabbccddee"));

    // awdl_peer_op: the series' layout (2014 awdl_peer_op_t + IEs): an add is flagged 1 and at least 0x25 bytes,
    // a delete 9
    NCHECK(bcm_awdl_peer_op(b, sizeof b, BCM_AWDL_PEER_OP_ADD, phone, NULL, 0) == 0x25 &&
           bytes_eq(b, 9, "00 00 0a1b2c3d4e5f 01") && zero(b + 9, 0x25 - 9));
    NCHECK(bcm_awdl_peer_op(b, sizeof b, BCM_AWDL_PEER_OP_DEL, phone, NULL, 0) == 9 &&
           bytes_eq(b, 9, "00 01 0a1b2c3d4e5f 00"));
    uint8_t ie[BCM_HT_CAP_IE_LEN];
    const uint8_t mcs[2] = {0xff, 0xff};
    NCHECK(bcm_ht_cap_ie(ie, 0x11ce, 0x17, mcs) == 28 && bytes_eq(ie, 7, "2d 1a ce11 17 ff ff") && zero(ie + 7, 21));
    NCHECK(bcm_awdl_peer_op(b, sizeof b, BCM_AWDL_PEER_OP_ADD, phone, ie, sizeof ie) == 0x25 && b[8] == 1 &&
           !memcmp(b + 9, ie, 28));
    uint8_t big[200];
    memset(big, 0x11, sizeof big);
    NCHECK(bcm_awdl_peer_op(b, sizeof b, BCM_AWDL_PEER_OP_ADD, phone, big, sizeof big) == 0);  // > 128 bytes of IEs

    // 7.x awdl_config_params_t (28, natural alignment)
    struct bcm_awdl_config c;
    memset(&c, 0, sizeof c);
    c.channel = 149;
    c.aw_period = 16;
    c.aw_cmn_length = 16;
    c.action_frame_period = 110;
    c.pkt_lifetime_ms = 500;
    c.max_no_master = 8;
    c.ext_count = 3;
    c.aw_ext_length = 16;
    c.nmode = 2;
    memset(c.dest, 0xff, 6);
    NCHECK(bcm_awdl_config_encode(b, &c) == 28 &&
           bytes_eq(b, 28, "01000000 95 00 1000 1000 6e00 f401 0800 0300 1000 0200 ffffffffffff"));
    // 7.x awdl_channel_sequence_t, operating-class pairs
    struct awdl_chan seq[16];
    for (int i = 0; i < 16; ++i)
        seq[i] = (struct awdl_chan){155, 0x80};
    seq[1] = (struct awdl_chan){6, 81};
    NCHECK(
        bcm_awdl_chan_seq(b, sizeof b, seq, 16) == 38 &&
        bytes_eq(b, 38,
                 "0f 03 00 03 ffff 9b80 0651 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80"));
    NCHECK(bcm_awdl_chan_seq(b, 10, seq, 16) == 0);

    // slot_bss: chanspecs high byte first as traced (0xe22a = 44 primary in the 36-48 block at 80 MHz, 0x1006 = 6)
    NCHECK(bcm_chanspec_of((struct awdl_chan){42, 0x80}) == 0xe22a);
    NCHECK(bcm_chanspec_of((struct awdl_chan){6, 81}) == 0x1006);
    NCHECK(bcm_chanspec_of((struct awdl_chan){155, 0x80}) == 0xe09b);
    // Apple's on-air form names the primary: the same 80 MHz chanspecs
    NCHECK(bcm_chanspec_of((struct awdl_chan){149, 0x80}) == 0xe09b &&
           bcm_chanspec_of((struct awdl_chan){44, 0x80}) == 0xe22a);
    NCHECK(bcm_chanspec20(149, 0) == 0xd095 && bcm_chanspec20(6, 0) == 0x1006 && bcm_chanspec20(44, 1) == 0x1b2c &&
           bcm_chanspec20(6, 1) == 0x2b06 && bcm_chanspec20(0, 0) == 0);
    NCHECK(bcm_chanspec_of((struct awdl_chan){36, 115}) == 0xd024 && bcm_chanspec_of((struct awdl_chan){0, 0}) == 0);

    // actframe: brcmfmac's whole brcmf_fil_af_params_le: chanspec and dwell, the frame's BSSID (addr3), pad, its
    // destination (addr1), body length, packet id, then the body from the category byte (AWDL: 127, Apple OUI, type 8),
    // zero to BRCMF_FIL_ACTION_FRAME_SIZE.
    {
        uint8_t af[24 + 6] = {0xd0, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0xaa, 0xbb, 0xcc, 0xdd,
                              0xee, 0x00, 0x25, 0x00, 0xff, 0x94, 0x73, 0x00, 0x00, 0x7f, 0x00, 0x17, 0xf2, 0x08, 0x10};
        static uint8_t req[BCM_ACTFRAME_REQUEST_LEN];
        memset(req, 0xee, sizeof req);
        const size_t n = bcm_actframe_request(req, sizeof req, af, sizeof af, 0xd095, 66, 7);
        int tail_zero = 1;
        for (size_t k = 37 + 6; k < sizeof req; ++k)
            tail_zero &= req[k] == 0;
        NCHECK(n == 1837 && !memcmp(req, "actframe", 9) &&
               bytes_eq(req + 9, 28, "95d00000 42000000 002500ff9473 0000 ffffffffffff 0600 07000000") &&
               bytes_eq(req + 37, 6, "7f0017f20810") && tail_zero);
        NCHECK(bcm_actframe_request(req, sizeof req - 1, af, sizeof af, 0xd095, 0, 7) == 0);  // no room
        af[0] = 0x08;                                                                         // a data frame
        NCHECK(bcm_actframe_request(req, sizeof req, af, sizeof af, 0xd095, 0, 7) == 0);
        NCHECK(bcm_actframe_request(req, sizeof req, af, 24, 0xd095, 0, 7) == 0);  // no body
    }
    NCHECK(bcm_chanspec_primary(0xe22a) == 44 && bcm_chanspec_primary(0x1006) == 6 &&
           bcm_chanspec_primary(0xe09b) == 149);
    const uint16_t cs44 = bcm_chanspec_of((struct awdl_chan){42, 0x80}),
                   cs6 = bcm_chanspec_of((struct awdl_chan){6, 81});
    // the trace's SDB form: slice 0 (0x4c) 44 in slots 0-4 and 9-12, slice 1 (0x78) 6 in slot 8, and the advertised
    // sequence at 0x1e both merged
    const uint8_t *t = bcm_trace_slot_bss;
    for (int i = 0; i < 16; ++i) {
        const int on44 = i <= 4 || (i >= 9 && i <= 12);
        NCHECK(((t[0x4c + 2 * i] << 8) | t[0x4d + 2 * i]) == (on44 ? cs44 : 0));
        NCHECK(((t[0x78 + 2 * i] << 8) | t[0x79 + 2 * i]) == (i == 8 ? cs6 : 0));
        NCHECK(((t[0x1e + 2 * i] << 8) | t[0x1f + 2 * i]) == (on44 ? cs44 : i == 8 ? cs6 : 0));
    }
    // the series' transform for a chip without SDB (BCM4364): byte 0x0c = 9, slice 1 moved into slice 0's empty slots
    uint8_t want[158];
    memcpy(want, t, 158);
    want[0x0c] = 9;
    for (int i = 0; i < 16; ++i) {
        uint8_t *s0 = want + 0x4c + 2 * i, *s1 = want + 0x78 + 2 * i;
        if (!(s0[0] | s0[1]))
            memcpy(s0, s1, 2);
        memset(s1, 0, 2);
    }
    NCHECK(bcm_slot_bss(b, AWDL_SLOTS_TRACE) == 158 && !memcmp(b, want, 158));
    NCHECK(((b[0x4c + 16] << 8) | b[0x4d + 16]) == cs6 && zero(b + 0x78, 32));
    // another mask (idle: 44 in 0, 9, 10): those slots in slice 0 and in the advertised sequence, 6 in 8, no others
    NCHECK(bcm_slot_bss(b, AWDL_SLOTS_IDLE) == 158 && !memcmp(b, want, 0x1e) &&
           !memcmp(b + 0x3e, want + 0x3e, 0x4c - 0x3e));
    for (int i = 0; i < 16; ++i) {
        const uint16_t w = i == 8 ? cs6 : (i == 0 || i == 9 || i == 10) ? cs44 : 0;
        NCHECK(((b[0x4c + 2 * i] << 8) | b[0x4d + 2 * i]) == w && ((b[0x1e + 2 * i] << 8) | b[0x1f + 2 * i]) == w);
    }

    // the data path's AWDL header (the series' brcmf_awdl_encap/decap)
    uint8_t eth[18] = {0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x02, 0xaa, 0xbb,
                       0xcc, 0xdd, 0xee, 0x86, 0xdd, 'a',  'b',  'c',  'd'};
    uint8_t enc[64], dec[64];
    NCHECK(bcm_awdl_encap(eth, sizeof eth, 0x1234, enc, sizeof enc) == 34 &&
           bytes_eq(enc, 34, "0a1b2c3d4e5f 02aabbccddee 0014 aaaa0300 17f20800 0304 3412 0000 86dd 61626364"));
    uint16_t sq = 0;
    NCHECK(bcm_awdl_decap(enc, 34, dec, sizeof dec, &sq) == 18 && !memcmp(dec, eth, 18) && sq == 0x1234);
    NCHECK(bcm_awdl_decap(eth, sizeof eth, dec, sizeof dec, &sq) == 0);  // a plain Ethernet frame is not AWDL data
    enc[19] ^= 1;
    NCHECK(bcm_awdl_decap(enc, 34, dec, sizeof dec, &sq) == 0);  // another LLC/SNAP
    NCHECK(bcm_awdl_encap(eth, 13, 0, enc, sizeof enc) == 0 && bcm_awdl_encap(eth, sizeof eth, 0, enc, 33) == 0);

    // received frames: the be16-versioned header, big-endian fields, then the frame from its category
    uint8_t ev[64] = {0x00, 0x01, 0xe0, 0x9b, 0xff, 0xff, 0xff, 0xc4, 0,    0,    0,    1,
                      0,    0,    0,    12,   0x7f, 0x00, 0x17, 0xf2, 0x08, 0x10, 0x03, 0x00};
    struct bcm_rx_frame meta;
    const uint8_t *body;
    size_t n;
    NCHECK(!bcm_awdl_rx_af(ev, 24, &meta, &body, &n) && body == ev + 16 && n == 8 && meta.version == 1 &&
           meta.chanspec == 0xe09b && meta.rssi == -60 && meta.mactime == 1 && meta.rate == 12);
    uint8_t ev2[64] = {0x00, 0x02};
    memcpy(ev2 + 24, ev + 16, 8);
    NCHECK(!bcm_awdl_rx_af(ev2, 32, &meta, &body, &n) && body == ev2 + 24 && n == 8 && meta.version == 2);
    ev[0] = 0x01;  // version 0x0101: unknown
    NCHECK(bcm_awdl_rx_af(ev, 24, &meta, &body, &n) < 0);
    ev[0] = 0;
    ev[20] = 0x09;  // OUI type 9: not AWDL
    NCHECK(bcm_awdl_rx_af(ev, 24, &meta, &body, &n) < 0);
    // WLC_E_IF
    const uint8_t ife[] = {2, 1, 0, 2, 9};
    struct bcm_if_event ie2;
    NCHECK(!bcm_if_event_decode(ife, 5, &ie2) && ie2.ifidx == 2 && ie2.opcode == 1 && ie2.bssidx == 2 && ie2.role == 9);
    NCHECK(bcm_if_event_decode(ife, 4, &ie2) < 0);
    NCHECK(!strcmp(bcme_name(-23), "UNSUPPORTED") && !strcmp(bcme_name(0), "OK") && !strcmp(bcme_name(-2), "BADARG"));
}

// ---- the probe ------------------------------------------------------------------------------------------------------------
static void test_probe(void)
{
    uint8_t b[64];
    NCHECK(bcm_probe_request(BCM_PROBE_INTERFACE_CREATE, b, sizeof b) == 21 &&
           !memcmp(b, "interface_create\0\0\0\0\0", 21));
    NCHECK(bcm_probe_request(BCM_PROBE_AWDL_PEER_OP, b, sizeof b) == 13 + 9 && !memcmp(b, "awdl_peer_op", 13) &&
           b[13] == 0 && b[14] == BCM_AWDL_PEER_OP_INFO && zero(b + 15, 7));
    NCHECK(bcm_probe_request(BCM_PROBE_MCHAN, b, sizeof b) == 6 && !memcmp(b, "mchan", 6));
    NCHECK(bcm_probe_request(BCM_PROBE_COUNT, b, sizeof b) == 0 && bcm_probe_request(BCM_PROBE_AWDL, b, 4) == 0);
    struct bcm_awdl_probe p;
    memset(&p, 0, sizeof p);
    NCHECK(bcm_awdl_api_of(&p) == BCM_AWDL_API_NONE);  // not run: nothing
    for (unsigned i = 0; i < BCM_PROBE_COUNT; ++i)
        bcm_probe_record(&p, i, BCME_UNSUPPORTED, NULL, 0);
    p.done = 1;
    NCHECK(bcm_awdl_api_of(&p) == BCM_AWDL_API_NONE && bcm_probe_absent(&p) == (1u << BCM_PROBE_COUNT) - 1);
    // a 7.x table: awdl_if answers BADARG (exists), interface_create UNSUPPORTED (the BCM4350's answer)
    bcm_probe_record(&p, BCM_PROBE_AWDL_IF, BCME_BADARG, NULL, 0);
    bcm_probe_record(&p, BCM_PROBE_AWDL_CHAN_SEQ, 0, b, 38);
    NCHECK(bcm_awdl_api_of(&p) == BCM_AWDL_API_AWDL_IF && bcm_probe_has(&p, BCM_PROBE_AWDL_CHAN_SEQ) &&
           !bcm_probe_has(&p, BCM_PROBE_SLOT_BSS) && !(bcm_probe_absent(&p) & (1u << BCM_PROBE_AWDL_IF)));
    // interface_create answering version 3 (the series' condition)
    const uint8_t v3[4] = {3, 0, 0, 0};
    bcm_probe_record(&p, BCM_PROBE_INTERFACE_CREATE, 0, v3, 4);
    NCHECK(p.ifc_version == 3 && bcm_awdl_api_of(&p) == BCM_AWDL_API_IFC_V3);
    const uint8_t v2[4] = {2, 0, 0, 0};
    bcm_probe_record(&p, BCM_PROBE_INTERFACE_CREATE, 0, v2, 4);
    NCHECK(bcm_awdl_api_of(&p) == BCM_AWDL_API_AWDL_IF);
    bcm_probe_record(&p, BCM_PROBE_AWDL_IF, BCM_PROBE_NO_ANSWER, NULL, 0);  // no answer is not "exists"
    NCHECK(bcm_awdl_api_of(&p) == BCM_AWDL_API_NONE);
    bcm_probe_record(&p, BCM_PROBE_AWDL_IF2, BCME_NOTFOUND, NULL, 0);  // awdl_if2 alone is enough
    NCHECK(bcm_awdl_api_of(&p) == BCM_AWDL_API_AWDL_IF);
}

// 7.35.252.108's probe on the device: every item UNSUPPORTED but event_msgs_ext (VERSION).
static void probe_7_35_252_108(struct bcm_awdl_probe *p)
{
    memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < BCM_PROBE_COUNT; ++i)
        bcm_probe_record(p, i, BCME_UNSUPPORTED, NULL, 0);
    bcm_probe_record(p, BCM_PROBE_EVENT_MSGS_EXT, -37, NULL, 0);
    p->done = 1;
}

static void test_monitor_probe(void)
{
    uint8_t b[32];
    NCHECK(bcm_monitor_probe_request(BCM_MONITOR_PROBE_GET_MONITOR, b, sizeof b) == 0);
    NCHECK(bcm_monitor_probe_request(BCM_MONITOR_PROBE_PROMISC_LEVEL, b, sizeof b) == 22 &&
           !memcmp(b, "monitor_promisc_level", 22));
    NCHECK(bcm_monitor_probe_request(BCM_MONITOR_PROBE_STA_MONITOR, b, sizeof b) == 12 &&
           !memcmp(b, "sta_monitor", 12));
    NCHECK(bcm_monitor_probe_request(BCM_MONITOR_PROBE_STA_MONITOR, b, 11) == 0);
    NCHECK(bcm_monitor_probe_request(BCM_MONITOR_PROBE_AF_FLYING_MAX, b, sizeof b) == 14 &&
           !memcmp(b, "af_flying_max", 14));
    NCHECK(bcm_monitor_probe_request(BCM_MONITOR_PROBE_TSF, b, sizeof b) == 4 && !memcmp(b, "tsf", 4));
    NCHECK(bcm_monitor_probe_request(BCM_MONITOR_PROBE_COUNT, b, sizeof b) == 0);
    NCHECK(BCM_WLC_GET_MONITOR == 107 &&
           !strcmp(bcm_monitor_probe_names[BCM_MONITOR_PROBE_GET_MONITOR], "WLC_GET_MONITOR"));
}

static void test_radio_verdict(void)
{
    struct bcm_awdl_probe p;
    probe_7_35_252_108(&p);
    NCHECK(bcm_probe_absent(&p) == (((1u << BCM_PROBE_COUNT) - 1) & ~(1u << BCM_PROBE_EVENT_MSGS_EXT)));
    struct awdl_radio_verdict v = awdl_radio_verdict_of(0, 1, 1, 0, 0, 0, 0, &p);
    NCHECK(v.mode == AWDL_RADIO_OFF && v.off == AWDL_OFF_NO_API && v.api == BCM_AWDL_API_NONE && !v.trial);
    // the debug trial runs the soft backend on this firmware, and says what it lacks
    v = awdl_radio_verdict_of(0, 1, 1, 0, 0, 1, 1, &p);
    NCHECK(v.mode == AWDL_RADIO_SOFT && v.trial && v.off == AWDL_OFF_NO_API);
    v = awdl_radio_verdict_of(-EAGAIN, 0, 0, 0, 0, 0, 1, NULL);
    NCHECK(v.mode == AWDL_RADIO_UNDECIDED && !v.trial);
    v = awdl_radio_verdict_of(-ENODEV, 0, 0, 0, 0, 0, 1, NULL);
    NCHECK(v.mode == AWDL_RADIO_OFF && v.off == AWDL_OFF_NO_RADIO && !v.trial);
    v = awdl_radio_verdict_of(0, 0, 1, 0, 0, 0, 1, &p);
    NCHECK(v.mode == AWDL_RADIO_OFF && v.off == AWDL_OFF_ABI && !v.trial);
    v = awdl_radio_verdict_of(0, 1, 0, 1, 0, 1, 0, NULL);
    NCHECK(v.mode == AWDL_RADIO_OFF && v.off == AWDL_OFF_NO_BACKEND);  // monitor and action frames, no data out
    v = awdl_radio_verdict_of(0, 1, 1, 1, 1, 0, 0, &p);
    NCHECK(v.mode == AWDL_RADIO_OFF && v.off == AWDL_OFF_NO_API);  // no way to its channels
    v = awdl_radio_verdict_of(0, 1, 1, 1, 1, 1, 0, &p);
    NCHECK(v.mode == AWDL_RADIO_SOFT && v.off == AWDL_OFF_NOT && !v.trial);
    v = awdl_radio_verdict_of(0, 1, 0, 1, 1, 1, 0, NULL);
    NCHECK(v.mode == AWDL_RADIO_SOFT);
    bcm_probe_record(&p, BCM_PROBE_AWDL_IF, BCME_BADARG, NULL, 0);
    v = awdl_radio_verdict_of(0, 1, 1, 0, 0, 0, 0, &p);
    NCHECK(v.mode == AWDL_RADIO_NATIVE && v.api == BCM_AWDL_API_AWDL_IF);
    const uint8_t v3[4] = {3, 0, 0, 0};
    bcm_probe_record(&p, BCM_PROBE_INTERFACE_CREATE, 0, v3, 4);
    v = awdl_radio_verdict_of(0, 1, 1, 0, 0, 0, 0, &p);
    NCHECK(v.mode == AWDL_RADIO_NATIVE && v.api == BCM_AWDL_API_IFC_V3);
}

_Static_assert(A80211_IFF_BROADCAST == IFF_BROADCAST && A80211_IFF_SIMPLEX == IFF_SIMPLEX &&
                   A80211_IFF_MULTICAST == IFF_MULTICAST && A80211_IFM_AVALID == IFM_AVALID,
               "the BSD values awdl_apple80211.h carries");

static void test_down_interface(void)
{
    const uint8_t station[6] = {0x98, 0xb6, 0xe9, 0x5c, 0x10, 0xc7};
    struct a80211_awdl_down_if d;
    NCHECK(a80211_awdl_down_if(station, &d) == 0);
    NCHECK(!(d.if_flags & (IFF_UP | IFF_RUNNING)) && (d.if_flags & IFF_MULTICAST) && (d.if_flags & IFF_BROADCAST));
    NCHECK(d.mtu == 1500);
    NCHECK(d.lladdr[0] == 0x9a && !memcmp(d.lladdr + 1, station + 1, 5));
    const uint8_t local[6] = {0x02, 1, 2, 3, 4, 5};
    NCHECK(a80211_awdl_down_if(local, &d) == 0 && memcmp(d.lladdr, local, 6) && !(d.lladdr[0] & 0x01) &&
           (d.lladdr[0] & 0x02));
    const uint8_t zero6[6] = {0}, multicast[6] = {0x01, 0, 0x5e, 0, 0, 1};
    NCHECK(a80211_awdl_down_if(zero6, &d) == -1 && a80211_awdl_down_if(multicast, &d) == -1);
    NCHECK(a80211_awdl_down_media_status() == IFM_AVALID && !(a80211_awdl_down_media_status() & IFM_ACTIVE));
}

// ---- the schedule ---------------------------------------------------------------------------------------------------------
static int count_primary(const struct awdl_chan *s, uint8_t ch)
{
    int n = 0;
    for (int i = 0; i < 16; ++i)
        n += awdl_chan_primary(s[i]) == ch;
    return n;
}

static void test_schedule(void)
{
    NCHECK(awdl_opclass_20mhz(6) == 81 && awdl_opclass_20mhz(14) == 82 && awdl_opclass_20mhz(36) == 115 &&
           awdl_opclass_20mhz(48) == 115 && awdl_opclass_20mhz(52) == 118 && awdl_opclass_20mhz(100) == 121 &&
           awdl_opclass_20mhz(149) == 0x7c && awdl_opclass_20mhz(165) == 125 && awdl_opclass_20mhz(0) == 0);
    struct awdl_chan c = awdl_chan_of_freq(5745);
    NCHECK(c.number == 149 && c.opclass == 0x7c && awdl_chan_freq(c) == 5745);
    c = awdl_chan_of_freq(2437);
    NCHECK(c.number == 6 && c.opclass == 81);
    c = awdl_chan_of_freq(0);
    NCHECK(c.number == 0 && awdl_chan_freq(c) == 0);

    struct awdl_chan s[16];
    // idle, unassociated, no master yet: Stute et al.'s "Low Power" (149 in slot 0, 6 in 8, 44 in 9-10), the rest empty
    awdl_schedule(s, 0, 0, AWDL_MODE_IDLE);
    for (int i = 0; i < 16; ++i) {
        const uint8_t p = awdl_chan_primary(s[i]);
        NCHECK(i == 0                ? p == 149 && s[i].number == 149 && s[i].opclass == 0x80
               : (i == 9 || i == 10) ? p == 44 && s[i].number == 44 && s[i].opclass == 0x80
               : i == 8              ? p == 6 && s[i].opclass == 81
                                     : p == 0);
    }
    NCHECK(awdl_social_map(s) == 0x7 && awdl_slots_away(s, 0) == 4);
    NCHECK(awdl_slot_mask(2437, AWDL_MODE_IDLE) == AWDL_SLOTS_IDLE &&
           awdl_slot_mask(0, AWDL_MODE_IDLE) == AWDL_SLOTS_IDLE);
    // idle on channel 6: 3 of 16 slots away (0, 9, 10); the access point has the other 13
    awdl_schedule(s, 2437, 0, AWDL_MODE_IDLE);
    NCHECK(awdl_slots_away(s, 2437) == 3 && count_primary(s, 6) == 13);
    // transfer on channel 6 ("Data+Infra 75 %"): 11 away; the access point keeps 6, 7, 14, 15 and 8 (channel 6)
    awdl_schedule(s, 2437, 149, AWDL_MODE_TRANSFER);
    NCHECK(awdl_slots_away(s, 2437) == 11 && count_primary(s, 149) == 11 && count_primary(s, 6) == 5);
    NCHECK(awdl_chan_primary(s[6]) == 6 && awdl_chan_primary(s[7]) == 6 && awdl_chan_primary(s[14]) == 6 &&
           awdl_chan_primary(s[15]) == 6 && awdl_chan_primary(s[5]) == 149 && awdl_chan_primary(s[13]) == 149);
    // transfer on 36 with the master on 44: 12 away, the access point's 4 slots on 36 (opclass 115)
    awdl_schedule(s, 5180, 44, AWDL_MODE_TRANSFER);
    NCHECK(awdl_slots_away(s, 5180) == 12 && count_primary(s, 44) == 11 && count_primary(s, 36) == 4 &&
           s[14].opclass == 115);
    NCHECK(awdl_social_map(s) == 0x3);
    // idle on 36: 4 away (0, 8, 9, 10)
    awdl_schedule(s, 5180, 44, AWDL_MODE_IDLE);
    NCHECK(awdl_slots_away(s, 5180) == 4 && count_primary(s, 36) == 12);
    // transfer unassociated ("Data"): every slot
    awdl_schedule(s, 0, 149, AWDL_MODE_TRANSFER);
    NCHECK(awdl_slots_away(s, 0) == 16 && count_primary(s, 149) == 15 && count_primary(s, 6) == 1);
    // infra on 149 (5 GHz social): only slot 8 leaves it, in either mode
    awdl_schedule(s, 5745, 0, AWDL_MODE_IDLE);
    NCHECK(awdl_slots_away(s, 5745) == 1 && count_primary(s, 149) == 15);
    NCHECK(s[0].number == 149 && s[0].opclass == 0x80 && s[5].number == 149 && s[5].opclass == 0x7c);
    awdl_schedule(s, 5745, 0, AWDL_MODE_TRANSFER);
    NCHECK(awdl_slots_away(s, 5745) == 1);
    awdl_schedule(s, 5220, 0, AWDL_MODE_TRANSFER);  // infra on 44 likewise
    NCHECK(awdl_slots_away(s, 5220) == 1 && count_primary(s, 44) == 15);
}

// ---- end to end -----------------------------------------------------------------------------------------------------------
static struct awdl_core *make_phone(const uint8_t addr[6], uint32_t metric)
{
    struct awdl_core *phone = (struct awdl_core *)calloc(1, awdl_core_size());
    struct awdl_config pc;
    awdl_config_default(&pc, addr);
    pc.backend = AWDL_BACKEND_SOFT;
    pc.self_metric = metric;
    snprintf(pc.hostname, sizeof pc.hostname, "iPhone");
    const struct awdl_host ph = {NULL, mock_tx, NULL, NULL, NULL, NULL, NULL, NULL};
    NCHECK(awdl_core_init(phone, &ph, &pc) == 0);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(phone, seq, 5000000);
    awdl_core_start(phone, 5000000);
    return phone;
}

static struct awdl_core *make_native(const uint8_t me[6], uint8_t api, uint32_t absent, struct a80211_awdl *f)
{
    struct awdl_core *core = (struct awdl_core *)calloc(1, awdl_core_size());
    struct awdl_config cfg;
    awdl_config_default(&cfg, me);
    NCHECK(cfg.backend == AWDL_BACKEND_SOFT);
    cfg.backend = AWDL_BACKEND_NATIVE;
    cfg.fw_api = api;
    cfg.fw_absent = absent;
    const struct awdl_host h = {NULL, mock_tx, NULL, mock_peer, mock_deliver, NULL, mock_iovar, mock_tx_eth};
    NCHECK(awdl_core_init(core, &h, &cfg) == 0);
    const uint8_t caps[12] = {0x4c, 0x46, 0x0a, 0, 0x01};
    const struct a80211_awdl_ops ops = {NULL, NULL, NULL, NULL, NULL};
    a80211_awdl_init(f, core, &ops, "awdl0", "en0", caps);
    facade = f;
    return core;
}

static void sync_enabled(struct a80211_awdl *f, int on, uint64_t now)
{
    struct a80211_awdl_sync_enabled s = {1, 0, (uint32_t)on};
    uint32_t len = sizeof s;
    int32_t val = 0;
    NCHECK(a80211_awdl_request(f, 1, A80211_IOC_AWDL_SYNC_ENABLED, &val, &s, &len, sizeof s, now) == 0);
}

static void if_event(struct awdl_core *core, const uint8_t addr[6], uint8_t ifidx, uint8_t opcode, uint64_t now)
{
    struct awdl_fw_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = WLC_E_IF;
    memcpy(ev.addr, addr, 6);
    const uint8_t d[5] = {ifidx, opcode, 0, ifidx, 9};
    awdl_core_fw_event(core, &ev, d, sizeof d, now);
}

// The phone's frame as WLC_E_ACTION_FRAME_RX: the v1 header (rssi -60, big endian), then the frame from its category
static void phone_frame(struct awdl_core *core, struct awdl_core *phone, const uint8_t addr[6], uint8_t subtype,
                        uint64_t phone_now, uint64_t now)
{
    uint8_t f[1600], ev[1700] = {0x00, 0x01, 0xe0, 0x9b, 0xff, 0xff, 0xff, 0xc4};
    const size_t n = awdl_core_build_af(phone, subtype, phone_now, f, sizeof f);
    NCHECK(n > 100);
    memcpy(ev + 16, f + 24, n - 24);
    struct awdl_fw_event e;
    memset(&e, 0, sizeof e);
    e.type = WLC_E_ACTION_FRAME_RX;
    memcpy(e.addr, addr, 6);
    awdl_core_fw_event(core, &e, ev, 16 + n - 24, now);
}

static void test_flow_ifc_v3(void)
{
    reset_calls();
    const uint8_t me[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, phone_addr[6] = {0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f};
    struct awdl_core *phone = make_phone(phone_addr, 200);
    static struct a80211_awdl f;
    struct awdl_core *core = make_native(me, BCM_AWDL_API_IFC_V3, 0, &f);

    // AWDL_SYNC_ENABLED: interface_create v3, a GET on the station's interface, then nothing until WLC_E_IF
    sync_enabled(&f, 1, 1000000);
    NCHECK(awdl_core_running(core) && ncalls == 1 && !strcmp(calls[0].name, "interface_create") && calls[0].get &&
           calls[0].ifidx == 0 && calls[0].len == 28 && calls[0].buf[6] == 2 && calls[0].buf[24] == 2 &&
           !memcmp(calls[0].buf + 12, me, 6) && awdl_core_fw_ifidx(core) == -1);
    NCHECK(awdl_core_timer(core, 1000000) == 1000000 + 1000000);  // the next deadline: the 1 s tick before the 2 s one

    // WLC_E_IF add with our address: the series' brcmf_awdl_enable, in its order, on ifidx 2
    if_event(core, me, 2, 1, 1100000);
    NCHECK(awdl_core_fw_ifidx(core) == 2);
    static const char *const order[] = {
        "awdl_payload",      "awdl_aftxmode",      "awdl_sync_params", "awdl_osoc_chan",     "awdl_config",
        "awdl_psf_dwell",    "slot_bss",           "awdl_payload",     "awdl_election_tree", "awdl",
        "awdl_presencemode", "slot_bss",           "awdl_oob_af_auto", "awdl_payload",       "awdl_sync_params",
        "awdl_opmode",       "awdl_election_tree", "awdl_sync_params"};
    const int n = (int)(sizeof order / sizeof order[0]);
    NCHECK(ncalls == 1 + n);
    for (int i = 0; i < n && 1 + i < ncalls; ++i) {
        NCHECK(!strcmp(calls[1 + i].name, order[i]));
        NCHECK(calls[1 + i].ifidx == 2 && !calls[1 + i].get);
    }
    NCHECK(rd32(find_call("awdl_aftxmode", 0)->buf) == 0 && rd32(find_call("awdl_osoc_chan", 0)->buf) == 0);
    NCHECK(find_call("awdl_config", 0)->len == 4 && rd32(find_call("awdl_config", 0)->buf) == 0x83);
    NCHECK(rd32(find_call("awdl", 0)->buf) == 1 && rd32(find_call("awdl_presencemode", 0)->buf) == 4);
    NCHECK(!memcmp(find_call("awdl_sync_params", 0)->buf, bcm_trace_sync_params, 36));
    NCHECK(!memcmp(find_call("awdl_psf_dwell", 0)->buf, bcm_trace_psf_dwell, 8));
    uint8_t sb[158];
    bcm_slot_bss(sb, AWDL_SLOTS_IDLE);  // AWDL starts idle: the least presence
    NCHECK(find_call("slot_bss", 0)->len == 158 && !memcmp(find_call("slot_bss", 1)->buf, sb, 158));
    // we are our own master until the election hears better: role master with our address
    const struct fw_call *om = find_call("awdl_opmode", 0);
    NCHECK(om->len == 28 && om->buf[0] == 1 && om->buf[1] == BCM_AWDL_ROLE_MASTER && !memcmp(om->buf + 4, me, 6));
    const struct fw_call *et = find_call("awdl_election_tree", 0);
    NCHECK(et->len == 42 && !memcmp(et->buf + 0x0f, me, 6) && rd32(et->buf + 0x15) == 65);
    // the payload: the series' u16 length, then well-formed AWDL TLVs with our name, address and social map
    {
        const struct fw_call *p = find_call("awdl_payload", 0);
        const uint32_t len = (uint32_t)p->buf[0] | ((uint32_t)p->buf[1] << 8);
        NCHECK(len + 2 == p->len);
        uint8_t frame[600] = {0xd0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
        memcpy(frame + 16, awdl_bssid, 6);
        const uint8_t fixed[16] = {0x7f, 0x00, 0x17, 0xf2, 0x08, 0x10, 0x03, 0};
        memcpy(frame + 24, fixed, 16);
        memcpy(frame + 40, p->buf + 2, len);
        struct awdl_af a;
        NCHECK(awdl_af_parse(frame, 40 + len, &a) == 0);
        NCHECK((a.present & (1u << AWDL_TLV_ARPA)) && !strcmp(a.hostname, "switch"));
        NCHECK((a.present & (1u << AWDL_TLV_DATA_PATH_STATE)) && !memcmp(a.dps.awdl_address, me, 6) &&
               a.dps.social_channel_map == 0x3);  // the series' 44 and 6
        NCHECK((a.present & (1u << AWDL_TLV_HT_CAPS)) && a.ht_mcs[0] == 0xff && a.ht_mcs[1] == 0xff);
    }
    // the sequence awdl0 reports is the one the firmware runs: 44 and 6, free slots empty
    NCHECK(count_primary(awdl_core_sequence(core), 44) == 3 && count_primary(awdl_core_sequence(core), 6) == 1 &&
           awdl_core_mode(core) == AWDL_MODE_IDLE);
    // an infra change does not touch the v3 firmware's slots (it places the station itself)
    int before = ncalls;
    awdl_core_set_infra(core, 5180, 1200000);
    NCHECK(ncalls == before);

    // 500 ms: aftxmode 0 and osoc_chan 44; 5 s: awdl_config 0x73 and psf_dwell off
    before = ncalls;
    awdl_core_timer(core, 1600000);
    NCHECK(ncalls == before + 2 && !strcmp(calls[before].name, "awdl_aftxmode") &&
           !strcmp(calls[before + 1].name, "awdl_osoc_chan") && rd32(calls[before + 1].buf) == 44);

    // the phone's MIF (WLC_E_ACTION_FRAME_RX, body only): a reachable peer, awdl_peer_op ADD with its HT element, and
    // the election follows the phone (metric 200 > 65): opmode slave + election tree with the phone as master
    before = ncalls;
    phone_frame(core, phone, phone_addr, AWDL_SUBTYPE_MIF, 5100000, 2000000);
    NCHECK(presents == 1 && !memcmp(last_presence, phone_addr, 6));
    const struct awdl_peer *p = awdl_core_find(core, phone_addr);
    NCHECK(p && p->reachable && p->rssi == -60 && !strcmp(p->hostname, "iPhone") && p->has_seq && p->has_ht);
    NCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER && !memcmp(awdl_core_election(core)->root, phone_addr, 6));
    const struct fw_call *add = NULL, *slave = NULL, *tree = NULL;
    for (int i = before; i < ncalls; ++i) {
        if (!strcmp(calls[i].name, "awdl_peer_op"))
            add = &calls[i];
        if (!strcmp(calls[i].name, "awdl_opmode"))
            slave = &calls[i];
        if (!strcmp(calls[i].name, "awdl_election_tree"))
            tree = &calls[i];
    }
    NCHECK(add && add->len == 0x25 && bytes_eq(add->buf, 16, "00 00 0a1b2c3d4e5f 01 2d 1a ce11 1b ff ff"));
    NCHECK(slave && slave->buf[0] == 1 && slave->buf[1] == BCM_AWDL_ROLE_SLAVE &&
           !memcmp(slave->buf + 4, phone_addr, 6));
    NCHECK(tree && !memcmp(tree->buf + 0x0f, phone_addr, 6) && rd32(tree->buf + 0x15) == 200 && tree->buf[0x19] == 1);
    NCHECK(awdl_core_stats(core)->syncs_pushed == 2);
    // the same frame again: no second add, no second push
    before = ncalls;
    phone_frame(core, phone, phone_addr, AWDL_SUBTYPE_PSF, 5150000, 2050000);
    NCHECK(presents == 1 && ncalls == before && awdl_core_stats(core)->af_rx == 2);
    // the firmware's role is its status; the host's election stays in charge
    struct awdl_fw_event e;
    memset(&e, 0, sizeof e);
    e.type = WLC_E_AWDL_ROLE;
    e.status = BCM_AWDL_ROLE_SLAVE;
    awdl_core_fw_event(core, &e, NULL, 0, 2100000);
    NCHECK(awdl_core_stats(core)->fw_role == 0 && awdl_core_role(core) == AWDL_ROLE_FOLLOWER);
    // an AWDL_EVENT without a frame is logged, not counted bad
    e.type = WLC_E_AWDL_EVENT;
    e.status = 5;
    const uint8_t junk[4] = {0, 0, 0, 1};
    awdl_core_fw_event(core, &e, junk, 4, 2100000);
    NCHECK(awdl_core_stats(core)->af_bad == 0);

    // data out: the AWDL header added (the series' 802.3 frame), to the firmware's AWDL interface
    uint8_t eth[64] = {0};
    memcpy(eth, phone_addr, 6);
    memcpy(eth + 6, me, 6);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    NCHECK(awdl_core_output(core, eth, sizeof eth, 2200000) == 0 && eths == 1 && last_eth_len == 64 + 16);
    NCHECK(last_eth[12] == 0 && last_eth[13] == 64 + 16 - 14 && last_eth[14] == 0xaa && last_eth[22] == 0x03 &&
           last_eth[23] == 0x04 && last_eth[28] == 0x86 && last_eth[29] == 0xdd);
    NCHECK(awdl_core_output(core, eth, sizeof eth, 2200000) == 0 && last_eth[24] == 1);  // the sequence counts
    eth[0] = 0x33;
    NCHECK(awdl_core_output(core, eth, sizeof eth, 2200000) == 0 && eths == 3);
    eth[0] = 0x02;
    eth[5] = 0x99;
    NCHECK(awdl_core_output(core, eth, sizeof eth, 2200000) < 0 && eths == 3);  // a stranger: no route
    NCHECK(awdl_core_mode(core) == AWDL_MODE_IDLE);                             // none of these was TCP: still idle
    // AirDrop's first TCP segment to the phone: transfer mode, slot_bss with "Data+Infra" (associated on 36)
    memcpy(eth, phone_addr, 6);
    eth[14] = 0x60;  // IPv6
    eth[20] = 6;     // next header TCP
    before = ncalls;
    NCHECK(awdl_core_output(core, eth, sizeof eth, 2300000) == 0 && awdl_core_mode(core) == AWDL_MODE_TRANSFER);
    {
        uint8_t want_sb[158];
        bcm_slot_bss(want_sb, AWDL_SLOTS_TRANSFER_INFRA);
        const struct fw_call *sbt = NULL;
        for (int i = before; i < ncalls; ++i)
            if (!strcmp(calls[i].name, "slot_bss"))
                sbt = &calls[i];
        NCHECK(sbt && !memcmp(sbt->buf, want_sb, 158) && awdl_core_stats(core)->mode_changes == 1);
    }
    eth[14] = eth[20] = 0;
    // data in: the AWDL header removed before awdl0 sees it; it keeps the phone alive
    uint8_t in[64] = {0}, enc[96];
    memcpy(in, me, 6);
    memcpy(in + 6, phone_addr, 6);
    in[12] = 0x86;
    in[13] = 0xdd;
    in[20] = 0x42;
    const size_t en = bcm_awdl_encap(in, sizeof in, 7, enc, sizeof enc);
    awdl_core_rx_eth(core, enc, en, 4000000);
    NCHECK(delivered == 1 && last_rx_len == 64 && !memcmp(last_rx, in, 64));
    awdl_core_rx_eth(core, in, sizeof in, 4000000);  // not AWDL data: dropped, counted
    NCHECK(delivered == 1 && awdl_core_stats(core)->data_not_awdl == 1);
    awdl_core_timer(core, 4100000);
    NCHECK(absents == 0);

    // silence: lost after 3 s, deleted from the firmware (9 bytes), PEER_PRESENCE absent; we are master again
    before = ncalls;
    awdl_core_timer(core, 7200000);
    NCHECK(absents == 1);
    const struct fw_call *del = NULL;
    for (int i = before; i < ncalls; ++i)
        if (!strcmp(calls[i].name, "awdl_peer_op"))
            del = &calls[i];
    NCHECK(del && del->len == 9 && del->buf[1] == BCM_AWDL_PEER_OP_DEL);
    NCHECK(awdl_core_role(core) == AWDL_ROLE_ROOT);
    NCHECK(awdl_core_mode(core) == AWDL_MODE_IDLE && awdl_core_stats(core)->mode_changes == 2);  // 3 s after 2.3 s
    // the 5 s step has passed meanwhile: awdl_config 0x73, psf_dwell zero
    const struct fw_call *cfg2 = find_call("awdl_config", 1), *dw2 = find_call("awdl_psf_dwell", 1);
    NCHECK(cfg2 && rd32(cfg2->buf) == 0x73 && dw2 && zero(dw2->buf, 8));

    // off: awdl 0 and interface_remove on the AWDL interface
    before = ncalls;
    sync_enabled(&f, 0, 8000000);
    NCHECK(!awdl_core_running(core) && ncalls == before + 2 && !strcmp(calls[before].name, "awdl") &&
           rd32(calls[before].buf) == 0 && calls[before].ifidx == 2 &&
           !strcmp(calls[before + 1].name, "interface_remove") && calls[before + 1].len == 0 &&
           calls[before + 1].ifidx == 2);
    facade = NULL;
    awdl_core_stop(phone);
    free(core);
    free(phone);
}

static void test_flow_awdl_if(void)
{
    reset_calls();
    const uint8_t me[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, phone_addr[6] = {0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f};
    static struct a80211_awdl f;
    // a 7.x table without the series' newer variables
    const uint32_t absent = 1u << BCM_PROBE_INTERFACE_CREATE | 1u << BCM_PROBE_SLOT_BSS | 1u << BCM_PROBE_MCHAN |
                            1u << BCM_PROBE_AWDL_PSF_DWELL | 1u << BCM_PROBE_AWDL_OSOC_CHAN |
                            1u << BCM_PROBE_AWDL_AFTXMODE | 1u << BCM_PROBE_AWDL_OOB_AF_AUTO |
                            1u << BCM_PROBE_INTERFACE_REMOVE | 1u << BCM_PROBE_EVENT_MSGS_EXT |
                            1u << BCM_PROBE_AWDL_IF2;
    struct awdl_core *core = make_native(me, BCM_AWDL_API_AWDL_IF, absent, &f);
    awdl_core_set_infra(core, 2437, 900000);  // associated on channel 6 before AWDL starts
    sync_enabled(&f, 1, 1000000);
    NCHECK(ncalls == 1 && !strcmp(calls[0].name, "awdl_if") && !calls[0].get && calls[0].ifidx == 0 &&
           bytes_eq(calls[0].buf, 20, "02000000 01000000 002500ff9473 02aabbccddee"));
    if_event(core, me, 1, 1, 1100000);
    NCHECK(awdl_core_fw_ifidx(core) == 1);
    // the series' order with the 7.x forms and without what the table lacks
    static const char *const order[] = {"awdl_payload",
                                        "awdl_sync_params",
                                        "awdl_config",
                                        "awdl_chan_seq",
                                        "awdl_payload",
                                        "awdl_election_tree",
                                        "awdl",
                                        "awdl_presencemode",
                                        "awdl_chan_seq",
                                        "awdl_payload",
                                        "awdl_sync_params",
                                        "awdl_opmode",
                                        "awdl_election_tree",
                                        "awdl_sync_params"};
    const int n = (int)(sizeof order / sizeof order[0]);
    NCHECK(ncalls == 1 + n);
    for (int i = 0; i < n && 1 + i < ncalls; ++i)
        NCHECK(!strcmp(calls[1 + i].name, order[i]) && calls[1 + i].ifidx == 1);
    NCHECK(awdl_core_stats(core)->iovar_skipped == 4);  // aftxmode, osoc_chan, psf_dwell, oob_af_auto
    const struct fw_call *cfg = find_call("awdl_config", 0);
    NCHECK(cfg->len == 28 && rd32(cfg->buf) == 1 && cfg->buf[4] == 149 && cfg->buf[20] == 2);
    const struct fw_call *pl = find_call("awdl_payload", 0);
    NCHECK(rd32(pl->buf) + 4 == pl->len);  // 7.x: a u32 length
    // the idle sequence: 149/80 in 0, 6 in 8, 44/80 in 9-10, the access point's channel 6 everywhere else
    const struct fw_call *cs = find_call("awdl_chan_seq", 0);
    NCHECK(cs->len == 38 &&
           bytes_eq(cs->buf, 38,
                    "0f 03 00 03 ffff 9580 0651 0651 0651 0651 0651 0651 0651 0651 2c80 2c80 0651 0651 0651 "
                    "0651 0651"));
    // the phone's master channel is 149 (its default sequence): our 44 slots follow it
    struct awdl_core *phone = make_phone(phone_addr, 200);
    int before = ncalls;
    phone_frame(core, phone, phone_addr, AWDL_SUBTYPE_MIF, 5100000, 1500000);
    NCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER && count_primary(awdl_core_sequence(core), 149) == 3);
    const struct fw_call *cs2 = NULL;
    for (int i = before; i < ncalls; ++i)
        if (!strcmp(calls[i].name, "awdl_chan_seq"))
            cs2 = &calls[i];
    NCHECK(cs2 && cs2->buf[6 + 2 * 9] == 149);
    // a roam to 5 GHz channel 36: the free slots move there (opclass 115), sent again
    before = ncalls;
    awdl_core_set_infra(core, 5180, 1600000);
    NCHECK(ncalls == before + 1 && !strcmp(calls[before].name, "awdl_chan_seq") && calls[before].buf[6 + 2 * 5] == 36 &&
           calls[before].buf[7 + 2 * 5] == 115);  // the social map is unchanged: no payload
    // AirDrop data from the phone (TCP, decapsulated): transfer mode, 12 of 16 slots away, the AP keeps 6, 7, 14, 15
    {
        uint8_t in[80] = {0}, enc[112];
        memcpy(in, me, 6);
        memcpy(in + 6, phone_addr, 6);
        in[12] = 0x86;
        in[13] = 0xdd;
        in[14] = 0x60;
        in[20] = 6;
        const size_t en = bcm_awdl_encap(in, sizeof in, 1, enc, sizeof enc);
        before = ncalls;
        awdl_core_rx_eth(core, enc, en, 1650000);
        NCHECK(delivered == 1 && awdl_core_mode(core) == AWDL_MODE_TRANSFER && ncalls == before + 1 &&
               !strcmp(calls[before].name, "awdl_chan_seq"));
        NCHECK(bytes_eq(calls[before].buf, 38,
                        "0f 03 00 03 ffff 9580 9580 9580 9580 9580 9580 2473 2473 0651 9580 9580 9580 9580 9580 "
                        "2473 2473"));
        NCHECK(awdl_slots_away(awdl_core_sequence(core), 5180) == 12);
        // more data keeps it; 3 s after the last frame the deadline returns it to idle (one chan_seq each way)
        awdl_core_rx_eth(core, enc, en, 2500000);
        NCHECK(awdl_core_timer(core, 2600000) <= 2500000 + AWDL_TRANSFER_HOLD_US);
        awdl_core_timer(core, 5400000);
        NCHECK(awdl_core_mode(core) == AWDL_MODE_TRANSFER);
        before = ncalls;
        awdl_core_timer(core, 5500000);
        NCHECK(awdl_core_mode(core) == AWDL_MODE_IDLE && awdl_slots_away(awdl_core_sequence(core), 5180) == 4);
        int resent = 0;
        for (int i = before; i < ncalls; ++i)
            resent += !strcmp(calls[i].name, "awdl_chan_seq");
        NCHECK(resent == 1);
        // wifip2pd's traffic registration (a session start) switches to transfer before any data
        struct a80211_awdl_peer_traffic_registration t;
        memset(&t, 0, sizeof t);
        t.version = 1;
        memcpy(t.address, phone_addr, 6);
        memcpy(t.unique_id, "com.apple.sharing.airdrop", 25);
        t.unique_id_length = 25;
        t.active = 1;
        uint32_t tl = sizeof t;
        int32_t tv = 0;
        NCHECK(a80211_awdl_request(&f, 1, A80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION, &tv, &t, &tl, sizeof t, 5600000) ==
               0);
        NCHECK(awdl_core_mode(core) == AWDL_MODE_TRANSFER);
        awdl_core_timer(core, 5600000 + AWDL_TRANSFER_HOLD_US);  // no data followed: idle again
        NCHECK(awdl_core_mode(core) == AWDL_MODE_IDLE && awdl_core_stats(core)->mode_changes == 4);
    }
    // the firmware refuses the u32 payload: sent once more with a u16 length, then left alone
    before = ncalls;
    awdl_core_fw_result(core, "awdl_payload", BCME_BADLEN, 1700000);
    NCHECK(ncalls == before + 1 && !strcmp(calls[before].name, "awdl_payload") &&
           (uint32_t)(calls[before].buf[0] | calls[before].buf[1] << 8) + 2 == calls[before].len);
    awdl_core_fw_result(core, "awdl_payload", BCME_BADLEN, 1700000);
    NCHECK(ncalls == before + 1 && awdl_core_running(core));
    // off: awdl 0 on the AWDL interface, then awdl_if down on the station's
    before = ncalls;
    sync_enabled(&f, 0, 8000000);
    NCHECK(!strcmp(calls[before].name, "awdl_peer_op") || !strcmp(calls[before].name, "awdl"));
    const struct fw_call *last = &calls[ncalls - 1];
    NCHECK(!awdl_core_running(core) && !strcmp(last->name, "awdl_if") && last->ifidx == 0 && last->buf[4] == 0);
    NCHECK(!strcmp(calls[ncalls - 2].name, "awdl") && rd32(calls[ncalls - 2].buf) == 0 && calls[ncalls - 2].ifidx == 1);
    facade = NULL;
    awdl_core_stop(phone);
    free(core);
    free(phone);
}

static void test_create_failures(void)
{
    const uint8_t me[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    static struct a80211_awdl f;
    // interface_create refused although it answered version 3: awdl_if instead (the table has it)
    reset_calls();
    struct awdl_core *core = make_native(me, BCM_AWDL_API_IFC_V3, 0, &f);
    sync_enabled(&f, 1, 1000000);
    awdl_core_fw_result(core, "interface_create", BCME_UNSUPPORTED, 1010000);
    NCHECK(awdl_core_running(core) && ncalls == 2 && !strcmp(calls[1].name, "awdl_if"));
    // ... and a WLC_E_IF for another address (not ours) does not count
    const uint8_t other[6] = {0x02, 1, 2, 3, 4, 5};
    if_event(core, other, 1, 1, 1020000);
    NCHECK(awdl_core_fw_ifidx(core) == -1);
    // no WLC_E_IF within 2 s of awdl_if: off
    awdl_core_timer(core, 3000000);
    NCHECK(awdl_core_running(core));
    awdl_core_timer(core, 3020000);
    NCHECK(!awdl_core_running(core) && awdl_core_timer(core, 3100000) == UINT64_MAX);
    free(core);
    // neither creation in the table: off at once, nothing sent
    reset_calls();
    core = make_native(me, BCM_AWDL_API_NONE, 0, &f);
    struct a80211_awdl_sync_enabled on = {1, 0, 1};
    uint32_t len = sizeof on;
    int32_t val = 0;
    a80211_awdl_request(&f, 1, A80211_IOC_AWDL_SYNC_ENABLED, &val, &on, &len, sizeof on, 1000000);
    NCHECK(!awdl_core_running(core) && ncalls == 0);
    free(core);
    // unassociated 7.x: the free slots are empty; a firmware refusing that gets them on slot 0's channel, once
    reset_calls();
    core = make_native(me, BCM_AWDL_API_AWDL_IF, 1u << BCM_PROBE_INTERFACE_CREATE | 1u << BCM_PROBE_SLOT_BSS, &f);
    sync_enabled(&f, 1, 1000000);
    if_event(core, me, 1, 1, 1100000);
    const struct fw_call *seq0 = find_call("awdl_chan_seq", 0);
    NCHECK(seq0 && seq0->buf[6 + 2 * 5] == 0 && seq0->buf[6 + 2 * 15] == 0);
    int was = ncalls;
    awdl_core_fw_result(core, "awdl_chan_seq", BCME_BADCHAN, 1200000);
    NCHECK(ncalls == was + 1 && !strcmp(calls[was].name, "awdl_chan_seq") && calls[was].buf[6 + 2 * 5] == 149 &&
           calls[was].buf[7 + 2 * 5] == 0x80 && count_primary(awdl_core_sequence(core), 0) == 0);
    awdl_core_fw_result(core, "awdl_chan_seq", BCME_BADCHAN, 1200000);
    NCHECK(ncalls == was + 1 && awdl_core_running(core));
    free(core);
    // only awdl_if2 in the table: that name, same 20 bytes
    reset_calls();
    core = make_native(me, BCM_AWDL_API_AWDL_IF, 1u << BCM_PROBE_INTERFACE_CREATE | 1u << BCM_PROBE_AWDL_IF, &f);
    sync_enabled(&f, 1, 1000000);
    NCHECK(ncalls == 1 && !strcmp(calls[0].name, "awdl_if2") && calls[0].len == 20);
    awdl_core_fw_result(core, "awdl_if2", BCME_BADARG, 1010000);
    NCHECK(!awdl_core_running(core));
    free(core);
    // awdl_if refused with no other way: off
    reset_calls();
    core = make_native(me, BCM_AWDL_API_AWDL_IF, 1u << BCM_PROBE_INTERFACE_CREATE, &f);
    sync_enabled(&f, 1, 1000000);
    awdl_core_fw_result(core, "awdl_if", BCME_BADARG, 1010000);
    NCHECK(!awdl_core_running(core));
    facade = NULL;
    free(core);
}

void test_native_main(void)
{
    test_encoders();
    test_probe();
    test_monitor_probe();
    test_radio_verdict();
    test_down_interface();
    test_schedule();
    test_flow_ifc_v3();
    test_flow_awdl_if();
    test_create_failures();
}
