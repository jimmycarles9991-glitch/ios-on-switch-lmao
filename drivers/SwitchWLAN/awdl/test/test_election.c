// SPDX-License-Identifier: BSD-2-Clause-Patent
// The election against root changes a Mac logged (metrics and counters from its runElection lines, RSSI as the Switch
// heard each device; the addresses are stand-ins that keep their order), the parent kept through alternating
// announcements, the radio schedule left alone through the parent's countdown jitter and re-installed when the parent
// changes or drift needs it, and the channel maps beside an Apple device's own sequences.
#include "../awdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
#define ECHECK(x)                                                        \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            test_failures++;                                             \
        }                                                                \
    } while (0)

static uint32_t e_rand = 77;

static uint32_t rnd(uint32_t n)
{
    e_rand = e_rand * 1103515245u + 12345u;
    return (e_rand >> 8) % n;
}

static int e_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)ctx;
    (void)frame;
    (void)len;
    (void)freq;
    (void)stay_until;
    (void)no_ack;
    return 0;
}

static unsigned e_installs;
static uint64_t e_anchor;
static struct awdl_chan e_installed[AWDL_SEQ_SLOTS];

static void e_schedule(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor)
{
    (void)ctx;
    ++e_installs;
    e_anchor = anchor;
    memcpy(e_installed, seq, sizeof e_installed);
}

static struct awdl_core *make_with(uint8_t last, uint32_t metric,
                                   void (*schedule)(void *, const struct awdl_chan *, uint64_t))
{
    struct awdl_core *c = (struct awdl_core *)calloc(1, awdl_core_size());
    const uint8_t addr[6] = {0x02, 0, 0, 0, 0, last};
    struct awdl_config cfg;
    awdl_config_default(&cfg, addr);
    cfg.self_metric = metric;
    const struct awdl_host h = {NULL, e_tx, schedule, NULL, NULL, NULL, NULL, NULL};
    ECHECK(awdl_core_init(c, &h, &cfg) == 0);
    return c;
}

// Our core: its radio schedule is the one recorded.
static struct awdl_core *make(uint8_t last, uint32_t metric)
{
    return make_with(last, metric, e_schedule);
}

static struct awdl_core *make_peer(uint8_t last, uint32_t metric)
{
    return make_with(last, metric, NULL);
}

static void row_seq(struct awdl_chan out[AWDL_SEQ_SLOTS], const uint8_t row[AWDL_SEQ_SLOTS])
{
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        out[i] = !row[i]       ? (struct awdl_chan){0, 0}
                 : row[i] == 6 ? (struct awdl_chan){6, 81}
                               : (struct awdl_chan){row[i], 0x80};
}

static int is_row(const struct awdl_chan seq[AWDL_SEQ_SLOTS], const uint8_t row[AWDL_SEQ_SLOTS])
{
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        if (awdl_chan_primary(seq[i]) != row[i])
            return 0;
    return 1;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint8_t *tlv(uint8_t *f, size_t n, uint8_t type)
{
    for (size_t at = 40; at + 3 <= n; at += 3 + (size_t)(f[at + 1] | f[at + 2] << 8))
        if (f[at] == type)
            return f + at + 3;
    return NULL;
}

// ---- the election -------------------------------------------------------------------------------------------------------
struct heard {
    uint8_t from, root;
    uint32_t metric, counter, hops;
    int8_t rssi;
};

static struct awdl_core *e_shape;

static void hear(struct awdl_core *us, const struct heard *h, uint64_t now)
{
    uint8_t f[AWDL_MAX_FRAME];
    const size_t n = awdl_core_build_af(e_shape, AWDL_SUBTYPE_MIF, now, f, sizeof f);
    f[15] = h->from;
    uint8_t *v = tlv(f, n, AWDL_TLV_ELECTION_PARAMS_V2);
    ECHECK(v != NULL);
    if (!v)
        return;
    v[5] = v[11] = h->root;
    put32(v + 12, h->counter);
    put32(v + 16, h->hops);
    put32(v + 20, h->metric);
    const struct awdl_rx_meta m = {now, 5745, h->rssi};
    awdl_core_rx(us, f, n, &m);
}

enum {
    MAC = 0xaa,
    E2 = 0xe2,
    A3 = 0x3a,
    W9F = 0x22,
    WD2 = 0xda,
    WB8 = 0x26,
    US = 0x86
};

static uint8_t root_of(const struct awdl_core *c)
{
    return awdl_core_election(c)->root[5];
}

// The Mac's decision with these candidates, heard in either order, is our decision too.
static void decide(const struct heard *k, unsigned n, uint8_t want)
{
    for (unsigned order = 0; order < 2; ++order) {
        struct awdl_core *us = make(US, 60);
        awdl_core_start(us, 1000000);
        for (unsigned i = 0; i < n; ++i)
            hear(us, &k[order ? n - 1 - i : i], 1000000 + 1000 * (i + 1));
        if (root_of(us) != want)
            fprintf(stderr, "  candidates from %02x: root %02x, the Mac's %02x\n", k[0].from, root_of(us), want);
        ECHECK(awdl_core_role(us) == AWDL_ROLE_FOLLOWER && root_of(us) == want);
        awdl_core_stop(us);
        free(us);
    }
}

static void test_mac_decisions(void)
{
    // 22:12:02: E2 (514, counter 9) over the Mac (510, counter 142344): the metric leads, whatever the counter
    const struct heard d1[] = {{MAC, MAC, 510, 142344, 0, -42}, {E2, E2, 514, 9, 0, -40}};
    decide(d1, 2, E2);
    // 22:12:20: 3A announces 518 beside E2 (514) and the Mac
    const struct heard d2[] = {
        {MAC, MAC, 510, 142344, 0, -42}, {E2, E2, 514, 14, 0, -40}, {A3, A3, 518, 32714, 0, -30}};
    decide(d2, 3, A3);
    // 22:14:26: 22:9F at -85 dBm with the Mac's metric and a higher counter; the Mac stayed root, and would on the
    // address alone at any RSSI
    const struct heard d3[] = {{MAC, MAC, 510, 142361, 0, -42}, {W9F, W9F, 510, 173856, 0, -85}};
    decide(d3, 2, MAC);
    const struct heard d3_near[] = {{MAC, MAC, 510, 142361, 0, -42}, {W9F, W9F, 510, 173856, 0, -50}};
    decide(d3_near, 2, MAC);
    // 22:15:35: DA:D2 announces 537 but is heard at -86 dBm: E2 (511) is the best root above the floor
    const struct heard d4[] = {{MAC, MAC, 510, 142369, 0, -40},
                               {E2, E2, 511, 36, 0, -51},
                               {WD2, WD2, 537, 1978, 0, -86},
                               {W9F, W9F, 510, 173877, 0, -86}};
    decide(d4, 4, E2);
    // 22:13:31: the Mac root beside two far devices with its metric and higher or lower counters
    const struct heard d5[] = {
        {W9F, W9F, 510, 173834, 0, -85}, {MAC, MAC, 510, 142345, 0, -42}, {WB8, WB8, 510, 2507, 0, -87}};
    decide(d5, 3, MAC);

    // what we then announce: E2's tree one hop down, its metric and counter, our own metric
    struct awdl_core *us = make(US, 60);
    awdl_core_start(us, 1000000);
    for (unsigned i = 0; i < 4; ++i)
        hear(us, &d4[i], 1000000 + 1000 * (i + 1));
    uint8_t f[AWDL_MAX_FRAME];
    struct awdl_af a;
    const size_t n = awdl_core_build_af(us, AWDL_SUBTYPE_MIF, 1100000, f, sizeof f);
    ECHECK(n && !awdl_af_parse(f, n, &a));
    ECHECK(a.election2.root[5] == E2 && a.election2.parent[5] == E2 && a.election2.hops == 1);
    ECHECK(a.election2.root_metric == 511 && a.election2.root_counter == 36 && a.election2.self_metric == 60);
    ECHECK(a.election.master[5] == E2 && a.election.distance == 1 && a.election.master_metric == 511);
    ECHECK(a.sync.master[5] == E2);
    awdl_core_stop(us);
    free(us);
}

// Each root heard every 2.5 s, the way a radio with few AWDL slots hears them: the better one stays the root, and is
// given up only once silent for the peer timeout.
static void test_alternating_roots(void)
{
    struct awdl_core *us = make(US, 60);
    awdl_core_start(us, 0);
    const struct heard a = {A3, A3, 518, 32714, 0, -30}, b = {E2, E2, 514, 14, 0, -40};
    unsigned changes = 0;
    uint8_t root = root_of(us);
    uint64_t t = 100000;
    uint64_t last_a = 0;
    for (unsigned k = 0; k < 24; ++k) {
        if (k % 2) {
            hear(us, &b, t);
        } else {
            hear(us, &a, t);
            last_a = t;
        }
        for (uint64_t s = t; s < t + 1250000; s += 50000) {
            awdl_core_timer(us, s);
            changes += root_of(us) != root;
            root = root_of(us);
        }
        t += 1250000;
    }
    ECHECK(changes == 1 && root == A3);
    hear(us, &b, t);
    awdl_core_timer(us, last_a + 2900000);
    ECHECK(root_of(us) == A3);
    for (uint64_t s = last_a + 2900000; s <= last_a + AWDL_PEER_TIMEOUT_US + AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
         s += 50000)
        awdl_core_timer(us, s);
    ECHECK(root_of(us) == E2);
    awdl_core_stop(us);
    free(us);

    // one root through two relays with the same counter and distance: the parent we have is kept
    us = make(US, 60);
    awdl_core_start(us, 0);
    const struct heard p1 = {0x20, 0x50, 520, 100, 1, -45}, p2 = {0x30, 0x50, 520, 100, 1, -45};
    for (unsigned k = 0; k < 10; ++k) {
        hear(us, k % 2 ? &p2 : &p1, 100000 + 250000 * k);
        ECHECK(awdl_core_election(us)->parent[5] == 0x20 && awdl_core_election(us)->hops == 2);
    }
    // a counter more than 2 ahead is a fresher way to the same root
    const struct heard p2_fresh = {0x30, 0x50, 520, 103, 1, -45};
    hear(us, &p2_fresh, 3000000);
    ECHECK(awdl_core_election(us)->parent[5] == 0x30 && awdl_core_election(us)->root_counter == 103);
    awdl_core_stop(us);
    free(us);

    // two relays of one root that each outrank the other, one by its metric and one by its hops: the parent we have
    // is kept and the radio's schedule is left alone
    us = make(US, 60);
    awdl_core_start(us, 0);
    const struct heard near = {0x20, 0x50, 520, 100, 1, -45}, rich = {0x30, 0x50, 524, 100, 2, -45};
    hear(us, &near, 100000);
    ECHECK(awdl_core_election(us)->parent[5] == 0x20);
    const unsigned installs = e_installs;
    for (unsigned k = 1; k < 30; ++k) {
        const uint64_t now = 100000 + 100000 * (uint64_t)k;
        hear(us, k % 2 ? &rich : &near, now);
        awdl_core_timer(us, now + 1000);
        ECHECK(awdl_core_election(us)->parent[5] == 0x20);
    }
    ECHECK(e_installs == installs);
    awdl_core_stop(us);
    free(us);
}

// ---- synchronisation -----------------------------------------------------------------------------------------------------
#define R_OFFSET 3456789

static int64_t r_ppm;

static uint64_t r_local(uint64_t g)
{
    return (uint64_t)((int64_t)g + R_OFFSET + (int64_t)g * r_ppm / 1000000);
}

// The installed schedule's slot 0 against the master's sequence start, within half a sequence.
static int64_t anchor_error(const struct awdl_core *master, uint64_t g, int64_t clock_offset)
{
    const uint16_t aw = awdl_core_aw_seq(master, g);
    const uint64_t start = awdl_core_aw_start(master, (uint16_t)(aw - aw % 64), g);
    const uint64_t local = r_local(start - (uint64_t)clock_offset);
    const uint64_t period = (uint64_t)AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    int64_t d = (int64_t)((e_anchor - local) % period);
    return d > (int64_t)period / 2 ? d - (int64_t)period : d;
}

// A master's PSF built at its time g + clock_offset, received at our r_local(g) plus up to 1 TU late.
static void master_psf(struct awdl_core *us, struct awdl_core *master, uint64_t g, int64_t clock_offset)
{
    uint8_t f[AWDL_MAX_FRAME];
    const size_t n = awdl_core_build_af(master, AWDL_SUBTYPE_PSF, g + (uint64_t)clock_offset, f, sizeof f);
    const uint64_t rx = r_local(g) + rnd(AWDL_TU_US);
    const struct awdl_rx_meta m = {rx, 5745, -45};
    awdl_core_rx(us, f, n, &m);
    awdl_core_timer(us, rx);
}

static const uint8_t mac_idle[AWDL_SEQ_SLOTS] = {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0};

static struct awdl_core *master_core(uint8_t last, uint32_t metric, uint64_t start)
{
    struct awdl_core *m = make_peer(last, metric);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    row_seq(seq, mac_idle);
    awdl_core_set_sequence(m, seq, start);
    awdl_core_start(m, start);
    return m;
}

static void test_resync(void)
{
    r_ppm = 0;
    struct awdl_core *mac = master_core(MAC, 510, 0);
    struct awdl_core *us = make(US, 60);
    awdl_core_start(us, r_local(5000));
    master_psf(us, mac, 100000, 0);
    ECHECK(awdl_core_role(us) == AWDL_ROLE_FOLLOWER && root_of(us) == MAC);
    ECHECK(awdl_core_stats(us)->resyncs == 1);
    const unsigned adopted = e_installs;
    // a minute of the countdown's whole-TU jitter: no re-install, and the schedule stays within that TU
    int64_t worst = 0;
    uint64_t g = 100000;
    for (; g < 60000000; g += 112640) {
        master_psf(us, mac, g, 0);
        const int64_t d = anchor_error(mac, g, 0);
        worst = d < 0 ? (-d > worst ? -d : worst) : (d > worst ? d : worst);
    }
    ECHECK(awdl_core_stats(us)->resyncs == 1 && e_installs == adopted);
    ECHECK(worst < (int64_t)AWDL_TU_US);
    const int32_t last = awdl_core_stats(us)->last_sync_error_us;
    ECHECK(last > -(int32_t)AWDL_TU_US && last < (int32_t)AWDL_TU_US);
    struct awdl_core *same = master_core(0x22, 514, 0);
    master_psf(us, same, g, 0);
    ECHECK(root_of(us) == 0x22);
    ECHECK(awdl_core_stats(us)->resyncs == 1 && e_installs == adopted);
    awdl_core_stop(same);
    free(same);
    // a better master on its own clock: followed, and the schedule moves to it at once
    const int64_t other = 777777;
    struct awdl_core *a3 = master_core(A3, 518, 0);
    master_psf(us, a3, g, other);
    ECHECK(root_of(us) == A3);
    ECHECK(awdl_core_stats(us)->resyncs == 2 && e_installs == adopted + 1);
    ECHECK(anchor_error(a3, g + (uint64_t)other, other) < (int64_t)AWDL_TU_US &&
           anchor_error(a3, g + (uint64_t)other, other) > -(int64_t)AWDL_TU_US);
    awdl_core_stop(us);
    free(us);
    free(a3);

    // 40 ppm apart either way for two minutes: re-installed only as drift needs, never more than the slot guard off
    for (int sign = -1; sign <= 1; sign += 2) {
        r_ppm = 40 * sign;
        us = make(US, 60);
        awdl_core_start(us, r_local(5000));
        master_psf(us, mac, 100000, 0);
        worst = 0;
        for (g = 100000; g < 120000000; g += 112640) {
            master_psf(us, mac, g, 0);
            const int64_t d = anchor_error(mac, g, 0);
            worst = d < 0 ? (-d > worst ? -d : worst) : (d > worst ? d : worst);
        }
        const uint32_t resyncs = awdl_core_stats(us)->resyncs;
        printf("  %+d ppm for 120 s: %u schedule installs, the radio within %lld us of the master\n", 40 * sign,
               resyncs, (long long)worst);
        ECHECK(resyncs >= 3 && resyncs <= 12);
        ECHECK(worst < 2 * (int64_t)AWDL_TU_US);
        awdl_core_stop(us);
        free(us);
    }
    awdl_core_stop(mac);
    free(mac);
}

// ---- channel maps --------------------------------------------------------------------------------------------------------
static struct awdl_core *device(uint16_t infra_mhz)
{
    struct awdl_core *c = make(US, 60);
    awdl_core_set_infra(c, infra_mhz, 0);
    switch_awdl_core_set_channels(c, 7, 0);
    awdl_core_start(c, 0);
    return c;
}

static void hear_master(struct awdl_core *c, uint8_t last, uint32_t metric, const uint8_t row[AWDL_SEQ_SLOTS],
                        uint64_t now)
{
    struct awdl_core *m = make_peer(last, metric);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    row_seq(seq, row);
    awdl_core_set_sequence(m, seq, 0);
    awdl_core_start(m, 0);
    uint8_t f[AWDL_MAX_FRAME];
    const size_t n = awdl_core_build_af(m, AWDL_SUBTYPE_PSF, now, f, sizeof f);
    const struct awdl_rx_meta meta = {now, 5745, -45};
    awdl_core_rx(c, f, n, &meta);
    awdl_core_stop(m);
    free(m);
}

// What we advertise is what the radio runs: slot 0 occupied, 20 MHz channels, the same 16 slots in both TLVs.
static int advertised(struct awdl_core *c, const uint8_t row[AWDL_SEQ_SLOTS], uint64_t now)
{
    const struct awdl_chan *seq = awdl_core_sequence(c);
    uint8_t f[AWDL_MAX_FRAME];
    struct awdl_af a;
    const size_t n = awdl_core_build_af(c, AWDL_SUBTYPE_MIF, now, f, sizeof f);
    int ok = n && !awdl_af_parse(f, n, &a) && seq[0].number && is_row(seq, row) &&
             !memcmp(seq, e_installed, sizeof e_installed) && !memcmp(a.chanseq.chan, seq, sizeof e_installed) &&
             a.sync.has_seq && !memcmp(a.sync.seq.chan, seq, sizeof e_installed);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        ok &= !seq[i].number || seq[i].opclass == awdl_opclass_20mhz(seq[i].number);
    if (!ok) {
        fprintf(stderr, "  sequence");
        for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
            fprintf(stderr, " %u", seq[i].number);
        fprintf(stderr, "\n");
    }
    return ok;
}

// An IPv6 TCP segment between us and the master, out through awdl0 or in as an AWDL data frame.
static void tcp(struct awdl_core *c, int out, uint64_t now)
{
    uint8_t eth[80] = {0};
    const uint8_t us[6] = {0x02, 0, 0, 0, 0, US}, mac[6] = {0x02, 0, 0, 0, 0, MAC};
    memcpy(eth, out ? mac : us, 6);
    memcpy(eth + 6, out ? us : mac, 6);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    eth[14] = 0x60;
    eth[20] = 6;
    if (out) {
        ECHECK(awdl_core_output(c, eth, sizeof eth, now) == 0);
        return;
    }
    uint8_t f[AWDL_MAX_FRAME];
    const size_t n = awdl_data_encap(eth, sizeof eth, mac, (uint16_t)now, f, sizeof f);
    const struct awdl_rx_meta meta = {now, 2437, -45};
    awdl_core_rx(c, f, n, &meta);
}

static void test_channel_map(void)
{
    static const uint8_t mostly149[AWDL_SEQ_SLOTS] = {149, 149, 149, 149, 0, 0, 0, 0, 6, 149, 149, 149, 0, 0, 0, 0};
    static const uint8_t on6[AWDL_SEQ_SLOTS] = {6, 6, 149, 6, 6, 6, 6, 6, 6, 6, 44, 6, 6, 6, 6, 6};
    static const uint8_t on6_active[AWDL_SEQ_SLOTS] = {44, 44, 149, 44, 6, 6, 6, 6, 6, 149, 44, 149, 6, 6, 6, 6};
    static const uint8_t on36[AWDL_SEQ_SLOTS] = {6, 0, 149, 0, 0, 0, 0, 0, 6, 0, 44, 0, 0, 0, 0, 0};
    static const uint8_t on149[AWDL_SEQ_SLOTS] = {149, 149, 149, 149, 149, 149, 149, 149,
                                                  6,   149, 44,  149, 149, 149, 149, 149};
    // beside an access point on 6: 149 in 2, 6 in 8, 44 in 10 as in every sequence the Mac ran, 6 everywhere else
    struct awdl_core *c = device(2437);
    hear_master(c, MAC, 510, mac_idle, 1000000);
    ECHECK(awdl_core_role(c) == AWDL_ROLE_FOLLOWER && advertised(c, on6, 1000000));
    ECHECK(awdl_social_map(awdl_core_sequence(c)) == 7 && awdl_slots_away(awdl_core_sequence(c), 2437) == 2);
    // Unicast TCP with a peer advertising active 5 GHz slots escalates once on the 4th segment and returns to idle 2 s after the last.
    const unsigned installed = e_installs;
    for (uint64_t t = 1000000; t < 6000000; t += 50000) {
        if (!(t % 1000000))
            hear_master(c, MAC, 510, mac_idle, t);
        if (t < 2000000)
            tcp(c, (t / 50000) % 2, t);
        awdl_core_timer(c, t);
        ECHECK(advertised(c, t >= 1150000 && t < 3950000 ? on6_active : on6, t));
    }
    ECHECK(e_installs == installed + 2);
    // a better master whose sequence is mostly 149: the same map
    hear_master(c, A3, 530, mostly149, 6000000);
    ECHECK(root_of(c) == A3 && advertised(c, on6, 6000000));
    awdl_core_stop(c);
    free(c);
    // an access point on 36 is no AWDL channel: its slots are advertised empty, slot 0 takes 6
    c = device(5180);
    hear_master(c, MAC, 510, mac_idle, 1000000);
    ECHECK(advertised(c, on36, 1000000));
    awdl_core_stop(c);
    free(c);
    // on 149 the access point's slots are 149 too; unassociated as beside 36
    c = device(5745);
    hear_master(c, MAC, 510, mac_idle, 1000000);
    ECHECK(advertised(c, on149, 1000000) && awdl_slots_away(awdl_core_sequence(c), 5745) == 2);
    awdl_core_stop(c);
    free(c);
    c = device(0);
    hear_master(c, MAC, 510, mac_idle, 1000000);
    ECHECK(advertised(c, on36, 1000000));
    awdl_core_stop(c);
    free(c);
}

void test_election_main(void)
{
    const int before = test_failures;
    e_shape = make_peer(0x77, 60);
    awdl_core_start(e_shape, 0);
    test_mac_decisions();
    test_alternating_roots();
    test_resync();
    test_channel_map();
    awdl_core_stop(e_shape);
    free(e_shape);
    printf("election, synchronisation and channel maps against a Mac's decisions and sequences: %s\n",
           test_failures == before ? "ok" : "FAILED");
}
