// SPDX-License-Identifier: BSD-2-Clause-Patent
// The soft backend's data path: awdl0's Ethernet frames out as AWDL data frames when the peer listens on our channel
// (now, or queued for its next slot), and AWDL data frames in, A-MSDUs included, as Ethernet frames.
#include "awdl_internal.h"

enum {
    SWITCH_AWDL_UNICAST_GUARD_US = 3u * AWDL_TU_US,
    SWITCH_AWDL_MULTICAST_GUARD_US = 16u * AWDL_TU_US,
    SWITCH_AWDL_MIN_TX_US = 2u * AWDL_TU_US,
    SWITCH_AWDL_LARGE_FRAME_BYTES = 1024u
};
// The radio reports a handed frame at the latest 1.1 s after taking it (SwitchWLANA3Radio.cpp's transport deadline).
#define SWITCH_AWDL_REPORT_US 1200000u

static int switch_awdl_data_immediate_payload(uint16_t ethertype, const uint8_t *payload, size_t len)
{
    if (len >= SWITCH_AWDL_LARGE_FRAME_BYTES)
        return 1;
    return ethertype == 0x86dd && len >= 60 && (payload[0] >> 4) == 6 && payload[6] == 6 &&
           (payload[40 + 13] & 0x02) != 0;
}

struct channel_window {
    uint64_t open, close, boundary;
    uint8_t channel;
};

static struct channel_window channel_window(const struct awdl_chan *seq, unsigned count, uint64_t phase, uint64_t now,
                                            unsigned guard, int bonjour)
{
    const unsigned slot = (unsigned)((phase / AWDL_EAW_US) % AWDL_SEQ_SLOTS);
    const uint64_t elapsed = phase % AWDL_EAW_US;
    const uint8_t current = awdl_chan_primary(seq[slot % count]);
    const uint64_t boundary = now + AWDL_EAW_US - elapsed;
    struct channel_window w = {now, boundary, boundary, current};
    if (bonjour && slot != 0 && slot != 10)
        w.channel = 0;
    if (elapsed < guard &&
        (bonjour || current != awdl_chan_primary(seq[((slot + AWDL_SEQ_SLOTS - 1) % AWDL_SEQ_SLOTS) % count])))
        w.open += guard - elapsed;
    if (bonjour || current != awdl_chan_primary(seq[((slot + 1) % AWDL_SEQ_SLOTS) % count]))
        w.close = boundary - now > guard ? boundary - guard : now;
    return w;
}

// Both phases are expressed in local radio TSF. Their slot boundaries may fall at different times.
static uint64_t transmit_window(struct awdl_core *c, const uint8_t *dst, const struct awdl_peer *p, uint64_t now,
                                uint64_t earliest)
{
    const uint8_t mdns6[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
    const uint8_t mdns4[6] = {1, 0, 0x5e, 0, 0, 0xfb};
    const int bonjour = !p && (awdl_addr_eq(dst, mdns6) || awdl_addr_eq(dst, mdns4));
    const unsigned guard =
        bonjour ? SWITCH_AWDL_MULTICAST_GUARD_US : (p ? SWITCH_AWDL_UNICAST_GUARD_US : AWDL_SLOT_GUARD_US);
    const uint64_t start = earliest > now ? earliest : now;
    for (uint64_t at = start; at - start < AWDL_SEQ_US;) {
        const uint64_t phase = at - c->base_tsf + (uint64_t)c->base_seq * AWDL_AW_US;
        struct channel_window w = channel_window(c->cfg.seq, AWDL_SEQ_SLOTS, phase, at, guard, bonjour);
        if (p) {
            const uint64_t peer_phase = at - p->next_aw_tsf + (uint64_t)p->next_aw_seq * AWDL_AW_US;
            const struct channel_window other = channel_window(p->seq.chan, p->seq.count, peer_phase, at, guard, 0);
            if (other.channel != w.channel)
                w.channel = 0;
            if (other.open > w.open)
                w.open = other.open;
            if (other.close < w.close)
                w.close = other.close;
            if (other.boundary < w.boundary)
                w.boundary = other.boundary;
        }
        if (w.channel && w.open < w.close && w.close - w.open >= SWITCH_AWDL_MIN_TX_US) {
            if (w.open == now)
                return w.close;
            if (w.open < c->next_data)
                c->next_data = w.open;
            break;
        }
        at = w.boundary;
    }
    return 0;
}

static int send(struct awdl_core *c, const uint8_t *frame, size_t len, int group, uint64_t now, uint64_t stay)
{
    const uint16_t freq = awdl_chan_freq(awdl_core_channel_at(c, now));
    const int result = c->host.tx(c->host.ctx, frame, len, freq, stay, group);
    if (result) {
        c->stats.tx_refused++;
        return result;
    }
    c->stats.data_tx++;
    return 0;
}

#if DEVELOPMENT || DEBUG
static const char *const outcome_names[] = {"acked", "unacked", "expired", "cancelled", "failed"};

static struct switch_awdl_data_probe *probe_of(struct awdl_core *c, const struct awdl_peer *p)
{
    if (!p)
        return NULL;
    struct switch_awdl_data_probe *probe = &c->switch_probe[p - c->peers];
    if (!awdl_addr_eq(probe->addr, p->addr)) {
        memset(probe, 0, sizeof *probe);
        memcpy(probe->addr, p->addr, 6);
    }
    return probe;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int tracing(struct awdl_core *c)
{
    if (c->switch_tcp_generation != c->switch_generation) {
        c->switch_tcp_generation = c->switch_generation;
        c->switch_tcp_lines = 0;
    }
    return c->switch_tcp_lines < 800;
}

static int traced_tcp(const struct awdl_msdu *m)
{
    return m->ethertype == 0x86dd && m->len >= 60 && m->payload[6] == 6;
}

// Transmit lines print retry -1: the radio's own retransmissions are not reported per frame.
static void trace_tcp(struct awdl_core *c, const char *direction, const struct awdl_msdu *m, uint16_t freq,
                      uint64_t tsf, int retry, uint16_t awdl_seq, const char *outcome)
{
    if (!traced_tcp(m))
        return;
    const uint8_t *tcp = m->payload + 40;
    const unsigned header = (tcp[12] >> 4) * 4u;
    const unsigned length = (unsigned)m->payload[4] << 8 | m->payload[5];
    if (header < 20 || length < header)
        return;
    ++c->switch_tcp_lines;
    awdl_probe(c, "probe tcp %s %u>%u seq %u ack %u flags %#x len %u ch %u tsf %llu retry %d awdl-seq %u%s%s",
               direction, (unsigned)tcp[0] << 8 | tcp[1], (unsigned)tcp[2] << 8 | tcp[3], be32(tcp + 4), be32(tcp + 8),
               tcp[13], length - header, awdl_chan_of_freq(freq).number, (unsigned long long)tsf, retry, awdl_seq,
               outcome ? " " : "", outcome ? outcome : "");
}

static void trace_sent(struct awdl_core *c, const struct awdl_txq_entry *e, uint64_t tsf, const char *outcome)
{
    struct awdl_msdu m;
    uint16_t seq = 0;
    if (tracing(c) && awdl_data_msdus(e->frame, e->len, &m, 1, &seq, NULL) == 1)
        trace_tcp(c, "out", &m, e->freq, tsf, -1, seq, outcome);
}

int switch_awdl_core_data_probe(struct awdl_core *c, unsigned index, struct switch_awdl_data_probe *out)
{
    const struct awdl_peer *p = index < AWDL_MAX_PEERS ? &c->peers[index] : NULL;
    if (!p || !p->used || !awdl_addr_eq(c->switch_probe[index].addr, p->addr))
        return 0;
    *out = c->switch_probe[index];
    memset(&c->switch_probe[index], 0, sizeof c->switch_probe[index]);
    memcpy(c->switch_probe[index].addr, p->addr, 6);
    uint32_t traffic = out->rx_retry + out->rx_dup + out->rx_delivered;
    for (unsigned i = 0; i < 4; ++i)
        traffic += out->rx_frames[i];
    return traffic + out->tx_queued + out->tx_handed + out->tx_acked + out->tx_unacked + out->tx_expired +
               out->tx_cancelled + out->tx_failed != 0;
}
#endif

int switch_awdl_data_token(const uint8_t *frame, size_t len, uint16_t *token)
{
    if (len < 40 || frame[0] != 0x08 || (frame[4] & 1) || frame[32] != 0x03 || frame[33] != 0x04)
        return 0;
    *token = (uint16_t)(frame[34] | frame[35] << 8);
    return 1;
}

void switch_awdl_core_tx_done(struct awdl_core *c, uint16_t token, enum switch_awdl_tx_outcome outcome, uint64_t tsf,
                              uint64_t now)
{
    for (unsigned i = 0; i < c->txq_count; ++i) {
        struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        uint16_t named = 0;
        if (!e->len || !e->in_flight || !switch_awdl_data_token(e->frame, e->len, &named) || named != token)
            continue;
        e->in_flight = 0;
        if (outcome == SWITCH_AWDL_TX_EXPIRED && e->closes > e->retry_after)
            e->retry_after = e->closes;
        const uint64_t ready_at = outcome == SWITCH_AWDL_TX_EXPIRED && e->retry_after > now ? e->retry_after : now;
        if (ready_at < c->next_data)
            c->next_data = ready_at;
        const int unsent = outcome == SWITCH_AWDL_TX_EXPIRED || outcome == SWITCH_AWDL_TX_CANCELLED;
#if DEVELOPMENT || DEBUG
        struct switch_awdl_data_probe *probe = probe_of(c, awdl_core_find(c, e->dst));
        if (probe) {
            uint32_t *const counts[] = {&probe->tx_acked, &probe->tx_unacked, &probe->tx_expired, &probe->tx_cancelled,
                                        &probe->tx_failed};
            ++*counts[outcome];
            if (now >= e->handed_at && now - e->handed_at > probe->max_done_us)
                probe->max_done_us = now - e->handed_at;
            probe->tx_requeued += unsent && now < e->expires;
        }
        trace_sent(c, e, tsf, outcome_names[outcome]);
        if (now)
            e->waiting_since = now;
#else
        (void)tsf;
#endif
        if (unsent && now < e->expires)
            return;
        c->stats.data_drop += unsent;
        e->len = 0;
        while (c->txq_count && !c->txq[c->txq_head].len) {
            c->txq_head = (c->txq_head + 1) % AWDL_TXQ_FRAMES;
            --c->txq_count;
        }
        return;
    }
}

static int earlier_blocks_peer(const struct awdl_core *c, unsigned before, const uint8_t *dst, uint64_t now)
{
    for (unsigned i = 0; i < before; ++i) {
        const struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        if (!e->len || !awdl_addr_eq(e->dst, dst))
            continue;
        if (!e->in_flight || (e->closes <= now && now <= e->closes + SWITCH_AWDL_REPORT_US))
            return 1;
    }
    return 0;
}

static int all_in_flight(const struct awdl_core *c, const uint8_t *dst, uint64_t now)
{
    for (unsigned i = 0; i < c->txq_count; ++i) {
        const struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        if (e->len && !e->in_flight)
            return 0;
    }
    return !earlier_blocks_peer(c, c->txq_count, dst, now);
}

static void handed(struct awdl_core *c, struct awdl_txq_entry *e, const struct awdl_peer *p, uint64_t now,
                   uint64_t stay)
{
    e->in_flight = 1;
    e->closes = stay;
#if DEVELOPMENT || DEBUG
    struct switch_awdl_data_probe *probe = probe_of(c, p);
    if (probe) {
        ++probe->tx_handed;
        if (now >= e->waiting_since && now - e->waiting_since > probe->max_wait_us)
            probe->max_wait_us = now - e->waiting_since;
    }
    e->handed_at = now;
    e->freq = awdl_chan_freq(awdl_core_channel_at(c, now));
#else
    (void)c;
    (void)p;
    (void)now;
#endif
}

void awdl_data_forget(struct awdl_core *c, const uint8_t peer[6])
{
    unsigned kept = 0;
    for (unsigned i = 0; i < c->txq_count; ++i) {
        struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        if (!e->len || awdl_addr_eq(e->dst, peer)) {
            c->stats.data_drop += e->len != 0;
            continue;
        }
        if (kept != i)
            c->txq[(c->txq_head + kept) % AWDL_TXQ_FRAMES] = *e;
        ++kept;
    }
    c->txq_count = kept;
}

void awdl_data_flush(struct awdl_core *c, uint64_t now)
{
    if (!switch_awdl_core_ready(c))
        return;
    c->waiting_credit = 0;
    c->next_data = UINT64_MAX;
    unsigned kept = 0;
    int blocked = 0;
    const unsigned n = c->txq_count;
    for (unsigned i = 0; i < n; ++i) {
        struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        if (!e->len || (e->in_flight && now > e->closes + SWITCH_AWDL_REPORT_US)) {
            c->stats.data_drop += e->len != 0;
            continue;
        }
        const struct awdl_peer *p = e->group ? NULL : awdl_core_find(c, e->dst);
        if (now >= e->expires || (!e->group && (!p || !p->reachable || !p->seq.count))) {
#if DEVELOPMENT || DEBUG
            if (!e->in_flight)
                trace_sent(c, e, 0, "dropped");
#endif
            c->stats.data_drop++;
            continue;
        }
        const int eligible = !e->in_flight && !blocked && (e->group || !earlier_blocks_peer(c, kept, e->dst, now));
        const uint64_t stay = eligible ? transmit_window(c, e->dst, p, now, e->retry_after) : 0;
        if (stay) {
            if (!switch_awdl_core_ready(c)) {
                blocked = 1;
            } else {
                const int result = send(c, e->frame, e->len, e->group, now, stay);
                if (!result && e->group)
                    continue;
                if (!result) {
                    handed(c, e, p, now, stay);
                } else if (result == AWDL_TX_LATE) {
                    e->retry_after = stay;
                    transmit_window(c, e->dst, p, now, stay);
                } else if (result != AWDL_TX_BUSY) {
                    c->stats.data_drop++;
                    continue;
                } else {
                    c->waiting_credit = 1;
                    blocked = 1;
                }
            }
        }
        struct awdl_txq_entry *k = &c->txq[(c->txq_head + kept) % AWDL_TXQ_FRAMES];
        if (k != e)
            *k = *e;
        if (k->expires < c->next_data)
            c->next_data = k->expires;
        if (k->in_flight && k->closes + SWITCH_AWDL_REPORT_US < c->next_data)
            c->next_data = k->closes + SWITCH_AWDL_REPORT_US + 1;
        kept++;
    }
    c->txq_count = kept;
    if (!kept)
        c->next_data = UINT64_MAX;
}

int awdl_core_output(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now)
{
    if (!c->running || len < 14 || len > AWDL_MAX_FRAME - 26) {
        c->stats.data_drop++;
        return -1;
    }
    const int group = awdl_is_group(eth);
    const struct awdl_peer *p = group ? NULL : awdl_core_find(c, eth);
    if (!group && (!p || !p->reachable)) {
        c->stats.data_drop++;  // no such peer: no route (the kernel only knows peers we announced)
        return -2;
    }
    if (awdl_native(c))
        return awdl_native_output(c, eth, len, now);
    if (p && !p->seq.count)
        return -2;
    if (c->txq_count == AWDL_TXQ_FRAMES) {
        c->stats.data_drop++;
        return -4;
    }
    struct awdl_txq_entry *e = &c->txq[(c->txq_head + c->txq_count) % AWDL_TXQ_FRAMES];
    const size_t n = awdl_data_encap(eth, len, c->cfg.addr, c->data_seq, e->frame, sizeof e->frame);
    if (!n)
        return -5;
    e->len = (uint16_t)n;
    e->expires = (now > c->last_observation ? now : c->last_observation) + AWDL_TXQ_LIFETIME_US;
    e->retry_after = 0;
    e->group = (uint8_t)group;
    e->in_flight = 0;
    memcpy(e->dst, eth, 6);
#if DEVELOPMENT || DEBUG
    e->waiting_since = now;
    struct switch_awdl_data_probe *probe = probe_of(c, p);
    if (probe)
        ++probe->tx_queued;
#endif
    if (!group && p) {
        const uint16_t ethertype = (uint16_t)((uint16_t)eth[12] << 8 | eth[13]);
        const int immediate = switch_awdl_data_immediate_payload(ethertype, eth + 14, len - 14);
        switch_awdl_sched_data_activity(c, p, now, immediate);
    }
    const uint64_t stay = switch_awdl_core_ready(c) ? transmit_window(c, eth, p, now, 0) : 0;
    if (stay && all_in_flight(c, eth, now)) {
        const int result = send(c, e->frame, e->len, group, now, stay);
        if (!result) {
            c->data_seq++;
            if (group)
                return 0;
            handed(c, e, p, now, stay);
            c->txq_count++;
            if (e->expires < c->next_data)
                c->next_data = e->expires;
            return 0;
        }
        if (result != AWDL_TX_BUSY && result != AWDL_TX_LATE) {
            c->stats.data_drop++;
            return result;
        }
        if (result == AWDL_TX_BUSY) {
            c->waiting_credit = 1;
        } else {
            e->retry_after = stay;
            transmit_window(c, eth, p, now, stay);
        }
    } else if (stay && !c->waiting_credit) {
        c->next_data = now;
    }
    c->data_seq++;
    c->txq_count++;
    if (e->expires < c->next_data)
        c->next_data = e->expires;
    return 0;
}

void awdl_data_rx(struct awdl_core *c, const uint8_t *f, size_t len, const struct awdl_rx_meta *m)
{
    struct awdl_msdu msdus[AWDL_MAX_MSDUS];
    uint16_t seq = 0;
    int retry = 0;
    const size_t n = awdl_data_msdus(f, len, msdus, AWDL_MAX_MSDUS, &seq, &retry);
    if (!n) {
        c->stats.data_bad++;
#if DEVELOPMENT || DEBUG
        if (c->switch_bad_data_generation != c->switch_generation) {
            c->switch_bad_data_generation = c->switch_generation;
            c->switch_bad_data_lines = 0;
        }
        if (c->switch_bad_data_lines < 16) {
            c->switch_bad_data_lines++;
            char hex[145];
            size_t at = 0;
            const size_t show = len < 48 ? len : 48;
            static const char digits[] = "0123456789abcdef";
            for (size_t i = 0; i < show && at + 3 < sizeof hex; ++i) {
                if (i)
                    hex[at++] = ' ';
                hex[at++] = digits[f[i] >> 4];
                hex[at++] = digits[f[i] & 0xf];
            }
            hex[at] = 0;
            const uint16_t fc = len >= 2 ? (uint16_t)(f[0] | ((uint16_t)f[1] << 8)) : 0;
            awdl_probe(c, "AWDL: bad data frame len %zu fc %#x freq %u: %s", len, fc, m->freq_mhz, hex);
        }
#endif
        return;
    }
    if (!awdl_is_group(f + 4) && !awdl_addr_eq(f + 4, c->cfg.addr))
        return;  // someone else's
    struct awdl_peer *p = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS && !p; ++i) {
        if (c->peers[i].used && awdl_addr_eq(c->peers[i].addr, f + 10))
            p = &c->peers[i];
    }
#if DEVELOPMENT || DEBUG
    struct switch_awdl_data_probe *probe = awdl_is_group(f + 4) ? NULL : probe_of(c, p);
    if (probe) {
        const uint8_t channel = awdl_chan_of_freq(m->freq_mhz).number;
        ++probe->rx_frames[channel == 6 ? 0 : channel == 44 ? 1 : channel == 149 ? 2 : 3];
        probe->rx_retry += retry != 0;
    }
#endif
    if (p) {
        if (m->tsf > p->last_seen)
            p->last_seen = m->tsf;
        if (retry && p->has_data_seq && p->last_data_seq == seq) {
#if DEVELOPMENT || DEBUG
            if (probe)
                ++probe->rx_dup;
            for (size_t i = 0; i < n && tracing(c); ++i)
                trace_tcp(c, "in", &msdus[i], m->freq_mhz, m->tsf, retry, seq, "dup");
#endif
            return;  // a retransmission we already took
        }
        p->last_data_seq = seq;
        p->has_data_seq = 1;
        if (!awdl_is_group(f + 4)) {
            int immediate = 0;
            for (size_t i = 0; i < n && !immediate; ++i)
                immediate = switch_awdl_data_immediate_payload(msdus[i].ethertype, msdus[i].payload, msdus[i].len);
            switch_awdl_sched_data_activity(c, p, m->tsf, immediate);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t l = awdl_msdu_ether(&msdus[i], c->scratch, sizeof c->scratch);
        if (!l) {
            c->stats.data_bad++;
            continue;
        }
#if DEVELOPMENT || DEBUG
        if (probe)
            ++probe->rx_delivered;
#endif
        c->stats.data_rx++;
        if (c->host.deliver)
            c->host.deliver(c->host.ctx, c->scratch, l);
#if DEVELOPMENT || DEBUG
        if (tracing(c) && traced_tcp(&msdus[i])) {
            char queued[64] = "";
            if (c->host.queued)
                c->host.queued(c->host.ctx, queued, sizeof queued);
            trace_tcp(c, "in", &msdus[i], m->freq_mhz, m->tsf, retry, seq, queued[0] ? queued : NULL);
        }
#endif
    }
}
