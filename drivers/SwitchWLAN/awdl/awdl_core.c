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

#if DEVELOPMENT || DEBUG
void awdl_probe(struct awdl_core *c, const char *fmt, ...)
{
    if (!c->host.probe)
        return;
    char line[208];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    c->host.probe(c->host.ctx, line);
}
#endif

uint16_t awdl_core_aw_seq(const struct awdl_core *c, uint64_t now)
{
    if (now >= c->base_tsf)
        return (uint16_t)(c->base_seq + (now - c->base_tsf) / AWDL_AW_US);
    const uint64_t back = c->base_tsf - now;
    return (uint16_t)(c->base_seq - (back + AWDL_AW_US - 1) / AWDL_AW_US);
}

uint64_t awdl_core_aw_start(const struct awdl_core *c, uint16_t seq, uint64_t now)
{
    const uint64_t current = now - (now - c->base_tsf) % AWDL_AW_US;
    const int16_t delta = (int16_t)(uint16_t)(seq - awdl_core_aw_seq(c, now));
    return current + (uint64_t)((int64_t)delta * (int64_t)AWDL_AW_US);
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

static int discovery_slot(const struct awdl_core *c, unsigned slot)
{
    const uint8_t ch = awdl_chan_primary(c->cfg.seq[slot]);
    return awdl_chan_freq(c->cfg.seq[slot]) &&
           (c->seq_fixed || c->switch_channels_limited || ch == 6 || ch == 44 || ch == 149);
}

static uint64_t next_discovery(const struct awdl_core *c, uint64_t now)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    uint16_t first = (uint16_t)((aw / AWDL_EAW_AWS + 1) * AWDL_EAW_AWS);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i, first = (uint16_t)(first + AWDL_EAW_AWS)) {
        if (discovery_slot(c, slot_of(first)))
            return awdl_core_aw_start(c, first, now) + AWDL_SLOT_GUARD_US;
    }
    return UINT64_MAX;
}

static uint64_t slot_end(const struct awdl_core *c, uint64_t now)
{
    return awdl_eaw_start(c, now) + AWDL_EAW_US;
}

void awdl_soft_install(struct awdl_core *c, uint64_t now)
{
    if (!c->running || awdl_native(c))
        return;
    if (c->receiving) {
        c->schedule_pending = 1;
        return;
    }
    c->schedule_pending = 0;
    const uint16_t aw = awdl_core_aw_seq(c, now);
    c->anchor = awdl_core_aw_start(c, (uint16_t)(aw - aw % (AWDL_EAW_AWS * AWDL_SEQ_SLOTS)), now);
    if (c->host.schedule)
        c->host.schedule(c->host.ctx, c->cfg.seq, c->anchor);
    c->next_slot = next_discovery(c, now);
    const uint64_t mid = awdl_eaw_start(c, now) + AWDL_EAW_US / 2;
    c->next_mif = discovery_slot(c, slot_of(aw)) && mid >= now ? mid : 0;
    c->next_data = c->txq_count ? now : UINT64_MAX;
    if (c->next_slot == UINT64_MAX)
        c->next_psf = UINT64_MAX;
    else if (c->next_psf == UINT64_MAX)
        c->next_psf = now;
}

// ---- election -----------------------------------------------------------------------------------------------------
// One way to a root: through ourselves (dist 0) or through a peer (`via`, at its own distance from its root).
struct candidate {
    const uint8_t *root, *via;
    uint32_t metric, counter, dist;
};

// IO80211Family's runParentElection: between two roots the higher metric, then the higher address; towards one root
// the higher metric, then a counter more than 2 ahead, then, on an equal counter, the shorter way.
static int better(const struct candidate *a, const struct candidate *b)
{
    if (!awdl_addr_eq(a->root, b->root))
        return a->metric != b->metric ? a->metric > b->metric : memcmp(a->root, b->root, 6) > 0;
    if (a->metric > b->metric)
        return 1;
    if (a->counter != b->counter)
        return a->counter > b->counter && a->counter - b->counter > 2;
    return a->dist < b->dist;
}

static int electable(const struct awdl_core *c, const struct awdl_peer *p, uint64_t now)
{
    if (!p || !p->used || !p->has_v2 || (now >= p->last_seen && now - p->last_seen > AWDL_PEER_TIMEOUT_US))
        return 0;
    const struct awdl_election_v2 *e = &p->election;
    return !awdl_addr_eq(e->root, c->cfg.addr) && !awdl_addr_eq(e->parent, c->cfg.addr) &&
           e->hops + 1 <= AWDL_ELECTION_MAX_HOPS;
}

static struct candidate through(const struct awdl_peer *p)
{
    return (struct candidate){p->election.root, p->addr, p->election.root_metric, p->election.root_counter,
                              p->election.hops};
}

// The parent we have is weighed first, so a candidate it equals or also outranks never displaces it; when no peer is
// loud enough to be a candidate it is kept until it falls silent (macOS does the same).
void awdl_run_election(struct awdl_core *c, uint64_t now)
{
    const uint8_t *self = c->cfg.addr;
    const struct candidate me = {self, self, c->cfg.self_metric, c->self_counter, 0};
    const struct awdl_peer *current = c->role == AWDL_ROLE_FOLLOWER ? awdl_core_find(c, c->adv.parent) : NULL;
    if (!electable(c, current, now))
        current = NULL;
    const struct awdl_peer *parent = current && current->rssi >= AWDL_ELECTION_RSSI_DBM ? current : NULL;
    struct candidate best = parent ? through(parent) : me;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        const struct awdl_peer *p = &c->peers[i];
        if (p == current || p->rssi < AWDL_ELECTION_RSSI_DBM || !electable(c, p, now))
            continue;
        const struct candidate k = through(p);
        if (!parent || (better(&k, &best) && !better(&best, &k))) {
            best = k;
            parent = p;
        }
    }
    if (parent && !better(&best, &me))
        parent = NULL;
    else if (!parent)
        parent = current;
    best = parent ? through(parent) : me;
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

// The counter of a root grows while it stays one; followers read it to tell a fresh way to that root from a stale one,
// so only its rate matters, and it is caught up here rather than given a wake-up of its own.
static void tick_counter(struct awdl_core *c, uint64_t now)
{
    if (now < c->last_counter_tick)
        return;
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

static void peer_clear_data(struct awdl_core *c, const struct awdl_peer *p)
{
    const unsigned idx = (unsigned)(p - c->peers);
    c->switch_data_burst[idx] = 0;
    c->switch_data_window[idx] = 0;
    c->switch_data_until[idx] = 0;
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
    peer_clear_data(c, p);
    p->used = 1;
    memcpy(p->addr, addr, 6);
    p->first_seen = now;
    return p;
}

static void peer_lost(struct awdl_core *c, struct awdl_peer *p, const char *why)
{
    uint8_t addr[6];
    memcpy(addr, p->addr, sizeof addr);
    const int reachable = p->reachable;
    const int8_t rssi = p->rssi;
    p->used = p->reachable = 0;
    if (c->txq_count) {
        awdl_data_forget(c, addr);
        c->next_data = c->last_observation;
    }
    if (reachable && awdl_native(c))
        awdl_native_peer(c, p, 0);
    if (reachable && c->host.peer)
        c->host.peer(c->host.ctx, addr, 0, rssi);
    awdl_log(c, "AWDL: peer %02x:%02x:%02x:%02x:%02x:%02x lost (%s)", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5], why);
    peer_clear_data(c, p);
    memset(p, 0, sizeof *p);
}

void awdl_expire_peers(struct awdl_core *c, uint64_t now)
{
    int lost = 0;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used && now >= c->peers[i].last_seen && now - c->peers[i].last_seen > AWDL_PEER_TIMEOUT_US) {
            peer_lost(c, &c->peers[i], "timeout");
            lost = 1;
        }
    }
    if (lost || c->switch_timed_peer)
        switch_awdl_sched_peer_timing(c, NULL, now);
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
    const uint64_t countdown = (uint64_t)a->sync.tx_counter * AWDL_TU_US;
    if (rx_tsf > UINT64_MAX - countdown || rx_tsf + countdown < (uint64_t)delay)
        return 0;
    *tsf = rx_tsf + countdown - (uint64_t)delay;
    *seq = (uint16_t)((a->sync.aw_seq & ~(AWDL_EAW_AWS - 1u)) + AWDL_EAW_AWS);
    return 1;
}

// Our clock follows every measurement of the parent's; the radio's installed sequence follows it on the first
// synchronisation or when the two have drifted apart by more than AWDL_RESYNC_US (AWDL_SEQ_US is a power of two).
static void sync_to(struct awdl_core *c, const struct awdl_peer *p, uint64_t now)
{
    c->base_tsf = p->next_aw_tsf;
    c->base_seq = p->next_aw_seq;
    if (c->schedule_pending) {  // the install still to come takes this clock
        memcpy(c->synced_to, p->addr, 6);
        return;
    }
    const uint16_t aw = awdl_core_aw_seq(c, now);
    const uint64_t start = awdl_core_aw_start(c, (uint16_t)(aw - aw % (AWDL_EAW_AWS * AWDL_SEQ_SLOTS)), now);
    int64_t err = (int64_t)((start - c->anchor) % AWDL_SEQ_US);
    if (err > (int64_t)(AWDL_SEQ_US / 2))
        err -= (int64_t)AWDL_SEQ_US;
    c->stats.last_sync_error_us = (int32_t)err;
    static const uint8_t unsynced[6] = {0};
    const int first = awdl_addr_eq(c->synced_to, unsynced);
    memcpy(c->synced_to, p->addr, 6);
    if (first || err > AWDL_RESYNC_US || err < -AWDL_RESYNC_US) {
        c->stats.resyncs++;
        awdl_soft_install(c, now);
    }
}

static void learn(struct awdl_peer *p, const struct awdl_af *a, const struct awdl_rx_meta *m)
{
    if (m->tsf > p->last_seen)
        p->last_seen = m->tsf;
    p->last_action = m->tsf;
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
    if (c->last_observation > m->tsf && c->last_observation - m->tsf > AWDL_PEER_TIMEOUT_US)
        return;
    struct awdl_peer *p = peer_get(c, a->src, m->tsf);
    if (!p || (p->frames && m->tsf < p->last_action))
        return;
    learn(p, a, m);
    int timed = 0;
    if (a->present & awdl_tlv_bit(AWDL_TLV_SYNC_PARAMS)) {
        p->sync = a->sync;
        timed = sender_next_eaw(a, m->tsf, &p->next_aw_tsf, &p->next_aw_seq);
    }
    const uint64_t now = c->last_observation > m->tsf ? c->last_observation : m->tsf;
    awdl_run_election(c, now);
    const int parent = c->role == AWDL_ROLE_FOLLOWER && awdl_addr_eq(p->addr, c->adv.parent);
    if (awdl_native(c)) {
        awdl_native_after_af(c);
        if (!c->running)
            return;
    } else if (timed && parent) {
        sync_to(c, p, now);
    }
    if (timed && c->txq_count)
        c->next_data = now;
    if (!p->reachable && timed && p->has_seq &&
        (a->subtype == AWDL_SUBTYPE_MIF || (a->present & awdl_tlv_bit(AWDL_TLV_DATA_PATH_STATE)))) {
        p->reachable = 1;
        awdl_log(c, "AWDL: peer %02x:%02x:%02x:%02x:%02x:%02x \"%s\" reachable, %d dBm, version %#x class %u",
                 p->addr[0], p->addr[1], p->addr[2], p->addr[3], p->addr[4], p->addr[5], p->hostname, p->rssi,
                 p->awdl_version, p->device_class);
        if (awdl_native(c))
            awdl_native_peer(c, p, 1);  // data to it from now on
        if (!c->running)
            return;
        if (c->host.peer)
            c->host.peer(c->host.ctx, p->addr, 1, p->rssi);
    }
    switch_awdl_sched_peer_timing(c, p, now);
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
    const uint64_t stay = slot_end(c, now);
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

uint16_t awdl_psf_period_tu(enum awdl_role role)
{
    return role == AWDL_ROLE_FOLLOWER ? AWDL_PSF_FOLLOWER_TU : AWDL_PSF_INTERVAL_TU;
}

// ---- entry points -------------------------------------------------------------------------------------------------
int awdl_core_init(struct awdl_core *c, const struct awdl_host *host, const struct awdl_config *cfg)
{
    if (!host || !host->tx)
        return -1;
    memset(c, 0, sizeof *c);
    c->radio_ready = 1;
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
    c->switch_generation++;
    c->running = 1;
    if (awdl_native(c)) {
        awdl_native_start(c, now);
        return;
    }
    c->base_tsf = now - now % AWDL_AW_US;
    c->base_seq = 0;
    c->self_counter = 0;
    memset(c->synced_to, 0, sizeof c->synced_to);
    c->last_counter_tick = now;
    c->last_observation = now;
    c->schedule_pending = 0;
    c->waiting_credit = 0;
    c->role = AWDL_ROLE_ROOT;
    awdl_run_election(c, now);
    awdl_sched_start(c);
    c->next_psf = now;
    awdl_soft_install(c, now);
    awdl_sched_log(c, "%s", "start");
}

#if defined(SWITCH_IPSW_KIND_24B119)
void switch_awdl_core_service_changed(struct awdl_core *c, uint64_t now)
{
    if (!c || !c->running || awdl_native(c))
        return;
    if (!now)
        now = c->last_observation;
    if (!c->next_mif && discovery_slot(c, slot_of(awdl_core_aw_seq(c, now)))) {
        const uint64_t due = now + 4u * AWDL_TU_US;
        if (due + AWDL_SLOT_GUARD_US < slot_end(c, now))
            c->next_mif = due;
    }
}
#endif

void awdl_core_stop(struct awdl_core *c)
{
    if (!c->running)
        return;
    c->switch_generation++;
    c->running = 0;
    c->switch_data_active = 0;
    memset(c->switch_data_burst, 0, sizeof c->switch_data_burst);
    memset(c->switch_data_window, 0, sizeof c->switch_data_window);
    memset(c->switch_data_until, 0, sizeof c->switch_data_until);
    c->switch_discovery_active = 0;
    c->switch_discovery_until = 0;
#if DEVELOPMENT || DEBUG
    c->switch_home_trial_channel = 0;
#endif
    c->txq_count = 0;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used)
            peer_lost(c, &c->peers[i], "stopped");
    }
    if (awdl_native(c))
        awdl_native_stop(c);
    c->running = 0;
    c->role = AWDL_ROLE_OFF;
    awdl_log(c, "AWDL: stopped");
}

int awdl_core_running(const struct awdl_core *c)
{
    return c->running;
}

int switch_awdl_core_ready(const struct awdl_core *c)
{
    return c->running && c->radio_ready && (!awdl_native(c) || c->nstate == AWDL_N_UP);
}

void switch_awdl_core_radio_ready(struct awdl_core *c, int ready)
{
    if (ready && !c->radio_ready && c->txq_count)
        c->next_data = c->last_observation;
    c->radio_ready = (uint8_t)(ready != 0);
}

void switch_awdl_core_tx_ready(struct awdl_core *c, uint64_t now)
{
    if (switch_awdl_core_ready(c) && !awdl_native(c)) {
        if (c->schedule_pending)
            awdl_soft_install(c, now);
        if (c->waiting_credit || now >= c->next_data)
            awdl_data_flush(c, now);
    }
}

uint32_t switch_awdl_core_generation(const struct awdl_core *c)
{
    return c->switch_generation;
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
    if (awdl_addr_eq(c->cfg.addr, addr))
        return;
    ++c->switch_generation;
    c->stats.data_drop += c->txq_count;
    c->txq_head = c->txq_count = 0;
    c->next_data = UINT64_MAX;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used)
            peer_lost(c, &c->peers[i], "address changed");
    }
    switch_awdl_sched_peer_timing(c, NULL, c->last_observation);
    memcpy(c->cfg.addr, addr, 6);
    if (c->running)
        awdl_run_election(c, c->last_observation);
}

int awdl_core_rx(struct awdl_core *c, const uint8_t *f, size_t len, const struct awdl_rx_meta *m)
{
    if (!c->running || !f || !m || len < 24)
        return 0;
    if (m->tsf > c->last_observation)
        c->last_observation = m->tsf;
    if ((f[0] & 0x0c) == 0x08) {
        c->receiving = 1;
        awdl_data_rx(c, f, len, m);
        c->receiving = 0;
        if (c->schedule_pending && c->radio_ready)
            awdl_soft_install(c, c->last_observation);
        return 0;
    }
    if ((f[0] & 0xfc) != 0xd0)
        return 0;
    struct awdl_af a;
    if (awdl_af_parse(f, len, &a)) {
        // other action frames are not ours; a truncated AWDL one is counted
        if (len >= 24 + 5 && f[24] == 0x7f && !memcmp(f + 25, awdl_oui, 3))
            c->stats.af_bad++;
        return 0;
    }
    c->stats.af_rx++;
    c->receiving = 1;
    awdl_handle_af(c, &a, m);
    c->receiving = 0;
    if (c->schedule_pending && c->radio_ready)
        awdl_soft_install(c, c->last_observation);
    const uint32_t services = awdl_tlv_bit(AWDL_TLV_SERVICE_REQUEST) | awdl_tlv_bit(AWDL_TLV_SERVICE_RESPONSE);
    return (a.present & services) && !awdl_is_group(a.src) && !awdl_addr_eq(a.src, c->cfg.addr) &&
           (awdl_is_group(a.dst) || awdl_addr_eq(a.dst, c->cfg.addr));
}

uint64_t awdl_core_timer(struct awdl_core *c, uint64_t now)
{
    if (!c->running || !c->radio_ready)
        return UINT64_MAX;
    if (awdl_native(c))
        return awdl_native_timer(c, now);
    if (now > c->last_observation)
        c->last_observation = now;
    tick_counter(c, now);
    awdl_sched_deadline(c, now);
    if (c->schedule_pending && c->radio_ready)
        awdl_soft_install(c, now);
    if (!c->radio_ready)
        return UINT64_MAX;
    if (now >= c->next_slot) {
        awdl_expire_peers(c, now);
        awdl_run_election(c, now);
        const uint64_t mid = awdl_eaw_start(c, now) + AWDL_EAW_US / 2;
        if (discovery_slot(c, slot_of(awdl_core_aw_seq(c, now))) && now <= mid) {
            if (now < c->next_psf && awdl_chan_freq(awdl_core_channel_at(c, now)) != c->infra_mhz)
                send_af(c, AWDL_SUBTYPE_PSF, now);
            c->next_mif = mid;
        }
        c->next_slot = next_discovery(c, now);
    }
    if (c->next_mif && now >= c->next_mif) {
        if (now - c->next_mif < AWDL_EAW_US / 2 - AWDL_SLOT_GUARD_US &&
            discovery_slot(c, slot_of(awdl_core_aw_seq(c, now))))
            send_af(c, AWDL_SUBTYPE_MIF, now);
        c->next_mif = 0;
    }
    if (now >= c->next_psf) {
        if (awdl_chan_freq(awdl_core_channel_at(c, now))) {
            send_af(c, AWDL_SUBTYPE_PSF, now);
            c->next_psf = now + awdl_psf_period_tu(c->role) * AWDL_TU_US;
        } else {
            c->next_psf = c->next_slot;
        }
    }
    if (now >= c->next_data)
        awdl_data_flush(c, now);
    if (!c->radio_ready)
        return UINT64_MAX;
    uint64_t next = c->next_psf < c->next_slot ? c->next_psf : c->next_slot;
    if (c->next_data < next)
        next = c->next_data;
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
