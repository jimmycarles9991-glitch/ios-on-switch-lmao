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
_Static_assert(sizeof(struct switch_a80211_virtual_if_state) == 20, "VIRTUAL_IF_STATE payload");
_Static_assert(sizeof(struct a80211_awdl_sync_channel_sequence) == 400, "AWDL_SYNC_CHANNEL_SEQUENCE payload");
_Static_assert(sizeof(struct a80211_awdl_peer_traffic_registration) == 184, "PEER_TRAFFIC_REGISTRATION payload");
_Static_assert(sizeof(struct a80211_peer_service_request) == 20, "PEER_SERVICE_REQUEST payload");
_Static_assert(sizeof(struct a80211_awdl_peer_advert) == 44, "awdl_peer_advert_add");
_Static_assert(sizeof(struct a80211_ppp_done) == 12, "POST_PRESENCE_DONE payload");
_Static_assert(sizeof(struct a80211_peer_presence) == 68, "PEER_PRESENCE payload");
_Static_assert(offsetof(struct a80211_virt_if_create, role) == 12 &&
                   offsetof(struct a80211_virt_if_create, bsd_name) == 16,
               "VIRTUAL_IF_CREATE fields");
_Static_assert(offsetof(struct switch_a80211_virtual_if_state, role) == 4 &&
                   offsetof(struct switch_a80211_virtual_if_state, channel) == 8,
               "VIRTUAL_IF_STATE fields");
_Static_assert(offsetof(struct a80211_peer_presence, state) == 8 && offsetof(struct a80211_peer_presence, rssi) == 12,
               "PEER_PRESENCE fields");
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
#if defined(SWITCH_IPSW_KIND_24B119)
    if (core && switch_awdl_services_clear(core))
        core->service_update_indicator++;
#endif
    if (core)
        switch_awdl_core_set_traffic(core, 0, 0);
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

static int switch_awdl_payload(const void *data, uint32_t length, uint32_t expected)
{
    uint32_t version;
    if (!data || length != expected)
        return EINVAL;
    memcpy(&version, data, sizeof version);
    return version == 1 ? 0 : EINVAL;
}

static void fill_channel(struct a80211_channel *ch, struct awdl_chan c)
{
    const uint8_t p = awdl_chan_primary(c);
    ch->version = 1;
    ch->channel = p;
    // APPLE80211_C_FLAG_*: 20 MHz 0x2, 40 MHz 0x4, 2.4 GHz 0x8, 5 GHz 0x10, 80 MHz 0x400
    ch->flags = p <= 14 ? 0x2u | 0x8u : (c.opclass == 0x80 ? 0x400u : 0x2u) | 0x10u;
}

static int switch_awdl_set_running(struct a80211_awdl *f, int on, uint64_t now)
{
    const int running = awdl_core_running(f->core);
    if (on && !running) {
        const int e = f->ops.start ? f->ops.start(f->ops.ctx) : 0;
        if (e && e != EINPROGRESS) {
            flog(f, "AWDL: wifip2pd enabled AWDL, but it did not start (%d)", e);
            return e;
        }
        if (!e)
            awdl_core_start(f->core, now);
    } else if (!on && (running || f->enabled)) {
        awdl_core_stop(f->core);
        if (f->ops.stop)
            f->ops.stop(f->ops.ctx);
    }
    f->enabled = (uint8_t)on;
    return 0;
}

void switch_a80211_awdl_suspend(struct a80211_awdl *f)
{
    if (f->core)
        awdl_core_stop(f->core);
    switch_a80211_awdl_refresh(f);
}

int switch_a80211_awdl_resume(struct a80211_awdl *f, uint64_t now)
{
    if (!f->core)
        return ENXIO;
    return switch_awdl_set_running(f, f->enabled, now);
}

#if defined(SWITCH_IPSW_KIND_24B119)
static int switch_awdl_virtual_state(const void *data, size_t length, struct switch_a80211_virtual_if_state *state)
{
    if (!data || length != sizeof *state)
        return EINVAL;
    memcpy(state, data, sizeof *state);
    if (state->role != A80211_VIF_ROLE_AWDL)
        return EOPNOTSUPP;
    if (state->active > 1 || state->channel.version != 1)
        return EINVAL;
    return state->channel.channel || state->channel.flags ? EOPNOTSUPP : 0;
}

int switch_a80211_awdl_parent_state(const struct a80211_awdl *f, const void *data, size_t length)
{
    struct switch_a80211_virtual_if_state state;
    return f && f->core ? switch_awdl_virtual_state(data, length, &state) : ENXIO;
}

static int switch_awdl_request_activity(struct a80211_awdl *f, uint64_t now)
{
    if (f->switch_activation_explicit || f->enabled || awdl_native(f->core))
        return 0;
    return switch_awdl_set_running(f, 1, now);
}

static void switch_awdl_stop_if_idle(struct a80211_awdl *f, uint64_t now)
{
    if (!f->switch_activation_explicit && !awdl_native(f->core) && !f->core->switch_service_bytes &&
        !f->traffic_registrations)
        switch_awdl_set_running(f, 0, now);
}

static int switch_awdl_service_request(struct a80211_awdl *f, const void *data, size_t length, uint64_t now)
{
    struct awdl_core *const c = f->core;
    struct a80211_peer_service_request request;
    if (length < sizeof request)
        return EINVAL;
    memcpy(&request, data, sizeof request);
    if (request.version != 1 || request.delete_request > 1 || request.data_len != length - sizeof request)
        return EINVAL;
    const uint8_t zero[6] = {0};
    if (request.aw_offset || request.token || request.expect_fragmented_response || memcmp(request.peer, zero, sizeof zero))
        return EOPNOTSUPP;
    if (!request.data_len) {
        if (!request.delete_request)
            return EOPNOTSUPP;
        if (switch_awdl_services_clear(c)) {
            c->service_update_indicator++;
            switch_awdl_stop_if_idle(f, now);
        }
        return 0;
    }
    if (awdl_native(c))
        return EOPNOTSUPP;
    const int staged = switch_awdl_service_stage(c, (const uint8_t *)data + sizeof request, request.data_len);
    if (request.delete_request) {
        if (!staged && switch_awdl_service_drop(c))
            switch_awdl_stop_if_idle(f, now);
        return staged == ENOSPC ? 0 : staged;  // too large to have been kept
    }
    if (staged)
        return staged;
    if (switch_awdl_service_held(c))
        return 0;
    if (!switch_awdl_service_fits(c))
        return ENOSPC;
    const int start = switch_awdl_request_activity(f, now);
    if (start)
        return start;
    switch_awdl_service_keep(c);
    switch_awdl_sched_discovery(c, now);
    if (c->switch_staged[0] == AWDL_TLV_SERVICE_RESPONSE)
        switch_awdl_core_service_changed(c, now);
    return 0;
}
#endif

int a80211_awdl_request(struct a80211_awdl *f, int set, int32_t type, int32_t *val, void *data, uint32_t *len,
                        uint32_t capacity, uint64_t now)
{
    if (!f || !f->core || !val || !len || (set && (*len > capacity || (*len && !data))))
        return EINVAL;
    const uint32_t in = set ? *len : 0;
    if (set) {
        switch (type) {
        case A80211_IOC_AWDL_SYNC_ENABLED: {
            if (switch_awdl_payload(data, in, sizeof(struct a80211_awdl_sync_enabled)))
                return EINVAL;
            struct a80211_awdl_sync_enabled s;
            memset(&s, 0, sizeof s);
            memcpy(&s, data, in < sizeof s ? in : sizeof s);
            if (s.enabled > 1)
                return EINVAL;
            const int on = s.enabled != 0;
            const int e = switch_awdl_set_running(f, on, now);
            if (e)
                return e;
            f->sync_flags = s.flags;
#if defined(SWITCH_IPSW_KIND_24B119)
            f->switch_activation_explicit = 1;
#endif
            return 0;
        }
#if defined(SWITCH_IPSW_KIND_24B119)
        case A80211_IOC_VIRTUAL_IF_STATE: {
            struct switch_a80211_virtual_if_state s;
            const int invalid = switch_awdl_virtual_state(data, in, &s);
            if (invalid)
                return invalid;
            const int e = switch_awdl_set_running(f, (int)s.active, now);
            if (e)
                return e;
            f->switch_activation_explicit = 1;
            return 0;
        }
#endif
        case A80211_IOC_AWDL_STRATEGY: {
            if (switch_awdl_payload(data, in, sizeof(struct a80211_awdl_strategy)))
                return EINVAL;
            struct a80211_awdl_strategy s;
            memset(&s, 0, sizeof s);
            memcpy(&s, data, in < sizeof s ? in : sizeof s);
            f->strategy = s.strategy;
            f->strategy_param = s.param;
            return 0;
        }
        case A80211_IOC_AWDL_OPER_MODE: {
            if (switch_awdl_payload(data, in, sizeof(struct a80211_awdl_oper_mode)))
                return EINVAL;
            struct a80211_awdl_oper_mode m;
            memset(&m, 0, sizeof m);
            memcpy(&m, data, in < sizeof m ? in : sizeof m);
            f->oper_mode = m.mode;
            return 0;
        }
        case A80211_IOC_AWDL_SOCIAL_TIME_SLOTS: {
            if (switch_awdl_payload(data, in, sizeof(struct a80211_awdl_social_time_slots)))
                return EINVAL;
            struct a80211_awdl_social_time_slots s;
            memset(&s, 0, sizeof s);
            memcpy(&s, data, in < sizeof s ? in : sizeof s);
            if (s.enabled > 1)
                return EINVAL;
            f->social_time_slots = s.enabled;
            return 0;
        }
        case A80211_IOC_AWDL_PREFERRED_CHANNELS: {
            if (switch_awdl_payload(data, in, sizeof(struct a80211_awdl_preferred_channels)))
                return EINVAL;
            struct a80211_awdl_preferred_channels p;
            memset(&p, 0, sizeof p);
            memcpy(&p, data, in < sizeof p ? in : sizeof p);
            f->preferred = p.preferred;
            f->master_channel = p.master;
            f->secondary_preferred = p.secondary_preferred;
            return 0;
        }
        case A80211_IOC_USERSPACE_P2P_OPTIONS: {
            if (switch_awdl_payload(data, in, sizeof(struct a80211_userspace_p2p_options)))
                return EINVAL;
            struct a80211_userspace_p2p_options o;
            memset(&o, 0, sizeof o);
            memcpy(&o, data, in < sizeof o ? in : sizeof o);
            if (o.flags != f->userspace_p2p_flags)
                flog(f, "AWDL: userspace P2P options %#llx", (unsigned long long)o.flags);
            f->userspace_p2p_flags = o.flags;
            return 0;
        }
        case A80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION: {
            if (switch_awdl_payload(data, in, sizeof(struct a80211_awdl_peer_traffic_registration)))
                return EINVAL;
            struct a80211_awdl_peer_traffic_registration t;
            memset(&t, 0, sizeof t);
            memcpy(&t, data, in < sizeof t ? in : sizeof t);
            if (t.active > 1 || t.unique_id_length > sizeof t.unique_id)
                return EINVAL;
            const uint32_t unique_bytes =
                t.unique_id_length < sizeof t.unique_id ? t.unique_id_length : sizeof t.unique_id - 1;
            t.unique_id[unique_bytes] = 0;
            if (t.active) {
#if defined(SWITCH_IPSW_KIND_24B119)
                const int start = switch_awdl_request_activity(f, now);
                if (start)
                    return start;
#endif
                f->traffic_registrations++;
                if (f->traffic_registrations == 1)
                    switch_awdl_core_set_traffic(f->core, 1, now);
            } else if (f->traffic_registrations) {
                f->traffic_registrations--;
                if (!f->traffic_registrations)
                    switch_awdl_core_set_traffic(f->core, 0, now);
#if defined(SWITCH_IPSW_KIND_24B119)
                switch_awdl_stop_if_idle(f, now);
#endif
            }
            flog(f, "AWDL: traffic %s for %02x:%02x:%02x:%02x:%02x:%02x (%.40s), %u active",
                 t.active ? "registered" : "cleared", t.address[0], t.address[1], t.address[2], t.address[3],
                 t.address[4], t.address[5], (const char *)t.unique_id, f->traffic_registrations);
            return 0;
        }
        case A80211_IOC_POST_PRESENCE_DONE:
            return switch_awdl_payload(data, in, sizeof(struct a80211_ppp_done));
        case A80211_IOC_AWDL_PEER_SERVICE_REQUEST:
#if defined(SWITCH_IPSW_KIND_24B119)
            return switch_awdl_service_request(f, data, in, now);
#else
            return EOPNOTSUPP;
#endif
        case A80211_IOC_AWDL_CTL:
        case A80211_IOC_AWDL_ADVERTISERS:
        case A80211_IOC_AWDL_UMI_TX_PEER:
        case A80211_IOC_GATEWAY_ADDRESS:
        case A80211_IOC_UNICAST_INFRA_PEER_ADDRESS:
            return EOPNOTSUPP;
        default:
            break;
        }
    } else {
        switch (type) {
        case A80211_IOC_AWDL_SYNC_ENABLED: {
            const struct a80211_awdl_sync_enabled s = {1, f->sync_flags, switch_a80211_awdl_active(f)};
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
            return answer_u32(data, len, capacity, val, 1, switch_a80211_awdl_active(f) ? 4 : 0);
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
            return EOPNOTSUPP;
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

int switch_a80211_awdl_active(const struct a80211_awdl *f)
{
    return f && f->core && f->enabled && awdl_core_running(f->core) &&
           (f->ops.active ? f->ops.active(f->ops.ctx) : switch_awdl_core_ready(f->core));
}

void switch_a80211_awdl_refresh(struct a80211_awdl *f)
{
    if (!f->core)
        return;
    const uint8_t ready = switch_a80211_awdl_active(f);
    if (ready == f->published_ready)
        return;
    f->published_ready = ready;
    if (f->ops.post)
        f->ops.post(f->ops.ctx, A80211_M_AWDL_SYNC_STATE_CHANGED, NULL, 0);
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

#if defined(SWITCH_IPSW_KIND_24B119)
size_t switch_a80211_awdl_action_rx(void *out, size_t capacity, const uint8_t *frame, size_t length,
                                   const struct awdl_rx_meta *meta)
{
    enum { mac_header = 24, event_header = 72 };
    if (length < mac_header + 16 || length > AWDL_MAX_FRAME || capacity < event_header + length - mac_header)
        return 0;
    const struct awdl_chan channel = awdl_chan_of_freq(meta->freq_mhz);
    if (!channel.number)
        return 0;
    struct a80211_channel channel_info;
    fill_channel(&channel_info, channel);

    uint8_t *event = out;
    memset(event, 0, event_header);
    memcpy(event, frame + 10, 6);
    memcpy(event + 6, frame + 16, 6);
    memcpy(event + 12, &channel_info, sizeof channel_info);
    awdl_put32(event + 44, (uint32_t)(int32_t)meta->rssi);
    memcpy(event + 56, &meta->tsf, sizeof meta->tsf);
    memcpy(event + event_header, frame + mac_header, length - mac_header);
    return event_header + length - mac_header;
}
#endif

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
