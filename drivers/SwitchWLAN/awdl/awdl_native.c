// SPDX-License-Identifier: BSD-2-Clause-Patent
// The native backend: the firmware's own AWDL (awdl_bcm.h), brought up as the brcmfmac AWDL series does on firmware
// with interface_create v3, or in the 7.x awdl_if form. The host elects; the firmware keeps the time and sends.
#include "awdl_internal.h"
#include "awdl_bcm.h"

// Native backend: whether the bring-up probe found `name` missing from the firmware's table.
static int fw_absent(const struct awdl_core *c, const char *name)
{
    for (unsigned i = 0; i < BCM_PROBE_COUNT; ++i) {
        if (!strcmp(bcm_probe_names[i], name))
            return (c->cfg.fw_absent >> i) & 1;
    }
    return 0;
}

// Native backend: one request to the firmware, through the integration's ordered queue, on firmware interface
// `ifidx` (the AWDL interface's for every awdl_* variable, as the series' brcmf_fil_iovar_data_set on its ifp).
// Variables the probe found missing are not sent (counted).
static void fw_req(struct awdl_core *c, int32_t ifidx, int get, const char *name, const void *buf, size_t len)
{
    if (fw_absent(c, name)) {
        c->stats.iovar_skipped++;
        return;
    }
    if (!len && strcmp(name, "interface_remove"))
        return;  // an encoder refused (too long): nothing to send
    c->stats.iovars++;
    if (!c->host.iovar || c->host.iovar(c->host.ctx, ifidx, get, name, buf, len)) {
        c->stats.iovar_refused++;
        awdl_log(c, "AWDL: iovar %s not queued", name);
    }
}

static void fw_set(struct awdl_core *c, const char *name, const void *buf, size_t len)
{
    fw_req(c, c->fw_ifidx, 0, name, buf, len);
}

static void fw_u32(struct awdl_core *c, const char *name, uint32_t v)
{
    uint8_t b[4];
    fw_set(c, name, b, bcm_u32(b, v));
}

// awdl_peer_op: an add carries the peer's HT capabilities element, built from its AWDL HT TLV (as macOS passes it).
static void fw_peer_op(struct awdl_core *c, uint8_t op, const struct awdl_peer *p)
{
    uint8_t ie[BCM_HT_CAP_IE_LEN];
    uint8_t b[BCM_AWDL_PEER_OP_HDR_LEN + BCM_AWDL_PEER_IES_MAX];
    size_t ies = 0;
    if (op == BCM_AWDL_PEER_OP_ADD && p->has_ht)
        ies = bcm_ht_cap_ie(ie, p->ht_caps, p->ht_ampdu, p->ht_mcs);
    fw_set(c, "awdl_peer_op", b, bcm_awdl_peer_op(b, sizeof b, op, p->addr, ie, ies));
    if (op == BCM_AWDL_PEER_OP_ADD) {
        c->stats.peer_adds++;
    } else {
        c->stats.peer_dels++;
    }
}

static uint8_t social5_of(const struct awdl_chan seq[AWDL_SEQ_SLOTS])
{
    return awdl_chan_primary(seq[0]);
}

// The sequence the native backend runs for its mode: on interface_create v3 the series' slot_bss form (44 and 6, the
// free slots the firmware's for the station), on 7.x our own (the infra channel and the learnt social channel).
void awdl_native_build_seq(struct awdl_core *c)
{
    if (c->created_with == BCM_AWDL_API_IFC_V3) {
        awdl_schedule_from(c->cfg.seq, awdl_slot_mask(c->infra_mhz, (enum awdl_mode)c->mode), (struct awdl_chan){0, 0},
                           44, 44);
        return;
    }
    awdl_schedule(c->cfg.seq, c->infra_mhz, c->social5, (enum awdl_mode)c->mode);
    for (unsigned i = 0; c->seq_no_empty && i < AWDL_SEQ_SLOTS; ++i) {
        if (!c->cfg.seq[i].number)
            c->cfg.seq[i] = c->cfg.seq[0];
    }
}

static void native_schedule(struct awdl_core *c)
{
    uint8_t b[BCM_AWDL_SLOT_BSS_LEN];
    if (c->created_with == BCM_AWDL_API_IFC_V3 && !fw_absent(c, "slot_bss")) {
        fw_set(c, "slot_bss", b, bcm_slot_bss(b, awdl_slot_mask(c->infra_mhz, (enum awdl_mode)c->mode)));
    } else {
        fw_set(c, "awdl_chan_seq", b, bcm_awdl_chan_seq(b, sizeof b, c->cfg.seq, AWDL_SEQ_SLOTS));
    }
}

static void native_payload(struct awdl_core *c)
{
    uint8_t tlvs[256];
    uint8_t b[512];
    const size_t n = awdl_core_build_payload(c, tlvs, sizeof tlvs);
    fw_set(c, "awdl_payload", b, bcm_awdl_payload(b, sizeof b, c->payload_u16, tlvs, n));
}

static void native_election(struct awdl_core *c)
{
    uint8_t b[BCM_AWDL_ELECTION_TREE_LEN];
    if (c->role == AWDL_ROLE_FOLLOWER) {
        bcm_awdl_election_tree(b, c->adv.root, c->adv.root_metric, (uint8_t)c->adv.hops);
    } else {
        bcm_awdl_election_tree(b, c->cfg.addr, BCM_AWDL_SELF_METRIC, 0);
    }
    fw_set(c, "awdl_election_tree", b, sizeof b);
}

static void native_opmode(struct awdl_core *c)
{
    uint8_t b[BCM_AWDL_OPMODE_LEN];
    if (c->role == AWDL_ROLE_FOLLOWER) {
        bcm_awdl_opmode(b, BCM_AWDL_ROLE_SLAVE, c->adv.parent);
    } else {
        bcm_awdl_opmode(b, BCM_AWDL_ROLE_MASTER, c->cfg.addr);
    }
    fw_set(c, "awdl_opmode", b, sizeof b);
}

static void native_sync_params(struct awdl_core *c)
{
    uint8_t b[BCM_AWDL_SYNC_PARAMS_LEN];
    fw_set(c, "awdl_sync_params", b, bcm_awdl_sync_params(b, 6));
}

// The series' brcmf_awdl_refresh: our frames' TLVs and the synchronisation state again.
static void native_refresh(struct awdl_core *c)
{
    uint8_t tlvs[256];
    uint8_t b[512];
    struct awdl_writer w = {tlvs, sizeof tlvs, 0, 0, 0};
    awdl_write_extras(c, &w, 0);  // out-of-band frames: what a PSF carries beyond sync and election
    fw_set(c, "awdl_oob_af_auto", b, w.overflow ? 0 : bcm_awdl_oob_auto(b, sizeof b, tlvs, w.at));
    native_payload(c);
    native_sync_params(c);
    native_opmode(c);
    native_election(c);
    native_sync_params(c);
    c->stats.syncs_pushed++;
    c->pushed_role = (uint8_t)c->role;
    memcpy(c->pushed_parent, c->adv.parent, 6);
    memcpy(c->pushed_root, c->adv.root, 6);
}

static void native_push_sync(struct awdl_core *c)
{
    if (c->nstate != AWDL_N_UP)
        return;
    if (c->pushed_role == (uint8_t)c->role && awdl_addr_eq(c->pushed_parent, c->adv.parent) &&
        awdl_addr_eq(c->pushed_root, c->adv.root))
        return;
    native_refresh(c);
}

void awdl_native_fail(struct awdl_core *c, const char *fmt, ...)
{
    char why[120];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    awdl_log(c, "AWDL: %s: AWDL off", why);
    c->nstate = AWDL_N_OFF;
    c->create_deadline = c->aftx_at = c->settle_at = 0;
    c->running = 0;
    c->role = AWDL_ROLE_OFF;
}

// 7.x: the variable is awdl_if (the name Apple's drivers send), or awdl_if2 when only that is in the table.
static const char *awdl_if_name(const struct awdl_core *c)
{
    return fw_absent(c, "awdl_if") && !fw_absent(c, "awdl_if2") ? "awdl_if2" : "awdl_if";
}

// The AWDL interface: interface_create v3 (a GET carrying the request, on the station's interface), or 7.x awdl_if.
static void native_create(struct awdl_core *c, uint8_t api, uint64_t now)
{
    uint8_t b[BCM_IFC_V3_LEN];
    c->created_with = api;
    c->payload_u16 = api == BCM_AWDL_API_IFC_V3;
    c->payload_switched = 0;
    c->seq_no_empty = 0;
    if (api == BCM_AWDL_API_IFC_V3) {
        fw_req(c, 0, 1, "interface_create", b, bcm_ifc_v3_awdl(b, c->cfg.addr));
    } else {
        fw_req(c, 0, 0, awdl_if_name(c), b, bcm_awdl_if2(b, BCM_AWDL_IFIDX, 1, awdl_bssid, c->cfg.addr));
    }
    c->nstate = AWDL_N_CREATING;
    c->create_deadline = now + AWDL_CREATE_TIMEOUT_US;
    awdl_log(c, "AWDL: making the firmware's AWDL interface (%s, %02x:%02x:%02x:%02x:%02x:%02x)",
             bcm_awdl_api_name((enum bcm_awdl_api)api), c->cfg.addr[0], c->cfg.addr[1], c->cfg.addr[2], c->cfg.addr[3],
             c->cfg.addr[4], c->cfg.addr[5]);
}

// The series' brcmf_awdl_enable, in its order, on the AWDL interface; the 7.x firmware gets its own forms of
// awdl_config (the 28-byte struct), the payload length (u32) and the sequence (awdl_chan_seq), and skips what the
// probe found missing.
static void native_enable(struct awdl_core *c, uint64_t now)
{
    uint8_t b[BCM_AWDL_CONFIG_LEN];
    const int v3 = c->created_with == BCM_AWDL_API_IFC_V3;
    awdl_native_build_seq(c);
    native_payload(c);
    fw_u32(c, "awdl_aftxmode", 0);
    native_sync_params(c);
    fw_u32(c, "awdl_osoc_chan", 0);
    if (v3) {
        fw_u32(c, "awdl_config", BCM_AWDL_CONFIG_START);
    } else {
        struct bcm_awdl_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.channel = social5_of(c->cfg.seq);
        cfg.aw_period = AWDL_AW_TU;
        cfg.aw_cmn_length = AWDL_AW_TU;
        cfg.action_frame_period = AWDL_PSF_INTERVAL_TU;
        cfg.pkt_lifetime_ms = 500;
        cfg.max_no_master = 8;
        cfg.ext_count = 3;
        cfg.aw_ext_length = AWDL_AW_TU;
        cfg.nmode = 2;  // 11n with A-MPDU
        memset(cfg.dest, 0xff, 6);
        fw_set(c, "awdl_config", b, bcm_awdl_config_encode(b, &cfg));
    }
    uint8_t d[BCM_AWDL_PSF_DWELL_LEN];
    fw_set(c, "awdl_psf_dwell", d, bcm_awdl_psf_dwell(d, 1));
    native_schedule(c);
    native_payload(c);
    native_election(c);
    fw_u32(c, "awdl", 1);
    fw_u32(c, "awdl_presencemode", BCM_AWDL_PRESENCEMODE);
    native_schedule(c);
    native_refresh(c);
    c->aftx_at = now + AWDL_AFTX_DELAY_US;
    c->settle_at = now + AWDL_SETTLE_DELAY_US;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        if (c->peers[i].used && c->peers[i].reachable)
            fw_peer_op(c, BCM_AWDL_PEER_OP_ADD, &c->peers[i]);
    }
    awdl_log(c, "AWDL: started (firmware AWDL, %s), ifidx %d bsscfg %u, social %u/%u, infra %u MHz",
             bcm_awdl_api_name((enum bcm_awdl_api)c->created_with), (int)c->fw_ifidx, c->fw_bsscfg,
             awdl_chan_primary(c->cfg.seq[0]), awdl_chan_primary(c->cfg.seq[9]), c->infra_mhz);
    awdl_sched_log(c, "%s", "start");
}

// One line per schedule: the mode, why, and how many slots leave the access point's channel.

void awdl_native_resend(struct awdl_core *c, uint16_t old_map)
{
    if (c->nstate != AWDL_N_UP)
        return;
    native_schedule(c);
    if (awdl_social_map(c->cfg.seq) != old_map)
        native_payload(c);
}

static void native_deadlines(struct awdl_core *c, uint64_t now)
{
    if (c->nstate == AWDL_N_CREATING && c->create_deadline && now >= c->create_deadline) {
        awdl_native_fail(c, "no WLC_E_IF for the AWDL interface %u ms after %s", AWDL_CREATE_TIMEOUT_US / 1000,
                         c->created_with == BCM_AWDL_API_IFC_V3 ? "interface_create" : awdl_if_name(c));
        return;
    }
    if (c->nstate != AWDL_N_UP)
        return;
    if (c->aftx_at && now >= c->aftx_at) {
        c->aftx_at = 0;
        fw_u32(c, "awdl_aftxmode", 0);
        const uint8_t s = social5_of(c->cfg.seq);  // 44 on interface_create v3 (the series), else our social channel
        fw_u32(c, "awdl_osoc_chan", s == 44 || s == 149 ? s : BCM_AWDL_OSOC_CHAN);
    }
    if (c->settle_at && now >= c->settle_at) {
        c->settle_at = 0;
        if (c->created_with == BCM_AWDL_API_IFC_V3)
            fw_u32(c, "awdl_config", BCM_AWDL_CONFIG_SETTLED);
        uint8_t d[BCM_AWDL_PSF_DWELL_LEN];
        fw_set(c, "awdl_psf_dwell", d, bcm_awdl_psf_dwell(d, 0));
    }
    awdl_sched_deadline(c, now);
}

static uint64_t native_next(const struct awdl_core *c)
{
    uint64_t t = c->next_slot;
    if (c->nstate == AWDL_N_UP && awdl_sched_next(c) < t)
        t = awdl_sched_next(c);
    if (c->nstate == AWDL_N_CREATING && c->create_deadline && c->create_deadline < t)
        t = c->create_deadline;
    if (c->aftx_at && c->aftx_at < t)
        t = c->aftx_at;
    if (c->settle_at && c->settle_at < t)
        t = c->settle_at;
    return t;
}

static void native_begin(struct awdl_core *c, uint64_t now)
{
    c->fw_ifidx = -1;
    c->fw_bsscfg = 0;
    c->cfg.self_metric = BCM_AWDL_SELF_METRIC;  // what the firmware advertises from awdl_election_tree
    c->self_counter = 0;
    c->role = AWDL_ROLE_ROOT;  // our own master until the election hears a better one (the series starts so)
    memcpy(c->adv.root, c->cfg.addr, 6);
    memcpy(c->adv.parent, c->cfg.addr, 6);
    c->adv.root_metric = c->adv.self_metric = c->cfg.self_metric;
    c->adv.hops = 0;
    c->pushed_role = 0xff;
    c->created_with = BCM_AWDL_API_NONE;
    awdl_sched_start(c);
    if (c->cfg.fw_api != BCM_AWDL_API_IFC_V3 && c->cfg.fw_api != BCM_AWDL_API_AWDL_IF) {
        awdl_native_fail(c, "%s",
                         "the firmware has neither interface_create v3 nor awdl_if (see the AWDL probe lines)");
        return;
    }
    native_create(c, c->cfg.fw_api, now);
}

void awdl_native_stop(struct awdl_core *c)
{
    uint8_t b[BCM_AWDL_IF2_LEN];
    if (c->nstate == AWDL_N_UP && c->fw_ifidx >= 0) {
        fw_u32(c, "awdl", 0);
        if (c->created_with == BCM_AWDL_API_IFC_V3)
            fw_set(c, "interface_remove", NULL, 0);
    }
    if (c->created_with == BCM_AWDL_API_AWDL_IF && c->nstate != AWDL_N_OFF)
        fw_req(c, 0, 0, awdl_if_name(c), b, bcm_awdl_if2(b, BCM_AWDL_IFIDX, 0, awdl_bssid, c->cfg.addr));
    c->nstate = AWDL_N_OFF;
    c->create_deadline = c->aftx_at = c->settle_at = 0;
}

void awdl_core_fw_result(struct awdl_core *c, const char *name, int status, uint64_t now)
{
    if (!status || !awdl_native(c))
        return;
    c->stats.iovar_refused++;
    if (!c->running)
        return;
    const int create =
        !strcmp(name, "interface_create") ||
        (!strncmp(name, "awdl_if", 7) && (!name[7] || !strcmp(name + 7, "2")) && c->nstate == AWDL_N_CREATING);
    if (create && c->nstate == AWDL_N_CREATING) {
        if (c->created_with == BCM_AWDL_API_IFC_V3 && (!fw_absent(c, "awdl_if") || !fw_absent(c, "awdl_if2"))) {
            awdl_log(c, "AWDL: interface_create refused (%d %s), trying awdl_if", status, bcme_name(status));
            native_create(c, BCM_AWDL_API_AWDL_IF, now);
            return;
        }
        awdl_native_fail(c, "the firmware refused %s (%d %s)", name, status, bcme_name(status));
        return;
    }
    // 7.x: empty slots (unassociated) are the one input of awdl_chan_seq no Apple trace shows a 7.x firmware taking
    if (!strcmp(name, "awdl_chan_seq") && !c->seq_no_empty &&
        (status == BCME_BADCHAN || status == BCME_OUTOFRANGECHAN || status == BCME_BADARG || status == BCME_BADLEN)) {
        int empty = 0;
        for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
            if (!c->cfg.seq[i].number)
                empty = 1;
        }
        if (empty) {
            c->seq_no_empty = 1;
            awdl_native_build_seq(c);
            awdl_log(c, "AWDL: awdl_chan_seq refused (%d %s) with empty slots: sending them on channel %u", status,
                     bcme_name(status), awdl_chan_primary(c->cfg.seq[0]));
            native_schedule(c);
            return;
        }
    }
    if (!strcmp(name, "awdl_payload") && !c->payload_switched &&
        (status == BCME_BADLEN || status == BCME_BUFTOOSHORT || status == BCME_BUFTOOLONG || status == BCME_BADARG)) {
        c->payload_switched = 1;
        c->payload_u16 = !c->payload_u16;
        awdl_log(c, "AWDL: awdl_payload refused (%d %s): sending its length as a u%u", status, bcme_name(status),
                 c->payload_u16 ? 16 : 32);
        native_payload(c);
    }
}

int awdl_core_fw_ifidx(const struct awdl_core *c)
{
    return c->nstate == AWDL_N_UP ? c->fw_ifidx : -1;
}

void awdl_native_if_up(struct awdl_core *c, uint8_t ifidx, uint8_t bssidx, uint64_t now)
{
    c->fw_ifidx = ifidx;
    c->fw_bsscfg = bssidx;
    c->nstate = AWDL_N_UP;
    c->create_deadline = 0;
    native_enable(c, now);
}

void awdl_native_start(struct awdl_core *c, uint64_t now)
{
    c->next_slot = now + 1000000;  // peer expiry and the election once a second
    native_begin(c, now);
}

uint64_t awdl_native_timer(struct awdl_core *c, uint64_t now)
{
    native_deadlines(c, now);
    if (!c->running)
        return UINT64_MAX;
    if (now >= c->next_slot) {
        awdl_expire_peers(c, now);
        awdl_run_election(c, now);  // a master gone quiet: we may be our own again
        native_push_sync(c);
        c->next_slot = now + 1000000;
    }
    return native_next(c);
}

// The host elects and the firmware keeps the time (the series: macOS runs the election above the firmware): a new
// master or parent is told to the firmware.
void awdl_native_after_af(struct awdl_core *c)
{
    native_push_sync(c);
}

void awdl_native_peer(struct awdl_core *c, const struct awdl_peer *p, int add)
{
    if (c->nstate == AWDL_N_UP)
        fw_peer_op(c, add ? BCM_AWDL_PEER_OP_ADD : BCM_AWDL_PEER_OP_DEL, p);
}

// The firmware's AWDL interface schedules; the host adds the AWDL data header (the series).
int awdl_native_output(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now)
{
    const size_t n = c->nstate == AWDL_N_UP ? bcm_awdl_encap(eth, len, c->data_seq, c->scratch, sizeof c->scratch) : 0;
    if (!n || !c->host.tx_eth || c->host.tx_eth(c->host.ctx, c->scratch, n)) {
        c->stats.tx_refused++;
        return -3;
    }
    c->data_seq++;
    c->stats.data_tx++;
    awdl_sched_traffic(c, eth, len, 1, now);
    return 0;
}
