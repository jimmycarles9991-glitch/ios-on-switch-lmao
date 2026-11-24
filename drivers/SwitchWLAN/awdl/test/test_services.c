// SPDX-License-Identifier: BSD-2-Clause-Patent
// wifip2pd's service records in the soft backend's frames. The D2D records are the ones a Mac received from
// us unchanged (its wifip2pd log), with names and identifiers replaced by others of the same length. Their on-air form
// is checked against the real iOS 26 MIF in switch_apple_frames.h and decoded with Wireshark's AWDL name dictionary.
#include "../awdl.h"
#include "../awdl_apple80211.h"
#include "switch_apple_frames.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SWITCH_IPSW_KIND_24B119)
extern int test_failures;
#define SCHECK(x)                                                        \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            test_failures++;                                             \
        }                                                                \
    } while (0)

static struct awdl_core *s_core;
static struct a80211_awdl s_facade;

static int s_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)ctx, (void)frame, (void)len, (void)freq, (void)stay_until, (void)no_ack;
    return 0;
}

static int s_logs;

// Counts the service-record lines only: the first record a core keeps also starts AWDL, which logs its schedule.
static void s_log(void *ctx, const char *line)
{
    (void)ctx;
    if (strstr(line, "service record"))
        s_logs++;
}

static void s_reset(void)
{
    free(s_core);
    s_core = (struct awdl_core *)calloc(1, awdl_core_size());
    const uint8_t addr[6] = {0x02, 0, 0, 0, 0, 0x01};
    struct awdl_config cfg;
    awdl_config_default(&cfg, addr);
    const struct awdl_host host = {NULL, s_tx, NULL, NULL, NULL, s_log, NULL, NULL};
    SCHECK(awdl_core_init(s_core, &host, &cfg) == 0);
    const uint8_t caps[12] = {0};
    const struct a80211_awdl_ops ops = {0};
    a80211_awdl_init(&s_facade, s_core, &ops, "awdl0", "en0", caps);
}

static size_t unhex(const char *s, uint8_t *out)
{
    size_t k = 0;
    for (; s[0] && s[1]; s += 2) {
        while (*s == ' ')
            s++;
        unsigned v = 0;
        if (sscanf(s, "%2x", &v) != 1)
            break;
        out[k++] = (uint8_t)v;
    }
    return k;
}

static int offer(const uint8_t *tlv, size_t bytes, uint8_t remove)
{
    uint8_t data[sizeof(struct a80211_peer_service_request) + AWDL_MAX_FRAME];
    const struct a80211_peer_service_request request = {
        .version = 1, .delete_request = remove, .data_len = (uint16_t)bytes};
    memcpy(data, &request, sizeof request);
    if (bytes)
        memcpy(data + sizeof request, tlv, bytes);
    uint32_t length = (uint32_t)(sizeof request + bytes);
    int32_t value = 0;
    return a80211_awdl_request(&s_facade, 1, A80211_IOC_AWDL_PEER_SERVICE_REQUEST, &value, data, &length, sizeof data,
                               0);
}

static int offer_hex(const char *hex, uint8_t remove)
{
    uint8_t tlv[AWDL_MAX_FRAME];
    return offer(tlv, unhex(hex, tlv), remove);
}

struct frame_tlvs {
    const uint8_t *arpa, *params;  // values
    size_t arpa_bytes, params_bytes;
    const uint8_t *records[40];  // whole service response TLVs
    size_t record_bytes[40];
    unsigned records_count, requests;
    uint8_t map[32];  // the service parameters' 256-bit map
};

static void split(const uint8_t *frame, size_t length, struct frame_tlvs *t)
{
    memset(t, 0, sizeof *t);
    for (size_t at = 40; at + 3 <= length;) {
        const size_t bytes = 3u + (size_t)(frame[at + 1] | frame[at + 2] << 8);
        SCHECK(at + bytes <= length);
        if (at + bytes > length)
            return;
        const uint8_t *v = frame + at + 3;
        if (frame[at] == AWDL_TLV_ARPA) {
            t->arpa = v;
            t->arpa_bytes = bytes - 3;
        } else if (frame[at] == AWDL_TLV_SERVICE_PARAMS && bytes >= 12) {
            t->params = v;
            t->params_bytes = bytes - 3;
            const uint32_t mask = (uint32_t)(v[5] | v[6] << 8 | v[7] << 16 | (uint32_t)v[8] << 24);
            for (unsigned i = 0, k = 9; i < 32 && k < bytes - 3; ++i)
                if (mask >> i & 1)
                    t->map[i] = v[k++];
        } else if (frame[at] == AWDL_TLV_SERVICE_RESPONSE && t->records_count < 40) {
            t->records[t->records_count] = frame + at;
            t->record_bytes[t->records_count++] = bytes;
        } else if (frame[at] == AWDL_TLV_SERVICE_REQUEST) {
            t->requests++;
        }
        at += bytes;
    }
}

static size_t s_build(uint8_t subtype, uint8_t *frame)
{
    return awdl_core_build_af(s_core, subtype, 0, frame, AWDL_MAX_FRAME);
}

static unsigned map_bits(const uint8_t map[32])
{
    unsigned n = 0;
    for (unsigned i = 0; i < 256; ++i)
        n += map[i >> 3] >> (i & 7) & 1;
    return n;
}

// Wireshark's awdl_dns_compression table; c0 00 is the record's own name, as Apple's driver writes it.
static const char *const kDictionary[] = {
    NULL,
    "_airplay._tcp.local",
    "_airplay._udp.local",
    "_airplay",
    "_raop._tcp.local",
    "_raop._udp.local",
    "_raop",
    "_airdrop._tcp.local",
    "_airdrop._udp.local",
    "_airdrop",
    "_tcp.local",
    "_udp.local",
    "local",
    "ip6.arpa",
    "ip4.arpa",
};

static int name_text(const uint8_t *p, size_t n, const char *own, char *out, size_t size)
{
    size_t at = 0;
    out[0] = 0;
    for (size_t i = 0; i < n;) {
        const char *part;
        size_t part_bytes;
        if (p[i] == 0xc0) {
            if (i + 1 >= n || p[i + 1] >= sizeof kDictionary / sizeof kDictionary[0])
                return -1;
            part = p[i + 1] ? kDictionary[p[i + 1]] : own;
            part_bytes = strlen(part);
            i += 2;
        } else {
            part = (const char *)p + i + 1;
            part_bytes = p[i];
            i += 1 + part_bytes;
        }
        if (i > n || at + part_bytes + 2 > size)
            return -1;
        if (at)
            out[at++] = '.';
        memcpy(out + at, part, part_bytes);
        at += part_bytes;
        out[at] = 0;
    }
    return 0;
}

#define UUID    "3862316634633265 2d30643361 2d34653562 2d39633664 2d376538663961306231633264"
#define ID      "303132333435363738396162"
#define AIRDROP "085f61697264726f70"

// fe80::ff:fe00:1's reverse-lookup name -> host 8b1f4c2e-0d3a-4e5b-9c6d-7e8f9a0b1c2d.local
static const char *const kReverse =
    "02 7a00 4d00"
    " 0131 0130 0130 0130 0130 0130 0165 0166 0166 0166 0130 0130 0130 0130 0130 0130"
    " 0130 0130 0130 0130 0130 0130 0130 0130 0130 0130 0130 0130 0130 0138 0165 0166 03697036 0461727061 00 000c01"
    " 2700 0000 24" UUID "c011";
// _airdrop._tcp.local PTR 0123456789ab._airdrop._tcp.local
static const char *const kPtr = "02 2300 0e00 " AIRDROP "c00c 000c01 0f00 0000 0c" ID "c027";
static const char *const kTxt = "02 2e00 1b00 0c" ID AIRDROP "c00c 001001 0d00 0000 0c 666c6167733d313131363131";
static const char *const kSrv = "02 4e00 1b00 0c" ID AIRDROP "c00c 002101 2d00 0000 0000 0000 2242 24" UUID "c011";

// On air: name length counting the type byte, name, type, data length, fragment offset 0.
static const char *const kAirPtr = "02 1800 0300 c007 0c 0f00 0000 0c" ID "c000";
static const char *const kAirSrv = "02 4300 1000 0c" ID "c007 21 2d00 0000 0000 0000 2242 24" UUID "c00c";
static const char *const kAirTxt = "02 2300 1000 0c" ID "c007 10 0d00 0000 0c 666c6167733d313131363131";

static int record_is(const struct frame_tlvs *t, unsigned i, const char *hex)
{
    uint8_t want[AWDL_MAX_FRAME];
    const size_t n = unhex(hex, want);
    return i < t->records_count && t->record_bytes[i] == n && !memcmp(t->records[i], want, n);
}

static void test_daemon_records(void)
{
    s_reset();
    uint8_t frame[AWDL_MAX_FRAME];
    struct frame_tlvs t;
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(!t.arpa && t.params_bytes == 9 && !t.records_count);

    SCHECK(offer_hex(kReverse, 0) == 0 && offer_hex(kPtr, 0) == 0);
    SCHECK(offer_hex(kTxt, 0) == 0 && offer_hex(kSrv, 0) == 0);
    const size_t n = s_build(AWDL_SUBTYPE_MIF, frame);
    split(frame, n, &t);
    uint8_t arpa[64];
    const size_t arpa_bytes = unhex("03 24" UUID "c00c", arpa);
    SCHECK(t.arpa_bytes == arpa_bytes && !memcmp(t.arpa, arpa, arpa_bytes));
    SCHECK(t.records_count == 3 && record_is(&t, 0, kAirPtr) && record_is(&t, 1, kAirSrv) && record_is(&t, 2, kAirTxt));
    SCHECK(map_bits(t.map) == 2);  // _airdrop._tcp.local and the instance

    char key[128], text[128];
    SCHECK(!name_text(t.records[0] + 5, 2, "", key, sizeof key) && !strcmp(key, "_airdrop._tcp.local"));
    SCHECK(!name_text(t.records[0] + 12, 15, key, text, sizeof text) &&
           !strcmp(text, "0123456789ab._airdrop._tcp.local"));
    SCHECK(!name_text(t.records[1] + 5, 15, "", key, sizeof key) && !strcmp(key, "0123456789ab._airdrop._tcp.local"));
    SCHECK(!name_text(t.records[1] + 31, 39, key, text, sizeof text) &&
           !strcmp(text, "8b1f4c2e-0d3a-4e5b-9c6d-7e8f9a0b1c2d.local"));

    struct awdl_af parsed;
    SCHECK(awdl_af_parse(frame, n, &parsed) == 0 && !strcmp(parsed.hostname, "8b1f4c2e-0d3a-4e5b-9c6d-7e8f9a0b1c2d"));
    uint8_t psf_frame[AWDL_MAX_FRAME];
    struct frame_tlvs psf;
    split(psf_frame, s_build(AWDL_SUBTYPE_PSF, psf_frame), &psf);
    SCHECK(psf.params_bytes == t.params_bytes && !memcmp(psf.params, t.params, t.params_bytes));
    SCHECK(!psf.arpa && !psf.records_count);
}

static size_t label(uint8_t *out, const char *s)
{
    const size_t n = strlen(s);
    out[0] = (uint8_t)n;
    memcpy(out + 1, s, n);
    return 1 + n;
}

static void offer_record(const uint8_t *key, size_t key_bytes, const uint8_t *value, size_t value_bytes)
{
    uint8_t tlv[AWDL_MAX_FRAME];
    const size_t body = 2 + key_bytes + 4 + value_bytes;
    tlv[0] = AWDL_TLV_SERVICE_RESPONSE;
    tlv[1] = (uint8_t)body;
    tlv[2] = (uint8_t)(body >> 8);
    tlv[3] = (uint8_t)key_bytes;
    tlv[4] = 0;
    memcpy(tlv + 5, key, key_bytes);
    tlv[5 + key_bytes] = (uint8_t)value_bytes;
    tlv[6 + key_bytes] = (uint8_t)(value_bytes >> 8);
    tlv[7 + key_bytes] = tlv[8 + key_bytes] = 0;
    memcpy(tlv + 9 + key_bytes, value, value_bytes);
    SCHECK(offer(tlv, 3 + body, 0) == 0);
}

// One of sharingd's pairing services as wifip2pd lays it out: PTR, TXT and SRV, the instance named `device`.
static void offer_pairing(const char *service, const char *device, int with_name, const char *host)
{
    static const uint8_t tcp[] = {0xc0, 0x0c, 0}, version = 1;
    uint8_t key[128], value[256];
    size_t k = label(key, service);
    memcpy(key + k, tcp, sizeof tcp);
    k += sizeof tcp;
    key[k++] = 12;
    key[k++] = version;
    size_t v = label(value, device);
    value[v++] = 0xc0;
    value[v++] = 0x27;
    offer_record(key, k, value, v);

    k = label(key, device);
    k += label(key + k, service);
    memcpy(key + k, tcp, sizeof tcp);
    k += sizeof tcp;
    key[k++] = 16;
    key[k++] = version;
    v = label(value, "sn=com.apple.sharingd.AirDrop");
    v += label(value + v, "at=0a1b2c3d4e5f");
    v += label(value + v, "sid=4F1E2D3C-5B6A-4789-8A9B-0C1D2E3F4A5B");
    if (with_name) {
        char dnm[64];
        snprintf(dnm, sizeof dnm, "dnm=%s", device);
        v += label(value + v, dnm);
    }
    offer_record(key, k, value, v);

    key[k - 2] = 33;
    static const uint8_t srv[] = {0, 0, 0, 0, 0xd8, 0x4e};
    memcpy(value, srv, sizeof srv);
    v = sizeof srv + label(value + sizeof srv, host);
    value[v++] = 0xc0;
    value[v++] = 0x11;
    offer_record(key, k, value, v);
}

// The iOS 26 device advertised the two pairing services under a 16-byte name and a 36-byte host: our conversion of
// the same records gives its six record lengths in its order, its ARPA layout and the hashes of its service types.
static void test_against_ios26(void)
{
    struct frame_tlvs real;
    split(switch_apple_mif, sizeof switch_apple_mif - 4, &real);
    SCHECK(real.records_count == 6 && real.arpa_bytes == 40 && map_bits(real.map) == 4);

    s_reset();
    const char *const device = "iPad of a tester";
    const char *const host = "8b1f4c2e-0d3a-4e5b-9c6d-7e8f9a0b1c2d";
    SCHECK(offer_hex(kReverse, 0) == 0);
    offer_pairing("_applicationservicepairing", device, 0, host);
    offer_pairing("_appsvcprepair", device, 1, host);
    uint8_t frame[AWDL_MAX_FRAME];
    struct frame_tlvs t;
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(t.records_count == real.records_count);
    for (unsigned i = 0; i < t.records_count && i < real.records_count; ++i)
        SCHECK(t.record_bytes[i] == real.record_bytes[i]);
    SCHECK(t.arpa_bytes == real.arpa_bytes && t.arpa[0] == real.arpa[0] && t.arpa[1] == real.arpa[1] &&
           !memcmp(t.arpa + t.arpa_bytes - 2, real.arpa + real.arpa_bytes - 2, 2));
    SCHECK(t.params_bytes == real.params_bytes && map_bits(t.map) == map_bits(real.map));

    s_reset();
    static const uint8_t tcp[] = {0xc0, 0x0c, 0, 12, 1}, instance[] = {4, 'n', 'a', 'm', 'e', 0xc0, 0x27};
    uint8_t key[64];
    for (int i = 0; i < 2; ++i) {
        const size_t k = label(key, i ? "_appsvcprepair" : "_applicationservicepairing");
        memcpy(key + k, tcp, sizeof tcp);
        offer_record(key, k + sizeof tcp, instance, sizeof instance);
    }
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(map_bits(t.map) == 2);
    for (unsigned i = 0; i < 32; ++i)
        SCHECK((t.map[i] & real.map[i]) == t.map[i]);
}

static void test_record_lifecycle(void)
{
    s_reset();
    uint8_t frame[AWDL_MAX_FRAME], tlv[AWDL_MAX_FRAME];
    const size_t base = s_build(AWDL_SUBTYPE_MIF, frame);
    struct awdl_af a;
    SCHECK(awdl_af_parse(frame, base, &a) == 0);
    const uint16_t first = a.service_update_indicator;

    size_t n = unhex(kPtr, tlv);
    SCHECK(offer(tlv, n - 1, 0) == EINVAL);
    tlv[3]++;
    SCHECK(offer(tlv, n, 0) == EINVAL);
    tlv[3]--;
    tlv[5 + 14]++;
    SCHECK(offer(tlv, n, 0) == EINVAL);
    tlv[5 + 14]--;
    tlv[7 + 14] = 1;
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP);  // a fragment
    tlv[7 + 14] = 0;
    tlv[0] = 3;
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP);
    tlv[0] = 2;
    SCHECK(s_build(AWDL_SUBTYPE_MIF, frame) == base && !s_facade.enabled);

    static const char *const browse = "01 1200 00000000 " AIRDROP "c00c 000c01";
    SCHECK(offer_hex(browse, 0) == 0 && s_facade.enabled && awdl_core_running(s_core));
    struct frame_tlvs t;
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(!t.requests && !t.records_count && t.params_bytes == 9);  // browses set no bit either

    SCHECK(offer(tlv, n, 0) == 0 && offer(tlv, n, 0) == 0);
    n = s_build(AWDL_SUBTYPE_MIF, frame);
    split(frame, n, &t);
    SCHECK(t.records_count == 1 && record_is(&t, 0, kAirPtr) && map_bits(t.map) == 1);
    SCHECK(awdl_af_parse(frame, n, &a) == 0 && a.service_update_indicator == first + 1);

    uint8_t other[AWDL_MAX_FRAME];
    const size_t other_bytes = unhex(kPtr, other);
    other[30] = 'c';
    SCHECK(offer(other, other_bytes, 1) == 0);
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(t.records_count == 1);
    SCHECK(offer_hex(kPtr, 1) == 0);
    split(frame, n = s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(!t.records_count && t.params_bytes == 9);
    SCHECK(awdl_af_parse(frame, n, &a) == 0 && a.service_update_indicator == first + 2);

    SCHECK(offer_hex(kReverse, 0) == 0);
    uint8_t reverse[AWDL_MAX_FRAME];
    const size_t reverse_bytes = unhex(kReverse, reverse);
    reverse[reverse_bytes - 3] = 'f';  // another host
    SCHECK(offer(reverse, reverse_bytes, 0) == 0);
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(t.arpa_bytes == 40 && t.arpa[37] == 'f' && !t.records_count && t.params_bytes == 9);
    SCHECK(offer_hex(kReverse, 1) == 0);
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(t.arpa_bytes == 40 && t.arpa[37] == 'f');
    SCHECK(offer(reverse, reverse_bytes, 1) == 0);
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(!t.arpa);

    SCHECK(offer_hex(kSrv, 0) == 0 && offer(NULL, 0, 1) == 0);
    SCHECK(s_build(AWDL_SUBTYPE_MIF, frame) == base);

    const size_t value_bytes = AWDL_MAX_FRAME - 320u - 2 - 3 - 2 - 16 - 1 - 4;
    static const uint8_t key[] = {8, '_', 'b', 'i', 'g', '-', 't', 'x', 't', 0xc0, 0x0c, 0, 16, 1};
    uint8_t value[AWDL_MAX_FRAME];
    memset(value, 'v', value_bytes);
    offer_record(key, sizeof key, value, value_bytes);
    SCHECK(offer_hex(kPtr, 0) == ENOSPC);
    offer_record(key, sizeof key, value, value_bytes);  // held already, so no room needed
    split(frame, n = s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(n && t.records_count == 1);
    a80211_awdl_init(&s_facade, s_core, &(const struct a80211_awdl_ops){0}, "awdl0", "en0", (const uint8_t[12]){0});
    SCHECK(s_build(AWDL_SUBTYPE_MIF, frame) == base);
}

static size_t dotted_labels(const char *dotted, uint8_t *out)
{
    size_t at = 0;
    for (const char *p = dotted; *p;) {
        const char *dot = strchr(p, '.');
        const size_t n = dot ? (size_t)(dot - p) : strlen(p);
        out[at] = (uint8_t)n;
        memcpy(out + at + 1, p, n);
        at += 1 + n;
        p += n + (dot ? 1 : 0);
    }
    out[at++] = 0;
    return at;
}

// Every dictionary name, spelt out in full in a record's key, goes on air as its two-byte code.
static void test_dictionary(void)
{
    s_reset();
    static const uint8_t txt[] = {0, 16, 1}, value[] = {1, 'x'};
    const unsigned codes = sizeof kDictionary / sizeof kDictionary[0];
    for (unsigned code = 1; code < codes; ++code) {
        uint8_t key[64];
        const size_t k = dotted_labels(kDictionary[code], key);
        memcpy(key + k, txt, sizeof txt);
        offer_record(key, k + sizeof txt, value, sizeof value);
    }
    uint8_t frame[AWDL_MAX_FRAME];
    struct frame_tlvs t;
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(t.records_count == codes - 1);
    for (unsigned i = 0; i < t.records_count; ++i) {
        const uint8_t want[] = {2, 11, 0, 3, 0, 0xc0, (uint8_t)(i + 1), 16, 2, 0, 0, 0, 1, 'x'};
        char text[64];
        SCHECK(t.record_bytes[i] == sizeof want && !memcmp(t.records[i], want, sizeof want));
        SCHECK(!name_text(t.records[i] + 5, 2, "", text, sizeof text) && !strcmp(text, kDictionary[i + 1]));
    }
}

static void offer_txt(const char *dotted_name, size_t value_bytes, int expected)
{
    uint8_t tlv[AWDL_MAX_FRAME];
    const size_t k = dotted_labels(dotted_name, tlv + 5);
    tlv[5 + k] = 0;
    tlv[6 + k] = 16;
    tlv[7 + k] = 1;
    const size_t key = k + 3, body = 2 + key + 4 + value_bytes;
    tlv[0] = AWDL_TLV_SERVICE_RESPONSE;
    tlv[1] = (uint8_t)body;
    tlv[2] = (uint8_t)(body >> 8);
    tlv[3] = (uint8_t)key;
    tlv[4] = 0;
    tlv[5 + key] = (uint8_t)value_bytes;
    tlv[6 + key] = (uint8_t)(value_bytes >> 8);
    tlv[7 + key] = tlv[8 + key] = 0;
    memset(tlv + 9 + key, 'v', value_bytes);
    SCHECK(offer(tlv, 3 + body, 0) == expected);
}

static void test_refusals(void)
{
    s_reset();
    uint8_t tlv[AWDL_MAX_FRAME];
    size_t n = unhex(kPtr, tlv);
    tlv[5 + 10] = 0x2a;  // c0 0c -> c0 2a
    int logs = s_logs;
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP && s_logs == logs + 1);
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP && s_logs == logs + 1);
    tlv[5 + 10] = 0x2b;
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP && s_logs == logs + 2);
    tlv[5 + 10] = 0x27;  // the record's own name inside its own key
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP && s_logs == logs + 3);
    tlv[5 + 10] = 0x0c;
    tlv[5 + 13] = 2;  // compression version
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP);
    tlv[5 + 13] = 1;
    tlv[5 + 11] = 1;  // type 0x010c
    SCHECK(offer(tlv, n, 0) == EOPNOTSUPP);
    tlv[5 + 11] = 0;
    tlv[5 + 12] = 28;  // AAAA
    logs = s_logs;
    SCHECK(offer(tlv, n, 0) == 0 && s_logs == logs + 1);
    SCHECK(offer(tlv, n, 0) == 0 && s_logs == logs + 1);
    uint8_t frame[AWDL_MAX_FRAME];
    struct frame_tlvs t;
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(!t.records_count && map_bits(t.map) == 1);
    SCHECK(offer(tlv, n, 1) == 0);
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(!t.records_count && t.params_bytes == 9);

    n = unhex(kSrv, tlv);
    tlv[1] = (uint8_t)(tlv[1] - 40);
    tlv[5 + 27] = 5;  // value length
    SCHECK(offer(tlv, n - 40, 0) == EINVAL);

    static const char *const browse = "01 1200 00000000 " AIRDROP "c00c 000c01";
    n = unhex(browse, tlv);
    SCHECK(offer(tlv, n - 1, 0) == EINVAL);
    static const char *const short_browse = "01 0600 00000000 0c01";
    SCHECK(offer_hex(short_browse, 0) == EINVAL);
    static const char *const address_browse = "01 1000 00000000 06737769746368 c011 001c01";
    SCHECK(offer_hex(address_browse, 0) == 0);  // browses of any type stay off air

    uint8_t key[128];
    for (unsigned i = 0; i < 47; ++i) {  // 47 x "_tcp.local", past the name limit once expanded
        key[2 * i] = 0xc0;
        key[2 * i + 1] = 0x0c;
    }
    static const uint8_t txt[] = {0, 16, 1}, value[] = {1, 'x'};
    memcpy(key + 94, txt, sizeof txt);
    const size_t body = 2 + 97 + 4 + sizeof value;
    tlv[0] = AWDL_TLV_SERVICE_RESPONSE;
    tlv[1] = (uint8_t)body;
    tlv[2] = 0;
    tlv[3] = 97;
    tlv[4] = 0;
    memcpy(tlv + 5, key, 97);
    tlv[102] = sizeof value;
    tlv[103] = tlv[104] = tlv[105] = 0;
    memcpy(tlv + 106, value, sizeof value);
    SCHECK(offer(tlv, 3 + body, 0) == EINVAL);
    split(frame, s_build(AWDL_SUBTYPE_MIF, frame), &t);
    SCHECK(!t.records_count && t.params_bytes == 9);
}

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        crc = (uint8_t)(crc ^ *p++);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

// The largest service block: every byte of the map in use and the store filled to the byte.
static void test_worst_case_frame(void)
{
    s_reset();
    char names[32][24];
    uint8_t taken[32] = {0};
    unsigned found = 0;
    for (unsigned i = 0; found < 32 && i < 4096; ++i) {
        char name[24];
        snprintf(name, sizeof name, "_s%04u._tcp.local", i);
        uint8_t wire[32];
        const size_t bytes = dotted_labels(name, wire);
        wire[bytes] = 0;  // the type's high byte, which the hash covers
        const uint8_t slot = crc8(wire, bytes + 1) >> 3;
        if (!taken[slot]) {
            taken[slot] = 1;
            memcpy(names[found++], name, sizeof name);
        }
    }
    SCHECK(found == 32);
    // each entry: hash and reverse flag, TLV header, name length, "\x06_sNNNN" c0 0a, type, data length and offset
    const size_t entry = 2 + 3 + 2 + 9 + 1 + 4, room = AWDL_MAX_FRAME - 320u;
    for (unsigned k = 0; k + 1 < found; ++k)
        offer_txt(names[k], 1, 0);
    const size_t last = room - (found - 1) * (entry + 1) - entry;
    offer_txt(names[found - 1], last + 1, ENOSPC);
    offer_txt(names[found - 1], last, 0);
    offer_txt("_full._tcp.local", 1, ENOSPC);
    uint8_t frame[AWDL_MAX_FRAME];
    struct frame_tlvs t;
    const size_t mif = s_build(AWDL_SUBTYPE_MIF, frame);
    split(frame, mif, &t);
    SCHECK(mif && t.params_bytes == 9 + 32 && t.records_count == found);
    SCHECK(s_build(AWDL_SUBTYPE_PSF, frame) != 0);
}

void test_services_main(void)
{
    test_dictionary();
    test_refusals();
    test_worst_case_frame();
    test_daemon_records();
    test_against_ios26();
    test_record_lifecycle();
    awdl_core_stop(s_core);
    free(s_core);
    s_core = NULL;
}
#endif
