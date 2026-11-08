// SPDX-License-Identifier: BSD-2-Clause-Patent
// The resolver's and the neighbour cache's traffic through en0, counted at the driver's edge: DNS queries out and
// their answers in (paired by the client's UDP port and the message id, with the time between), ARP each way. And the
// firmware's "counters" block, as the deltas of its 32-bit slots between two stats windows: which slot is which
// depends on the block's version (wl_cnt_t in bcmdhd's wlioctl.h), so it is decoded off the device. Plain C on byte
// buffers, for the host test. The includer declares snprintf.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum swlan_frame_kind {
    SWLAN_FRAME_OTHER,
    SWLAN_FRAME_DNS_QUERY,
    SWLAN_FRAME_DNS_ANSWER,
    SWLAN_FRAME_ARP_REQUEST,
    SWLAN_FRAME_ARP_REPLY,
};

struct swlan_frame_facts {
    enum swlan_frame_kind kind;
    uint32_t key;        // DNS: the client's port << 16 | the message id (never 0: port 0 is not a client's)
    uint32_t source_ip;  // the IPv4 source, or the ARP sender
    uint32_t target_ip;  // ARP only
};

static inline uint16_t swlan_trace_be16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static inline uint32_t swlan_trace_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// An Ethernet II frame: ARP for IPv4 over Ethernet, or the first fragment of an IPv4 UDP datagram whose DNS header's
// QR bit agrees with its port 53 (a query to it, an answer from it).
static inline struct swlan_frame_facts swlan_frame_classify(const uint8_t *f, size_t n)
{
    struct swlan_frame_facts out = {SWLAN_FRAME_OTHER, 0, 0, 0};
    if (n < 14)
        return out;
    const uint16_t type = swlan_trace_be16(f + 12);
    if (type == 0x0806) {
        if (n < 42 || swlan_trace_be16(f + 14) != 1 || swlan_trace_be16(f + 16) != 0x0800 || f[18] != 6 || f[19] != 4)
            return out;
        const uint16_t op = swlan_trace_be16(f + 20);
        if (op != 1 && op != 2)
            return out;
        out.kind = op == 1 ? SWLAN_FRAME_ARP_REQUEST : SWLAN_FRAME_ARP_REPLY;
        out.source_ip = swlan_trace_be32(f + 28);
        out.target_ip = swlan_trace_be32(f + 38);
        return out;
    }
    if (type != 0x0800 || n < 14 + 20)
        return out;
    const uint8_t *ip = f + 14;
    const size_t ihl = (size_t)(ip[0] & 0xF) * 4u;
    if ((ip[0] >> 4) != 4 || ihl < 20 || n < 14 + ihl + 8 + 3)
        return out;
    if (ip[9] != 17 || (swlan_trace_be16(ip + 6) & 0x1FFF))
        return out;
    const uint8_t *udp = ip + ihl;
    const uint16_t sport = swlan_trace_be16(udp);
    const uint16_t dport = swlan_trace_be16(udp + 2);
    const uint16_t id = swlan_trace_be16(udp + 8);
    const bool answer = (udp[10] & 0x80) != 0;
    if (dport == 53 && !answer && sport) {
        out.kind = SWLAN_FRAME_DNS_QUERY;
        out.key = ((uint32_t)sport << 16) | id;
    } else if (sport == 53 && answer && dport) {
        out.kind = SWLAN_FRAME_DNS_ANSWER;
        out.key = ((uint32_t)dport << 16) | id;
    } else {
        return out;
    }
    out.source_ip = swlan_trace_be32(ip + 12);
    return out;
}

// Answer delay buckets: a resolver's own answers (resolver.arpa, its cache) come in a few ms, forwarded ones later.
enum {
    SWLAN_DNS_BUCKETS = 4
};

static inline unsigned swlan_dns_bucket(uint64_t waited_ns)
{
    if (waited_ns <= 3000000ULL)
        return 0;
    if (waited_ns <= 20000000ULL)
        return 1;
    return waited_ns <= 200000000ULL ? 2 : 3;
}

struct swlan_trace_counts {
    uint32_t dns_queries, dns_answers, dns_unmatched;
    uint32_t dns_within[SWLAN_DNS_BUCKETS];
    uint32_t arp_out_requests, arp_out_replies;
    uint32_t arp_in_requests, arp_in_requests_for_us, arp_in_requests_unicast, arp_in_replies;
};

// A query sent again with the same port and id (a resolver's retry) restarts its clock: an answer is timed from the
// last copy sent. Only the newest SWLAN_DNS_OUTSTANDING queries can be paired; an answer to an older one, or a second
// answer, counts as unmatched.
enum {
    SWLAN_DNS_OUTSTANDING = 32
};

struct swlan_trace {
    struct {
        uint32_t key;
        uint64_t sent_ns;
    } pending[SWLAN_DNS_OUTSTANDING];

    unsigned next;
    uint32_t our_ip;  // the IPv4 source of what we last sent: whose ARP requests are ours
    struct swlan_trace_counts c;
};

static inline void swlan_trace_note_sent(struct swlan_trace *t, struct swlan_frame_facts x, uint64_t now_ns)
{
    if (x.kind == SWLAN_FRAME_OTHER)
        return;
    if (x.source_ip)
        t->our_ip = x.source_ip;
    if (x.kind == SWLAN_FRAME_ARP_REQUEST) {
        ++t->c.arp_out_requests;
    } else if (x.kind == SWLAN_FRAME_ARP_REPLY) {
        ++t->c.arp_out_replies;
    } else if (x.kind == SWLAN_FRAME_DNS_QUERY) {
        ++t->c.dns_queries;
        unsigned k = 0;
        while (k < SWLAN_DNS_OUTSTANDING && t->pending[k].key != x.key)
            ++k;
        if (k == SWLAN_DNS_OUTSTANDING) {
            k = t->next;
            t->next = (t->next + 1) % SWLAN_DNS_OUTSTANDING;
        }
        t->pending[k].key = x.key;
        t->pending[k].sent_ns = now_ns;
    }
}

// `unicast`: the frame's destination address was not a group address.
static inline void swlan_trace_note_received(struct swlan_trace *t, struct swlan_frame_facts x, bool unicast,
                                             uint64_t now_ns)
{
    if (x.kind == SWLAN_FRAME_ARP_REQUEST) {
        ++t->c.arp_in_requests;
        if (t->our_ip && x.target_ip == t->our_ip)
            ++t->c.arp_in_requests_for_us;
        if (unicast)
            ++t->c.arp_in_requests_unicast;
    } else if (x.kind == SWLAN_FRAME_ARP_REPLY) {
        ++t->c.arp_in_replies;
    } else if (x.kind == SWLAN_FRAME_DNS_ANSWER) {
        ++t->c.dns_answers;
        unsigned k = 0;
        while (k < SWLAN_DNS_OUTSTANDING && t->pending[k].key != x.key)
            ++k;
        if (k == SWLAN_DNS_OUTSTANDING) {
            ++t->c.dns_unmatched;
            return;
        }
        ++t->c.dns_within[swlan_dns_bucket(now_ns - t->pending[k].sent_ns)];
        t->pending[k].key = 0;
    }
}

static inline void swlan_trace_sent(struct swlan_trace *t, const uint8_t *f, size_t n, uint64_t now_ns)
{
    swlan_trace_note_sent(t, swlan_frame_classify(f, n), now_ns);
}

static inline void swlan_trace_received(struct swlan_trace *t, const uint8_t *f, size_t n, uint64_t now_ns)
{
    swlan_trace_note_received(t, swlan_frame_classify(f, n), (n != 0u) && ((f[0] & 1) == 0), now_ns);
}

// "DNS out 61 answered 2 (3 ms 2, 20 ms 0, 200 ms 0, later 0, unmatched 0), ARP out 1 request 0 replies, in 3
// requests (3 for us, 2 unicast) 1 reply" for the window between `was` and `now`.
static inline int swlan_trace_describe(const struct swlan_trace_counts *now, const struct swlan_trace_counts *was,
                                       char *out, size_t size)
{
    return snprintf(
        out, size,
        "DNS out %u answered %u (3 ms %u, 20 ms %u, 200 ms %u, later %u, unmatched %u), ARP out %u "
        "request%s %u repl%s, in %u request%s (%u for us, %u unicast) %u repl%s",
        now->dns_queries - was->dns_queries, now->dns_answers - was->dns_answers,
        now->dns_within[0] - was->dns_within[0], now->dns_within[1] - was->dns_within[1],
        now->dns_within[2] - was->dns_within[2], now->dns_within[3] - was->dns_within[3],
        now->dns_unmatched - was->dns_unmatched, now->arp_out_requests - was->arp_out_requests,
        now->arp_out_requests - was->arp_out_requests == 1 ? "" : "s", now->arp_out_replies - was->arp_out_replies,
        now->arp_out_replies - was->arp_out_replies == 1 ? "y" : "ies", now->arp_in_requests - was->arp_in_requests,
        now->arp_in_requests - was->arp_in_requests == 1 ? "" : "s",
        now->arp_in_requests_for_us - was->arp_in_requests_for_us,
        now->arp_in_requests_unicast - was->arp_in_requests_unicast, now->arp_in_replies - was->arp_in_replies,
        now->arp_in_replies - was->arp_in_replies == 1 ? "y" : "ies");
}

// The "counters" block: a 16-bit version and length, then 32-bit counters. "v10 1004 B: 6+12 7+1 ... (+N more)"
// with the slot index (from the block's start, in 32-bit words) of every counter that moved since `was`; with no
// earlier block of the same version and length, the header alone.
static inline int swlan_fw_counters_delta(const uint8_t *now, size_t now_len, const uint8_t *was, size_t was_len,
                                          char *out, size_t size)
{
    if (!size)
        return 0;
    out[0] = 0;
    if (now_len < 4)
        return snprintf(out, size, "%zu bytes", now_len);
    const unsigned version = now[0] | ((unsigned)now[1] << 8);
    const unsigned length = now[2] | ((unsigned)now[3] << 8);
    int k = snprintf(out, size, "v%u %u B:", version, length);
    size_t at = k > 0 ? (size_t)k : 0;
    if (!was || was_len != now_len || was[0] != now[0] || was[1] != now[1]) {
        if (at < size) {
            k = snprintf(out + at, size - at, " first");
            at += k > 0 ? (size_t)k : 0;
        }
        return (int)(at < size ? at : size - 1);
    }
    const size_t slots = now_len / 4;
    unsigned more = 0;
    for (size_t i = 1; i < slots; ++i) {
        const uint8_t *a = now + 4 * i;
        const uint8_t *b = was + 4 * i;
        const uint32_t va = (uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
        const uint32_t vb = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        if (va == vb)
            continue;
        char item[24];
        const int m = snprintf(item, sizeof(item), " %zu%+d", i, (int)(va - vb));
        if (m <= 0)
            continue;
        if (more || at + (size_t)m + 16 >= size) {
            ++more;
            continue;
        }
        for (int j = 0; j < m; ++j)
            out[at++] = item[j];
        out[at] = 0;
    }
    if (!more && at == (size_t)k && at < size) {
        k = snprintf(out + at, size - at, " none");
        at += k > 0 ? (size_t)k : 0;
    }
    if (more && at < size) {
        k = snprintf(out + at, size - at, " (+%u more)", more);
        at += k > 0 ? (size_t)k : 0;
    }
    return (int)(at < size ? at : size - 1);
}

// SwitchWLANNet.cpp: en0's counts so far. Under SwitchWLAN's gate.
void switch_wlan_net_trace(struct swlan_trace_counts *out);

#ifdef __cplusplus
}
#endif
