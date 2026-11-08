// SPDX-License-Identifier: BSD-2-Clause-Patent
// The soft backend sharing one radio with the station: a simulated minute next to an Apple-like master. Our radio has
// no hops; it stays on the access point's channel (36) and leaves only for the excursions host.tx asks for (an
// actframe with a dwell): it gets there 1-3 ms after the request and comes back when the stay ends. The master runs
// its own schedule. Measured: the access point's share of our radio and the longest absence, the master's frames we
// catch in our AWDL slots, our EAW boundaries against the master's (AWDL accepts 3 ms, Stute et al. §6.2), whether the
// master sees us, and how often our core wakes and transmits.
#include "../awdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
#define SCHECK(x)                                                        \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            test_failures++;                                             \
        }                                                                \
    } while (0)

#define HOME_MHZ  5180u
#define OFFSET_US 2345678
#define PPM       20

static uint64_t s_now;  // global microseconds
static uint32_t s_rand = 12345;

static uint32_t rnd(uint32_t n)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return (s_rand >> 8) % n;
}

static uint64_t us_local(uint64_t g)
{
    return g + OFFSET_US + g * PPM / 1000000;
}

static uint64_t us_global(uint64_t l)
{
    const int64_t x = (int64_t)l - OFFSET_US;
    return (uint64_t)(x - x * PPM / 1000000);
}

struct pending {
    uint64_t at;
    int from;  // 0 the master, 1 us
    uint16_t freq;
    size_t len;
    uint8_t data[1600];
};

static struct pending air[64];
static int nair;

// our radio: home unless inside [away_from, away_until) on away_freq
static uint64_t away_from, away_until;
static uint16_t away_freq;
static uint64_t away_time, longest_away, run_start;
static unsigned tx_calls, excursions;
static struct awdl_chan m_seq[AWDL_SEQ_SLOTS];
static struct awdl_core *m_core, *u_core;
static uint64_t m_anchor;

static uint16_t our_freq(uint64_t g)
{
    return g >= away_from && g < away_until ? away_freq : HOME_MHZ;
}

static uint16_t master_freq(uint64_t g)
{
    if (g < m_anchor)
        return 0;
    return awdl_chan_freq(m_seq[((g - m_anchor) / 65536) % AWDL_SEQ_SLOTS]);
}

static void queue(int from, const uint8_t *frame, size_t len, uint16_t freq, uint64_t at)
{
    if (nair == (int)(sizeof air / sizeof air[0]) || len > sizeof air[0].data)
        return;
    struct pending *p = &air[nair++];
    p->at = at;
    p->from = from;
    p->freq = freq;
    p->len = len;
    memcpy(p->data, frame, len);
}

static void close_excursion(void)
{
    if (away_until > away_from) {
        const uint64_t d = away_until - away_from;
        away_time += d;
        if (d > longest_away)
            longest_away = d;
    }
    away_from = away_until = 0;
}

static int u_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)ctx;
    (void)no_ack;
    tx_calls++;
    const uint64_t send = s_now + 1000 + rnd(2000);
    if (freq != HOME_MHZ) {
        const uint64_t until = stay_until ? us_global(stay_until) : send + 500;
        if (away_freq == freq && s_now < away_until) {
            away_until = until > away_until ? until : away_until;  // one more frame on the same stay
        } else {
            if (s_now < away_until)
                away_until = s_now;
            if (s_now >= run_start)
                close_excursion();
            away_from = s_now + 300;
            away_until = until;
            away_freq = freq;
            excursions++;
        }
    }
    queue(1, frame, len, freq, send);
    return 0;
}

static int m_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)ctx;
    (void)stay_until;
    (void)no_ack;
    queue(0, frame, len, freq, s_now);
    return 0;
}

static void m_schedule(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor)
{
    (void)ctx;
    memcpy(m_seq, seq, sizeof m_seq);
    m_anchor = anchor;
}

static struct awdl_core *make(uint8_t last, uint32_t metric,
                              int (*tx)(void *, const uint8_t *, size_t, uint16_t, uint64_t, int),
                              void (*sched)(void *, const struct awdl_chan *, uint64_t))
{
    struct awdl_core *c = (struct awdl_core *)calloc(1, awdl_core_size());
    const uint8_t addr[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last};
    struct awdl_config cfg;
    awdl_config_default(&cfg, addr);
    cfg.self_metric = metric;
    const struct awdl_host h = {NULL, tx, sched, NULL, NULL, NULL, NULL, NULL};
    SCHECK(awdl_core_init(c, &h, &cfg) == 0);
    return c;
}

struct counts {
    unsigned m_frames_in_our_slots, m_frames_caught, wakeups;
    int64_t worst_sync;
};

// In one of our AWDL slots, past the first 4 ms (our 2 ms guard, then the trip there).
static int our_awdl_slot(uint64_t g)
{
    const struct awdl_chan *seq = awdl_core_sequence(u_core);
    const uint16_t aw = awdl_core_aw_seq(u_core, us_local(g));
    const uint16_t f = awdl_chan_freq(seq[(aw / 4) % 16]);
    const uint64_t start = us_global(awdl_core_aw_start(u_core, (uint16_t)(aw & ~3u), us_local(g)));
    return f && f != HOME_MHZ && g >= start + 4000;
}

// Runs both cores to global time `until`; counts from run_start on.
static void run(uint64_t until, uint64_t *m_deadline, uint64_t *u_deadline, struct counts *k)
{
    while (s_now < until) {
        uint64_t next = until;
        int what = -1;
        if (*m_deadline < next) {
            next = *m_deadline;
            what = 0;
        }
        if (us_global(*u_deadline) < next) {
            next = us_global(*u_deadline);
            what = 1;
        }
        for (int i = 0; i < nair; ++i) {
            if (air[i].at <= next) {
                next = air[i].at;
                what = 2 + i;
            }
        }
        if (next > s_now)
            s_now = next;
        if (what < 0)
            break;
        if (what == 0) {
            *m_deadline = awdl_core_timer(m_core, s_now);
        } else if (what == 1) {
            uint64_t l = us_local(s_now);
            if (l < *u_deadline)
                l = *u_deadline;
            *u_deadline = awdl_core_timer(u_core, l);
            if (s_now >= run_start)
                k->wakeups++;
        } else {
            const struct pending p = air[what - 2];
            air[what - 2] = air[--nair];
            if (p.from == 0) {
                const int counted = s_now >= run_start && our_awdl_slot(p.at);
                k->m_frames_in_our_slots += counted;
                if (our_freq(p.at) == p.freq) {
                    k->m_frames_caught += counted;
                    const struct awdl_rx_meta m = {us_local(p.at) + rnd(500), p.freq, -55};
                    awdl_core_rx(u_core, p.data, p.len, &m);
                    *u_deadline = awdl_core_timer(u_core, us_local(s_now));
                }
            } else if (master_freq(p.at) == p.freq) {
                const struct awdl_rx_meta m = {p.at, p.freq, -55};
                awdl_core_rx(m_core, p.data, p.len, &m);
                *m_deadline = awdl_core_timer(m_core, s_now);
            }
        }
    }
    s_now = until;
}

static void measure_sync(struct counts *k)
{
    for (uint64_t g = s_now; g < s_now + 1048576; g += 4099) {
        const uint16_t aw = awdl_core_aw_seq(m_core, g);
        const uint64_t m_eaw = awdl_core_aw_start(m_core, (uint16_t)(aw & ~3u), g);
        const uint64_t u_eaw = us_global(awdl_core_aw_start(u_core, (uint16_t)(aw & ~3u), us_local(g)));
        const int64_t d = (int64_t)u_eaw - (int64_t)m_eaw;
        if ((d < 0 ? -d : d) > k->worst_sync)
            k->worst_sync = d < 0 ? -d : d;
    }
}

static void test_shared_radio(void)
{
    // The master: AirDrop open (Stute et al.'s "Data": 149 everywhere but channel 6 in slot 8), unassociated.
    m_core = make(0x0a, 520, m_tx, m_schedule);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = i == 8 ? (struct awdl_chan){6, 0x51} : (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(m_core, seq, 0);
    u_core = make(0x0b, 60, u_tx, NULL);
    awdl_core_set_infra(u_core, HOME_MHZ, us_local(0));
    awdl_core_start(m_core, 0);
    awdl_core_start(u_core, us_local(37000));
    uint64_t m_deadline = awdl_core_timer(m_core, 0);
    uint64_t u_deadline = awdl_core_timer(u_core, us_local(37000));
    s_now = 37000;
    struct counts k = {0};
    run_start = 10000000;
    run(run_start, &m_deadline, &u_deadline, &k);
    SCHECK(awdl_core_role(u_core) == AWDL_ROLE_FOLLOWER && awdl_core_election(u_core)->root[5] == 0x0a);
    SCHECK(awdl_slots_away(awdl_core_sequence(u_core), HOME_MHZ) == 4);  // idle: 0, 8, 9, 10
    away_time = longest_away = 0;
    tx_calls = excursions = 0;
    const uint64_t window = 57 * 1048576ull;  // whole sequences
    run(run_start + window, &m_deadline, &u_deadline, &k);
    close_excursion();
    const double home = 1.0 - (double)away_time / (double)window;
    double caught = k.m_frames_in_our_slots ? (double)k.m_frames_caught / k.m_frames_in_our_slots : 0;
    measure_sync(&k);
    const struct awdl_peer *seen = awdl_core_find(m_core, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0b});
    printf("  shared radio, idle: AP keeps %.1f %%, longest absence %.1f ms, %.1f excursions/s, caught %.1f %% of "
           "the master's frames in our AWDL slots, EAW within %lld us, %.1f wakeups/s, %.1f sends/s\n",
           home * 100, (double)longest_away / 1000, excursions / (window / 1e6), caught * 100, (long long)k.worst_sync,
           k.wakeups / (window / 1e6), tx_calls / (window / 1e6));
    SCHECK(home > 0.74 && home < 0.77);
    SCHECK(longest_away <= 3 * 65536 + 3000);  // slots 8-10, under any access point's power-save buffering
    SCHECK(caught > 0.9);
    SCHECK(k.worst_sync < 3000);
    SCHECK(seen && seen->reachable);
    SCHECK(k.wakeups < 12 * 60 && tx_calls < 11 * 60);

    // wifip2pd registers AirDrop traffic: "Data+Infra", the access point keeps 4 of 16 slots
    awdl_core_traffic_registered(u_core, "com.apple.airdrop", us_local(s_now));
    u_deadline = awdl_core_timer(u_core, us_local(s_now));  // the driver re-arms after every request
    SCHECK(awdl_slots_away(awdl_core_sequence(u_core), HOME_MHZ) == 12);
    away_time = 0;
    excursions = 0;
    run_start = s_now;
    memset(&k, 0, sizeof k);
    run(s_now + 2 * 1048576, &m_deadline, &u_deadline, &k);
    close_excursion();
    const double home_t = 1.0 - (double)away_time / (2 * 1048576.0);
    caught = k.m_frames_in_our_slots ? (double)k.m_frames_caught / k.m_frames_in_our_slots : 0;
    printf("  shared radio, transfer: AP keeps %.1f %%, caught %.1f %%, %u excursions\n", home_t * 100, caught * 100,
           excursions);
    SCHECK(home_t > 0.24 && home_t < 0.3);
    SCHECK(caught > 0.9);
    free(m_core);
    free(u_core);
}

void test_sched_main(void)
{
    const int before = test_failures;
    test_shared_radio();
    printf("shared radio: idle and transfer schedules beside the access point: %s\n",
           test_failures == before ? "ok" : "FAILED");
}
