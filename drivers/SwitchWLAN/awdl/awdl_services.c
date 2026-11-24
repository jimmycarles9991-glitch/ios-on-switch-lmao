// SPDX-License-Identifier: BSD-2-Clause-Patent
// wifip2pd's service records and their on-air form. Apple's driver expands the daemon's name pointers and then
// substitutes AWDL dictionary entries at every byte, longest first; this does the same, byte for byte.
//
// wifip2pd hands over mDNSResponder's D2D records. Key: the record name, the big-endian record type and D2D's
// compression version 1. Names point into D2D's base message: c0 0c "_tcp.local", c0 11 "local", c0 1c "_udp.local",
// and in record data c0 27 is the record's own name. On air a record is its name, a one-byte type and its data, with
// names pointing into AWDL's fixed dictionary and c0 00 standing for the record's own name. The reverse-lookup PTR
// record is not sent as a record: its target is the ARPA TLV's host name.
#include "awdl_internal.h"

#if defined(SWITCH_IPSW_KIND_24B119)
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

enum {
    DNS_PTR = 12,
    DNS_TXT = 16,
    DNS_SRV = 33,
};

#define SWITCH_AWDL_ENTRY_HEAD 2u  // the key's hash, then 1 for the reverse-lookup record

struct switch_awdl_label_run {
    uint8_t code;
    size_t bytes;
    const char *labels;
};

#define SWITCH_AWDL_LABEL_RUN(code, labels) {(code), sizeof(labels) - 1, (labels)}

static const struct switch_awdl_label_run d2d_pointers[] = {
    SWITCH_AWDL_LABEL_RUN(0x0c, "\x04_tcp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x1c, "\x04_udp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x11, "\x05local"),
};

// Tried in this order at every byte, so the longer entries win.
static const struct switch_awdl_label_run awdl_dictionary[] = {
    SWITCH_AWDL_LABEL_RUN(0x01, "\x08_airplay\x04_tcp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x02, "\x08_airplay\x04_udp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x03, "\x08_airplay"),
    SWITCH_AWDL_LABEL_RUN(0x04, "\x05_raop\x04_tcp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x05, "\x05_raop\x04_udp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x06, "\x05_raop"),
    SWITCH_AWDL_LABEL_RUN(0x07, "\x08_airdrop\x04_tcp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x08, "\x08_airdrop\x04_udp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x09, "\x08_airdrop"),
    SWITCH_AWDL_LABEL_RUN(0x0a, "\x04_tcp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x0b, "\x04_udp\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x0c, "\x05local"),
    SWITCH_AWDL_LABEL_RUN(0x0d, "\x03ip6\x04"
                                "arpa"),
    SWITCH_AWDL_LABEL_RUN(0x0e, "\x03ip4\x04"
                                "arpa"),
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void log_once(struct awdl_core *c, uint8_t set[32], uint8_t value, const char *what)
{
    const uint8_t bit = (uint8_t)(1u << (value & 7));
    if (!(set[value >> 3] & bit)) {
        set[value >> 3] |= bit;
        awdl_log(c, "AWDL: wifip2pd service record %s %02x", what, (unsigned)value);
    }
}

// As in Apple's driver, a pointer is c0 xx anywhere in the name, not only at a label start. Without `own` (a key),
// c0 27 has nothing to stand for.
static int d2d_expand(struct awdl_core *c, struct switch_awdl_dns_name *out, const uint8_t *in, size_t length,
                      const struct switch_awdl_dns_name *own)
{
    size_t at = 0;
    for (size_t i = 0; i < length;) {
        const uint8_t *run = in + i;
        size_t bytes = 1, used = 1;
        if (in[i] == 0xc0) {
            if (i + 1 >= length)
                return EINVAL;
            run = NULL;
            used = 2;
            if (in[i + 1] == 0x27 && own) {
                run = own->bytes;
                bytes = own->length;
            }
            for (unsigned k = 0; k < sizeof d2d_pointers / sizeof d2d_pointers[0]; ++k) {
                if (d2d_pointers[k].code == in[i + 1]) {
                    run = (const uint8_t *)d2d_pointers[k].labels;
                    bytes = d2d_pointers[k].bytes;
                }
            }
            if (!run) {
                log_once(c, c->switch_refused_pointers, in[i + 1], "refused, name pointer c0");
                return EOPNOTSUPP;
            }
        }
        if (at + bytes >= SWITCH_AWDL_NAME_MAX)
            return EINVAL;
        memcpy(out->bytes + at, run, bytes);
        at += bytes;
        i += used;
    }
    out->length = at;
    return 0;
}

// Up to the first zero byte, the record's own name (c0 00) or a dictionary entry wherever its bytes match. The own name
// is compared as a C string, as Apple's driver compares it, so the zero bytes that end it need not all be present.
static size_t awdl_compress(struct awdl_writer *w, const struct switch_awdl_dns_name *in,
                            const struct switch_awdl_dns_name *own)
{
    const size_t start = w->at;
    const char *text = (const char *)in->bytes;
    for (size_t i = 0; i < in->length && in->bytes[i];) {
        if (own && i + own->length <= in->length && !strncmp(text + i, (const char *)own->bytes, own->length)) {
            awdl_w_u8(w, 0xc0);
            awdl_w_u8(w, 0x00);
            i += own->length;
            continue;
        }
        const struct switch_awdl_label_run *hit = NULL;
        for (unsigned k = 0; !hit && k < sizeof awdl_dictionary / sizeof awdl_dictionary[0]; ++k) {
            const struct switch_awdl_label_run *d = &awdl_dictionary[k];
            if (in->length - i >= d->bytes && !memcmp(text + i, d->labels, d->bytes))
                hit = d;
        }
        if (hit) {
            awdl_w_u8(w, 0xc0);
            awdl_w_u8(w, hit->code);
            i += hit->bytes;
        } else {
            awdl_w_u8(w, in->bytes[i++]);
        }
    }
    const size_t bytes = w->at - start;
    return bytes <= SWITCH_AWDL_NAME_MAX && !w->overflow ? bytes : 0;
}

static uint8_t crc8_ccitt(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        crc = (uint8_t)(crc ^ *p++);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

static int is_reverse_lookup(const struct switch_awdl_dns_name *n)
{
    static const char suffix[] = "\x03ip6\x04"
                                 "arpa";  // with its terminator
    size_t end = 0;
    while (end < n->length && n->bytes[end])
        end++;
    return end >= sizeof suffix - 1 && end < n->length &&
           !memcmp(n->bytes + end - (sizeof suffix - 1), suffix, sizeof suffix);
}

static int write_data(struct awdl_core *c, struct awdl_writer *w, uint8_t type, const uint8_t *value, size_t length)
{
    if (type != DNS_PTR && type != DNS_SRV) {
        awdl_w_bytes(w, value, length);
        return 0;
    }
    const size_t fixed = type == DNS_SRV ? 6u : 0u;  // priority, weight, port
    if (length < fixed)
        return EINVAL;
    awdl_w_bytes(w, value, fixed);
    struct switch_awdl_dns_name *target = &c->switch_service_target;
    const int e = d2d_expand(c, target, value + fixed, length - fixed, &c->switch_service_key);
    if (e)
        return e;
    if (!target->length || target->bytes[target->length - 1])
        target->bytes[target->length++] = 0;
    return awdl_compress(w, target, &c->switch_service_key) ? 0 : EINVAL;
}

// TLV 1 (browse): type, length, four bytes Apple's driver ignores, key. TLV 2 (advertise): type, length, key length,
// key, value length, fragment offset, value. Either key is the name, the big-endian record type and version 1.
int switch_awdl_service_stage(struct awdl_core *c, const uint8_t *tlv, size_t length)
{
    if (length < 3 || le16(tlv + 1) != length - 3)
        return EINVAL;
    const int response = tlv[0] == AWDL_TLV_SERVICE_RESPONSE;
    if (!response && tlv[0] != AWDL_TLV_SERVICE_REQUEST)
        return EOPNOTSUPP;
    const size_t key_at = response ? 5u : 7u;
    if (length < key_at + 3)
        return EINVAL;
    const size_t key_bytes = response ? le16(tlv + 3) : length - key_at;
    const uint8_t *key = tlv + key_at, *value = NULL;
    size_t value_bytes = 0;
    if (response) {
        if (key_at + key_bytes + 4 > length)
            return EINVAL;
        value = key + key_bytes + 4;
        value_bytes = le16(key + key_bytes);
        if (key_at + key_bytes + 4 + value_bytes != length || !value_bytes)
            return EINVAL;
        if (le16(key + key_bytes + 2))
            return EOPNOTSUPP;  // a fragment of a record
    }
    if (key_bytes < 3)
        return EINVAL;
    if (key[key_bytes - 1] != 1 || key[key_bytes - 3])
        return EOPNOTSUPP;  // another compression version, or a record type above 255
    const uint8_t type = key[key_bytes - 2];
    // Apple's driver keeps an advertised record of any type and counts it in the service map, but sends only these.
    if (response && type != DNS_PTR && type != DNS_SRV && type != DNS_TXT)
        log_once(c, c->switch_unsent_types, type, "kept but not sent, record type");
    struct switch_awdl_dns_name *name = &c->switch_service_key;
    const int expanded = d2d_expand(c, name, key, key_bytes - 3, NULL);
    if (expanded)
        return expanded;
    // Apple's driver hashes and compares the expanded name followed by the type's high byte, one zero.
    name->bytes[name->length++] = 0;

    struct awdl_writer w = {c->switch_staged, sizeof c->switch_staged, 0, 0, 0};
    awdl_w_begin(&w, tlv[0]);
    const size_t name_length_at = w.at;
    awdl_w_u16(&w, 0);
    if (!response)
        awdl_w_u16(&w, 0);
    const size_t key_on_air = awdl_compress(&w, name, NULL);
    if (!key_on_air)
        return w.overflow ? ENOSPC : EINVAL;
    awdl_put16(c->switch_staged + name_length_at, (uint16_t)(response ? key_on_air + 1 : 0));
    awdl_w_u8(&w, type);
    if (response) {
        const size_t data_length_at = w.at;
        awdl_w_u32(&w, 0);
        const int e = write_data(c, &w, type, value, value_bytes);
        if (e)
            return w.overflow ? ENOSPC : e;
        awdl_put16(c->switch_staged + data_length_at, (uint16_t)(w.at - data_length_at - 4));
    }
    awdl_w_end(&w);
    if (w.overflow)
        return ENOSPC;
    c->switch_staged_bytes = (uint16_t)w.at;
    c->switch_staged_hash = crc8_ccitt(name->bytes, name->length);
    c->switch_staged_arpa = (uint8_t)(response && type == DNS_PTR && is_reverse_lookup(name));
    return 0;
}

static size_t entry_bytes(const uint8_t *entry)
{
    return SWITCH_AWDL_ENTRY_HEAD + 3u + le16(entry + SWITCH_AWDL_ENTRY_HEAD + 1);
}

static int staged_is(const struct awdl_core *c, const uint8_t *entry)
{
    return entry_bytes(entry) == SWITCH_AWDL_ENTRY_HEAD + c->switch_staged_bytes &&
           !memcmp(entry + SWITCH_AWDL_ENTRY_HEAD, c->switch_staged, c->switch_staged_bytes);
}

// One bit per advertised record: CRC-8 (polynomial 0x07, initial 0) of its expanded name. The reverse-lookup record
// and browses set none; Apple's driver keeps browses in a map of their own that no frame carries.
static void update_bloom(struct awdl_core *c)
{
    memset(c->switch_service_bloom, 0, sizeof c->switch_service_bloom);
    for (size_t at = 0; at < c->switch_service_bytes; at += entry_bytes(c->switch_services + at)) {
        const uint8_t *entry = c->switch_services + at;
        if (entry[SWITCH_AWDL_ENTRY_HEAD] == AWDL_TLV_SERVICE_RESPONSE && !entry[1])
            c->switch_service_bloom[entry[0] >> 3] |= (uint8_t)(1u << (entry[0] & 7));
    }
}

int switch_awdl_service_held(const struct awdl_core *c)
{
    for (size_t at = 0; at < c->switch_service_bytes; at += entry_bytes(c->switch_services + at))
        if (staged_is(c, c->switch_services + at))
            return 1;
    return 0;
}

int switch_awdl_service_fits(const struct awdl_core *c)
{
    return SWITCH_AWDL_ENTRY_HEAD + c->switch_staged_bytes <= sizeof c->switch_services - c->switch_service_bytes;
}

void switch_awdl_service_keep(struct awdl_core *c)
{
    uint8_t *entry = c->switch_services + c->switch_service_bytes;
    entry[0] = c->switch_staged_hash;
    entry[1] = c->switch_staged_arpa;
    memcpy(entry + SWITCH_AWDL_ENTRY_HEAD, c->switch_staged, c->switch_staged_bytes);
    c->switch_service_bytes = (uint16_t)(c->switch_service_bytes + SWITCH_AWDL_ENTRY_HEAD + c->switch_staged_bytes);
    if (c->switch_staged[0] == AWDL_TLV_SERVICE_RESPONSE)
        c->service_update_indicator++;
    update_bloom(c);
}

int switch_awdl_service_drop(struct awdl_core *c)
{
    for (size_t at = 0; at < c->switch_service_bytes;) {
        uint8_t *entry = c->switch_services + at;
        const size_t bytes = entry_bytes(entry);
        if (!staged_is(c, entry)) {
            at += bytes;
            continue;
        }
        memmove(entry, entry + bytes, c->switch_service_bytes - at - bytes);
        c->switch_service_bytes = (uint16_t)(c->switch_service_bytes - bytes);
        if (c->switch_staged[0] == AWDL_TLV_SERVICE_RESPONSE)
            c->service_update_indicator++;
        update_bloom(c);
        return 1;
    }
    return 0;
}

int switch_awdl_services_clear(struct awdl_core *c)
{
    const int held = c->switch_service_bytes != 0;
    c->switch_service_bytes = 0;
    update_bloom(c);
    return held;
}

// The service parameters carry the map as a mask of its non-zero bytes, then those bytes.
void switch_awdl_write_service_bloom(struct awdl_core *c, struct awdl_writer *w)
{
    uint32_t mask = 0;
    for (unsigned i = 0; i < sizeof c->switch_service_bloom; ++i)
        mask |= (uint32_t)(c->switch_service_bloom[i] != 0) << i;
    awdl_w_u32(w, mask);
    for (unsigned i = 0; i < sizeof c->switch_service_bloom; ++i)
        if (c->switch_service_bloom[i])
            awdl_w_u8(w, c->switch_service_bloom[i]);
}

static const uint8_t *record_data(const uint8_t *tlv, size_t *bytes)
{
    const size_t key = le16(tlv + 3) - 1u;
    *bytes = le16(tlv + 6 + key);
    return tlv + 10 + key;
}

// The newest reverse-lookup record still registered names the host.
void switch_awdl_write_arpa(struct awdl_core *c, struct awdl_writer *w)
{
    const uint8_t *newest = NULL;
    for (size_t at = 0; at < c->switch_service_bytes; at += entry_bytes(c->switch_services + at))
        if (c->switch_services[at + 1])
            newest = c->switch_services + at;
    if (!newest)
        return;
    size_t bytes;
    const uint8_t *host = record_data(newest + SWITCH_AWDL_ENTRY_HEAD, &bytes);
    awdl_w_begin(w, AWDL_TLV_ARPA);
    awdl_w_u8(w, 3);
    awdl_w_bytes(w, host, bytes);
    awdl_w_end(w);
}

// PTR records, then SRV, then TXT, as Apple's frames carry them.
void switch_awdl_write_service_responses(struct awdl_core *c, struct awdl_writer *w)
{
    static const uint8_t order[] = {DNS_PTR, DNS_SRV, DNS_TXT};
    for (unsigned pass = 0; pass < sizeof order; ++pass) {
        for (size_t at = 0; at < c->switch_service_bytes; at += entry_bytes(c->switch_services + at)) {
            const uint8_t *entry = c->switch_services + at;
            const uint8_t *tlv = entry + SWITCH_AWDL_ENTRY_HEAD;
            if (tlv[0] == AWDL_TLV_SERVICE_RESPONSE && !entry[1] && tlv[4 + le16(tlv + 3)] == order[pass])
                awdl_w_bytes(w, tlv, entry_bytes(entry) - SWITCH_AWDL_ENTRY_HEAD);
        }
    }
}
#endif
