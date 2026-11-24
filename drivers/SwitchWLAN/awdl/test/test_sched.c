// SPDX-License-Identifier: BSD-2-Clause-Patent
// The soft backend sharing one radio with the station: a simulated minute next to an Apple-like master. Our radio has
// no hops; it stays on the access point's channel (36) and leaves only for the excursions host.tx asks for (an
// actframe with a dwell): it gets there 1-3 ms after the request and comes back when the stay ends. The master runs
// its own schedule. Measured: the access point's share of our radio and the longest absence, the master's frames we
// catch in our AWDL slots, our EAW boundaries against the master's (AWDL accepts 3 ms, Stute et al. §6.2), whether the
// master sees us, and how often our core wakes and transmits.
#include "../awdl.h"
#include "../awdl_apple80211.h"
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
    // The master: a Mac's own sequence beside its access point (wifip2pd), the access point's slots empty here.
    m_core = make(0x0a, 520, m_tx, m_schedule);
    static const uint8_t mac[AWDL_SEQ_SLOTS] = {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0};
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = !mac[i]       ? (struct awdl_chan){0, 0}
                 : mac[i] == 6 ? (struct awdl_chan){6, 0x51}
                               : (struct awdl_chan){mac[i], 0x80};
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
    SCHECK(awdl_slots_away(awdl_core_sequence(u_core), HOME_MHZ) == 3);  // 2, 8, 10
    away_time = longest_away = 0;
    tx_calls = excursions = 0;
    const uint64_t window = 57 * 1048576ull;  // whole sequences
    run(run_start + window, &m_deadline, &u_deadline, &k);
    close_excursion();
    const double home = 1.0 - (double)away_time / (double)window;
    double caught = k.m_frames_in_our_slots ? (double)k.m_frames_caught / k.m_frames_in_our_slots : 0;
    measure_sync(&k);
    const struct awdl_peer *seen = awdl_core_find(m_core, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0b});
    printf("  shared radio: AP keeps %.1f %%, longest absence %.1f ms, %.1f excursions/s, caught %.1f %% of "
           "the master's frames in our AWDL slots, EAW within %lld us, %.1f wakeups/s, %.1f sends/s\n",
           home * 100, (double)longest_away / 1000, excursions / (window / 1e6), caught * 100, (long long)k.worst_sync,
           k.wakeups / (window / 1e6), tx_calls / (window / 1e6));
    SCHECK(home > 0.80 && home < 0.84);
    SCHECK(longest_away <= 65536 + 3000);  // one slot at a time, under any access point's power-save buffering
    SCHECK(caught > 0.9);
    SCHECK(k.worst_sync < 3000);
    SCHECK(seen && seen->reachable);
    SCHECK(k.wakeups < 12 * 60 && tx_calls < 11 * 60);
    free(m_core);
    free(u_core);
}

static unsigned switch_installs;
static struct awdl_chan switch_installed[AWDL_SEQ_SLOTS];

static void switch_schedule(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor)
{
    (void)ctx;
    (void)anchor;
    ++switch_installs;
    memcpy(switch_installed, seq, sizeof switch_installed);
}

static void switch_test_legal_channels(void)
{
    struct awdl_core *core = make(0x0c, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, 5260, 0);
    switch_awdl_core_set_channels(core, 3, 0);
    switch_installs = 0;
    awdl_core_start(core, 0);
    SCHECK(switch_installs == 1);
    SCHECK(awdl_slots_away(switch_installed, 5260) == 4);
    SCHECK(awdl_social_map(switch_installed) == 3);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        SCHECK(switch_installed[i].number == 0 || switch_installed[i].opclass ==
               awdl_opclass_20mhz(switch_installed[i].number));
    SCHECK(!switch_installed[1].number && switch_installed[0].number == 44);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t length = awdl_core_build_af(core, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    struct awdl_af parsed;
    SCHECK(length && !awdl_af_parse(frame, length, &parsed));
    SCHECK(!memcmp(parsed.chanseq.chan, switch_installed, sizeof switch_installed));
    switch_awdl_core_set_channels(core, 3, 0);
    SCHECK(switch_installs == 1);
    awdl_core_set_infra(core, 5180, 0);
    SCHECK(switch_installs == 2 && !memcmp(parsed.chanseq.chan, switch_installed, sizeof switch_installed));
    awdl_core_set_infra(core, 5745, 0);
    SCHECK(switch_installed[0].number == 44 && !switch_installed[1].number);
    struct awdl_core *peer = make(0x0d, 520, m_tx, NULL);
    struct awdl_chan peer_seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        peer_seq[i] = (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(peer, peer_seq, 0);
    awdl_core_start(peer, 0);
    const size_t peer_length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    const struct awdl_rx_meta meta = {0, 5745, -55};
    awdl_core_rx(core, frame, peer_length, &meta);
    SCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER && awdl_social_map(switch_installed) == 2);
    awdl_core_stop(peer);
    free(peer);
    SCHECK(awdl_slots_away(switch_installed, 5745) == 4);
    switch_awdl_core_set_channels(core, 0, 0);
    SCHECK(!awdl_social_map(switch_installed));
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        SCHECK(!switch_installed[i].number);
    switch_awdl_core_set_channels(core, 7, 0);
    SCHECK(switch_installed[0].number == 149 && switch_installed[1].number == 149);
    struct awdl_chan requested[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        requested[i] = (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(core, requested, 0);
    switch_awdl_core_set_channels(core, 3, 0);
    SCHECK(awdl_social_map(switch_installed) == 1 && switch_installed[0].number == 6);
    switch_awdl_core_set_channels(core, 7, 0);
    SCHECK(switch_installed[0].number == 149 && switch_installed[0].opclass == 124);
    awdl_core_stop(core);
    free(core);
}

static const uint8_t switch_mac[][AWDL_SEQ_SLOTS] = {
    {6, 0, 149, 0, 0, 44, 0, 0, 6, 0, 44, 0, 0, 44, 0, 0},
    {0, 0, 149, 0, 0, 0, 0, 0, 0, 0, 149, 0, 0, 0, 0, 0},
    {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0},
};

static unsigned switch_mac_overlap(const uint8_t mac[AWDL_SEQ_SLOTS], unsigned offset)
{
    unsigned overlap = 0;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        if (mac[i] && mac[i] == switch_installed[(i + offset) % AWDL_SEQ_SLOTS].number)
            ++overlap;
    return overlap;
}

static void switch_check_home_six(unsigned mask, unsigned away, unsigned pass)
{
    SCHECK(away <= 4);
    if (mask == 1)
        SCHECK(away == 0);
    if (mask & 1u)
        for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
            SCHECK(switch_installed[i].number != 0);
    if ((mask & 6u) == 6u) {
        const uint8_t slot2 = switch_installed[2].number;
        const uint8_t slot10 = switch_installed[10].number;
        SCHECK(slot2 == 149 || (slot2 == 44 && slot10 == 149));
        if (pass % 3u == 0u)
            SCHECK(slot2 == 149 && slot10 == 44);
    } else {
        if (mask & 4u)
            SCHECK(switch_installed[2].number == 149);
        if (mask & 2u)
            SCHECK(switch_installed[10].number == 44);
    }
}

static void switch_test_search_windows(uint16_t infra_mhz)
{
    const uint64_t period = 2 * AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    const uint8_t channels[] = {6, 44, 149};
    const int home_six = infra_mhz == 2437;
    for (unsigned mask = 1; mask <= 7; ++mask) {
        struct awdl_core *core = make(0x0d, 60, m_tx, switch_schedule);
        awdl_core_set_infra(core, infra_mhz, 0);
        switch_awdl_core_set_channels(core, (uint16_t)mask, 0);
        awdl_core_start(core, 0);
        const unsigned bands = (mask & 1u) + ((mask >> 1) & 1u) + (mask >> 2);
        const unsigned cycle = home_six ? ((mask & 6u) == 6u ? 24u : 16u) : 8u * bands;
        const uint8_t primary = (mask & 4u) ? 149 : (mask & 2u) ? 44 : 6;
        const uint8_t secondary = (mask & 1u) ? 6 : (mask & 2u) ? 44 : primary;
        struct awdl_chan first[AWDL_SEQ_SLOTS];
        memcpy(first, switch_installed, sizeof first);
        unsigned visits[3] = {0};
        unsigned away_total = 0;
        unsigned gaps[3][AWDL_SEQ_SLOTS] = {{0}};
        unsigned max_gap[3] = {0};
        for (unsigned pass = 0; pass <= 2 * cycle; ++pass) {
            const uint64_t now = period * pass;
            awdl_core_timer(core, now);
            const unsigned away = awdl_slots_away(switch_installed, infra_mhz);
            if (home_six) {
                switch_check_home_six(mask, away, pass);
            } else {
                SCHECK(away == (pass % 4u ? 5u : 4u));
                SCHECK(switch_installed[2].number == primary || switch_installed[10].number == primary);
                SCHECK(switch_installed[8].number == secondary || switch_installed[0].number == secondary);
            }
            SCHECK(!(awdl_social_map(switch_installed) & ~mask));
            if (!(pass % cycle)) {
                SCHECK(!memcmp(first, switch_installed, sizeof first));
                if (pass) {
                    for (unsigned n = 0; n < 3; ++n) {
                        if (home_six && n == 0 && (mask & 1u))
                            SCHECK(visits[n] != 0);
                        else
                            SCHECK(visits[n] == ((mask & (1u << n)) ? 0xffffu : 0u));
                    }
                    if (home_six)
                        SCHECK(away_total * bands <= 4u * (bands - (mask & 1u)) * cycle);
                }
                memset(visits, 0, sizeof visits);
                away_total = 0;
            }
            for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
                for (unsigned n = 0; n < 3; ++n)
                    if (switch_installed[i].number == channels[n])
                        visits[n] |= 1u << i;
            away_total += away;
            if (mask == 7 || (home_six && mask == 6)) {
                for (unsigned n = 0; n < sizeof switch_mac / sizeof switch_mac[0]; ++n) {
                    SCHECK(switch_mac_overlap(switch_mac[n], 0));
                    if (home_six && mask == 7) {
                        for (unsigned offset = 0; offset < AWDL_SEQ_SLOTS; ++offset) {
                            unsigned *gap = &gaps[n][offset];
                            *gap = switch_mac_overlap(switch_mac[n], offset) ? 0 : *gap + 1;
                            if (*gap > max_gap[n])
                                max_gap[n] = *gap;
                        }
                    }
                }
            }
            for (unsigned n = 0; n < 2; ++n) {
                uint8_t frame[AWDL_MAX_FRAME];
                struct awdl_af action;
                const size_t size = awdl_core_build_af(core, n ? AWDL_SUBTYPE_MIF : AWDL_SUBTYPE_PSF,
                                                       now, frame, sizeof frame);
                SCHECK(size && !awdl_af_parse(frame, size, &action));
                SCHECK(action.sync.has_seq && !memcmp(action.sync.seq.chan, switch_installed, sizeof switch_installed));
                SCHECK(action.chanseq.count == AWDL_SEQ_SLOTS &&
                       !memcmp(action.chanseq.chan, switch_installed, sizeof switch_installed));
            }
        }
        if (home_six && mask == 7) {
            SCHECK(max_gap[0] <= 2 && max_gap[1] <= 11 && max_gap[2] <= 2);
            struct awdl_chan requested[AWDL_SEQ_SLOTS] = {{6, 81}, {44, 115}};
            const uint64_t end = 2 * cycle * period;
            awdl_core_set_sequence(core, requested, end);
            const unsigned installed = switch_installs;
            awdl_core_timer(core, end + period);
            SCHECK(installed == switch_installs && !memcmp(switch_installed, requested, sizeof requested));
        }
        if (!(mask & (mask - 1u))) {
            const uint64_t end = 2 * cycle * period;
            const uint16_t freq = awdl_chan_freq((struct awdl_chan){primary, awdl_opclass_20mhz(primary)});
            awdl_core_set_infra(core, freq, end);
            const unsigned fixed = switch_installs;
            awdl_core_timer(core, end + period);
            SCHECK(switch_installs == fixed);
        }
        awdl_core_stop(core);
        free(core);
    }
}

static unsigned switch_first_slot_illegal(const struct awdl_core *core, unsigned mask)
{
    const uint8_t first = awdl_core_sequence(core)[0].number;
    const unsigned bit = first == 6 ? 1u : first == 44 ? 2u : first == 149 ? 4u : 0u;
    return !(bit & mask);
}

static struct awdl_core *switch_first_slot_core(uint16_t infra, unsigned mask)
{
    struct awdl_core *core = make(0x0f, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, infra, 0);
    switch_awdl_core_set_channels(core, (uint16_t)mask, 0);
    awdl_core_start(core, 0);
    return core;
}

static void switch_hear_peer_on(struct awdl_core *core, uint8_t channel, uint64_t now)
{
    struct awdl_core *peer = make(0x10, 520, m_tx, NULL);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){channel, awdl_opclass_20mhz(channel)};
    awdl_core_set_sequence(peer, seq, 0);
    awdl_core_start(peer, 0);
    awdl_core_timer(peer, now);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t bytes = awdl_core_build_af(peer, AWDL_SUBTYPE_PSF, now, frame, sizeof frame);
    const struct awdl_rx_meta meta = {now, awdl_chan_freq(seq[0]), -45};
    awdl_core_rx(core, frame, bytes, &meta);
    awdl_core_stop(peer);
    free(peer);
}

// Every search pass, then a timed peer's sequence.
static void switch_test_first_slot(void)
{
    const uint64_t period = 2 * AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    for (unsigned channel = 0; channel <= 177; ++channel) {
        if (channel && !awdl_opclass_20mhz((uint8_t)channel))
            continue;
        const uint16_t infra = awdl_chan_freq((struct awdl_chan){(uint8_t)channel, 0});
        for (unsigned mask = 1; mask <= 7; ++mask) {
            unsigned illegal = 0;
            struct awdl_core *core = switch_first_slot_core(infra, mask);
            for (unsigned pass = 0; pass < 48; ++pass) {
                awdl_core_timer(core, period * pass);
                illegal += switch_first_slot_illegal(core, mask);
            }
            awdl_core_stop(core);
            free(core);
            core = switch_first_slot_core(infra, mask);
            switch_hear_peer_on(core, mask & 1u ? 6 : mask & 2u ? 44 : 149, period);
            SCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER);
            illegal += switch_first_slot_illegal(core, mask);
            awdl_core_stop(core);
            free(core);
            if (illegal)
                fprintf(stderr, "  slot 0 outside the legal channels %u times: AP channel %u, legal mask %u\n", illegal,
                        channel, mask);
            SCHECK(!illegal);
        }
    }
}

static void switch_test_acquisition(void)
{
    struct awdl_core *core = make(0x0d, 60, m_tx, switch_schedule);
    const uint64_t period = 2 * AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    awdl_core_set_infra(core, 2437, 0);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_start(core, 0);
    SCHECK(switch_installed[2].number == 149 && switch_installed[10].number == 44);
    for (unsigned slot = 0; slot < AWDL_SEQ_SLOTS; ++slot)
        awdl_core_timer(core, slot * (period / 32) + period / 64);
    SCHECK(awdl_core_stats(core)->mif_tx == AWDL_SEQ_SLOTS);
    awdl_core_set_infra(core, 5180, period);
    struct awdl_core *peer = make(0x0e, 520, m_tx, NULL);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){44, 115};
    const uint64_t now = 12 * period;
    awdl_core_set_sequence(peer, seq, 0);
    awdl_core_start(peer, 0);
    awdl_core_timer(peer, now);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t bytes = awdl_core_build_af(peer, AWDL_SUBTYPE_PSF, now, frame, sizeof frame);
    const struct awdl_rx_meta meta = {now, 5220, -45};
    const unsigned before = switch_installs;
    awdl_core_rx(core, frame, bytes, &meta);
    SCHECK(switch_installs == before + 1);
    SCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER);
    awdl_core_set_infra(core, 2437, now);
    SCHECK(switch_installed[0].number == 6 && switch_installed[2].number == 149 && switch_installed[10].number == 44);
    awdl_core_set_infra(core, 5745, now);
    SCHECK(switch_installed[0].number == 149 && switch_installed[8].number == 6 && switch_installed[10].number == 44);
    unsigned installed = switch_installs;
    awdl_core_timer(core, now + period / 2);
    SCHECK(switch_installs == installed);
    switch_awdl_core_set_channels(core, 1, now + period / 2);
    SCHECK(awdl_slots_away(switch_installed, 5745) == 4 && !switch_installed[10].number);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){6, 81};
    awdl_core_set_sequence(peer, seq, now + period);
    const size_t six = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, now + period, frame, sizeof frame);
    const struct awdl_rx_meta ch6 = {now + period, 2437, -45};
    awdl_core_rx(core, frame, six, &ch6);
    SCHECK(switch_installed[0].number == 6 && switch_installed[10].number == 6 && !switch_installed[1].number);

    const uint8_t src[] = {2, 0x11, 0x22, 0x33, 0x44, 0x0e};
    uint8_t eth[54] = {2, 0x11, 0x22, 0x33, 0x44, 0x0d};
    memcpy(eth + 6, src, sizeof src);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    const size_t data = awdl_data_encap(eth, sizeof eth, src, 1, frame, sizeof frame);
    const struct awdl_rx_meta late = {ch6.tsf + AWDL_PEER_TIMEOUT_US + period / 2, 2437, -45};
    SCHECK(data != 0);
    awdl_core_rx(core, frame, data, &late);
    awdl_core_timer(core, late.tsf);
    const struct awdl_peer *known = awdl_core_find(core, src);
    SCHECK(known && known->last_seen == late.tsf);
    SCHECK(awdl_slots_away(switch_installed, 5745) == 4 && !switch_installed[10].number);
    const size_t fresh = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, late.tsf, frame, sizeof frame);
    awdl_core_rx(core, frame, fresh, &late);
    SCHECK(switch_installed[10].number == 6);

    awdl_core_set_addr(core, (const uint8_t[6]){2, 1, 2, 3, 4, 5});
    awdl_core_set_infra(core, 0, late.tsf);
    switch_awdl_core_set_channels(core, 3, late.tsf);
    for (unsigned pass = 0; pass < 2; ++pass) {
        awdl_core_timer(core, late.tsf + period * pass);
        for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
            SCHECK(switch_installed[i].number == (pass ? 6 : 44));
    }
    awdl_core_stop(peer);
    awdl_core_stop(core);
    free(peer);
    free(core);
}

#if DEVELOPMENT || DEBUG
static void switch_test_home_trial(void)
{
    struct awdl_core *core = make(0x0d, 60, m_tx, switch_schedule);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_set_infra(core, 2437, 0);
    SCHECK(!switch_awdl_core_home_trial(core, 1, 0));
    awdl_core_start(core, 0);
    SCHECK(switch_installed[2].number == 149 && switch_installed[10].number == 44);
    SCHECK(switch_awdl_core_home_trial(core, 1, 1000) == 6);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        SCHECK(switch_installed[i].number == 6 && switch_installed[i].opclass == 81);
        SCHECK(switch_mac_overlap(switch_mac[2], i) == 1);
    }
    uint8_t frame[AWDL_MAX_FRAME];
    const uint8_t subtypes[] = {AWDL_SUBTYPE_PSF, AWDL_SUBTYPE_MIF};
    for (unsigned i = 0; i < sizeof subtypes; ++i) {
        const size_t bytes = awdl_core_build_af(core, subtypes[i], 1000, frame, sizeof frame);
        struct awdl_af action;
        SCHECK(bytes && !awdl_af_parse(frame, bytes, &action));
        SCHECK(action.sync.has_seq && !memcmp(action.sync.seq.chan, switch_installed, sizeof switch_installed));
        SCHECK(!memcmp(action.chanseq.chan, switch_installed, sizeof switch_installed));
    }

    struct awdl_chan desired[AWDL_SEQ_SLOTS] = {{44, 115}};
    awdl_core_set_sequence(core, desired, 2000);
    SCHECK(switch_installed[0].number == 6 && switch_installed[15].number == 6);
    switch_awdl_core_home_trial(core, 0, 3000);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));

    SCHECK(switch_awdl_core_home_trial(core, 1, 4000) == 6);
    switch_awdl_core_set_channels(core, 2, 4100);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));
    SCHECK(!switch_awdl_core_home_trial(core, 1, 4200));
    awdl_core_set_infra(core, 5220, 4300);
    SCHECK(switch_awdl_core_home_trial(core, 1, 4400) == 44);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        SCHECK(switch_installed[i].number == 44 && switch_installed[i].opclass == 115);
    awdl_core_set_infra(core, 5180, 4500);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));
    SCHECK(!switch_awdl_core_home_trial(core, 1, 4600));
    awdl_core_set_infra(core, 5220, 4700);
    SCHECK(switch_awdl_core_home_trial(core, 1, 4800) == 44);
    awdl_core_stop(core);
    awdl_core_start(core, 5000);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));
    awdl_core_stop(core);
    free(core);
}
#endif

static void switch_test_traffic_schedule(void)
{
    struct awdl_core *core = make(0x0e, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, 5180, 0);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_start(core, 0);
    struct awdl_core *peer = make(0x0f, 520, m_tx, NULL);
    struct awdl_chan peer_seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        peer_seq[i] = (struct awdl_chan){(i & 1) ? 44 : 149, 0x80};
    awdl_core_set_sequence(peer, peer_seq, 0);
    awdl_core_start(peer, 0);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    const struct awdl_rx_meta meta = {0, 5745, -55};
    awdl_core_rx(core, frame, length, &meta);
    SCHECK(awdl_slots_away(switch_installed, 5180) == 4);
    SCHECK(!switch_installed[1].number && !switch_installed[3].number && !switch_installed[9].number &&
           !switch_installed[11].number);

    switch_awdl_core_set_traffic(core, 1, 1000);
    SCHECK(awdl_slots_away(switch_installed, 5180) == 8);
    static const uint8_t active[AWDL_SEQ_SLOTS] = {44, 44, 149, 44, 0, 0, 0, 0, 6, 149, 44, 149, 0, 0, 0, 0};
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        SCHECK(switch_installed[i].number == active[i]);

    switch_awdl_core_set_channels(core, 3, 1500);
    SCHECK(awdl_slots_away(switch_installed, 5180) == 8);
    SCHECK(awdl_social_map(switch_installed) == 3);
    for (unsigned i = 4; i < 8; ++i)
        SCHECK(!switch_installed[i].number && !switch_installed[i + 8].number);

    switch_awdl_core_set_channels(core, 7, 1800);
    switch_awdl_core_set_traffic(core, 0, 2000);
    SCHECK(awdl_slots_away(switch_installed, 5180) == 4);
    SCHECK(!switch_installed[1].number && !switch_installed[3].number && !switch_installed[9].number &&
           !switch_installed[11].number);
    awdl_core_stop(peer);
    awdl_core_stop(core);
    free(peer);
    free(core);
}

static void switch_test_rx_data_schedule(void)
{
    struct awdl_core *core = make(0x0e, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, 2437, 0);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_start(core, 0);
    struct awdl_core *peer = make(0x0f, 520, m_tx, NULL);
    struct awdl_chan peer_seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        peer_seq[i] = (struct awdl_chan){(i & 1) ? 44 : 149, 0x80};
    awdl_core_set_sequence(peer, peer_seq, 0);
    awdl_core_start(peer, 0);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    const struct awdl_rx_meta af_meta = {1000, 5745, -55};
    awdl_core_rx(core, frame, length, &af_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    const uint8_t dst[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0e};
    const uint8_t src[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0f};
    const uint8_t mcast[6] = {0x33, 0x33, 0x00, 0x00, 0x00, 0xfb};
    uint8_t eth[64] = {0};
    memcpy(eth, mcast, 6);
    memcpy(eth + 6, src, 6);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    for (uint16_t s = 1; s <= 5; ++s) {
        const size_t dlen = awdl_data_encap(eth, sizeof eth, src, s, frame, sizeof frame);
        const struct awdl_rx_meta m = {10000 + s * 1000, 2437, -50};
        awdl_core_rx(core, frame, dlen, &m);
    }
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    memcpy(eth, dst, 6);
    for (uint16_t s = 10; s < 13; ++s) {
        const size_t dlen = awdl_data_encap(eth, sizeof eth, src, s, frame, sizeof frame);
        const struct awdl_rx_meta m = {20000 + (uint64_t)(s - 10) * 1000, 2437, -50};
        awdl_core_rx(core, frame, dlen, &m);
    }
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    const size_t dup_len = awdl_data_encap(eth, sizeof eth, src, 12, frame, sizeof frame);
    frame[1] |= 0x08;
    const struct awdl_rx_meta dup_meta = {23000, 2437, -50};
    awdl_core_rx(core, frame, dup_len, &dup_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    const size_t fourth_len = awdl_data_encap(eth, sizeof eth, src, 13, frame, sizeof frame);
    const struct awdl_rx_meta fourth_meta = {24000, 5220, -50};
    awdl_core_rx(core, frame, fourth_len, &fourth_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);

    const size_t refresh_len = awdl_data_encap(eth, sizeof eth, src, 14, frame, sizeof frame);
    const struct awdl_rx_meta refresh_meta = {1500000, 5220, -50};
    awdl_core_rx(core, frame, refresh_len, &refresh_meta);
    awdl_core_timer(core, 2500000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);

    switch_awdl_core_set_traffic(core, 1, 3000000);
    awdl_core_timer(core, 3600000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    switch_awdl_core_set_traffic(core, 0, 3700000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    for (uint16_t s = 20; s < 24; ++s) {
        const size_t dlen = awdl_data_encap(eth, sizeof eth, src, s, frame, sizeof frame);
        const struct awdl_rx_meta m = {4000000 + (uint64_t)(s - 20) * 1000, 5220, -50};
        awdl_core_rx(core, frame, dlen, &m);
    }
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    awdl_core_timer(core, 6003000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        peer_seq[i] = (struct awdl_chan){switch_mac[0][i], awdl_opclass_20mhz(switch_mac[0][i])};
    awdl_core_set_sequence(peer, peer_seq, 6100000);
    const size_t idle_len = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 6100000, frame, sizeof frame);
    const struct awdl_rx_meta idle_meta = {6100000, 5745, -55};
    awdl_core_rx(core, frame, idle_len, &idle_meta);
    for (uint16_t s = 30; s < 35; ++s) {
        const size_t dlen = awdl_data_encap(eth, sizeof eth, src, s, frame, sizeof frame);
        const struct awdl_rx_meta m = {6110000 + (uint64_t)(s - 30) * 1000, 2437, -50};
        awdl_core_rx(core, frame, dlen, &m);
    }
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    awdl_core_stop(peer);
    awdl_core_stop(core);
    free(peer);
    free(core);
}

static void switch_test_tx_data_and_keepalive(void)
{
    struct awdl_core *core = make(0x0e, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, 2437, 0);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_start(core, 0);
    struct awdl_core *peer = make(0x0f, 520, m_tx, NULL);
    struct awdl_chan peer_seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        peer_seq[i] = (struct awdl_chan){(i & 1) ? 44 : 149, 0x80};
    awdl_core_set_sequence(peer, peer_seq, 0);
    awdl_core_start(peer, 0);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t mif_len = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    const struct awdl_rx_meta af_meta = {1000, 5745, -55};
    awdl_core_rx(core, frame, mif_len, &af_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    const uint8_t us[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0e};
    const uint8_t them[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0f};
    uint8_t rx_eth[64] = {0}, tx_eth[64] = {0};
    memcpy(rx_eth, us, 6);
    memcpy(rx_eth + 6, them, 6);
    rx_eth[12] = 0x86;
    rx_eth[13] = 0xdd;
    memcpy(tx_eth, them, 6);
    memcpy(tx_eth + 6, us, 6);
    tx_eth[12] = 0x86;
    tx_eth[13] = 0xdd;

    for (unsigned k = 0; k < 4; ++k) {
        const uint64_t t = 100000 + (uint64_t)k * 2100000;
        const size_t dlen = awdl_data_encap(rx_eth, sizeof rx_eth, them, (uint16_t)(k + 1), frame, sizeof frame);
        const struct awdl_rx_meta m = {t, 2437, -50};
        awdl_core_rx(core, frame, dlen, &m);
        SCHECK(awdl_core_output(core, tx_eth, sizeof tx_eth, t + 1000) == 0);
        SCHECK(awdl_slots_away(switch_installed, 2437) == 2);
    }

    const uint64_t burst = 9000000;
    const size_t fresh_mif = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, burst, frame, sizeof frame);
    const struct awdl_rx_meta fresh_af = {burst, 5745, -55};
    awdl_core_rx(core, frame, fresh_mif, &fresh_af);
    for (unsigned i = 0; i < 3; ++i)
        SCHECK(awdl_core_output(core, tx_eth, sizeof tx_eth, burst + i * 1000) == 0);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);
    SCHECK(awdl_core_output(core, tx_eth, sizeof tx_eth, burst + 3000) == 0);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    SCHECK(awdl_core_output(core, tx_eth, sizeof tx_eth, burst + 1500000) == 0);
    awdl_core_timer(core, burst + 2500000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    awdl_core_timer(core, burst + 3500000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    awdl_core_stop(peer);
    awdl_core_stop(core);
    free(peer);
    free(core);
}

#if defined(SWITCH_IPSW_KIND_24B119)
static int switch_offer_service(struct a80211_awdl *facade, const uint8_t *tlv, size_t bytes, uint64_t now)
{
    uint8_t data[sizeof(struct a80211_peer_service_request) + 64];
    const struct a80211_peer_service_request request = {.version = 1, .data_len = (uint16_t)bytes};
    memcpy(data, &request, sizeof request);
    memcpy(data + sizeof request, tlv, bytes);
    uint32_t len = (uint32_t)(sizeof request + bytes);
    int32_t val = 0;
    return a80211_awdl_request(facade, 1, A80211_IOC_AWDL_PEER_SERVICE_REQUEST, &val, data, &len, sizeof data, now);
}

static void switch_test_discovery_boost(void)
{
    struct awdl_core *core = make(0x0e, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, 2437, 0);
    switch_awdl_core_set_channels(core, 7, 0);
    struct a80211_awdl facade;
    const uint8_t caps[12] = {0};
    const struct a80211_awdl_ops ops = {0};
    a80211_awdl_init(&facade, core, &ops, "awdl0", "en0", caps);

    static const uint8_t browse[] = {
        AWDL_TLV_SERVICE_REQUEST, 18, 0, 0, 0, 0, 0, 8, '_', 'a', 'i', 'r', 'd', 'r', 'o', 'p', 0xc0, 0x0c, 0, 12, 1,
    };
    static const uint8_t advert[] = {
        AWDL_TLV_SERVICE_RESPONSE, 35,  0,   14,  0,   8,   '_', 'a', 'i', 'r', 'd', 'r', 'o', 'p', 0xc0, 0x0c, 0,
        12,                        1,   15,  0,   0,   0,   12,  '0', '1', '2', '3', '4', '5', '6', '7',  '8',  '9',
        'a',                       'b', 0xc0, 0x27,
    };

    awdl_core_start(core, 0);
    struct awdl_core *peer = make(0x0f, 520, m_tx, NULL);
    struct awdl_chan peer_seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        peer_seq[i] = (struct awdl_chan){(i & 1) ? 44 : 149, 0x80};
    awdl_core_set_sequence(peer, peer_seq, 0);
    awdl_core_start(peer, 0);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t mif_len = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    const struct awdl_rx_meta af_meta = {1000, 5745, -55};
    awdl_core_rx(core, frame, mif_len, &af_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    SCHECK(switch_offer_service(&facade, browse, sizeof browse, 10000) == 0);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    const uint64_t mid = 1000 + AWDL_EAW_TU * AWDL_TU_US / 2;
    awdl_core_timer(core, mid);
    const uint32_t mifs = awdl_core_stats(core)->mif_tx;
    SCHECK(switch_offer_service(&facade, advert, sizeof advert, mid + 2000) == 0);
    const uint64_t due = awdl_core_timer(core, mid + 2000);
    SCHECK(due == mid + 2000 + 4u * AWDL_TU_US);
    awdl_core_timer(core, due);
    SCHECK(awdl_core_stats(core)->mif_tx == mifs + 1);

    awdl_core_timer(core, mid + 2000 + 3000000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    awdl_core_stop(core);
    awdl_core_start(core, 4000000);
    const size_t fresh_len = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 4500000, frame, sizeof frame);
    const struct awdl_rx_meta fresh_meta = {4500000, 5745, -55};
    awdl_core_rx(core, frame, fresh_len, &fresh_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    awdl_core_timer(core, 7000000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    awdl_core_stop(peer);
    awdl_core_stop(core);
    free(peer);
    free(core);
}
#endif

static void switch_test_syn_and_deferred_data_schedule(void)
{
    struct awdl_core *core = make(0x0e, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, 2437, 0);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_start(core, 0);
    struct awdl_core *peer = make(0x0f, 520, m_tx, NULL);
    struct awdl_chan idle_seq[AWDL_SEQ_SLOTS], active_seq[AWDL_SEQ_SLOTS];
    static const uint8_t mac_active[AWDL_SEQ_SLOTS] = {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0};
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        idle_seq[i] = (struct awdl_chan){switch_mac[0][i], awdl_opclass_20mhz(switch_mac[0][i])};
        active_seq[i] = (struct awdl_chan){mac_active[i], awdl_opclass_20mhz(mac_active[i])};
    }
    awdl_core_set_sequence(peer, idle_seq, 0);
    awdl_core_start(peer, 0);

    uint8_t frame[AWDL_MAX_FRAME];
    size_t length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    struct awdl_rx_meta af_meta = {1000, 5745, -55};
    awdl_core_rx(core, frame, length, &af_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    const uint8_t us[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0e};
    const uint8_t them[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0f};
    uint8_t rx_syn[74] = {0}, tx_syn[74] = {0};
    memcpy(rx_syn, us, 6);
    memcpy(rx_syn + 6, them, 6);
    rx_syn[12] = 0x86;
    rx_syn[13] = 0xdd;
    rx_syn[14] = 0x60;
    rx_syn[20] = 6;
    rx_syn[14 + 40 + 13] = 0x02;
    memcpy(tx_syn, them, 6);
    memcpy(tx_syn + 6, us, 6);
    memcpy(tx_syn + 12, rx_syn + 12, sizeof tx_syn - 12);

    size_t dlen = awdl_data_encap(rx_syn, sizeof rx_syn, them, 1, frame, sizeof frame);
    struct awdl_rx_meta data_meta = {100000, 2437, -50};
    awdl_core_rx(core, frame, dlen, &data_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    awdl_core_set_sequence(peer, active_seq, 600000);
    length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 600000, frame, sizeof frame);
    af_meta.tsf = 600000;
    awdl_core_rx(core, frame, length, &af_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);

    awdl_core_timer(core, 2099999);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    awdl_core_timer(core, 2100000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    SCHECK(awdl_core_output(core, tx_syn, sizeof tx_syn, 2500000) == 0);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    awdl_core_timer(core, 4500000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    uint8_t large_rx[1260] = {0};
    memcpy(large_rx, us, 6);
    memcpy(large_rx + 6, them, 6);
    large_rx[12] = 0x86;
    large_rx[13] = 0xdd;
    large_rx[14] = 0x60;
    large_rx[20] = 17;
    dlen = awdl_data_encap(large_rx, sizeof large_rx, them, 2, frame, sizeof frame);
    data_meta.tsf = 5000000;
    awdl_core_rx(core, frame, dlen, &data_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);

    awdl_core_set_sequence(peer, idle_seq, 5500000);
    length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 5500000, frame, sizeof frame);
    af_meta.tsf = 5500000;
    awdl_core_rx(core, frame, length, &af_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    awdl_core_set_sequence(peer, active_seq, 6000000);
    length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 6000000, frame, sizeof frame);
    af_meta.tsf = 6000000;
    awdl_core_rx(core, frame, length, &af_meta);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    awdl_core_timer(core, 7000000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    for (uint64_t t = 7500000; t <= 15000000; t += 1500000) {
        SCHECK(awdl_core_output(core, tx_syn, sizeof tx_syn, t) == 0);
        SCHECK(awdl_slots_away(switch_installed, 2437) == 7);
    }
    awdl_core_timer(core, 16500000);
    SCHECK(awdl_slots_away(switch_installed, 2437) == 2);

    awdl_core_stop(peer);
    awdl_core_stop(core);
    free(peer);
    free(core);
}

void test_sched_main(void)
{
    const int before = test_failures;
    test_shared_radio();
    switch_test_legal_channels();
    switch_test_search_windows(2462);
    switch_test_search_windows(2437);
    switch_test_first_slot();
    switch_test_acquisition();
    switch_test_traffic_schedule();
    switch_test_rx_data_schedule();
    switch_test_tx_data_and_keepalive();
    switch_test_syn_and_deferred_data_schedule();
#if defined(SWITCH_IPSW_KIND_24B119)
    switch_test_discovery_boost();
#endif
#if DEVELOPMENT || DEBUG
    switch_test_home_trial();
#endif
    printf("shared radio: the schedule beside the access point: %s\n", test_failures == before ? "ok" : "FAILED");
}
