// SPDX-License-Identifier: BSD-2-Clause-Patent
// AWDL on the air: action frame parsing, the TLV encoders the core uses, and the data frame encapsulation. Layouts
// as described in awdl.h; every read is bounds-checked against the frame length.
#include "awdl_internal.h"

const uint8_t awdl_bssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
const uint8_t awdl_oui[3] = {0x00, 0x17, 0xf2};
static const uint8_t kLlc[8] = {0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00};

// Apple's frames name a 5 GHz 80 MHz slot by its primary channel with operating class 0x80 ({149, 0x80}, OWL's
// CHAN_OPCLASS_149), not by the block's centre as IEEE 802.11 Annex E would. A centre index is still read as its
// block's AWDL primary, so either form a peer sends lands on the right channel.
uint8_t awdl_chan_primary(struct awdl_chan c)
{
    if (c.opclass != 0x80 && c.opclass != 0x81)
        return c.number;
    switch (c.number) {
    case 42:
        return 44;
    case 155:
        return 149;
    case 58:
    case 106:
    case 122:
    case 138:
    case 171:
        return (uint8_t)(c.number - 6);
    default:
        return c.number;
    }
}

uint8_t awdl_chan_centre80(uint8_t primary)
{
    if (primary >= 149 && primary <= 161)
        return 155;
    if (primary >= 36 && primary <= 144)
        return (uint8_t)(primary - (primary - 36) % 16 + 6);
    return 0;
}

uint8_t awdl_opclass_20mhz(uint8_t ch)
{
    if (ch >= 1 && ch <= 13)
        return 81;
    if (ch == 14)
        return 82;
    if (ch >= 36 && ch <= 48)
        return 115;
    if (ch >= 52 && ch <= 64)
        return 118;
    if (ch >= 100 && ch <= 144)
        return 121;
    if (ch >= 149 && ch <= 161)
        return 124;
    if (ch >= 165 && ch <= 177)
        return 125;
    return 0;
}

struct awdl_chan awdl_chan_of_freq(uint16_t mhz)
{
    uint8_t ch = 0;
    if (mhz == 2484) {
        ch = 14;
    } else if (mhz >= 2412 && mhz <= 2472) {
        ch = (uint8_t)((mhz - 2407) / 5);
    } else if (mhz >= 5180 && mhz <= 5885) {
        ch = (uint8_t)((mhz - 5000) / 5);
    }
    const uint8_t opclass = awdl_opclass_20mhz(ch);
    return opclass ? (struct awdl_chan){ch, opclass} : (struct awdl_chan){0, 0};
}

uint16_t awdl_chan_freq(struct awdl_chan c)
{
    const uint8_t p = awdl_chan_primary(c);
    if (!p)
        return 0;
    if (p <= 14)
        return p == 14 ? 2484 : (uint16_t)(2407 + 5 * p);
    return (uint16_t)(5000 + 5 * p);
}

// ---- reading -----------------------------------------------------------------------------------------------------
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Channel sequence as carried in TLV 18 and at the end of TLV 4. Returns bytes consumed or -1.
static int parse_chanseq(const uint8_t *p, size_t n, struct awdl_chanseq *s)
{
    if (n < 6)
        return -1;
    memset(s, 0, sizeof *s);
    const unsigned count = (unsigned)p[0] + 1;
    s->encoding = p[1];
    s->duplicate_count = p[2];
    s->step_count = p[3];
    s->fill_channel = rd16(p + 4);
    const unsigned width = s->encoding == AWDL_CHAN_ENC_SIMPLE ? 1 : 2;
    if (s->encoding != AWDL_CHAN_ENC_SIMPLE && s->encoding != AWDL_CHAN_ENC_LEGACY &&
        s->encoding != AWDL_CHAN_ENC_OPCLASS)
        return -1;
    if (6 + count * width > n)
        return -1;
    s->count = (uint8_t)(count > AWDL_SEQ_SLOTS ? AWDL_SEQ_SLOTS : count);
    for (unsigned i = 0; i < s->count; ++i) {
        const uint8_t *e = p + 6 + (size_t)i * width;
        if (s->encoding == AWDL_CHAN_ENC_SIMPLE) {
            s->chan[i] = (struct awdl_chan){e[0], awdl_opclass_20mhz(e[0])};
        } else if (s->encoding == AWDL_CHAN_ENC_LEGACY) {  // {flags, channel}
            s->chan[i] = (struct awdl_chan){e[1], awdl_opclass_20mhz(e[1])};
        } else {
            s->chan[i] = (struct awdl_chan){e[0], e[1]};
        }
    }
    return (int)(6 + count * width);
}

static int parse_sync(const uint8_t *v, size_t n, struct awdl_sync_params *s)
{
    if (n < 33)
        return -1;
    memset(s, 0, sizeof *s);
    s->tx_channel = v[0];
    s->tx_counter = rd16(v + 1);
    s->master_channel = v[3];
    s->guard_time = v[4];
    s->aw_period = rd16(v + 5);
    s->af_period = rd16(v + 7);
    s->flags = rd16(v + 9);
    s->aw_ext_length = rd16(v + 11);
    s->aw_cmn_length = rd16(v + 13);
    s->aw_remaining = rd16(v + 15);
    s->min_ext = v[17];
    s->max_ext_mcast = v[18];
    s->max_ext_ucast = v[19];
    s->max_ext_af = v[20];
    memcpy(s->master, v + 21, 6);
    s->presence_mode = v[27];
    s->aw_seq = rd16(v + 29);
    s->ap_beacon_delta = (int16_t)rd16(v + 31);
    if (n > 33 && parse_chanseq(v + 33, n - 33, &s->seq) > 0)
        s->has_seq = 1;
    return 0;
}

static void parse_dps(const uint8_t *v, size_t n, struct awdl_data_path_state *d)
{
    memset(d, 0, sizeof *d);
    if (n < 2)
        return;
    d->flags = rd16(v);
    size_t at = 2;
#define NEED(k)           \
    do {                  \
        if (at + (k) > n) \
            return;       \
    } while (0)
    if (d->flags & AWDL_DPS_COUNTRY_CODE) {
        NEED(3);
        memcpy(d->country, v + at, 2);
        at += 3;
    }
    if (d->flags & AWDL_DPS_SOCIAL_CHANNEL_MAP) {
        NEED(2);
        d->social_channel_map = rd16(v + at);
        at += 2;
    }
    if (d->flags & AWDL_DPS_INFRA_INFO) {
        NEED(8);
        memcpy(d->infra_bssid, v + at, 6);
        d->infra_channel = rd16(v + at + 6);
        at += 8;
    }
    if (d->flags & AWDL_DPS_INFRA_ADDRESS) {
        NEED(6);
        memcpy(d->infra_address, v + at, 6);
        at += 6;
    }
    if (d->flags & AWDL_DPS_AWDL_ADDRESS) {
        NEED(6);
        memcpy(d->awdl_address, v + at, 6);
    }
#undef NEED
}

// Arpa: flags, then the host name as DNS labels; a 0xc0 compression byte ends it (AWDL compresses ".local").
static void parse_arpa(const uint8_t *v, size_t n, char *out, size_t size)
{
    size_t at = 1;
    size_t w = 0;
    out[0] = 0;
    while (at < n && v[at] && v[at] < 0xc0) {
        const size_t l = v[at++];
        if (at + l > n)
            break;
        if (w && w + 1 < size)
            out[w++] = '.';
        for (size_t i = 0; i < l && w + 1 < size; ++i) {
            const uint8_t ch = v[at + i];
            out[w++] = (char)(ch >= 0x20 && ch < 0x7f ? ch : '?');
        }
        at += l;
    }
    out[w < size ? w : size - 1] = 0;
}

int awdl_af_parse(const uint8_t *f, size_t len, struct awdl_af *a)
{
    // management / action, any destination; BSSID must be AWDL's
    if (len < 24 + 16 || (f[0] & 0xfc) != 0xd0)
        return -1;
    if (memcmp(f + 16, awdl_bssid, 6))
        return -2;
    const uint8_t *b = f + 24;
    if (b[0] != 0x7f || memcmp(b + 1, awdl_oui, 3) || b[4] != 0x08)
        return -3;
    memset(a, 0, sizeof *a);
    memcpy(a->dst, f + 4, 6);
    memcpy(a->src, f + 10, 6);
    a->version = b[5];
    a->subtype = b[6];
    a->phy_tx_time = rd32(b + 8);
    a->target_tx_time = rd32(b + 12);
    size_t at = 24 + 16;
    while (at + 3 <= len) {
        const uint8_t type = f[at];
        const size_t l = rd16(f + at + 1);
        if (at + 3 + l > len)
            return -4;  // truncated TLV: the whole frame is suspect
        const uint8_t *v = f + at + 3;
        int ok = 1;
        switch (type) {
        case AWDL_TLV_SYNC_PARAMS:
            ok = parse_sync(v, l, &a->sync) == 0;
            break;
        case AWDL_TLV_ELECTION_PARAMS:
            if (l < 19) {
                ok = 0;
                break;
            }
            a->election.flags = v[0];
            a->election.id = rd16(v + 1);
            a->election.distance = v[3];
            memcpy(a->election.master, v + 5, 6);
            a->election.master_metric = rd32(v + 11);
            a->election.self_metric = rd32(v + 15);
            break;
        case AWDL_TLV_ELECTION_PARAMS_V2:
            if (l < 40) {
                ok = 0;
                break;
            }
            memcpy(a->election2.root, v, 6);
            memcpy(a->election2.parent, v + 6, 6);
            a->election2.root_counter = rd32(v + 12);
            a->election2.hops = rd32(v + 16);
            a->election2.root_metric = rd32(v + 20);
            a->election2.self_metric = rd32(v + 24);
            a->election2.id = rd32(v + 28);
            a->election2.flags = rd32(v + 32);
            a->election2.self_counter = rd32(v + 36);
            break;
        case AWDL_TLV_CHANNEL_SEQUENCE:
            ok = parse_chanseq(v, l, &a->chanseq) > 0;
            break;
        case AWDL_TLV_DATA_PATH_STATE:
            parse_dps(v, l, &a->dps);
            break;
        case AWDL_TLV_SERVICE_PARAMS:
            if (l < 5) {
                ok = 0;
                break;
            }
            a->service_update_indicator = rd16(v + 3);
            break;
        case AWDL_TLV_VERSION:
            if (l < 2) {
                ok = 0;
                break;
            }
            a->awdl_version = v[0];
            a->device_class = v[1];
            break;
        case AWDL_TLV_ARPA:
            parse_arpa(v, l, a->hostname, sizeof a->hostname);
            break;
        case AWDL_TLV_HT_CAPS:  // 2 reserved bytes, capability info, A-MPDU parameters, MCS bitmask per stream
            if (l < 4) {
                ok = 0;
                break;
            }
            a->ht_caps = rd16(v + 2);
            a->ht_ampdu = l >= 5 ? v[4] : 0;
            a->ht_mcs[0] = l >= 6 ? v[5] : 0;
            a->ht_mcs[1] = l >= 9 ? v[6] : 0;  // a second stream's byte only when the TLV has room for it
            break;
        default:
            break;
        }
        if (ok && type < 32)
            a->present |= 1u << type;
        at += 3 + l;
    }
    return 0;
}

// ---- writing -----------------------------------------------------------------------------------------------------
void awdl_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void awdl_put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

// A TLV writer over a fixed buffer: begin() reserves the 3-byte header, end() fills in the length.
int awdl_w_begin(struct awdl_writer *w, uint8_t type)
{
    if (w->at + 3 > w->size) {
        w->overflow = 1;
        return -1;
    }
    w->tlv = w->at;
    w->buf[w->at] = type;
    w->at += 3;
    return 0;
}

void awdl_w_end(struct awdl_writer *w)
{
    if (!w->overflow)
        awdl_put16(w->buf + w->tlv + 1, (uint16_t)(w->at - w->tlv - 3));
}

void awdl_w_bytes(struct awdl_writer *w, const void *p, size_t n)
{
    if (w->at + n > w->size) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->at, p, n);
    w->at += n;
}

void awdl_w_u8(struct awdl_writer *w, uint8_t v)
{
    awdl_w_bytes(w, &v, 1);
}

void awdl_w_u16(struct awdl_writer *w, uint16_t v)
{
    uint8_t b[2];
    awdl_put16(b, v);
    awdl_w_bytes(w, b, 2);
}

void awdl_w_u32(struct awdl_writer *w, uint32_t v)
{
    uint8_t b[4];
    awdl_put32(b, v);
    awdl_w_bytes(w, b, 4);
}

// The 16-entry operating-class sequence (count = entries - 1, step 3, no duplicates, fill 0xffff).
void awdl_w_chanseq(struct awdl_writer *w, const struct awdl_chan seq[AWDL_SEQ_SLOTS])
{
    awdl_w_u8(w, AWDL_SEQ_SLOTS - 1);
    awdl_w_u8(w, AWDL_CHAN_ENC_OPCLASS);
    awdl_w_u8(w, 0);
    awdl_w_u8(w, 3);
    awdl_w_u16(w, 0xffff);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        awdl_w_u8(w, seq[i].number);
        awdl_w_u8(w, seq[i].opclass);
    }
}

// ---- data frames ---------------------------------------------------------------------------------------------------
size_t awdl_data_encap(const uint8_t *eth, size_t len, const uint8_t self[6], uint16_t seq, uint8_t *out, size_t size)
{
    if (len < 14)
        return 0;
    const size_t payload = len - 14;
    const size_t total = 24 + 8 + 8 + payload;
    if (total > size)
        return 0;
    memset(out, 0, 24);
    out[0] = 0x08;              // data, FromDS = ToDS = 0
    memcpy(out + 4, eth, 6);    // A1: destination
    memcpy(out + 10, self, 6);  // A2: us
    memcpy(out + 16, awdl_bssid, 6);
    awdl_put16(out + 22, (uint16_t)((seq & 0xfffu) << 4));
    memcpy(out + 24, kLlc, 8);
    out[32] = 0x03;  // AWDL data header: 0x0403, sequence, reserved, ethertype (network order)
    out[33] = 0x04;
    awdl_put16(out + 34, seq);
    out[36] = out[37] = 0;
    out[38] = eth[12];
    out[39] = eth[13];
    memcpy(out + 40, eth + 14, payload);
    return total;
}

// One MSDU's LLC/SNAP and AWDL data header (03 04, le16 sequence, 2 reserved, be16 ethertype), then its payload.
static int msdu_at(const uint8_t *p, size_t n, const uint8_t *dst, const uint8_t *src, struct awdl_msdu *m,
                   uint16_t *seq)
{
    if (n < 16 || memcmp(p, kLlc, 8) || p[8] != 0x03 || p[9] != 0x04)
        return 0;
    m->dst = dst;
    m->src = src;
    m->ethertype = (uint16_t)(((unsigned)p[14] << 8) | p[15]);
    m->payload = p + 16;
    m->len = n - 16;
    if (seq)
        *seq = rd16(p + 10);
    return 1;
}

size_t awdl_data_msdus(const uint8_t *f, size_t len, struct awdl_msdu *out, size_t max, uint16_t *seq, int *retry)
{
    if (len < 24 || (f[0] & 0x0c) != 0x08 || (f[1] & 0x03) || memcmp(f + 16, awdl_bssid, 6) || !max)
        return 0;  // not data, a DS bit set, or not the AWDL BSS
    if ((f[0] & 0x40) == 0x40)
        return 0;  // a null subtype carries no payload
    size_t at = 24;
    int amsdu = 0;
    if (f[0] & 0x80) {  // QoS data: QoS control (bit 7: A-MSDU present), then HT control if the order bit is set
        if (at + 2 > len)
            return 0;
        amsdu = (f[at] & 0x80) != 0;
        at += 2;
        if (f[1] & 0x80)
            at += 4;
    }
    if (at > len)
        return 0;
    if (retry)
        *retry = (f[1] & 0x08) != 0;
    if (!amsdu)
        return (size_t)msdu_at(f + at, len - at, f + 4, f + 10, out, seq);
    // A-MSDU (Apple aggregates AWDL data): subframes of DA, SA, be16 length and the MSDU, each padded to 4 bytes.
    size_t n = 0;
    while (at + 14 <= len && n < max) {
        const size_t l = ((size_t)f[at + 12] << 8) | f[at + 13];
        if (at + 14 + l > len)
            return 0;
        if (!msdu_at(f + at + 14, l, f + at, f + at + 6, &out[n], n ? NULL : seq))
            return 0;
        ++n;
        at += 14 + l;
        at += (4 - (14 + l) % 4) % 4;
    }
    return n;
}

size_t awdl_data_decap(const uint8_t *f, size_t len, uint8_t *out, size_t size, uint16_t *seq, int *retry)
{
    struct awdl_msdu m;
    if (awdl_data_msdus(f, len, &m, 1, seq, retry) != 1)
        return 0;
    return awdl_msdu_ether(&m, out, size);
}

size_t awdl_msdu_ether(const struct awdl_msdu *m, uint8_t *out, size_t size)
{
    if (14 + m->len > size)
        return 0;
    memcpy(out, m->dst, 6);
    memcpy(out + 6, m->src, 6);
    out[12] = (uint8_t)(m->ethertype >> 8);
    out[13] = (uint8_t)m->ethertype;
    memcpy(out + 14, m->payload, m->len);
    return 14 + m->len;
}
