// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the AWDL core and awdl0's Apple80211 side. Built and run by test:awdl_test (BUILD.bazel) with the Mac's
// clang, sanitizers on. The "air" is simulated: every node has its own TSF (offset and drift from a global clock), a
// radio that follows the schedule the core installs, and a medium that delivers each frame to the nodes in range that
// are on that channel when it goes out.
#include "../awdl.h"
#include "../awdl_apple80211.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int test_failures;
#define failures test_failures
void test_native_main(void);  // test_native.c
void test_owl_main(void);     // test_owl.c
void test_sched_main(void);   // test_sched.c
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static void hex(const char *s, uint8_t *out, size_t *n)
{
    size_t k = 0;
    for (; s[0] && s[1];) {
        if (s[0] == ' ') {
            s++;
            continue;
        }
        unsigned v;
        sscanf(s, "%2x", &v);
        out[k++] = (uint8_t)v;
        s += 2;
    }
    *n = k;
}

// ---- the simulated air -------------------------------------------------------------------------------------------------
#define NODES 4

struct node {
    struct awdl_core *core;
    struct awdl_tsf_clock clock;
    int64_t offset;  // local TSF = global + offset + global * ppm / 1e6
    int32_t ppm;
    struct awdl_chan sched[AWDL_SEQ_SLOTS];
    uint64_t anchor;
    int scheduled, schedules;
    uint64_t deadline;  // local
    int peers_present, peers_lost;
    uint8_t last_peer[6];
    uint8_t delivered[2048];
    size_t delivered_len;
    int deliveries;
    struct a80211_awdl *facade;
};
static struct node nodes[NODES];
static int nnodes;
static uint64_t sim_now;
static int reach[NODES][NODES];  // can i hear j

struct frame {
    uint64_t at;  // global
    int from;
    uint16_t freq;
    size_t len;
    uint8_t data[1700];
};
static struct frame air[512];
static int nair;
static int air_tx_count, air_lost_offchannel;

static uint64_t local_of(int i, uint64_t g)
{
    return (uint64_t)((int64_t)g + nodes[i].offset + (int64_t)g * nodes[i].ppm / 1000000);
}

static uint64_t global_of(int i, uint64_t l)
{
    const int64_t x = (int64_t)l - nodes[i].offset;
    return (uint64_t)(x - x * nodes[i].ppm / 1000000);
}

static int node_index(void *ctx)
{
    return (int)(intptr_t)ctx;
}

static int sim_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)no_ack;
    (void)stay_until;
    const int i = node_index(ctx);
    if (nair == (int)(sizeof air / sizeof air[0]) || len > sizeof air[0].data)
        return -1;
    struct frame *f = &air[nair++];
    f->from = i;
    f->freq = freq;
    f->len = len;
    memcpy(f->data, frame, len);
    f->at = 0;  // "now", fixed up by the loop
    air_tx_count++;
    return 0;
}

static void sim_schedule(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor)
{
    struct node *n = &nodes[node_index(ctx)];
    memcpy(n->sched, seq, sizeof n->sched);
    n->anchor = anchor;
    n->scheduled = 1;
    n->schedules++;
}

static void sim_peer(void *ctx, const uint8_t addr[6], int present, int8_t rssi)
{
    (void)rssi;
    struct node *n = &nodes[node_index(ctx)];
    if (present)
        n->peers_present++;
    else
        n->peers_lost++;
    memcpy(n->last_peer, addr, 6);
    if (n->facade)
        a80211_awdl_peer(n->facade, addr, present, rssi);
}

static void sim_deliver(void *ctx, const uint8_t *eth, size_t len)
{
    struct node *n = &nodes[node_index(ctx)];
    memcpy(n->delivered, eth, len < sizeof n->delivered ? len : sizeof n->delivered);
    n->delivered_len = len;
    n->deliveries++;
}

static int verbose;

static void sim_log(void *ctx, const char *line)
{
    if (verbose)
        printf("  [node %d] %s\n", node_index(ctx), line);
}

// the radio of node i is on which frequency at global time g
static uint16_t radio_freq(int i, uint64_t g)
{
    struct node *n = &nodes[i];
    if (!n->scheduled)
        return 0;
    const uint64_t l = local_of(i, g);
    if (l < n->anchor)
        return awdl_chan_freq(n->sched[0]);
    const uint64_t slot = ((l - n->anchor) / (64 * 1024)) % AWDL_SEQ_SLOTS;
    return awdl_chan_freq(n->sched[slot]);
}

static void sim_reset(int count)
{
    for (int i = 0; i < nnodes; ++i)
        free(nodes[i].core);
    memset(nodes, 0, sizeof nodes);
    memset(reach, 0, sizeof reach);
    nair = 0;
    sim_now = 0;
    air_tx_count = air_lost_offchannel = 0;
    nnodes = count;
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < count; ++j)
            reach[i][j] = i != j;
}

static void sim_node(int i, uint8_t last, uint32_t metric, int64_t offset, int32_t ppm)
{
    struct node *n = &nodes[i];
    n->core = (struct awdl_core *)calloc(1, awdl_core_size());
    n->offset = offset;
    n->ppm = ppm;
    const uint8_t addr[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last};
    struct awdl_config cfg;
    awdl_config_default(&cfg, addr);
    cfg.self_metric = metric;
    cfg.backend = AWDL_BACKEND_SOFT;  // the simulated air: the core runs the whole protocol
    snprintf(cfg.hostname, sizeof cfg.hostname, "node%d", i);
    const struct awdl_host h = {(void *)(intptr_t)i, sim_tx, sim_schedule, sim_peer, sim_deliver, sim_log};
    CHECK(awdl_core_init(n->core, &h, &cfg) == 0);
    // one social channel all the way round (OWL's static sequence), so nodes meet before they are synchronised
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned k = 0; k < AWDL_SEQ_SLOTS; ++k)
        seq[k] = (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(n->core, seq, 0);
}

static void sim_start(int i, uint64_t g)
{
    awdl_core_start(nodes[i].core, local_of(i, g));
    nodes[i].deadline = awdl_core_timer(nodes[i].core, local_of(i, g));
}

// Run until global time `until`: timers and frame deliveries in time order.
static void sim_run(uint64_t until)
{
    for (long steps = 0;; ++steps) {
        if (steps > 10000000) {  // a deadline that never advances: fail rather than hang
            fprintf(stderr, "FAIL: simulation stuck at %llu (%d frames queued)\n", (unsigned long long)sim_now, nair);
            for (int i = 0; i < nnodes; ++i)
                fprintf(stderr, "  node %d running %d deadline local %llu global %llu now local %llu\n", i,
                        awdl_core_running(nodes[i].core), (unsigned long long)nodes[i].deadline,
                        (unsigned long long)global_of(i, nodes[i].deadline), (unsigned long long)local_of(i, sim_now));
            for (int k = 0; k < nair && k < 4; ++k)
                fprintf(stderr, "  frame %d from %d at %llu\n", k, air[k].from, (unsigned long long)air[k].at);
            failures++;
            return;
        }
        uint64_t next = until;
        int which = -1, kind = 0;
        for (int i = 0; i < nnodes; ++i) {
            if (!awdl_core_running(nodes[i].core))
                continue;
            const uint64_t g = global_of(i, nodes[i].deadline);
            if (g < next) {
                next = g;
                which = i;
                kind = 1;
            }
        }
        for (int k = 0; k < nair; ++k) {
            if (!air[k].at)
                air[k].at = sim_now;
            if (air[k].at <= next) {
                next = air[k].at;
                which = k;
                kind = 2;
            }
        }
        if (next < sim_now)
            next = sim_now;
        if (which < 0 || next > until)
            break;
        sim_now = next;
        if (kind == 1) {
            // the node's own clock reads at least its deadline (global -> local rounding must not stall it)
            struct node *n = &nodes[which];
            uint64_t l = local_of(which, sim_now);
            if (l < n->deadline)
                l = n->deadline;
            n->deadline = awdl_core_timer(n->core, l);
            if (n->deadline <= l)
                n->deadline = l + 1;
        } else {
            struct frame f = air[which];
            air[which] = air[--nair];
            for (int j = 0; j < nnodes; ++j) {
                if (!reach[j][f.from] || !awdl_core_running(nodes[j].core))
                    continue;
                if (radio_freq(j, f.at) != f.freq || radio_freq(f.from, f.at) != f.freq) {
                    air_lost_offchannel++;
                    continue;
                }
                const struct awdl_rx_meta m = {local_of(j, f.at), f.freq, -50};
                awdl_core_rx(nodes[j].core, f.data, f.len, &m);
                nodes[j].deadline = awdl_core_timer(nodes[j].core, local_of(j, f.at));
            }
        }
    }
    sim_now = until;
}

// ---- tests ----------------------------------------------------------------------------------------------------------------
// A PSF and a MIF written by hand from the published field layout (not by our encoder), as an iPhone sends them:
// sync params with a 16-entry channel sequence on 149/80 MHz, election v1 + v2, channel sequence, service params,
// HT caps, data path state, arpa "iPhone", version 3.4 class 2.
static const char *kHandMif =
    "d000 0000 ffffffffffff 0a1b2c3d4e5f 002500ff9473 1000"
    "7f 0017f2 08 10 03 00 e8030000 e0030000"
    // sync params (4): tx chan 149, down counter 9 TU, master chan 149, guard 0, AW period 16, AF period 110,
    // flags 0x1800, ext 16, common 16, remaining 0, ext counts 3/3/3/3, master 0a1b2c3d4e5f, presence 4, rsvd,
    // AW seq 0x1234, beacon delta 0, chanseq (15, opclass, 0, 3, ffff, 16 x {155, 0x80}), padding
    "04 4900 95 0900 95 00 1000 6e00 0018 1000 1000 0000 03 03 03 03 0a1b2c3d4e5f 04 00 3412 0000"
    "0f 03 00 03 ffff 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 0000"
    // election v1 (5)
    "05 1500 00 0000 00 00 0a1b2c3d4e5f c8000000 c8000000 0000"
    // channel sequence (18)
    "12 2900 0f 03 00 03 ffff 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 000000"
    // election v2 (24): root = parent = self, counter 7, 0 hops, metrics 200
    "18 2800 0a1b2c3d4e5f 0a1b2c3d4e5f 07000000 00000000 c8000000 c8000000 00000000 00000000 07000000"
    // service params (6), HT caps (7)
    "06 0600 00 0000 0500 00"
    "07 0800 0000 ce11 17 ff 0000"
    // data path state (12): country "US", social map, AWDL address
    "0c 0d00 0403 555300 0400 0a1b2c3d4e5f"
    // arpa (16): flags 3, "iPhone", .local
    "10 0a00 03 06 6950686f6e65 c00c"
    // version (21)
    "15 0200 34 02";

static void test_parse_hand_frame(void)
{
    uint8_t f[512];
    size_t n;
    hex(kHandMif, f, &n);
    struct awdl_af a;
    CHECK(awdl_af_parse(f, n, &a) == 0);
    CHECK(a.subtype == AWDL_SUBTYPE_MIF && a.version == 0x10);
    CHECK(a.phy_tx_time == 1000 && a.target_tx_time == 992);
    const uint8_t src[6] = {0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f};
    CHECK(!memcmp(a.src, src, 6));
    CHECK(a.present & (1u << AWDL_TLV_SYNC_PARAMS));
    CHECK(a.sync.tx_channel == 149 && a.sync.tx_counter == 9 && a.sync.aw_period == 16 && a.sync.af_period == 110);
    CHECK(a.sync.aw_seq == 0x1234 && a.sync.presence_mode == 4 && !memcmp(a.sync.master, src, 6));
    CHECK(a.sync.has_seq && a.sync.seq.count == 16 && a.sync.seq.chan[3].number == 155 &&
          a.sync.seq.chan[3].opclass == 0x80);
    CHECK(a.present & (1u << AWDL_TLV_ELECTION_PARAMS));
    CHECK(a.election.master_metric == 200 && !memcmp(a.election.master, src, 6));
    CHECK(a.present & (1u << AWDL_TLV_ELECTION_PARAMS_V2));
    CHECK(a.election2.root_counter == 7 && a.election2.hops == 0 && a.election2.root_metric == 200);
    CHECK(a.present & (1u << AWDL_TLV_CHANNEL_SEQUENCE));
    CHECK(a.chanseq.count == 16 && awdl_chan_freq(a.chanseq.chan[0]) == 5745);
    CHECK(a.service_update_indicator == 5);
    CHECK(a.ht_caps == 0x11ce);
    CHECK(a.dps.flags == (AWDL_DPS_COUNTRY_CODE | AWDL_DPS_SOCIAL_CHANNEL_MAP | AWDL_DPS_AWDL_ADDRESS));
    CHECK(a.dps.country[0] == 'U' && a.dps.country[1] == 'S' && !memcmp(a.dps.awdl_address, src, 6));
    CHECK(!strcmp(a.hostname, "iPhone"));
    CHECK(a.awdl_version == 0x34 && a.device_class == 2);

    // every truncation of the frame is either rejected or parses without reading past the end (ASan watches)
    for (size_t k = 0; k < n; ++k) {
        uint8_t *copy = (uint8_t *)malloc(k ? k : 1);
        memcpy(copy, f, k);
        (void)awdl_af_parse(copy, k, &a);
        free(copy);
    }
    // random bytes behind a valid header: never a crash
    srand(1);
    for (int round = 0; round < 20000; ++round) {
        uint8_t r[300];
        memcpy(r, f, 40);
        const size_t len = 40 + (size_t)(rand() % 260);
        for (size_t k = 40; k < len; ++k)
            r[k] = (uint8_t)rand();
        uint8_t *copy = (uint8_t *)malloc(len);
        memcpy(copy, r, len);
        (void)awdl_af_parse(copy, len, &a);
        free(copy);
    }
    // not AWDL: wrong BSSID, wrong OUI type, not an action frame
    uint8_t g[512];
    memcpy(g, f, n);
    g[21] ^= 1;
    CHECK(awdl_af_parse(g, n, &a) < 0);
    memcpy(g, f, n);
    g[28] = 0x09;
    CHECK(awdl_af_parse(g, n, &a) < 0);
    memcpy(g, f, n);
    g[0] = 0x80;
    CHECK(awdl_af_parse(g, n, &a) < 0);
    // a TLV whose length runs past the end rejects the frame
    memcpy(g, f, n);
    g[n - 4] = 0x40;  // version TLV length 0x40
    CHECK(awdl_af_parse(g, n, &a) < 0);
}

static void test_build_roundtrip(void)
{
    sim_reset(1);
    sim_node(0, 0x01, 60, 5000000, 0);
    sim_start(0, 1000);
    uint8_t buf[1600];
    const uint64_t now = local_of(0, 77777);
    const size_t n = awdl_core_build_af(nodes[0].core, AWDL_SUBTYPE_MIF, now, buf, sizeof buf);
    CHECK(n > 100);
    struct awdl_af a;
    CHECK(awdl_af_parse(buf, n, &a) == 0);
    const uint32_t want = (1u << AWDL_TLV_SYNC_PARAMS) | (1u << AWDL_TLV_ELECTION_PARAMS) |
                          (1u << AWDL_TLV_CHANNEL_SEQUENCE) | (1u << AWDL_TLV_ELECTION_PARAMS_V2) |
                          (1u << AWDL_TLV_SERVICE_PARAMS) | (1u << AWDL_TLV_HT_CAPS) |
                          (1u << AWDL_TLV_DATA_PATH_STATE) | (1u << AWDL_TLV_ARPA) | (1u << AWDL_TLV_VERSION);
    CHECK((a.present & want) == want);
    CHECK(!strcmp(a.hostname, "node0"));
    CHECK(a.sync.seq.count == 16 && a.chanseq.count == 16);
    // the down counter is the TU to the next EAW (OWL's rounding up), exact at target_tx_time, which is at most a TU
    // before the build time in phy_tx_time; the AW number is the current one
    const uint64_t target = (now & ~0xffffffffull) | a.target_tx_time;
    const uint64_t next = target + (uint64_t)a.sync.tx_counter * AWDL_TU_US;
    CHECK(a.phy_tx_time == (uint32_t)now && a.sync.aw_seq == awdl_core_aw_seq(nodes[0].core, now));
    CHECK(awdl_core_aw_start(nodes[0].core, (uint16_t)((a.sync.aw_seq & ~3u) + 4), now) == next);
    CHECK(target <= now && now - target < AWDL_TU_US && a.sync.tx_counter >= 1 && a.sync.tx_counter <= 64);
    CHECK(a.sync.presence_mode == 4 && a.sync.ap_beacon_delta == (int16_t)a.sync.aw_seq);
    const size_t p = awdl_core_build_af(nodes[0].core, AWDL_SUBTYPE_PSF, now, buf, sizeof buf);
    CHECK(awdl_af_parse(buf, p, &a) == 0 && a.subtype == AWDL_SUBTYPE_PSF && !(a.present & (1u << AWDL_TLV_ARPA)));
    CHECK(awdl_core_build_af(nodes[0].core, AWDL_SUBTYPE_MIF, now, buf, 80) == 0);  // too small: nothing
}

static void test_data_encap(void)
{
    uint8_t eth[200], f[400], back[400];
    const uint8_t dst[6] = {0x33, 0x33, 0, 0, 0, 0xfb}, self[6] = {0x02, 1, 2, 3, 4, 5};
    memcpy(eth, dst, 6);
    memcpy(eth + 6, self, 6);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    for (int i = 14; i < 200; ++i)
        eth[i] = (uint8_t)i;
    const size_t n = awdl_data_encap(eth, 200, self, 0x123, f, sizeof f);
    CHECK(n == 200 - 14 + 40);
    const uint8_t want[] = {0x08, 0x00, 0,    0,    0x33, 0x33, 0,    0,    0,    0xfb, 0x02, 1,    2,    3,
                            4,    5,    0x00, 0x25, 0x00, 0xff, 0x94, 0x73, 0x30, 0x12, 0xaa, 0xaa, 0x03, 0x00,
                            0x17, 0xf2, 0x08, 0x00, 0x03, 0x04, 0x23, 0x01, 0x00, 0x00, 0x86, 0xdd};
    CHECK(!memcmp(f, want, sizeof want));
    uint16_t seq;
    int retry;
    CHECK(awdl_data_decap(f, n, back, sizeof back, &seq, &retry) == 200 && !memcmp(back, eth, 200));
    CHECK(seq == 0x123 && !retry);
    // QoS data (two more header bytes), retry bit set
    uint8_t q[420];
    memcpy(q, f, 24);
    q[0] = 0x88;
    q[1] = 0x08;
    q[24] = 0;
    q[25] = 0;
    memcpy(q + 26, f + 24, n - 24);
    CHECK(awdl_data_decap(q, n + 2, back, sizeof back, &seq, &retry) == 200 && !memcmp(back, eth, 200) && retry);
    // not AWDL: other LLC, ToDS set, other BSSID, too short
    memcpy(q, f, n);
    q[29] = 0x18;
    CHECK(awdl_data_decap(q, n, back, sizeof back, NULL, NULL) == 0);
    memcpy(q, f, n);
    q[1] = 0x01;
    CHECK(awdl_data_decap(q, n, back, sizeof back, NULL, NULL) == 0);
    memcpy(q, f, n);
    q[17] = 0x26;
    CHECK(awdl_data_decap(q, n, back, sizeof back, NULL, NULL) == 0);
    CHECK(awdl_data_decap(f, 39, back, sizeof back, NULL, NULL) == 0);
    CHECK(awdl_data_encap(eth, 13, self, 0, f, sizeof f) == 0);
}

// Two nodes 1.7 s of TSF apart with 40 ppm between their crystals: the low-metric node follows the other and its AWs
// line up with the master's to within a few microseconds; both see each other as reachable; data both ways.
static void test_sync_two_nodes(void)
{
    sim_reset(2);
    sim_node(0, 0x0a, 200, 1700000, 0);  // an "iPhone": higher metric
    sim_node(1, 0x0b, 60, 333333, 40);   // us
    sim_start(0, 0);
    sim_start(1, 50000);
    sim_run(3000000);
    CHECK(awdl_core_role(nodes[0].core) == AWDL_ROLE_ROOT);
    CHECK(awdl_core_role(nodes[1].core) == AWDL_ROLE_FOLLOWER);
    CHECK(!memcmp(awdl_core_election(nodes[1].core)->root, awdl_core_election(nodes[0].core)->root, 6));
    CHECK(awdl_core_election(nodes[1].core)->hops == 1);
    // the same AW number at the same instant, and its start agrees within 8 us in global time
    for (uint64_t g = 3000000; g < 3500000; g += 9973) {
        const uint16_t s0 = awdl_core_aw_seq(nodes[0].core, local_of(0, g));
        const uint16_t s1 = awdl_core_aw_seq(nodes[1].core, local_of(1, g));
        CHECK(s0 == s1);
        const uint64_t a0 = global_of(0, awdl_core_aw_start(nodes[0].core, s0, local_of(0, g)));
        const uint64_t a1 = global_of(1, awdl_core_aw_start(nodes[1].core, s1, local_of(1, g)));
        const int64_t d = (int64_t)a0 - (int64_t)a1;
        // between synchronisations the follower free-runs: 40 ppm apart, plus 4 us
        const int64_t bound = 4 + (int64_t)(g - 3000000 + 120000) * 45 / 1000000;
        if (!(d > -bound && d < bound))
            fprintf(stderr, "  g %llu seq %u: AW start differs by %lld us\n", (unsigned long long)g, s0, (long long)d);
        CHECK(d > -bound && d < bound);
    }
    CHECK(nodes[0].peers_present == 1 && nodes[1].peers_present == 1);
    CHECK(nodes[1].schedules >= 2);  // resynchronised at least once after starting on its own
    const struct awdl_peer *p = awdl_core_find(nodes[1].core, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0a});
    CHECK(p && p->reachable && !strcmp(p->hostname, "node0") && p->awdl_version == 0x34);
    const struct awdl_stats *st = awdl_core_stats(nodes[0].core);
    CHECK(st->psf_tx > 20 && st->mif_tx > 40);

    // data: an IPv6 packet from node 1 to node 0, and a multicast mDNS one
    uint8_t eth[120];
    memcpy(eth, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0a}, 6);
    memcpy(eth + 6, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0b}, 6);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    for (int i = 14; i < 120; ++i)
        eth[i] = (uint8_t)(i * 3);
    CHECK(awdl_core_output(nodes[1].core, eth, sizeof eth, local_of(1, sim_now)) == 0);
    sim_run(sim_now + 200000);
    CHECK(nodes[0].deliveries == 1 && nodes[0].delivered_len == sizeof eth &&
          !memcmp(nodes[0].delivered, eth, sizeof eth));
    memcpy(eth, (const uint8_t[6]){0x33, 0x33, 0, 0, 0, 0xfb}, 6);
    CHECK(awdl_core_output(nodes[0].core, eth, sizeof eth, local_of(0, sim_now)) == 0);  // (source rewritten)
    sim_run(sim_now + 200000);
    CHECK(nodes[1].deliveries == 1 && !memcmp(nodes[1].delivered, eth, 6));
    CHECK(!memcmp(nodes[1].delivered + 6, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0a}, 6));
    // to a peer nobody knows: refused (no route)
    memcpy(eth, (const uint8_t[6]){0x02, 0x99, 0x99, 0x99, 0x99, 0x99}, 6);
    CHECK(awdl_core_output(nodes[1].core, eth, sizeof eth, local_of(1, sim_now)) < 0);

    // the master goes quiet: the follower loses it after the timeout and becomes root
    reach[1][0] = 0;
    sim_run(sim_now + 4000000);
    CHECK(nodes[1].peers_lost == 1);
    CHECK(awdl_core_role(nodes[1].core) == AWDL_ROLE_ROOT);
    awdl_core_stop(nodes[0].core);
    awdl_core_stop(nodes[1].core);
}

// A chain A - B - C (A and C out of range of each other): C reaches A's tree through B, 2 hops; the slower crystal at
// the end still lines up. Then A disappears: B and C settle on a new root without a loop.
static void test_election_chain(void)
{
    sim_reset(3);
    sim_node(0, 0x0a, 300, 0, 0);
    sim_node(1, 0x0b, 100, 999999, -25);
    sim_node(2, 0x0c, 60, 123456, 30);
    reach[0][2] = reach[2][0] = 0;
    sim_start(0, 0);
    sim_start(1, 10000);
    sim_start(2, 20000);
    sim_run(4000000);
    CHECK(awdl_core_role(nodes[0].core) == AWDL_ROLE_ROOT);
    CHECK(awdl_core_role(nodes[1].core) == AWDL_ROLE_FOLLOWER && awdl_core_election(nodes[1].core)->hops == 1);
    CHECK(awdl_core_role(nodes[2].core) == AWDL_ROLE_FOLLOWER && awdl_core_election(nodes[2].core)->hops == 2);
    CHECK(!memcmp(awdl_core_election(nodes[2].core)->root, awdl_core_election(nodes[0].core)->root, 6));
    CHECK(
        !memcmp(awdl_core_election(nodes[2].core)->parent, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0b}, 6));
    const uint64_t g = 4000000;
    const uint16_t s0 = awdl_core_aw_seq(nodes[0].core, local_of(0, g));
    CHECK(awdl_core_aw_seq(nodes[2].core, local_of(2, g)) == s0);
    const int64_t d = (int64_t)global_of(0, awdl_core_aw_start(nodes[0].core, s0, local_of(0, g))) -
                      (int64_t)global_of(2, awdl_core_aw_start(nodes[2].core, s0, local_of(2, g)));
    CHECK(d > -16 && d < 16);
    // A leaves
    reach[1][0] = reach[0][1] = 0;
    sim_run(sim_now + 6000000);
    CHECK(awdl_core_role(nodes[1].core) == AWDL_ROLE_ROOT);
    CHECK(awdl_core_role(nodes[2].core) == AWDL_ROLE_FOLLOWER && awdl_core_election(nodes[2].core)->hops == 1);
    CHECK(!memcmp(awdl_core_election(nodes[2].core)->root, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0b}, 6));
    for (int i = 0; i < 3; ++i)
        awdl_core_stop(nodes[i].core);
}

// A sequence that alternates 149 and 6 slot by slot: frames only go out on the slot's channel, and the follower,
// which uses the same sequence, still synchronises.
static void test_two_channel_sequence(void)
{
    sim_reset(2);
    sim_node(0, 0x0a, 200, 0, 0);
    sim_node(1, 0x0b, 60, 77777, 10);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = i % 2 ? (struct awdl_chan){6, 0x51} : (struct awdl_chan){155, 0x80};
    sim_start(0, 0);
    sim_start(1, 0);
    awdl_core_set_sequence(nodes[0].core, seq, local_of(0, 0));
    awdl_core_set_sequence(nodes[1].core, seq, local_of(1, 0));
    sim_run(3000000);
    CHECK(awdl_core_role(nodes[1].core) == AWDL_ROLE_FOLLOWER);
    CHECK(nodes[1].peers_present == 1);
    const uint64_t g = 3000000;
    CHECK(awdl_core_aw_seq(nodes[0].core, local_of(0, g)) == awdl_core_aw_seq(nodes[1].core, local_of(1, g)));
    CHECK(awdl_chan_freq(awdl_core_channel_at(nodes[1].core, local_of(1, g))) ==
          awdl_chan_freq(awdl_core_channel_at(nodes[0].core, local_of(0, g))));
    awdl_core_stop(nodes[0].core);
    awdl_core_stop(nodes[1].core);
}

static void test_tsf_clock(void)
{
    struct awdl_tsf_clock k;
    memset(&k, 0, sizeof k);
    // the radio runs 20 ppm fast against the host
    for (uint64_t s = 0; s <= 10; ++s) {
        const uint64_t host = 5000000000ull + s * 1000000000ull;
        const uint64_t tsf = 1000000 + s * 1000020;
        awdl_tsf_clock_anchor(&k, tsf, host);
    }
    const uint64_t host = 5000000000ull + 10500000000ull;
    const uint64_t want = 1000000 + 10 * 1000020 + 500010;
    const int64_t err = (int64_t)awdl_tsf_clock_now(&k, host) - (int64_t)want;
    CHECK(err > -5 && err < 5);
    const int64_t back = (int64_t)awdl_tsf_clock_host(&k, want) - (int64_t)host;
    CHECK(back > -5000 && back < 5000);
}

static void test_ring(void)
{
    static uint8_t buf[512];
    static volatile uint32_t state[2];
    struct a80211_ring r;
    CHECK(a80211_ring_init(&r, buf, sizeof buf, state) == 0);
    uint8_t payload[200], out[4096];
    char ifname[16];
    uint32_t event, len;
    for (int i = 0; i < 200; ++i)
        payload[i] = (uint8_t)(i ^ 0x5a);
    // many rounds of uneven sizes force wraps of the payload and short tails
    for (int round = 0; round < 300; ++round) {
        const uint32_t n = (uint32_t)(round * 37 % 150);
        CHECK(a80211_ring_post(&r, 47, "awdl0", payload, n) == 0);
        CHECK(a80211_ring_take(&r, &event, ifname, out, &len, sizeof out) == 1);
        CHECK(event == 47 && len == n && !memcmp(out, payload, n) && !strcmp(ifname, "awdl0"));
        CHECK((state[0] & 3) == 0 && state[0] == state[1]);
    }
    CHECK(a80211_ring_take(&r, &event, ifname, out, &len, sizeof out) == 0);
    // a reader that does not read: the ring fills, then drops (never overwrites unread events)
    int posted = 0;
    while (a80211_ring_post(&r, 46, "awdl0", payload, 68) == 0)
        posted++;
    CHECK(posted >= 3 && r.dropped == 1);
    for (int i = 0; i < posted; ++i)
        CHECK(a80211_ring_take(&r, &event, ifname, out, &len, sizeof out) == 1 && event == 46);
    CHECK(a80211_ring_take(&r, &event, ifname, out, &len, sizeof out) == 0);
    CHECK(a80211_ring_post(&r, 1, "awdl0", payload, 0xff1) == -1);  // larger than IO80211 accepts
    // the header layout IO80211 reads: magic, lengths at 0x18/0x1a, event at 0x1c, name at 0x20
    CHECK(a80211_ring_init(&r, buf, sizeof buf, state) == 0);
    CHECK(a80211_ring_post(&r, 195, "awdl0", payload, 5) == 0);
    uint32_t w;
    uint16_t h;
    memcpy(&w, buf, 4);
    CHECK(w == 0x57);
    memcpy(&h, buf + 0x18, 2);
    CHECK(h == 5);
    memcpy(&h, buf + 0x1a, 2);
    CHECK(h == 3);
    memcpy(&w, buf + 0x1c, 4);
    CHECK(w == 195);
    CHECK(!memcmp(buf + 0x20, "awdl0", 6));
    CHECK(state[1] == 0x30 + 8);
}

// awdl0's requests as wifip2pd sends them, and the PEER_PRESENCE events the stack produces.
static struct a80211_ring facade_ring;
static int started, stopped;

static int f_start(void *ctx)
{
    (void)ctx;
    started++;
    return 0;
}

static void f_stop(void *ctx)
{
    (void)ctx;
    stopped++;
}

static void f_post(void *ctx, uint32_t event, const void *payload, uint32_t len)
{
    (void)ctx;
    a80211_ring_post(&facade_ring, event, "awdl0", payload, len);
}

static void test_facade(void)
{
    static uint8_t buf[8192];
    static volatile uint32_t state[2];
    CHECK(a80211_ring_init(&facade_ring, buf, sizeof buf, state) == 0);
    sim_reset(2);
    sim_node(0, 0x0a, 200, 0, 0);
    sim_node(1, 0x0b, 60, 4242, 0);
    static struct a80211_awdl f;
    const uint8_t caps[12] = {0x4c, 0x46, 0x0a, 0, 0x01};
    const struct a80211_awdl_ops ops = {NULL, f_start, f_stop, f_post, NULL};
    a80211_awdl_init(&f, nodes[1].core, &ops, "awdl0", "en0", caps);
    nodes[1].facade = &f;
    uint8_t data[512];
    uint32_t len;
    int32_t val = 0;
    // GETs before AWDL is on
    len = 0;
    CHECK(a80211_awdl_request(&f, 0, A80211_IOC_VIRTUAL_IF_PARENT, &val, data, &len, 16, 0) == 0 && len == 16 &&
          !strcmp((char *)data, "en0"));
    CHECK(a80211_awdl_request(&f, 0, A80211_IOC_CARD_CAPABILITIES, &val, data, &len, 16, 0) == 0 && (data[4] & 1));
    CHECK(a80211_awdl_request(&f, 0, A80211_IOC_SSID, &val, data, &len, 32, 0) == ENOENT);
    CHECK(a80211_awdl_request(&f, 0, A80211_IOC_DRIVER_VERSION, &val, data, &len, 256, 0) == 0 && data[0] == 'S');
    // wifip2pd turns AWDL on: {version 1, flags 0, enabled 1}
    const struct a80211_awdl_sync_enabled on = {1, 0, 1};
    memcpy(data, &on, sizeof on);
    len = sizeof on;
    sim_start(0, 0);
    CHECK(a80211_awdl_request(&f, 1, A80211_IOC_AWDL_SYNC_ENABLED, &val, data, &len, sizeof data, local_of(1, 0)) == 0);
    CHECK(started == 1 && awdl_core_running(nodes[1].core));
    nodes[1].deadline = awdl_core_timer(nodes[1].core, local_of(1, 0));
    len = 0;
    CHECK(a80211_awdl_request(&f, 0, A80211_IOC_AWDL_SYNC_CHANNEL_SEQUENCE, &val, data, &len, 400, 0) == 0 &&
          len == 400);
    struct a80211_awdl_sync_channel_sequence seq;
    memcpy(&seq, data, sizeof seq);
    CHECK(seq.length == 16 && seq.channel[0].channel == 149 && seq.channel[15].channel == 149 &&
          (seq.channel[0].flags & 0x10));
    // traffic registration (AirDrop's), strategy, unknown type
    struct a80211_awdl_peer_traffic_registration t;
    memset(&t, 0, sizeof t);
    t.active = 1;
    memcpy(t.unique_id, "com.apple.airdrop", 17);
    len = sizeof t;
    CHECK(a80211_awdl_request(&f, 1, A80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION, &val, &t, &len, sizeof t, 0) == 0 &&
          f.traffic_registrations == 1);
    len = 4;
    CHECK(a80211_awdl_request(&f, 1, 116, &val, data, &len, 4, 0) == EOPNOTSUPP);
    // a peer appears: one PEER_PRESENCE (68 bytes, its address) in the ring
    sim_run(2000000);
    char ifname[16];
    uint32_t event;
    uint8_t out[256];
    int presence = 0;
    while (a80211_ring_take(&facade_ring, &event, ifname, out, &len, sizeof out) == 1) {
        if (event != A80211_M_PEER_PRESENCE)
            continue;
        struct a80211_peer_presence p;
        CHECK(len == sizeof p);
        memcpy(&p, out, sizeof p);
        CHECK(!memcmp(p.address, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0a}, 6));
        CHECK(p.state == A80211_PEER_PRESENT);
        presence++;
    }
    CHECK(presence == 1);
    len = 0;
    CHECK(a80211_awdl_request(&f, 0, A80211_IOC_AWDL_ADVERTISERS, &val, data, &len, 44 * 4, 0) == 0 && len == 44);
    // off again: the peer is reported lost
    const struct a80211_awdl_sync_enabled off = {1, 0, 0};
    len = sizeof off;
    CHECK(a80211_awdl_request(&f, 1, A80211_IOC_AWDL_SYNC_ENABLED, &val, (void *)&off, &len, sizeof off, 0) == 0);
    CHECK(stopped == 1 && !awdl_core_running(nodes[1].core));
    CHECK(a80211_ring_take(&facade_ring, &event, ifname, out, &len, sizeof out) == 1 &&
          event == A80211_M_PEER_PRESENCE);
    struct a80211_peer_presence p;
    memcpy(&p, out, sizeof p);
    CHECK(p.state == A80211_PEER_ABSENT);
    awdl_core_stop(nodes[0].core);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_parse_hand_frame();
    test_build_roundtrip();
    test_data_encap();
    test_sync_two_nodes();
    test_election_chain();
    test_two_channel_sequence();
    test_tsf_clock();
    test_ring();
    test_facade();
    test_native_main();
    test_owl_main();
    test_sched_main();
    sim_reset(0);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("AWDL: hand-built MIF parse + fuzz, PSF/MIF round trip, data encap/decap, two-node sync (40 ppm), "
         "three-node chain election + root loss, two-channel sequence, TSF clock, IO80211 event ring, awdl0 requests "
         "and PEER_PRESENCE; firmware AWDL: payloads = the brcmfmac series' traced bytes and the 2014 layouts, probe, "
         "AWDL data header, schedule, interface_create v3 and awdl_if bring-ups, WLC_E_ACTION_FRAME_RX peers, "
         "awdl_peer_op with HT, host election to the firmware, idle/transfer schedules, the radio verdict and the "
         "down awdl0, creation failures: ok");
    return 0;
}
