// SPDX-License-Identifier: BSD-2-Clause-Patent
// The channel sequence both backends run (awdl.h awdl_schedule): macOS's single-radio states, the free slots on the
// access point's channel, the 5 GHz social channel learnt from the master, and the idle/transfer mode.
#include "awdl_internal.h"
#include "awdl_bcm.h"

static struct awdl_chan social5_chan(uint8_t ch)
{
    return (struct awdl_chan){ch == 44 ? 44 : 149, 0x80};
}

uint16_t awdl_slot_mask(uint16_t infra_mhz, enum awdl_mode mode)
{
    if (mode != AWDL_MODE_TRANSFER)
        return AWDL_SLOTS_IDLE;
    return infra_mhz ? AWDL_SLOTS_TRANSFER_INFRA : AWDL_SLOTS_TRANSFER_ALL;
}

void awdl_schedule_from(struct awdl_chan out[AWDL_SEQ_SLOTS], uint16_t mask, struct awdl_chan free_chan, uint8_t first,
                        uint8_t second)
{
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        if (i == 8) {
            out[i] = (struct awdl_chan){6, 81};
        } else if (((unsigned)mask >> i) & 1u) {
            out[i] = social5_chan(i < 8 ? first : second);
        } else {
            out[i] = free_chan;  // the access point's channel, or empty while unassociated
        }
    }
}

void awdl_schedule(struct awdl_chan out[AWDL_SEQ_SLOTS], uint16_t infra_mhz, uint8_t social5, enum awdl_mode mode)
{
    const struct awdl_chan infra = awdl_chan_of_freq(infra_mhz);
    const uint8_t ip = infra.number;
    uint8_t first = social5;
    uint8_t second = social5;
    if (ip == 44 || ip == 149) {
        first = second = ip;
    } else if (social5 != 44 && social5 != 149) {
        first = 149;  // no master heard yet: both 5 GHz social channels, so either kind of peer meets us
        second = 44;
    }
    awdl_schedule_from(out, awdl_slot_mask(infra_mhz, mode), infra, first, second);
}

unsigned awdl_slots_away(const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint16_t infra_mhz)
{
    const uint8_t ip = awdl_chan_of_freq(infra_mhz).number;
    unsigned n = 0;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        const uint8_t p = awdl_chan_primary(seq[i]);
        n += p && p != ip;
    }
    return n;
}

uint16_t awdl_social_map(const struct awdl_chan seq[AWDL_SEQ_SLOTS])
{
    uint16_t map = 0;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        const uint8_t p = awdl_chan_primary(seq[i]);
        if (p == 6)
            map |= 0x1u;
        if (p == 44)
            map |= 0x2u;
        if (p == 149)
            map |= 0x4u;
    }
    return map;
}

static void build(struct awdl_core *c)
{
    if (c->seq_fixed)
        return;
    if (awdl_native(c)) {
        awdl_native_build_seq(c);
        return;
    }
    awdl_schedule(c->cfg.seq, c->infra_mhz, c->social5, (enum awdl_mode)c->mode);
}

static void apply(struct awdl_core *c, uint16_t old_map, uint64_t now)
{
    if (awdl_native(c)) {
        awdl_native_resend(c, old_map);
    } else {
        awdl_soft_install(c, now);
    }
}

// One line per schedule: the mode, why, and how many slots leave the access point's channel.
void awdl_sched_log(struct awdl_core *c, const char *fmt, ...)
{
    char why[96];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    const char *mode = c->mode == AWDL_MODE_TRANSFER ? "transfer" : "idle";
    const unsigned n = awdl_slots_away(c->cfg.seq, c->infra_mhz);
    if (c->infra_mhz) {
        awdl_log(c, "AWDL: schedule %s (%s): %u of 16 slots away from infra (%u MHz)", mode, why, n, c->infra_mhz);
    } else {
        awdl_log(c, "AWDL: schedule %s (%s): AWDL in %u of 16 slots, not associated", mode, why, n);
    }
}

void awdl_sched_start(struct awdl_core *c)
{
    c->social5 = 0;
    c->mode = AWDL_MODE_IDLE;  // discoverable with the least presence until AirDrop data flows
    c->transfer_until = 0;
    build(c);
}

static void set_mode(struct awdl_core *c, enum awdl_mode mode, uint64_t now, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void set_mode(struct awdl_core *c, enum awdl_mode mode, uint64_t now, const char *fmt, ...)
{
    if (c->mode == (uint8_t)mode)
        return;
    char why[96];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    const uint16_t map = awdl_social_map(c->cfg.seq);
    c->mode = (uint8_t)mode;
    c->stats.mode_changes++;
    build(c);
    awdl_sched_log(c, "%s", why);
    apply(c, map, now);
}

// Our 5 GHz social channel follows the master's: the one of 44/149 most of its slots are on.
void awdl_sched_learn_social(struct awdl_core *c, const struct awdl_peer *p, uint64_t now)
{
    if (!p->has_seq || !p->seq.count)
        return;
    unsigned n44 = 0;
    unsigned n149 = 0;
    for (unsigned i = 0; i < p->seq.count && i < AWDL_SEQ_SLOTS; ++i) {
        const uint8_t ch = awdl_chan_primary(p->seq.chan[i]);
        n44 += ch == 44;
        n149 += ch == 149;
    }
    uint8_t want = n149 ? 149 : 0;
    if (n44 > n149)
        want = 44;
    if (!want || want == c->social5 || c->created_with == BCM_AWDL_API_IFC_V3)
        return;
    c->social5 = want;
    const uint16_t map = awdl_social_map(c->cfg.seq);
    build(c);
    awdl_sched_log(c, "master %02x:%02x:%02x:%02x:%02x:%02x on channel %u", p->addr[0], p->addr[1], p->addr[2],
                   p->addr[3], p->addr[4], p->addr[5], want);
    apply(c, map, now);
}

// AirDrop's data (TLS over TCP, unicast) on awdl0: transfer mode now, held for AWDL_TRANSFER_HOLD_US after it.
int awdl_transfer_frame(const uint8_t *eth, size_t len)
{
    if (len < 14 + 20 || (eth[0] & 1))
        return 0;
    const uint16_t type = (uint16_t)(((unsigned)eth[12] << 8) | eth[13]);
    if (type == 0x0800)
        return (eth[14] >> 4) == 4 && eth[14 + 9] == 6;
    if (type == 0x86dd)
        return len >= 14 + 40 && (eth[14] >> 4) == 6 && eth[14 + 6] == 6;
    return 0;
}

void awdl_sched_transfer(struct awdl_core *c, const uint8_t peer[6], int out, uint64_t now)
{
    c->transfer_until = now + AWDL_TRANSFER_HOLD_US;
    if (c->mode == AWDL_MODE_TRANSFER)
        return;
    const uint8_t *p = peer;
    set_mode(c, AWDL_MODE_TRANSFER, now, "TCP %s %02x:%02x:%02x:%02x:%02x:%02x", out ? "to" : "from", p[0], p[1], p[2],
             p[3], p[4], p[5]);
}

void awdl_sched_traffic(struct awdl_core *c, const uint8_t *eth, size_t len, int out, uint64_t now)
{
    if (awdl_transfer_frame(eth, len))
        awdl_sched_transfer(c, out ? eth : eth + 6, out, now);
}

void awdl_core_traffic_registered(struct awdl_core *c, const char *who, uint64_t now)
{
    if (!c->running)
        return;
    c->transfer_until = now + AWDL_TRANSFER_HOLD_US;  // a session start: the data that follows keeps it
    set_mode(c, AWDL_MODE_TRANSFER, now, "wifip2pd traffic registration %.40s", who ? who : "");
}

void awdl_sched_deadline(struct awdl_core *c, uint64_t now)
{
    if (c->mode == AWDL_MODE_TRANSFER && now >= c->transfer_until)
        set_mode(c, AWDL_MODE_IDLE, now, "no AirDrop data for %u s", AWDL_TRANSFER_HOLD_US / 1000000);
}

uint64_t awdl_sched_next(const struct awdl_core *c)
{
    return c->mode == AWDL_MODE_TRANSFER ? c->transfer_until : UINT64_MAX;
}

enum awdl_mode awdl_core_mode(const struct awdl_core *c)
{
    return (enum awdl_mode)c->mode;
}

void awdl_core_set_infra(struct awdl_core *c, uint16_t infra_mhz, uint64_t now)
{
    if (infra_mhz == c->infra_mhz)
        return;
    c->infra_mhz = infra_mhz;
    if (!c->running)
        return;
    const uint16_t map = awdl_social_map(c->cfg.seq);
    struct awdl_chan was[AWDL_SEQ_SLOTS];
    memcpy(was, c->cfg.seq, sizeof was);
    build(c);
    if (!memcmp(was, c->cfg.seq, sizeof was))
        return;  // (v3: only association on/off changes its slots)
    awdl_sched_log(c, "%s", infra_mhz ? "infra channel changed" : "infra lost");
    apply(c, map, now);
}
