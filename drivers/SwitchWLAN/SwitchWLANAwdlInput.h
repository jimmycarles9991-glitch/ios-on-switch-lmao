// SPDX-License-Identifier: BSD-2-Clause-Patent
// awdl0's received Ethernet frames as packets for ifnet_input (SwitchWLANAwdl.cpp's hostDeliver and deliverPackets).
#ifndef SWITCH_WLAN_AWDL_INPUT_H
#define SWITCH_WLAN_AWDL_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <libkern/libkern.h>
#include <net/ethernet.h>
#include <sys/mbuf.h>

// One buffer for the whole frame: deliverPackets points the link header at mbuf_data() of the first mbuf, where
// ether_demux reads it. For a frame over MHLEN but not over MINCLSIZE (289 to 576 bytes on arm64) mbuf_allocpacket
// otherwise returns a chain of two empty mbufs, and mbuf_copyback writes into an mbuf only what its length covers and
// grows only the last one: the first would stay empty and the stack would demux on whatever its previous user left
// there (mbufs are not cleared on free).
inline mbuf_t switch_wlan_awdl_input_packet(const uint8_t *eth, size_t length)
{
    unsigned int chunks = 1;
    mbuf_t m = nullptr;
    if (mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &m) || !m)
        return nullptr;
    if (mbuf_copyback(m, 0, length, eth, MBUF_DONTWAIT)) {
        mbuf_freem(m);
        return nullptr;
    }
    mbuf_pkthdr_setlen(m, length);
    return m;
}

#if DEVELOPMENT || DEBUG
struct switch_wlan_awdl_input_facts {
    bool valid;
    uint32_t frame, ip6, head;
    uint16_t csum;
};

// The frames awdl_data.c's TCP trace prints a line for.
inline bool switch_wlan_awdl_input_traced(const uint8_t *eth, size_t length)
{
    return length >= ETHER_HDR_LEN + 60 && eth[12] == 0x86 && eth[13] == 0xdd && eth[ETHER_HDR_LEN + 6] == 6;
}

struct switch_wlan_awdl_sum {
    uint32_t sum;
    bool odd;
};

inline void switch_wlan_awdl_sum_add(switch_wlan_awdl_sum &s, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        s.sum += s.odd ? bytes[i] : static_cast<uint32_t>(bytes[i]) << 8;
        s.odd = !s.odd;
    }
}

// The packet as the stack takes it once deliverPackets has trimmed the link header off its front: the IPv6 bytes in
// its chain, 40 plus the IPv6 header's payload length, the upper-layer checksum residue over those bytes with the
// pseudo-header (0 when good), and the bytes in its first mbuf, where deliverPackets points the link header.
inline switch_wlan_awdl_input_facts switch_wlan_awdl_input_inspect(mbuf_t m)
{
    switch_wlan_awdl_input_facts facts = {true, 0, 0, static_cast<uint32_t>(mbuf_len(m)), 0};
    size_t total = 0;
    for (mbuf_t n = m; n; n = mbuf_next(n))
        total += mbuf_len(n);
    facts.frame = total > ETHER_HDR_LEN ? static_cast<uint32_t>(total - ETHER_HDR_LEN) : 0;
    uint8_t ip6[40];
    if (facts.frame < sizeof ip6 || mbuf_copydata(m, ETHER_HDR_LEN, sizeof ip6, ip6))
        return facts;
    const uint32_t payload = static_cast<uint32_t>(ip6[4]) << 8 | ip6[5];
    facts.ip6 = static_cast<uint32_t>(sizeof ip6) + payload;
    const uint8_t pseudo[8] = {0, 0, ip6[4], ip6[5], 0, 0, 0, ip6[6]};
    switch_wlan_awdl_sum sum = {};
    switch_wlan_awdl_sum_add(sum, ip6 + 8, 32);
    switch_wlan_awdl_sum_add(sum, pseudo, sizeof pseudo);
    size_t skip = ETHER_HDR_LEN + sizeof ip6;
    size_t left = payload < facts.frame - sizeof ip6 ? payload : facts.frame - sizeof ip6;
    for (mbuf_t n = m; n && left; n = mbuf_next(n)) {
        const size_t length = mbuf_len(n);
        if (skip >= length) {
            skip -= length;
            continue;
        }
        const size_t take = length - skip < left ? length - skip : left;
        switch_wlan_awdl_sum_add(sum, static_cast<const uint8_t *>(mbuf_data(n)) + skip, take);
        skip = 0;
        left -= take;
    }
    while (sum.sum >> 16)
        sum.sum = (sum.sum & 0xffffu) + (sum.sum >> 16);
    facts.csum = static_cast<uint16_t>(~sum.sum & 0xffffu);
    return facts;
}

inline void switch_wlan_awdl_input_describe(const switch_wlan_awdl_input_facts &facts, char *line, size_t size)
{
    if (facts.valid)
        snprintf(line, size, "frame %u ip6 %u csum 0x%x head %u", facts.frame, facts.ip6, facts.csum, facts.head);
    else
        snprintf(line, size, "frame -");
}
#endif

#endif
