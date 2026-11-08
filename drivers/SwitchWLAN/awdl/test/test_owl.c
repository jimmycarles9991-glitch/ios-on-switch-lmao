// SPDX-License-Identifier: BSD-2-Clause-Patent
// The core against OWL (seemoo-lab/owl), the AWDL implementation shown to work with Apple devices: the bytes its
// awdl_init_full_action_frame writes for a given state (written out by hand from its src/tx.c and src/frame.h, not
// produced by running it), a master whose sync parameters come from OWL's src/sync.c formulas, and the cases of its
// tests/test_awdl_election.cpp. OWL is GPL-3.0; nothing of it is compiled here.
#include "../awdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
#define OCHECK(x)                                                        \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            test_failures++;                                             \
        }                                                                \
    } while (0)

static size_t unhex(const char *s, uint8_t *out, size_t size)
{
    size_t k = 0;
    while (s[0] && s[1] && k < size) {
        if (s[0] == ' ') {
            s++;
            continue;
        }
        unsigned v = 0;
        sscanf(s, "%2x", &v);
        out[k++] = (uint8_t)v;
        s += 2;
    }
    return k;
}

static int o_sent;
static uint8_t o_frame[1600];
static size_t o_len;
static uint16_t o_freq;
static uint64_t o_stay;

static int o_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)ctx;
    (void)no_ack;
    o_sent++;
    memcpy(o_frame, frame, len < sizeof o_frame ? len : sizeof o_frame);
    o_len = len;
    o_freq = freq;
    o_stay = stay_until;
    return 0;
}

static struct awdl_core *o_core(uint8_t last, uint32_t metric, const char *name, uint64_t start)
{
    struct awdl_core *c = (struct awdl_core *)calloc(1, awdl_core_size());
    const uint8_t addr[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last};
    struct awdl_config cfg;
    awdl_config_default(&cfg, addr);
    cfg.self_metric = metric;
    snprintf(cfg.hostname, sizeof cfg.hostname, "%s", name);
    cfg.device_class = 1;  // OWL's AWDL_DEVCLASS_MACOS
    cfg.ht_streams = 1;
    cfg.country[1] = '0';  // OWL's "X0"
    const struct awdl_host h = {NULL, o_tx, NULL, NULL, NULL, NULL, NULL, NULL};
    OCHECK(awdl_core_init(c, &h, &cfg) == 0);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){149, 0x80};  // OWL's CHAN_OPCLASS_149, awdl_chanseq_init_static
    awdl_core_set_sequence(c, seq, start);
    awdl_core_start(c, start);
    return c;
}

// OWL's frame for a root alone on 149 (election state as awdl_election_state_init leaves it: metric 60, counters 0,
// height 0), 5 TU into AW 0, at 802.11 sequence 0. phy_tx = target_tx = the build time (OWL's steady clock).
#define T0        (16384u * 100u)
#define OWL_SEQ16 "9580 9580 9580 9580 9580 9580 9580 9580 9580 9580 9580 9580 9580 9580 9580 9580"
static const char *const kOwlHead = "d000 0000 ffffffffffff 021122334401 002500ff9473 0000 7f 0017f2 08 10";
static const char *const kOwlBody =
    // sync params: next AW channel 149, 59 TU to the next EAW, master channel 149, guard 0, AW period 16, AF period
    // 110, flags 0x1800, extension 16, common 16, 11 TU of the first AW left, extensions 3/3/3/3, master, presence 4,
    // reserved, AW 0, AP alignment = AW, channel sequence, padding
    "04 4900 95 3b00 95 00 1000 6e00 0018 1000 1000 0b00 03030303 021122334401 04 00 0000 0000"
    " 0f 03 00 03 ffff " OWL_SEQ16 " 0000"
    // election params: flags, id, distance 0, unknown, top master, its metric 60, our metric 60, padding
    " 05 1500 00 0000 00 00 021122334401 3c000000 3c000000 0000"
    // channel sequence, three padding bytes
    " 12 2900 0f 03 00 03 ffff " OWL_SEQ16 " 000000"
    // election params v2: master, sync master, master counter, distance, metrics, unknown, reserved, self counter
    " 18 2800 021122334401 021122334401 00000000 00000000 3c000000 3c000000 00000000 00000000 00000000"
    // service params: 3 unknown, SUI 0, bitmask 0
    " 06 0900 000000 0000 00000000";
static const char *const kOwlMifOnly =
    // HT capabilities: 0x11ce, A-MPDU 0x1b, one stream's MCS 0-7; arpa "owl.local"
    " 07 0800 0000 ce11 1b ff 0000"
    " 10 0700 03 03 6f776c c00c";
static const char *const kOwlTail =
    // data path state: flags 0x8f24, "X0", social map 149, AWDL address, extension flags; version 3.4, macOS
    " 0c 0f00 248f 583000 0400 021122334401 0000"
    " 15 0200 34 01";

static void test_owl_bytes(void)
{
    struct awdl_core *c = o_core(0x01, 60, "owl", T0);
    const uint64_t now = T0 + 5 * 1024;
    char want_hex[4096];
    uint8_t want[1600];
    uint8_t got[1600];
    for (int mif = 0; mif <= 1; ++mif) {
        snprintf(want_hex, sizeof want_hex, "%s %s %08x %08x %s%s%s", kOwlHead, mif ? "03 00" : "00 00", 0u, 0u,
                 kOwlBody, mif ? kOwlMifOnly : "", kOwlTail);
        const size_t wn = unhex(want_hex, want, sizeof want);
        // phy_tx and target_tx: little endian `now`
        for (unsigned i = 0; i < 4; ++i)
            want[32 + i] = want[36 + i] = (uint8_t)(now >> (8 * i));
        const size_t n = awdl_core_build_af(c, mif ? AWDL_SUBTYPE_MIF : AWDL_SUBTYPE_PSF, now, got, sizeof got);
        want[22] = (uint8_t)(mif << 4);  // the 802.11 sequence number: 0, then 1
        OCHECK(n == wn);
        for (size_t k = 0; k < n && k < wn; ++k) {
            if (got[k] != want[k]) {
                fprintf(stderr, "  %s byte %zu: got %02x, OWL %02x\n", mif ? "MIF" : "PSF", k, got[k], want[k]);
                test_failures++;
                break;
            }
        }
    }
    free(c);
}

// ---- synchronisation to a master that follows OWL's sync.c ------------------------------------------------------------
struct owl_master {
    uint64_t last_update;  // the start of some EAW, on the master's clock
    uint16_t aw_counter;   // its AW number (a multiple of 4)
};

static uint16_t owl_next_aw_tu(const struct owl_master *m, uint64_t t)
{
    return (uint16_t)(64 - ((t - m->last_update) / 1024) % 64);
}

static uint16_t owl_current_aw(const struct owl_master *m, uint64_t t)
{
    const uint64_t since = (t - m->last_update) / 1024;
    return (uint16_t)(m->aw_counter + (since % 64) / 16 + 4 * (since / 64));
}

// A PSF from the OWL master at master time t: our encoder's frame with the master's sync fields written in (address,
// counter, AW number, phy_tx = target_tx = t as OWL stamps it).
static size_t owl_psf(struct awdl_core *shape, const struct owl_master *m, uint64_t t, uint8_t *f, size_t size)
{
    const size_t n = awdl_core_build_af(shape, AWDL_SUBTYPE_PSF, t, f, size);
    for (unsigned i = 0; i < 4; ++i)
        f[32 + i] = f[36 + i] = (uint8_t)(t >> (8 * i));
    uint8_t *v = f + 40 + 3;  // the sync params TLV's value
    const uint16_t counter = owl_next_aw_tu(m, t);
    const uint16_t aw = owl_current_aw(m, t);
    v[1] = (uint8_t)counter;
    v[2] = (uint8_t)(counter >> 8);
    v[29] = (uint8_t)aw;
    v[30] = (uint8_t)(aw >> 8);
    return n;
}

static void test_owl_sync(void)
{
    // the master: high metric, its EAW 0x1230 starting 777 us into its clock; 30 ppm apart from us
    struct awdl_core *shape = o_core(0x0a, 520, "mac", 0);
    const struct owl_master m = {777, 0x1230};
    struct awdl_core *us = o_core(0x0b, 60, "switch", 5000000);
    uint8_t f[1600];
    const int64_t ppm = 30;
    // our clock: local = master * (1 + 30e-6) + 4.2 s; frames reach us 50-450 us after they are stamped
    for (uint64_t t = 1000000; t < 4000000; t += 112640) {
        const size_t n = owl_psf(shape, &m, t, f, sizeof f);
        const uint64_t rx = t + 4200000 + t * ppm / 1000000 + 50 + (t / 112640 % 9) * 50;
        const struct awdl_rx_meta meta = {rx, 5745, -50};
        awdl_core_rx(us, f, n, &meta);
    }
    OCHECK(awdl_core_role(us) == AWDL_ROLE_FOLLOWER);
    // across a whole sequence, our slot is the master's current EAW mod 16 and our EAW starts within 1 TU of its
    int wrong_slot = 0;
    int64_t worst = 0;
    for (uint64_t t = 4000000; t < 4000000 + 1048576; t += 997) {
        const uint64_t into_aw = (t - m.last_update) % 16384;
        if (into_aw < 2048 || into_aw > 16384 - 2048)
            continue;  // within our error of an AW boundary: either number is right
        const uint64_t local = t + 4200000 + t * ppm / 1000000;
        const unsigned master_slot = (owl_current_aw(&m, t) / 4) % 16;
        const unsigned our_slot = (awdl_core_aw_seq(us, local) / 4) % 16;
        wrong_slot += master_slot != our_slot;
        const uint16_t our_aw = awdl_core_aw_seq(us, local);
        OCHECK(our_aw == owl_current_aw(&m, t));
        const uint64_t master_eaw = t - (t - m.last_update) % 65536;  // the master's EAW start, its clock
        const uint64_t ours = awdl_core_aw_start(us, (uint16_t)(our_aw & ~3u), local);
        const int64_t d = (int64_t)ours - (int64_t)(master_eaw + 4200000 + master_eaw * ppm / 1000000);
        if (d > worst || -d > worst)
            worst = d < 0 ? -d : d;
    }
    OCHECK(wrong_slot == 0);
    OCHECK(worst <= 1024 + 600);  // OWL's whole-TU counter, plus the delivery spread
    free(shape);
    free(us);
}

// ---- election (tests/test_awdl_election.cpp) --------------------------------------------------------------------------
// A frame from peer `last` whose election v2 TLV advertises root `root`, counter, distance and root metric.
static struct awdl_core *shape;

static void feed(struct awdl_core *us, uint8_t last, uint8_t root, uint32_t counter, uint32_t hops, uint32_t metric,
                 uint64_t now)
{
    uint8_t f[1600];
    const size_t n = awdl_core_build_af(shape, AWDL_SUBTYPE_MIF, now, f, sizeof f);
    f[15] = last;
    struct awdl_af a;
    OCHECK(awdl_af_parse(f, n, &a) == 0);
    size_t at = 40;
    while (at + 3 <= n && f[at] != AWDL_TLV_ELECTION_PARAMS_V2)
        at += 3 + (size_t)(f[at + 1] | f[at + 2] << 8);
    uint8_t *v = f + at + 3;
    const uint8_t r[6] = {0x02, 0x11, 0x22, 0x33, 0x44, root};
    const uint8_t p[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last};
    memcpy(v, r, 6);
    memcpy(v + 6, hops ? r : p, 6);
    const uint32_t w[3] = {counter, hops, metric};
    for (unsigned k = 0; k < 3; ++k)
        for (unsigned i = 0; i < 4; ++i)
            v[12 + 4 * k + i] = (uint8_t)(w[k] >> (8 * i));
    const struct awdl_rx_meta meta = {now, 5745, -50};
    awdl_core_rx(us, f, n, &meta);
}

static int root_is(const struct awdl_core *c, uint8_t last)
{
    return awdl_core_election(c)->root[5] == last;
}

static void test_owl_election(void)
{
    shape = o_core(0x77, 60, "peer", 0);
    // elect_simple: a peer with the same counter and metric, at the same height, wins on its higher address ...
    struct awdl_core *us = o_core(0x10, 60, "us", 1000000);
    feed(us, 0x20, 0x20, 0, 0, 60, 1000000);
    OCHECK(awdl_core_role(us) == AWDL_ROLE_FOLLOWER && root_is(us, 0x20));
    free(us);
    // ... and loses on a lower one
    us = o_core(0x30, 60, "us", 1000000);
    feed(us, 0x20, 0x20, 0, 0, 60, 1000000);
    OCHECK(awdl_core_role(us) == AWDL_ROLE_ROOT && root_is(us, 0x30));
    free(us);
    // counter_metric: metric 1000 against our 1000 (address), against 1001 (we win), then the counter decides first
    us = o_core(0x10, 1000, "us", 1000000);
    feed(us, 0x20, 0x20, 0, 0, 1000, 1000000);
    OCHECK(root_is(us, 0x20));
    free(us);
    us = o_core(0x10, 1001, "us", 1000000);
    feed(us, 0x20, 0x20, 0, 0, 1000, 1000000);
    OCHECK(awdl_core_role(us) == AWDL_ROLE_ROOT);
    feed(us, 0x20, 0x20, 1, 0, 1000, 1100000);
    OCHECK(awdl_core_role(us) == AWDL_ROLE_FOLLOWER && root_is(us, 0x20));
    free(us);
    // our own counter grows while we are root (every π s): 1 against the peer's 1, and our metric wins again
    us = o_core(0x10, 1001, "us", 1000000);
    awdl_core_timer(us, 1000000 + 3200000);
    feed(us, 0x20, 0x20, 1, 0, 1000, 1000000 + 3200000);
    OCHECK(awdl_core_role(us) == AWDL_ROLE_ROOT && awdl_core_election(us)->root_counter == 1);
    free(us);
    // elect_cycle: a peer whose tree runs through us is never our way to a root
    us = o_core(0x10, 60, "us", 1000000);
    feed(us, 0x20, 0x10, 0, 1, 1000, 1000000);
    OCHECK(awdl_core_role(us) == AWDL_ROLE_ROOT);
    free(us);
    // same root and score through two peers: the one nearer the root, then the higher address
    us = o_core(0x10, 60, "us", 1000000);
    feed(us, 0x40, 0x50, 9, 2, 500, 1000000);
    feed(us, 0x20, 0x50, 9, 1, 500, 1000000);
    OCHECK(awdl_core_election(us)->parent[5] == 0x20 && awdl_core_election(us)->hops == 2);
    feed(us, 0x30, 0x50, 9, 1, 500, 1000000);
    OCHECK(awdl_core_election(us)->parent[5] == 0x30);
    free(us);
    free(shape);
}

// ---- data frames, channels ------------------------------------------------------------------------------------------
static void test_amsdu(void)
{
    // QoS data, A-MSDU present: two subframes (an mDNS packet and a 3-byte one), the first padded to 4 bytes
    uint8_t f[256];
    size_t n = unhex("8800 0000 3333000000fb 0a1b2c3d4e5f 002500ff9473 2000 8000"
                     " 3333000000fb 0a1b2c3d4e5f 0015 aaaa03 0017f2 0800 0304 0700 0000 86dd 6000000000 00"
                     " 021122334401 0a1b2c3d4e5f 0013 aaaa03 0017f2 0800 0304 0800 0000 0800 aabbcc",
                     f, sizeof f);
    struct awdl_msdu m[AWDL_MAX_MSDUS];
    uint16_t seq = 0;
    int retry = 1;
    OCHECK(awdl_data_msdus(f, n, m, AWDL_MAX_MSDUS, &seq, &retry) == 2 && seq == 7 && !retry);
    OCHECK(m[0].ethertype == 0x86dd && m[0].len == 5 && m[0].dst[0] == 0x33 && m[1].ethertype == 0x0800 &&
           m[1].len == 3 && m[1].dst[5] == 0x01 && m[1].payload[2] == 0xcc);
    uint8_t eth[64];
    OCHECK(awdl_msdu_ether(&m[1], eth, sizeof eth) == 17 && eth[12] == 0x08 && !memcmp(eth + 6, f + 10, 6));
    OCHECK(awdl_data_msdus(f, n - 1, m, AWDL_MAX_MSDUS, NULL, NULL) == 0);  // the last subframe runs past the end
    f[24] = 0;                                                              // the same bytes as one MSDU: no LLC
    OCHECK(awdl_data_msdus(f, n, m, AWDL_MAX_MSDUS, NULL, NULL) == 0);
    // every truncation is rejected or parsed in bounds (ASan watches)
    f[24] = 0x80;
    for (size_t k = 0; k < n; ++k) {
        uint8_t *copy = (uint8_t *)malloc(k ? k : 1);
        memcpy(copy, f, k);
        (void)awdl_data_msdus(copy, k, m, AWDL_MAX_MSDUS, NULL, NULL);
        free(copy);
    }
}

static void test_channels(void)
{
    OCHECK(awdl_chan_primary((struct awdl_chan){149, 0x80}) == 149 &&
           awdl_chan_freq((struct awdl_chan){149, 0x80}) == 5745);
    OCHECK(awdl_chan_primary((struct awdl_chan){44, 0x80}) == 44 &&
           awdl_chan_primary((struct awdl_chan){6, 0x51}) == 6);
    OCHECK(awdl_chan_primary((struct awdl_chan){155, 0x80}) == 149 &&
           awdl_chan_primary((struct awdl_chan){42, 0x80}) == 44);
    OCHECK(awdl_chan_primary((struct awdl_chan){58, 0x80}) == 52 &&
           awdl_chan_primary((struct awdl_chan){36, 115}) == 36);
    OCHECK(awdl_chan_centre80(149) == 155 && awdl_chan_centre80(161) == 155 && awdl_chan_centre80(44) == 42 &&
           awdl_chan_centre80(100) == 106 && awdl_chan_centre80(6) == 0);
}

void test_owl_main(void)
{
    test_owl_bytes();
    test_owl_sync();
    test_owl_election();
    test_amsdu();
    test_channels();
    printf("OWL: PSF/MIF bytes, sync to an OWL-formula master (30 ppm), election order, A-MSDU, channel forms: %s\n",
           test_failures ? "FAILED" : "ok");
}
