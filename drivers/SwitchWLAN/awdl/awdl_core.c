// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AWDL core: timing model (AW / extended AW / channel sequence on the local TSF), synchronisation to the parent,
// master election, the peer table, and the soft backend's PSF/MIF transmission and data queue. See awdl.h.
//
// Timing model (Stute et al. §6.2): AWs of 16 TU are numbered by a 16-bit counter; 4 AWs make one EAW, one slot of the
// 16-slot channel sequence (1024 TU), and every EAW starts at an AW number that is a multiple of 4. base_tsf is the
// local TSF at which AW number base_seq starts; it moves only when we synchronise to a parent. A sender's sync
// parameters carry its current AW number and the TU left until its next EAW starts, counted from the time it meant to
// send (target_tx_time); phy_tx_time says when it really did. A receiver places the sender's next EAW, AW number
// (aw_seq & ~3) + 4, at rx_tsf - (phy_tx_time - target_tx_time) + counter TU.
#include "awdl_internal.h"

void awdl_config_default(struct awdl_config *c, const uint8_t addr[6])
{
    memset(c, 0, sizeof *c);
    memcpy(c->addr, addr, 6);
    memcpy(c->hostname, "switch", 7);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        c->seq[i] = (struct awdl_chan){149, 0x80};
    // The metric a node starts with (Stute et al. §6.1). Kept: an Apple device draws 505-536 once it finds no master, so
    // it stays the master and we follow its clock.
    c->self_metric = 60;
    c->awdl_version = 0x34;
    c->device_class = 2;
    c->ht_streams = 2;
    c->country[0] = 'X';
    c->country[1] = 'X';
    c->backend = AWDL_BACKEND_SOFT;
}

size_t awdl_core_size(void)
{
    return sizeof(struct awdl_core);
}

void awdl_log(struct awdl_core *c, const char *fmt, ...)
{
    if (!c->host.log)
        return;
    char line[160];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    c->host.log(c->host.ctx, line);
}

uint16_t awdl_core_aw_seq(const struct awdl_core *c, uint64_t now)
{
    if (now >= c->base_tsf)
        return (uint16_t)(c->base_seq + (now - c->base_tsf) / AWDL_AW_US);
    const uint64_t back = c->base_tsf - now;
    return (uint16_t)(c->base_seq - (back + AWDL_AW_US - 1) / AWDL_AW_US);
}

uint64_t awdl_core_aw_start(const struct awdl_core *c, uint16_t seq, uint64_t now)
{
    (void)now;
    const int64_t d = (int16_t)(uint16_t)(seq - c->base_seq);
    return (uint64_t)((int64_t)c->base_tsf + d * (int64_t)AWDL_AW_US);
}

static unsigned slot_of(uint16_t aw)
{
    return (aw / AWDL_EAW_AWS) % AWDL_SEQ_SLOTS;
}

struct awdl_chan awdl_core_channel_at(const struct awdl_core *c, uint64_t now)
{
    return c->cfg.seq[slot_of(awdl_core_aw_seq(c, now))];
}

uint64_t awdl_eaw_start(const struct awdl_core *c, uint64_t now)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    return awdl_core_aw_start(c, (uint16_t)(aw - aw % AWDL_EAW_AWS), now);
}

// Keep the base near `now` so 16-bit differences never wrap (it moves by whole AWs: the schedule is unchanged).
static void rebase(struct awdl_core *c, uint64_t now)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    c->base_tsf = awdl_core_aw_start(c, aw, now);
    c->base_seq = aw;
}

// A slot the radio has to leave its home channel for: any slot with a channel while unassociated, else one not on the
// access point's channel.
static int away_slot(const struct awdl_core *c, unsigned slot)
{
    const uint16_t f = awdl_chan_freq(c->cfg.seq[slot]);
    return f && f != c->infra_mhz;
}

// Where the next away slot starts after `now` (a sequence later when there is none).
static uint64_t next_away_start(const struct awdl_core *c, uint64_t now)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    uint16_t first = (uint16_t)((aw / AWDL_EAW_AWS + 1) * AWDL_EAW_AWS);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i, first = (uint16_t)(first + AWDL_EAW_AWS)) {
        if (away_slot(c, slot_of(first)))
            return awdl_core_aw_start(c, first, now);
    }
    return awdl_eaw_start(c, now) + AWDL_SEQ_US;
}

static uint64_t slot_end(const struct awdl_core *c, uint64_t now)
{
    return awdl_eaw_start(c, now) + AWDL_EAW_US;
}

void awdl_soft_install(struct awdl_core *c, uint64_t now)
{
    if (!c->running || awdl_native(c))
        return;
    const uint16_t aw = awdl_core_aw_seq(c, now);
    c->anchor = awdl_core_aw_start(c, (uint16_t)(aw - aw % (AWDL_EAW_AWS * AWDL_SEQ_SLOTS)), now);
    if (c->host.schedule)
        c->host.schedule(c->host.ctx, c->cfg.seq, c->anchor);
    c->next_slot = next_away_start(c, now) + AWDL_SLOT_GUARD_US;
}

// ---- election -----------------------------------------------------------------------------------------------------
// One way to a root: through ourselves (dist 0) or through a peer (`via`, at its own distance from its root).
struct candidate {
    const uint8_t *root, *via;
    uint32_t metric, counter, dist;
};

// OWL's order (src/election.c), Apple's v2 election: the root that has been master longest, then the higher metric,
// then the node nearer its root, then the higher address of the node we would follow.
static int better(const struct candidate *a, const struct candidate *b)
{
    if (a->counter != b->counter)
        return a->counter > b->counter;
    if (a->metric != b->metric)
        return a->metric > b->metric;
    if (a->dist != b->dist)
        return a->dist < b->dist;
    return memcmp(a->via, b->via, 6) > 0;
}

void awdl_run_election(struct awdl_core *c, uint64_t now)
{
    const uint8_t *self = c->cfg.addr;
    struct candidate best = {self, self, c->cfg.self_metric, c->self_counter, 0};
    const struct awdl_peer *parent = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        const struct awdl_peer *p = &c->peers[i];
        if (!p->used || !p->has_v2 || now - p->last_seen > AWDL_ELECTION_FRESH_US)
            continue;
        const struct awdl_election_v2 *e = &p->election;
        if (awdl_addr_eq(e->root, self) || awdl_addr_eq(e->parent, self))
            continue;  // our own subtree: no loops
        if (e->hops + 1 > AWDL_ELECTION_MAX_HOPS)
            continue;
        struct candidate k = {e->root, p->addr, e->root_metric, e->root_counter, e->hops};
        if (better(&k, &best)) {
            best = k;
            parent = p;
        }
    }
    const enum awdl_role was = c->role;
    uint8_t old_parent[6];
    uint8_t old_root[6];
    memcpy(old_parent, c->adv.parent, 6);
    memcpy(old_root, c->adv.root, 6);
    c->role = parent ? AWDL_ROLE_FOLLOWER : AWDL_ROLE_ROOT;
    memcpy(c->adv.root, best.root, 6);
    memcpy(c->adv.parent, best.via, 6);
    c->adv.root_counter = best.counter;
    c->adv.hops = parent ? best.dist + 1 : 0;
    c->adv.root_metric = best.metric;
    c->adv.self_metric = c->cfg.self_metric;
    c->adv.self_counter = c->self_counter;
    if (was != c->role || !awdl_addr_eq(old_parent, c->adv.parent) || !awdl_addr_eq(old_root, c->adv.root)) {
        awdl_log(c, "AWDL: %s, root %02x:%02x:%02x:%02x:%02x:%02x metric %u counter %u, %u hops",
                 c->role == AWDL_ROLE_ROOT ? "root" : "follower", best.root[0], best.root[1], best.root[2],
                 best.root[3], best.root[4], best.root[5], best.metric, best.counter, c->adv.hops);
    }
}

// The counter of a root grows while it stays one; it is our tie-breaker against other new roots, so only its rate
// matters, and it is caught up here rather than given a wake-up of its own.
static void tick_counter(struct awdl_core *c, uint64_t now)
{
    const uint64_t ticks = (now - c->last_counter_tick) / AWDL_ROOT_COUNTER_US;
    if (!ticks)
        return;
    c->last_counter_tick += ticks * AWDL_ROOT_COUNTER_US;
    if (c->role == AWDL_ROLE_ROOT)
        c->self_counter += (uint32_t)ticks;
}

uint8_t awdl_master_chan(const struct awdl_core *c)
{
    if (c->role == AWDL_ROLE_FOLLOWER) {
        const struct awdl_peer *p = awdl_core_find(c, c->adv.parent);
        if (p && p->sync.master_channel)
            return p->sync.master_channel;
    }
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        const uint8_t ch = awdl_chan_primary(c->cfg.seq[i]);
        if (ch == 44 || ch == 149)
            return ch;
    }
    return 6;
}

// ---- peers ---------------------------------------------------------------------------------------------------------
const struct awdl_peer *awdl_core_find(const struct awdl_core *c, const uint8_t addr[6])
{
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used && awdl_addr_eq(c->peers[i].addr, addr))
            return &c->peers[i];
    }
    return NULL;
}

static struct awdl_peer *peer_get(struct awdl_core *c, const uint8_t addr[6], uint64_t now)
{
    struct awdl_peer *free_slot = NULL;
    struct awdl_peer *oldest = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        struct awdl_peer *p = &c->peers[i];
        if (p->used && awdl_addr_eq(p->addr, addr))
            return p;
        if (!p->used && !free_slot)
            free_slot = p;
        if (p->used && !p->reachable && (!oldest || p->last_seen < oldest->last_seen))
            oldest = p;
    }
    struct awdl_peer *p = free_slot ? free_slot : oldest;  // a full table recycles the stalest unreachable peer
    if (!p)
        return NULL;
    memset(p, 0, sizeof *p);
    p->used = 1;
    memcpy(p->addr, addr, 6);
    p->first_seen = now;
    return p;
}

static void peer_lost(struct awdl_core *c, struct awdl_peer *p, const char *why)
{
    if (p->reachable && awdl_native(c))
        awdl_native_peer(c, p, 0);
    if (p->reachable && c->host.peer)
        c->host.peer(c->host.ctx, p->addr, 0, p->rssi);
    awdl_log(c, "AWDL: peer %02x:%02x:%02x:%02x:%02x:%02x lost (%s)", p->addr[0], p->addr[1], p->addr[2], p->addr[3],
             p->addr[4], p->addr[5], why);
    memset(p, 0, sizeof *p);
}

void awdl_expire_peers(struct awdl_core *c, uint64_t now)
{
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used && now - c->peers[i].last_seen > AWDL_PEER_TIMEOUT_US)
            peer_lost(c, &c->peers[i], "timeout");
    }
}

const struct awdl_peer *awdl_core_peer(const struct awdl_core *c, unsigned i)
{
    return i < AWDL_MAX_PEERS && c->peers[i].used ? &c->peers[i] : NULL;
}

// The sender's next EAW on our clock (Stute et al. §6.2: the master announces "the number of TUs to the next EAW").
static int sender_next_eaw(const struct awdl_af *a, uint64_t rx_tsf, uint64_t *tsf, uint16_t *seq)
{
    if (!a->sync.tx_counter || a->sync.tx_counter > AWDL_EAW_TU)
        return 0;
    int64_t delay = (int32_t)(a->phy_tx_time - a->target_tx_time);
    if (delay < 0 || delay > (int64_t)AWDL_EAW_US)
        delay = 0;  // nonsense: ignore it
    *tsf = (uint64_t)((int64_t)rx_tsf - delay) + (uint64_t)a->sync.tx_counter * AWDL_TU_US;
    *seq = (uint16_t)((a->sync.aw_seq & ~(AWDL_EAW_AWS - 1u)) + AWDL_EAW_AWS);
    return 1;
}

static void sync_to(struct awdl_core *c, const struct awdl_peer *p, uint64_t now)
{
    const uint64_t predicted = awdl_core_aw_start(c, p->next_aw_seq, now);
    const int64_t err = (int64_t)(p->next_aw_tsf - predicted);
    c->base_tsf = p->next_aw_tsf;
    c->base_seq = p->next_aw_seq;
    int64_t clamped = err < INT32_MIN ? INT32_MIN : err;
    if (err > INT32_MAX)
        clamped = INT32_MAX;
    c->stats.last_sync_error_us = (int32_t)clamped;
    if (err > AWDL_RESYNC_US || err < -AWDL_RESYNC_US) {
        c->stats.resyncs++;
        awdl_soft_install(c, now);
    }
}

static void learn(struct awdl_peer *p, const struct awdl_af *a, const struct awdl_rx_meta *m)
{
    p->last_seen = m->tsf;
    p->rssi = p->frames ? (int8_t)((3 * p->rssi + m->rssi) / 4) : m->rssi;
    p->frames++;
    if (a->present & awdl_tlv_bit(AWDL_TLV_ELECTION_PARAMS_V2)) {
        p->election = a->election2;
        p->has_v2 = 1;
    } else if (a->present & awdl_tlv_bit(AWDL_TLV_ELECTION_PARAMS)) {  // a v1-only peer: its master is the root
        memset(&p->election, 0, sizeof p->election);
        memcpy(p->election.root, a->election.master, 6);
        memcpy(p->election.parent, a->src, 6);
        p->election.hops = a->election.distance;
        p->election.root_metric = a->election.master_metric;
        p->election.self_metric = a->election.self_metric;
        p->has_v2 = 1;
    }
    if (a->present & awdl_tlv_bit(AWDL_TLV_CHANNEL_SEQUENCE)) {
        p->seq = a->chanseq;
        p->has_seq = 1;
    } else if ((a->present & awdl_tlv_bit(AWDL_TLV_SYNC_PARAMS)) && a->sync.has_seq) {
        p->seq = a->sync.seq;
        p->has_seq = 1;
    }
    if (a->present & awdl_tlv_bit(AWDL_TLV_VERSION)) {
        p->awdl_version = a->awdl_version;
        p->device_class = a->device_class;
    }
    if ((a->present & awdl_tlv_bit(AWDL_TLV_ARPA)) && a->hostname[0])
        memcpy(p->hostname, a->hostname, sizeof p->hostname);
    if (a->present & awdl_tlv_bit(AWDL_TLV_HT_CAPS)) {
        p->has_ht = 1;
        p->ht_caps = a->ht_caps;
        p->ht_ampdu = a->ht_ampdu;
        memcpy(p->ht_mcs, a->ht_mcs, 2);
    }
}

void awdl_handle_af(struct awdl_core *c, const struct awdl_af *a, const struct awdl_rx_meta *m)
{
    if (awdl_addr_eq(a->src, c->cfg.addr) || awdl_is_group(a->src))
        return;
    struct awdl_peer *p = peer_get(c, a->src, m->tsf);
    if (!p)
        return;
    learn(p, a, m);
    int timed = 0;
    if (a->present & awdl_tlv_bit(AWDL_TLV_SYNC_PARAMS)) {
        p->sync = a->sync;
        timed = sender_next_eaw(a, m->tsf, &p->next_aw_tsf, &p->next_aw_seq);
    }
    awdl_run_election(c, m->tsf);
    const int parent = c->role == AWDL_ROLE_FOLLOWER && awdl_addr_eq(p->addr, c->adv.parent);
    if (awdl_native(c)) {
        awdl_native_after_af(c);
    } else if (timed && parent) {
        sync_to(c, p, m->tsf);
    }
    if (parent)
        awdl_sched_learn_social(c, p, m->tsf);
    if (!p->reachable && timed && p->has_seq &&
        (a->subtype == AWDL_SUBTYPE_MIF || (a->present & awdl_tlv_bit(AWDL_TLV_DATA_PATH_STATE)))) {
        p->reachable = 1;
        awdl_log(c, "AWDL: peer %02x:%02x:%02x:%02x:%02x:%02x \"%s\" reachable, %d dBm, version %#x class %u",
                 p->addr[0], p->addr[1], p->addr[2], p->addr[3], p->addr[4], p->addr[5], p->hostname, p->rssi,
                 p->awdl_version, p->device_class);
        if (awdl_native(c))
            awdl_native_peer(c, p, 1);  // data to it from now on
        if (c->host.peer)
            c->host.peer(c->host.ctx, p->addr, 1, p->rssi);
    }
}

// ---- the soft backend's frames --------------------------------------------------------------------------------------
// An away slot opens with a PSF, which on a radio without hops is what takes it there for the rest of the slot, and
// carries our MIF in its middle, where Apple's cluster (Stute et al. §7.4; OWL sends one per EAW there). PSFs also
// go out at Apple's period wherever we are on an AWDL channel.
static void send_af(struct awdl_core *c, uint8_t subtype, uint64_t now)
{
    const unsigned slot = slot_of(awdl_core_aw_seq(c, now));
    const uint16_t freq = awdl_chan_freq(c->cfg.seq[slot]);
    if (!freq)
        return;
    const size_t n = awdl_core_build_af(c, subtype, now, c->scratch, sizeof c->scratch);
    if (!n)
        return;
    const uint64_t stay = away_slot(c, slot) ? slot_end(c, now) : 0;
    if (c->host.tx(c->host.ctx, c->scratch, n, freq, stay, 1)) {
        c->stats.tx_refused++;
        return;
    }
    c->stats.af_tx++;
    if (subtype == AWDL_SUBTYPE_PSF) {
        c->stats.psf_tx++;
    } else {
        c->stats.mif_tx++;
    }
}

static uint64_t psf_period(const struct awdl_core *c)
{
    return c->role == AWDL_ROLE_FOLLOWER ? AWDL_PSF_FOLLOWER_US : AWDL_PSF_US;
}

void awdl_core_set_sequence(struct awdl_core *c, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t now)
{
    memcpy(c->cfg.seq, seq, sizeof c->cfg.seq);
    c->seq_fixed = 1;
    awdl_soft_install(c, now);
}

// ---- entry points -------------------------------------------------------------------------------------------------
int awdl_core_init(struct awdl_core *c, const struct awdl_host *host, const struct awdl_config *cfg)
{
    if (!host || !host->tx)
        return -1;
    memset(c, 0, sizeof *c);
    c->host = *host;
    c->cfg = *cfg;
    c->role = AWDL_ROLE_OFF;
    c->fw_ifidx = -1;
    c->stats.fw_role = -1;
    return 0;
}

void awdl_core_start(struct awdl_core *c, uint64_t now)
{
    if (c->running)
        return;
    c->running = 1;
    if (awdl_native(c)) {
        awdl_native_start(c, now);
        return;
    }
    c->base_tsf = now - now % AWDL_AW_US;
    c->base_seq = 0;
    c->self_counter = 0;
    c->last_counter_tick = now;
    c->role = AWDL_ROLE_ROOT;
    awdl_run_election(c, now);
    awdl_sched_start(c);
    awdl_soft_install(c, now);
    c->next_psf = now + psf_period(c);
    awdl_sched_log(c, "%s", "start");
}

void awdl_core_stop(struct awdl_core *c)
{
    if (!c->running)
        return;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used)
            peer_lost(c, &c->peers[i], "stopped");
    }
    if (awdl_native(c))
        awdl_native_stop(c);
    c->txq_count = 0;
    c->running = 0;
    c->role = AWDL_ROLE_OFF;
    awdl_log(c, "AWDL: stopped");
}

int awdl_core_running(const struct awdl_core *c)
{
    return c->running;
}

enum awdl_role awdl_core_role(const struct awdl_core *c)
{
    return c->role;
}

const struct awdl_election_v2 *awdl_core_election(const struct awdl_core *c)
{
    return &c->adv;
}

const struct awdl_stats *awdl_core_stats(const struct awdl_core *c)
{
    return &c->stats;
}

const struct awdl_chan *awdl_core_sequence(const struct awdl_core *c)
{
    return c->cfg.seq;
}

void awdl_core_set_addr(struct awdl_core *c, const uint8_t addr[6])
{
    memcpy(c->cfg.addr, addr, 6);
    if (c->role == AWDL_ROLE_ROOT) {
        memcpy(c->adv.root, addr, 6);
        memcpy(c->adv.parent, addr, 6);
    }
}

void awdl_core_rx(struct awdl_core *c, const uint8_t *f, size_t len, const struct awdl_rx_meta *m)
{
    if (!c->running || len < 24)
        return;
    if ((f[0] & 0x0c) == 0x08) {
        awdl_data_rx(c, f, len, m);
        return;
    }
    if ((f[0] & 0xfc) != 0xd0)
        return;
    struct awdl_af a;
    if (awdl_af_parse(f, len, &a)) {
        // other action frames are not ours; a truncated AWDL one is counted
        if (len >= 24 + 5 && f[24] == 0x7f && !memcmp(f + 25, awdl_oui, 3))
            c->stats.af_bad++;
        return;
    }
    c->stats.af_rx++;
    awdl_handle_af(c, &a, m);
}

uint64_t awdl_core_timer(struct awdl_core *c, uint64_t now)
{
    if (!c->running)
        return UINT64_MAX;
    if (awdl_native(c))
        return awdl_native_timer(c, now);
    tick_counter(c, now);
    awdl_sched_deadline(c, now);
    if (now >= c->next_slot) {
        awdl_expire_peers(c, now);
        awdl_run_election(c, now);
        rebase(c, now);
        if (away_slot(c, slot_of(awdl_core_aw_seq(c, now)))) {
            send_af(c, AWDL_SUBTYPE_PSF, now);
            c->next_mif = awdl_eaw_start(c, now) + AWDL_EAW_US / 2;
        }
        awdl_data_flush(c, now);
        c->next_slot = next_away_start(c, now) + AWDL_SLOT_GUARD_US;
    }
    if (c->next_mif && now >= c->next_mif) {
        c->next_mif = 0;
        if (away_slot(c, slot_of(awdl_core_aw_seq(c, now))))
            send_af(c, AWDL_SUBTYPE_MIF, now);
    }
    if (now >= c->next_psf) {
        if (awdl_chan_freq(awdl_core_channel_at(c, now))) {
            send_af(c, AWDL_SUBTYPE_PSF, now);
            c->next_psf = now + psf_period(c);
        } else {
            c->next_psf = next_away_start(c, now) + psf_period(c);  // an empty slot: nothing to wake for until then
        }
    }
    uint64_t next = c->next_psf < c->next_slot ? c->next_psf : c->next_slot;
    if (c->next_mif && c->next_mif < next)
        next = c->next_mif;
    const uint64_t mode_end = awdl_sched_next(c);
    return mode_end < next ? mode_end : next;
}

// ---- TSF clock ------------------------------------------------------------------------------------------------------
void awdl_tsf_clock_anchor(struct awdl_tsf_clock *k, uint64_t tsf, uint64_t host_ns)
{
    if (k->valid && host_ns > k->host0_ns + 100000000ULL) {  // rate from anchors at least 100 ms apart
        const int64_t dh = (int64_t)((host_ns - k->host0_ns) / 1000);
        const int64_t dt = (int64_t)(tsf - k->tsf0);
        int64_t ppm16 = (dt - dh) * 16000000 / dh;
        if (ppm16 > (int64_t)16 * 500)
            ppm16 = (int64_t)16 * 500;  // a crystal is within ±500 ppm; more is a TSF jump
        if (ppm16 < (int64_t)-16 * 500)
            ppm16 = (int64_t)-16 * 500;
        k->ppm_x16 = (int32_t)((3 * (int64_t)k->ppm_x16 + ppm16) / 4);
    }
    k->tsf0 = tsf;
    k->host0_ns = host_ns;
    k->valid = 1;
}

uint64_t awdl_tsf_clock_now(const struct awdl_tsf_clock *k, uint64_t host_ns)
{
    if (!k->valid)
        return 0;
    const int64_t us = (int64_t)(host_ns - k->host0_ns) / 1000;
    return (uint64_t)((int64_t)k->tsf0 + us + us * k->ppm_x16 / 16000000);
}

uint64_t awdl_tsf_clock_host(const struct awdl_tsf_clock *k, uint64_t tsf)
{
    if (!k->valid)
        return 0;
    const int64_t us = (int64_t)(tsf - k->tsf0);
    const int64_t host_us = us - us * k->ppm_x16 / 16000000;
    return (uint64_t)((int64_t)k->host0_ns + host_us * 1000);
}
