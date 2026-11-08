// SPDX-License-Identifier: BSD-2-Clause-Patent
// The native backend's input: the firmware's AWDL events (its interface, role and received action frames) and the
// frames of its AWDL interface.
#include "awdl_internal.h"
#include "awdl_bcm.h"

static const char *fw_role_name(uint32_t status)
{
    switch (status) {
    case BCM_AWDL_ROLE_SLAVE:
        return "slave";
    case BCM_AWDL_ROLE_MASTER:
        return "master";
    case BCM_AWDL_ROLE_NE_MASTER:
        return "non-election master";
    default:
        return "?";
    }
}

void awdl_core_fw_event(struct awdl_core *c, const struct awdl_fw_event *e, const uint8_t *data, size_t len,
                        uint64_t now)
{
    if (!c->running || !awdl_native(c))
        return;
    if (c->stats.fw_events++ == 0)
        awdl_log(c, "AWDL: first firmware AWDL event: %u (%u bytes)", (unsigned)e->type, (unsigned)len);
    switch (e->type) {
    case WLC_E_ACTION_FRAME_RX:
    case WLC_E_AWDL_EVENT:
    case WLC_E_AWDL_RX_ACT_FRAME: {
        // The header (be16 version) and the frame from its category on; the sender is the event's address.
        struct bcm_rx_frame meta;
        const uint8_t *body = NULL;
        size_t n = 0;
        if (bcm_awdl_rx_af(data, len, &meta, &body, &n)) {
            if (e->type == WLC_E_AWDL_EVENT) {  // most AWDL_EVENTs carry no frame
                if (c->stats.fw_unknown++ < 8) {
                    awdl_log(c, "AWDL: firmware AWDL_EVENT status %u reason %u, %u bytes", (unsigned)e->status,
                             (unsigned)e->reason, (unsigned)len);
                }
            } else {
                c->stats.af_bad++;
            }
            return;
        }
        c->stats.fw_frames++;
        if (24 + n > sizeof c->scratch) {
            c->stats.af_bad++;
            return;
        }
        // as an 802.11 action frame for the parser: to broadcast, from the sender, in the AWDL BSS
        uint8_t *f = c->scratch;
        memset(f, 0, 24);
        f[0] = 0xd0;
        memset(f + 4, 0xff, 6);
        memcpy(f + 10, e->addr, 6);
        memcpy(f + 16, awdl_bssid, 6);
        memcpy(f + 24, body, n);
        struct awdl_af a;
        if (awdl_af_parse(f, 24 + n, &a)) {
            c->stats.af_bad++;
            return;
        }
        if (c->stats.af_rx++ == 0) {  // the native backend's first question: do the firmware's AWDL frames reach us
            awdl_log(c,
                     "AWDL: first AWDL action frame from the firmware (event %u, header v%u, chanspec %#x): "
                     "%02x:%02x:%02x:%02x:%02x:%02x, %s, %u bytes",
                     (unsigned)e->type, meta.version, meta.chanspec, a.src[0], a.src[1], a.src[2], a.src[3], a.src[4],
                     a.src[5], a.subtype == AWDL_SUBTYPE_MIF ? "MIF" : "PSF", (unsigned)n);
        }
        int32_t r = meta.rssi > 0 ? 0 : meta.rssi;
        if (meta.rssi < -128)
            r = -128;
        if (!meta.version)
            r = -60;
        const struct awdl_rx_meta m = {now, 0, (int8_t)r};
        awdl_handle_af(c, &a, &m);
        return;
    }
    case WLC_E_AWDL_ROLE: {  // the role is the status (the series); the host's election stays in charge
        const int32_t was = c->stats.fw_role;
        c->stats.fw_role = (int32_t)e->status;
        c->stats.roles++;
        if (was != c->stats.fw_role) {
            awdl_log(c, "AWDL: firmware role %s (%u), event address %02x:%02x:%02x:%02x:%02x:%02x, %u bytes",
                     fw_role_name(e->status), (unsigned)e->status, e->addr[0], e->addr[1], e->addr[2], e->addr[3],
                     e->addr[4], e->addr[5], (unsigned)len);
        }
        return;
    }
    case WLC_E_IF: {  // the AWDL interface the firmware made (or removed)
        struct bcm_if_event ie;
        if (bcm_if_event_decode(data, len, &ie))
            return;
        const char *change = ie.opcode == 2 ? "removed" : "changed";
        if (ie.opcode == 1)
            change = "added";
        awdl_log(c, "AWDL: firmware interface %s: ifidx %u bsscfg %u role %u, %02x:%02x:%02x:%02x:%02x:%02x", change,
                 ie.ifidx, ie.bssidx, ie.role, e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]);
        static const uint8_t zero[6];
        if (ie.opcode == 1 && c->nstate == AWDL_N_CREATING && ie.ifidx &&
            (awdl_addr_eq(e->addr, c->cfg.addr) || awdl_addr_eq(e->addr, zero))) {
            awdl_native_if_up(c, ie.ifidx, ie.bssidx, now);
        } else if (ie.opcode == 2 && c->nstate == AWDL_N_UP && ie.ifidx == c->fw_ifidx) {
            awdl_native_fail(c, "the firmware removed the AWDL interface (ifidx %u) on its own", ie.ifidx);
        }
        return;
    }
    case WLC_E_AWDL_AW:
        c->stats.aws++;
        return;
    default:
        // Payloads not settled: the first few events of other kinds are logged with their size.
        if (c->stats.fw_unknown++ < 8) {
            awdl_log(c, "AWDL: firmware event %u status %u reason %u, %u bytes", (unsigned)e->type, (unsigned)e->status,
                     (unsigned)e->reason, (unsigned)len);
        }
        return;
    }
}

void awdl_core_rx_eth(struct awdl_core *c, const uint8_t *frame, size_t len, uint64_t now)
{
    if (!c->running || len < 14)
        return;
    uint16_t seq = 0;
    const size_t n = bcm_awdl_decap(frame, len, c->scratch, sizeof c->scratch, &seq);
    if (!n) {
        c->stats.data_not_awdl++;  // not AWDL data (the series drops these too)
        return;
    }
    struct awdl_peer *p = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used && awdl_addr_eq(c->peers[i].addr, c->scratch + 6))
            p = &c->peers[i];
    }
    if (p)
        p->last_seen = now;
    c->stats.data_rx++;
    if (c->host.deliver)
        c->host.deliver(c->host.ctx, c->scratch, n);
    awdl_sched_traffic(c, c->scratch, n, 0, now);
}
