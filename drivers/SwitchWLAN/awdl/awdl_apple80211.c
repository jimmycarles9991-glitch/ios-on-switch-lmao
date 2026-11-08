// SPDX-License-Identifier: BSD-2-Clause-Patent
// awdl0's Apple80211 requests and its user-client event ring (see awdl_apple80211.h and AWDL-CONTRACT.md §4-§6).
#include "awdl_apple80211.h"
#include "awdl_internal.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

_Static_assert(sizeof(struct a80211_virt_if_create) == 32, "VIRTUAL_IF_CREATE payload");
_Static_assert(sizeof(struct a80211_virt_if_delete) == 20, "VIRTUAL_IF_DELETE payload");
_Static_assert(sizeof(struct a80211_awdl_sync_channel_sequence) == 400, "AWDL_SYNC_CHANNEL_SEQUENCE payload");
_Static_assert(sizeof(struct a80211_awdl_peer_traffic_registration) == 184, "PEER_TRAFFIC_REGISTRATION payload");
_Static_assert(sizeof(struct a80211_peer_service_request) == 20, "PEER_SERVICE_REQUEST payload");
_Static_assert(sizeof(struct a80211_awdl_peer_advert) == 44, "awdl_peer_advert_add");
_Static_assert(sizeof(struct a80211_ppp_done) == 12, "POST_PRESENCE_DONE payload");
_Static_assert(sizeof(struct a80211_peer_presence) == 68, "PEER_PRESENCE payload");
_Static_assert(sizeof(struct a80211_ring_header) == A80211_RING_HEADER, "ring header");

// ---- event ring -------------------------------------------------------------------------------------------------------
int a80211_ring_init(struct a80211_ring *r, void *buf, uint32_t size, volatile uint32_t *state)
{
    if (!buf || !state || size < 2 * A80211_RING_HEADER || (size & 3))
        return -1;
    r->buf = (uint8_t *)buf;
    r->size = size;
    r->state = state;
    r->posted = r->dropped = 0;
    memset(buf, 0, size);
    state[0] = state[1] = 0;
    return 0;
}

static void ring_copy_in(struct a80211_ring *r, uint32_t at, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    const uint32_t first = n < r->size - at ? n : r->size - at;
    if (s) {
        memcpy(r->buf + at, s, first);
        memcpy(r->buf, s + first, n - first);
    } else {
        memset(r->buf + at, 0, first);
        memset(r->buf, 0, n - first);
    }
}

int a80211_ring_post(struct a80211_ring *r, uint32_t event, const char *ifname, const void *payload, uint32_t len)
{
    if (len > A80211_RING_MAX_PAYLOAD)
        return -1;
    uint32_t w = r->state[1];
    const uint32_t rd = __atomic_load_n(&r->state[0], __ATOMIC_ACQUIRE);
    if (w >= r->size || rd >= r->size || ((w | rd) & 3)) {  // userland wrote nonsense into the state page
        r->dropped++;
        return -1;
    }
    const uint32_t pad = (4 - (len & 3)) & 3;
    const uint32_t need = A80211_RING_HEADER + len + pad;
    const uint32_t tail = r->size - w;
    const uint32_t skip = tail < A80211_RING_HEADER ? tail : 0;  // the reader skips a short tail 4 bytes at a time
    const uint32_t used = (w + r->size - rd) % r->size;
    const uint32_t room = r->size - used - 4;  // never fill completely: write == read means empty
    if (skip + need > room) {
        r->dropped++;
        return -1;
    }
    for (uint32_t i = 0; i < skip; i += 4) {
        const uint32_t p = A80211_RING_MAGIC_PAD;
        memcpy(r->buf + w + i, &p, 4);
    }
    if (skip)
        w = 0;
    struct a80211_ring_header h;
    memset(&h, 0, sizeof h);
    h.magic = A80211_RING_MAGIC_EVENT;
    h.payload_len = (uint16_t)len;
    h.pad_len = (uint16_t)pad;
    h.event = event;
    for (unsigned i = 0; i < sizeof h.ifname - 1 && ifname && ifname[i]; ++i)
        h.ifname[i] = ifname[i];
    memcpy(r->buf + w, &h, sizeof h);
    ring_copy_in(r, (w + A80211_RING_HEADER) % r->size, payload, len);
    ring_copy_in(r, (w + A80211_RING_HEADER + len) % r->size, NULL, pad);
    __atomic_store_n(&r->state[1], (w + need) % r->size, __ATOMIC_RELEASE);
    r->posted++;
    return 0;
}

int a80211_ring_take(struct a80211_ring *r, uint32_t *event, char ifname[16], void *payload, uint32_t *len,
                     uint32_t capacity)
{
    for (;;) {
        uint32_t rd = r->state[0];
        const uint32_t w = __atomic_load_n(&r->state[1], __ATOMIC_ACQUIRE);
        if (rd == w)
            return 0;
        if (rd >= r->size || (rd & 3))
            return -1;
        uint32_t magic = 0;
        memcpy(&magic, r->buf + rd, 4);
        if (r->size - rd < A80211_RING_HEADER || magic == A80211_RING_MAGIC_PAD) {
            r->state[0] = (rd + 4) % r->size;
            continue;
        }
        struct a80211_ring_header h;
        memcpy(&h, r->buf + rd, sizeof h);
        if (h.magic != A80211_RING_MAGIC_EVENT || h.payload_len > A80211_RING_MAX_PAYLOAD)
            return -1;
        const uint32_t n = h.payload_len < capacity ? h.payload_len : capacity;
        const uint32_t at = (rd + A80211_RING_HEADER) % r->size;
        const uint32_t first = n < r->size - at ? n : r->size - at;
        memcpy(payload, r->buf + at, first);
        memcpy((uint8_t *)payload + first, r->buf, n - first);
        *event = h.event;
        memcpy(ifname, h.ifname, 16);
        *len = h.payload_len;
        rd = (rd + A80211_RING_HEADER + h.payload_len + h.pad_len) % r->size;
        __atomic_store_n(&r->state[0], rd, __ATOMIC_RELEASE);
        return 1;
    }
}

// ---- requests -----------------------------------------------------------------------------------------------------------
void a80211_awdl_init(struct a80211_awdl *f, struct awdl_core *core, const struct a80211_awdl_ops *ops,
                      const char *ifname, const char *parent, const uint8_t caps[12])
{
    memset(f, 0, sizeof *f);
    f->core = core;
    f->ops = *ops;
    for (unsigned i = 0; i < sizeof f->ifname - 1 && ifname[i]; ++i)
        f->ifname[i] = ifname[i];
    for (unsigned i = 0; i < sizeof f->parent - 1 && parent[i]; ++i)
        f->parent[i] = parent[i];
    memcpy(f->caps, caps, 12);
    f->preferred = f->master_channel = 149;
}

static void flog(struct a80211_awdl *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void flog(struct a80211_awdl *f, const char *fmt, ...)
{
    if (!f->ops.log)
        return;
    char line[160];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    f->ops.log(f->ops.ctx, line);
}

static int answer(void *data, uint32_t *len, uint32_t capacity, const void *src, uint32_t n)
{
    if (!data || !capacity)
        return EINVAL;
    const uint32_t k = n < capacity ? n : capacity;
    memcpy(data, src, k);
    *len = k;
    return 0;
}

static int answer_u32(void *data, uint32_t *len, uint32_t capacity, int32_t *val, uint32_t version, uint32_t v)
{
    if (!data || !capacity) {  // a get without a buffer takes the value in req_val
        *val = (int32_t)v;
        return 0;
    }
    const uint32_t w[2] = {version, v};
    return answer(data, len, capacity, w, sizeof w);
}

static void fill_channel(struct a80211_channel *ch, struct awdl_chan c)
{
    const uint8_t p = awdl_chan_primary(c);
    ch->version = 1;
    ch->channel = p;
    // APPLE80211_C_FLAG_*: 20 MHz 0x2, 40 MHz 0x4, 2.4 GHz 0x8, 5 GHz 0x10, 80 MHz 0x400
    ch->flags = p <= 14 ? 0x2u | 0x8u : (c.opclass == 0x80 ? 0x400u : 0x2u) | 0x10u;
}

int a80211_awdl_request(struct a80211_awdl *f, int set, int32_t type, int32_t *val, void *data, uint32_t *len,
                        uint32_t capacity, uint64_t now)
{
    const uint32_t in = set ? *len : 0;
    if (set) {
        switch (type) {
        case A80211_IOC_AWDL_SYNC_ENABLED: {
            struct a80211_awdl_sync_enabled s;
            memset(&s, 0, sizeof s);
            memcpy(&s, data, in < sizeof s ? in : sizeof s);
            f->sync_flags = s.flags;
            const int on = s.enabled != 0;
            if (on == f->enabled)
                return 0;
            if (on) {
                const int e = f->ops.start ? f->ops.start(f->ops.ctx) : 0;
                if (e) {
                    flog(f, "AWDL: wifip2pd enabled AWDL, but it did not start (%d)", e);
                    return e;
                }
                awdl_core_start(f->core, now);
            } else {
                awdl_core_stop(f->core);
                if (f->ops.stop)
                    f->ops.stop(f->ops.ctx);
            }
            f->enabled = (uint8_t)on;
            return 0;
        }
        case A80211_IOC_AWDL_STRATEGY: {
            struct a80211_awdl_strategy s;
            memset(&s, 0, sizeof s);
            memcpy(&s, data, in < sizeof s ? in : sizeof s);
            f->strategy = s.strategy;
            f->strategy_param = s.param;
            return 0;
        }
        case A80211_IOC_AWDL_OPER_MODE: {
            struct a80211_awdl_oper_mode m;
            memset(&m, 0, sizeof m);
            memcpy(&m, data, in < sizeof m ? in : sizeof m);
            f->oper_mode = m.mode;
            return 0;
        }
        case A80211_IOC_AWDL_SOCIAL_TIME_SLOTS: {
            struct a80211_awdl_social_time_slots s;
            memset(&s, 0, sizeof s);
            memcpy(&s, data, in < sizeof s ? in : sizeof s);
            f->social_time_slots = s.enabled;
            return 0;
        }
        case A80211_IOC_AWDL_PREFERRED_CHANNELS: {
            struct a80211_awdl_preferred_channels p;
            memset(&p, 0, sizeof p);
            memcpy(&p, data, in < sizeof p ? in : sizeof p);
            f->preferred = p.preferred;
            f->master_channel = p.master;
            f->secondary_preferred = p.secondary_preferred;
            return 0;
        }
        case A80211_IOC_USERSPACE_P2P_OPTIONS: {
            struct a80211_userspace_p2p_options o;
            memset(&o, 0, sizeof o);
            memcpy(&o, data, in < sizeof o ? in : sizeof o);
            if (o.flags != f->userspace_p2p_flags)
                flog(f, "AWDL: userspace P2P options %#llx", (unsigned long long)o.flags);
            f->userspace_p2p_flags = o.flags;
            return 0;
        }
        case A80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION: {
            struct a80211_awdl_peer_traffic_registration t;
            memset(&t, 0, sizeof t);
            memcpy(&t, data, in < sizeof t ? in : sizeof t);
            t.unique_id[sizeof t.unique_id - 1] = 0;
            if (t.active) {
                f->traffic_registrations++;
                awdl_core_traffic_registered(f->core, (const char *)t.unique_id, now);  // a session starts
            } else if (f->traffic_registrations) {
                f->traffic_registrations--;
            }
            flog(f, "AWDL: traffic %s for %02x:%02x:%02x:%02x:%02x:%02x (%.40s), %u active",
                 t.active ? "registered" : "cleared", t.address[0], t.address[1], t.address[2], t.address[3],
                 t.address[4], t.address[5], (const char *)t.unique_id, f->traffic_registrations);
            return 0;
        }
        case A80211_IOC_POST_PRESENCE_DONE:  // wifip2pd took our PEER_PRESENCE
        case A80211_IOC_AWDL_CTL:
        case A80211_IOC_AWDL_PEER_SERVICE_REQUEST:  // Bonjour offload: answered by mDNS over awdl0 instead
        case A80211_IOC_AWDL_ADVERTISERS:           // peer hints from Bluetooth: the air tells us anyway
        case A80211_IOC_AWDL_UMI_TX_PEER:
        case A80211_IOC_GATEWAY_ADDRESS:
        case A80211_IOC_UNICAST_INFRA_PEER_ADDRESS:
            return 0;
        default:
            break;
        }
    } else {
        switch (type) {
        case A80211_IOC_AWDL_SYNC_ENABLED: {
            const struct a80211_awdl_sync_enabled s = {1, f->sync_flags, f->enabled};
            return answer(data, len, capacity, &s, sizeof s);
        }
        case A80211_IOC_AWDL_STRATEGY: {
            const struct a80211_awdl_strategy s = {1, f->strategy, f->strategy_param, 0};
            return answer(data, len, capacity, &s, sizeof s);
        }
        case A80211_IOC_AWDL_OPER_MODE:
            return answer_u32(data, len, capacity, val, 1, f->oper_mode);
        case A80211_IOC_AWDL_PREFERRED_CHANNELS: {
            const struct a80211_awdl_preferred_channels p = {1, f->preferred, f->master_channel,
                                                             f->secondary_preferred};
            return answer(data, len, capacity, &p, sizeof p);
        }
        case A80211_IOC_AWDL_SYNC_CHANNEL_SEQUENCE: {
            struct a80211_awdl_sync_channel_sequence s;
            memset(&s, 0, sizeof s);
            s.version = 1;
            s.length = AWDL_SEQ_SLOTS;
            s.encoding = AWDL_CHAN_ENC_OPCLASS;
            s.step_count = 3;
            s.fill_chan_num = 0xff;
            const struct awdl_chan *seq = awdl_core_sequence(f->core);
            for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
                fill_channel(&s.channel[i], seq[i]);
            s.strategy = f->strategy;
            return answer(data, len, capacity, &s, sizeof s);
        }
        case A80211_IOC_CHANNEL: {
            uint32_t w[4] = {1, 0, 0, 0};
            fill_channel((struct a80211_channel *)(void *)(w + 1), awdl_core_channel_at(f->core, now));
            return answer(data, len, capacity, w, sizeof w);
        }
        case A80211_IOC_BSSID:
            return answer(data, len, capacity, awdl_bssid, 6);
        case A80211_IOC_SSID:
            return ENOENT;
        case A80211_IOC_CARD_CAPABILITIES:
            return answer(data, len, capacity, f->caps, sizeof f->caps);
        case A80211_IOC_VIRTUAL_IF_PARENT: {
            char name[16];
            memcpy(name, f->parent, sizeof name);
            return answer(data, len, capacity, name, sizeof name);
        }
        case A80211_IOC_VIRTUAL_IF_ROLE:
            return answer_u32(data, len, capacity, val, 1, A80211_VIF_ROLE_AWDL);
        case A80211_IOC_STATE:
            return answer_u32(data, len, capacity, val, 1, f->enabled ? 4 : 0);
        case A80211_IOC_POWER: {
            const uint32_t w[6] = {1, 1, 1, 0, 0, 0};
            return answer(data, len, capacity, w, sizeof w);
        }
        case A80211_IOC_DRIVER_VERSION:
        case A80211_IOC_HARDWARE_VERSION: {
            char text[256];
            memset(text, 0, sizeof text);
            const char *s = type == A80211_IOC_DRIVER_VERSION ? "SwitchWLAN AWDL 1" : "BCM4356A3";
            for (unsigned i = 0; s[i]; ++i)
                text[i] = s[i];
            return answer(data, len, capacity, text, sizeof text);
        }
        case A80211_IOC_AWDL_STATISTICS:
        case A80211_IOC_AWDL_LOW_LATENCY_STATISTICS:
        case A80211_IOC_LOWLATENCY_STATISTICS:
            // all counters zero: valid for every version (the caller sized the buffer)
            if (!data || !capacity)
                return EINVAL;
            memset(data, 0, capacity);
            ((uint32_t *)data)[0] = 1;
            *len = capacity;
            return 0;
        case A80211_IOC_AWDL_ADVERTISERS: {
            // the peer table as awdl_peer_advert_add records, as many as fit
            uint32_t n = 0;
            for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
                const struct awdl_peer *p = awdl_core_peer(f->core, i);
                if (!p || (n + 1) * sizeof(struct a80211_awdl_peer_advert) > capacity)
                    continue;
                struct a80211_awdl_peer_advert a;
                memset(&a, 0, sizeof a);
                a.aw_counter = p->sync.aw_seq;
                a.tx_counter = p->sync.tx_counter;
                a.period_tu = AWDL_AW_TU;
                a.aw_cmn_length = p->sync.aw_cmn_length;
                a.aw_ext_length = p->sync.aw_ext_length;
                a.aw_period = p->sync.aw_period;
                a.rssi = p->rssi;
                a.guard_time = p->sync.guard_time;
                a.presence_mode = p->sync.presence_mode;
                a.dist_top = (uint8_t)p->election.hops;
                a.self_metrics = p->election.self_metric;
                a.top_master_metrics = p->election.root_metric;
                memcpy(a.top_master, p->election.root, 6);
                memcpy(a.addr, p->addr, 6);
                memcpy((uint8_t *)data + n * sizeof a, &a, sizeof a);
                n++;
            }
            *len = n * (uint32_t)sizeof(struct a80211_awdl_peer_advert);
            return 0;
        }
        default:
            break;
        }
    }
    // not ours: once per type in the log
    for (unsigned i = 0; i < sizeof f->unknown_seen / sizeof f->unknown_seen[0]; ++i) {
        if (f->unknown_seen[i] == (uint32_t)type + 1)
            return EOPNOTSUPP;
        if (!f->unknown_seen[i]) {
            f->unknown_seen[i] = (uint32_t)type + 1;
            flog(f, "AWDL: %s: %s %d not handled", f->ifname, set ? "set" : "get", type);
            break;
        }
    }
    return EOPNOTSUPP;
}

void a80211_awdl_peer(struct a80211_awdl *f, const uint8_t addr[6], int present, int8_t rssi)
{
    struct a80211_peer_presence p;
    memset(&p, 0, sizeof p);
    memcpy(p.address, addr, 6);
    p.handle = ++f->presence_handle;
    p.state = present ? A80211_PEER_PRESENT : A80211_PEER_ABSENT;
    p.rssi = rssi;
    p.link_quality = present ? 100 : 0;
    if (f->ops.post)
        f->ops.post(f->ops.ctx, A80211_M_PEER_PRESENCE, &p, sizeof p);
}

int a80211_awdl_down_if(const uint8_t station[6], struct a80211_awdl_down_if *out)
{
    static const uint8_t zero[6];
    if (station[0] & 0x01 || !memcmp(station, zero, 6))
        return -1;
    out->if_flags = A80211_IFF_BROADCAST | A80211_IFF_SIMPLEX | A80211_IFF_MULTICAST;
    out->mtu = 1500;
    memcpy(out->lladdr, station, 6);
    out->lladdr[0] |= 0x02;
    if (!memcmp(out->lladdr, station, 6))
        out->lladdr[0] ^= 0x04;
    return 0;
}

int a80211_awdl_down_media_status(void)
{
    return A80211_IFM_AVALID;
}
