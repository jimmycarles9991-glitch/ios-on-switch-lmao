// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU's mbufs for switch_wlan_awdl_input_test.cpp, as both arm64 kernels build them (no config_mbuf_mcache in
// MASTER.arm64*): 512-byte mbufs holding 288 bytes (struct m_hdr_common is 224 bytes, MHLEN == MLEN), 2 KB and 4 KB
// clusters, MINCLSIZE = MLEN + MHLEN. mbuf_allocpacket follows m_allocpacket_internal (bsd/kern/uipc_mbuf.c) for one
// packet of up to one cluster, and mbuf_copyback is kpi_mbuf.c's, the same in xnu-12377.121.6 and xnu-10063.141.1.
// Buffers come back holding what their last user left (mz_alloc_n passes Z_NOZZC, mz_free is zfree_nozero): here
// gStale.
#ifndef SWITCH_TEST_SWITCH_WLAN_AWDL_INPUT_STUBS_H
#define SWITCH_TEST_SWITCH_WLAN_AWDL_INPUT_STUBS_H
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

using errno_t = int;

#define ETHER_HDR_LEN 14

enum mbuf_how_t {
    MBUF_WAITOK = 0,
    MBUF_DONTWAIT = 1
};

constexpr size_t kMLEN = 288, kMHLEN = 288, kMINCLSIZE = kMLEN + kMHLEN;
constexpr size_t kMCLBYTES = 2048, kMBIGCLBYTES = 4096;

inline uint8_t gStale = 0xa5;
inline size_t gMbufLive;

struct mbuf {
    mbuf *m_next = nullptr;
    size_t m_len = 0;
    uint8_t *m_data = nullptr;
    bool pkthdr = false;
    size_t pktlen = 0;
    size_t size = 0;
    uint8_t *buffer = nullptr;
    uint8_t dat[kMLEN];
};

using mbuf_t = mbuf *;

inline mbuf_t testMbuf(bool pkthdr, size_t cluster)
{
    auto *m = new mbuf;
    m->pkthdr = pkthdr;
    m->size = cluster ? cluster : kMLEN;
    m->buffer = cluster ? new uint8_t[cluster] : m->dat;
    memset(m->buffer, gStale, m->size);
    m->m_data = m->buffer;
    ++gMbufLive;
    return m;
}

inline size_t testTrailing(mbuf_t m)
{
    return static_cast<size_t>(m->buffer + m->size - (m->m_data + m->m_len));
}

inline void mbuf_freem(mbuf_t m)
{
    while (m) {
        mbuf_t next = m->m_next;
        if (m->buffer != m->dat)
            delete[] m->buffer;
        delete m;
        --gMbufLive;
        m = next;
    }
}

inline errno_t mbuf_allocpacket(mbuf_how_t, size_t packetlen, unsigned int *maxchunks, mbuf_t *out)
{
    if (!packetlen || packetlen > kMBIGCLBYTES)
        return 22;
    size_t bufsize = packetlen <= kMINCLSIZE ? packetlen : packetlen > kMCLBYTES ? kMBIGCLBYTES : kMCLBYTES;
    unsigned nsegs = 1;
    if (bufsize > kMHLEN && bufsize <= kMINCLSIZE) {
        if (maxchunks && *maxchunks == 1)
            bufsize = kMCLBYTES;
        else
            nsegs = 2;
    }
    if (maxchunks) {
        if (*maxchunks && nsegs > *maxchunks) {
            *maxchunks = nsegs;
            return 55;
        }
        *maxchunks = nsegs;
    }
    mbuf_t m = testMbuf(true, bufsize > kMINCLSIZE ? bufsize : 0);
    if (nsegs == 2)
        m->m_next = testMbuf(false, 0);
    *out = m;
    return 0;
}

inline errno_t mbuf_copyback(mbuf_t m, size_t off, size_t len, const void *data, mbuf_how_t)
{
    if (!m || !len || !data)
        return 22;
    const mbuf_t start = m;
    size_t totlen = 0, mlen = 0;
    const auto *cp = static_cast<const uint8_t *>(data);
    while (off > (mlen = m->m_len)) {
        off -= mlen;
        totlen += mlen;
        if (!m->m_next) {
            mbuf_t n = testMbuf(false, 0);
            memset(n->buffer, 0, n->size);
            n->m_len = std::min(kMLEN, len + off);
            m->m_next = n;
        }
        m = m->m_next;
    }
    while (len > 0) {
        mlen = std::min(m->m_len - off, len);
        if (mlen < len && !m->m_next && testTrailing(m) > 0) {
            const size_t grow = std::min(testTrailing(m), len - mlen);
            mlen += grow;
            m->m_len += grow;
        }
        memcpy(m->m_data + off, cp, mlen);
        cp += mlen;
        len -= mlen;
        mlen += off;
        off = 0;
        totlen += mlen;
        if (!len)
            break;
        if (!m->m_next) {
            mbuf_t n = testMbuf(false, len > kMINCLSIZE ? kMCLBYTES : 0);
            n->m_len = std::min(n->size, len);
            m->m_next = n;
        }
        m = m->m_next;
    }
    if (start->pkthdr && start->pktlen < totlen)
        start->pktlen = totlen;
    return 0;
}

inline void *mbuf_data(mbuf_t m)
{
    return m->m_data;
}

inline size_t mbuf_len(mbuf_t m)
{
    return m->m_len;
}

inline mbuf_t mbuf_next(mbuf_t m)
{
    return m->m_next;
}

inline void mbuf_pkthdr_setlen(mbuf_t m, size_t len)
{
    m->pktlen = len;
}

inline size_t mbuf_pkthdr_len(mbuf_t m)
{
    return m->pktlen;
}

inline errno_t mbuf_copydata(mbuf_t m, size_t offset, size_t length, void *out)
{
    auto *dst = static_cast<uint8_t *>(out);
    for (; m && length; m = m->m_next) {
        if (offset >= m->m_len) {
            offset -= m->m_len;
            continue;
        }
        const size_t take = std::min(m->m_len - offset, length);
        memcpy(dst, m->m_data + offset, take);
        dst += take;
        length -= take;
        offset = 0;
    }
    return length ? 22 : 0;
}

// m_adj(m, len) for len > 0, which mbuf_adj calls: the bytes come off the front of the chain, mbuf by mbuf.
inline void mbuf_adj(mbuf_t m, size_t len)
{
    const mbuf_t start = m;
    const size_t trimmed = len;
    for (; m && len; m = m->m_next) {
        if (m->m_len <= len) {
            len -= m->m_len;
            m->m_len = 0;
        } else {
            m->m_len -= len;
            m->m_data += len;
            len = 0;
        }
    }
    start->pktlen -= trimmed - len;
}

#endif
