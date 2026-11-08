// SPDX-License-Identifier: BSD-2-Clause-Patent
// The soft backend's data path: awdl0's Ethernet frames out as AWDL data frames when the peer listens on our channel
// (now, or queued for its next slot), and AWDL data frames in, A-MSDUs included, as Ethernet frames.
#include "awdl_internal.h"

static unsigned slot_at(const struct awdl_core *c, uint64_t t)
{
    return (awdl_core_aw_seq(c, t) / AWDL_EAW_AWS) % AWDL_SEQ_SLOTS;
}

// Is `dst` (a peer, or any group address) on our channel in the slot containing `t`?
static int listening(const struct awdl_core *c, const uint8_t *dst, uint64_t t)
{
    const struct awdl_chan ours = awdl_core_channel_at(c, t);
    if (!awdl_chan_freq(ours))
        return 0;
    if (awdl_is_group(dst))
        return 1;
    const struct awdl_peer *p = awdl_core_find(c, dst);
    if (!p || !p->reachable)
        return 0;
    if (!p->has_seq || !p->seq.count)
        return 1;
    return awdl_chan_primary(p->seq.chan[slot_at(c, t) % p->seq.count]) == awdl_chan_primary(ours);
}

static int send(struct awdl_core *c, const uint8_t *frame, size_t len, int group, uint64_t now)
{
    const uint16_t freq = awdl_chan_freq(awdl_core_channel_at(c, now));
    const uint64_t stay = freq != c->infra_mhz ? awdl_eaw_start(c, now) + AWDL_EAW_US : 0;
    if (c->host.tx(c->host.ctx, frame, len, freq, stay, group)) {
        c->stats.tx_refused++;
        return -3;
    }
    c->stats.data_tx++;
    return 0;
}

void awdl_data_flush(struct awdl_core *c, uint64_t now)
{
    unsigned kept = 0;
    const unsigned n = c->txq_count;
    for (unsigned i = 0; i < n; ++i) {
        struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        if (listening(c, e->dst, now)) {
            if (!send(c, e->frame, e->len, e->group, now) && e->transfer)
                awdl_sched_transfer(c, e->dst, 1, now);
            continue;
        }
        if (!e->group && !awdl_core_find(c, e->dst)) {
            c->stats.data_drop++;  // the peer went away
            continue;
        }
        struct awdl_txq_entry *k = &c->txq[(c->txq_head + kept) % AWDL_TXQ_FRAMES];
        if (k != e)
            *k = *e;
        kept++;
    }
    c->txq_count = kept;
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
    const uint16_t seq = c->data_seq++;
    if (listening(c, eth, now) && !c->txq_count) {
        const size_t n = awdl_data_encap(eth, len, c->cfg.addr, seq, c->scratch, sizeof c->scratch);
        const int e = n ? send(c, c->scratch, n, group, now) : -3;
        if (!e)
            awdl_sched_traffic(c, eth, len, 1, now);
        return e;
    }
    if (c->txq_count == AWDL_TXQ_FRAMES) {
        c->stats.data_drop++;
        return -4;
    }
    struct awdl_txq_entry *e = &c->txq[(c->txq_head + c->txq_count) % AWDL_TXQ_FRAMES];
    const size_t n = awdl_data_encap(eth, len, c->cfg.addr, seq, e->frame, sizeof e->frame);
    if (!n)
        return -5;
    e->len = (uint16_t)n;
    e->group = (uint8_t)group;
    e->transfer = (uint8_t)awdl_transfer_frame(eth, len);
    memcpy(e->dst, eth, 6);
    c->txq_count++;
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
        return;
    }
    if (!awdl_is_group(f + 4) && !awdl_addr_eq(f + 4, c->cfg.addr))
        return;  // someone else's
    struct awdl_peer *p = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS && !p; ++i) {
        if (c->peers[i].used && awdl_addr_eq(c->peers[i].addr, f + 10))
            p = &c->peers[i];
    }
    if (p) {
        p->last_seen = m->tsf;
        if (retry && p->has_data_seq && p->last_data_seq == seq)
            return;  // a retransmission we already took
        p->last_data_seq = seq;
        p->has_data_seq = 1;
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t l = awdl_msdu_ether(&msdus[i], c->scratch, sizeof c->scratch);
        if (!l) {
            c->stats.data_bad++;
            continue;
        }
        c->stats.data_rx++;
        if (c->host.deliver)
            c->host.deliver(c->host.ctx, c->scratch, l);
        awdl_sched_traffic(c, c->scratch, l, 0, m->tsf);
    }
}
