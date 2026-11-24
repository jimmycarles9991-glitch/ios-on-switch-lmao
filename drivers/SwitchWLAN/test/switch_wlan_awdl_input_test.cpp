// SPDX-License-Identifier: BSD-2-Clause-Patent
// awdl0's receive path on the host: an A3 receive record (SwitchWLANA3Wire.h) through the AWDL core's data path to
// the packet hostDeliver builds (SwitchWLANAwdlInput.h), read the way XNU's stack reads it, for TCP segments in each
// shape the mbuf allocator gives (one mbuf, a chain of two, a cluster).
#include <gtest/gtest.h>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "bazel/testing/source_text.h"
#include "../SwitchWLANA3Wire.h"
#include "../SwitchWLANAwdlInput.h"
#include "../awdl/awdl.h"

namespace {
constexpr uint8_t kUs[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x0a};
constexpr uint8_t kPeer[6] = {0x3a, 0xd6, 0xaf, 0xed, 0x15, 0x01};
constexpr uint64_t kTsf = 296477332269ull;
constexpr size_t kTcpHeader = 32;  // with the timestamp option, as the Mac sends it

void put16(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}

uint32_t addWords(uint32_t sum, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += static_cast<uint32_t>(p[i]) << 8 | p[i + 1];
    if (n & 1)
        sum += static_cast<uint32_t>(p[n - 1]) << 8;
    return sum;
}

uint16_t fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return static_cast<uint16_t>(sum);
}

// An Ethernet frame from the peer to us: IPv6, then a TCP segment carrying `payload` bytes, checksummed.
std::vector<uint8_t> tcpFrame(size_t payload)
{
    const size_t segment = kTcpHeader + payload;
    std::vector<uint8_t> f(ETHER_HDR_LEN + 40 + segment);
    memcpy(f.data(), kUs, 6);
    memcpy(f.data() + 6, kPeer, 6);
    put16(f.data() + 12, 0x86dd);
    uint8_t *ip = f.data() + ETHER_HDR_LEN;
    ip[0] = 0x60;
    put16(ip + 4, static_cast<uint32_t>(segment));
    ip[6] = 6;
    ip[7] = 64;
    for (unsigned i = 0; i < 16; ++i) {
        ip[8 + i] = static_cast<uint8_t>(i ? 0x30 + i : 0xfe);
        ip[24 + i] = static_cast<uint8_t>(i ? 0x60 + i : 0xfe);
    }
    uint8_t *tcp = ip + 40;
    put16(tcp, 63390);
    put16(tcp + 2, 49767);
    put32(tcp + 4, 1938144569u);
    put32(tcp + 8, 4285386932u);
    tcp[12] = (kTcpHeader / 4) << 4;
    tcp[13] = 0x18;
    put16(tcp + 14, 2048);
    tcp[20] = tcp[21] = 1;
    tcp[22] = 8;
    tcp[23] = 10;
    put32(tcp + 24, 0x01020304u);
    put32(tcp + 28, 0x05060708u);
    for (size_t i = 0; i < payload; ++i)
        tcp[kTcpHeader + i] = static_cast<uint8_t>(i * 7 + 3);
    uint32_t sum = addWords(0, ip + 8, 32) + static_cast<uint32_t>(segment) + 6;
    put16(tcp + 16, static_cast<uint16_t>(~fold(addWords(sum, tcp, segment))));
    return f;
}

// What the payload hands the host for that frame on the air: the AWDL data frame and its FCS behind the A3 header.
std::vector<uint8_t> a3Record(const std::vector<uint8_t> &eth, uint16_t awdlSeq)
{
    std::vector<uint8_t> frame(AWDL_MAX_FRAME);
    frame.resize(awdl_data_encap(eth.data(), eth.size(), kPeer, awdlSeq, frame.data(), frame.size()));
    frame.insert(frame.end(), {0xde, 0xad, 0xbe, 0xef});
    switch_a3_raw_rx header = {};
    header.magic = SWITCH_A3_RAW_RX_MAGIC;
    header.version = SWITCH_A3_RAW_RX_VERSION;
    header.header_bytes = sizeof header;
    header.frame_bytes = static_cast<uint16_t>(frame.size());
    header.rssi = -40;
    header.flags = SWITCH_A3_RX_FCS_PRESENT;
    header.tsf = kTsf;
    header.session = header.clock_epoch = 1;
    header.config_generation = 4;
    std::vector<uint8_t> record(sizeof header);
    memcpy(record.data(), &header, sizeof header);
    record.insert(record.end(), frame.begin(), frame.end());
    return record;
}

// SwitchWLANAwdl.cpp's hostDeliver and hostQueued, which the test below pins to these calls.
struct Host {
    switch_wlan_awdl_input_facts last;
    std::vector<mbuf_t> packets;
    std::vector<std::string> probes;
};

Host gHost;

int hostTx(void *, const uint8_t *, size_t, uint16_t, uint64_t, int)
{
    return 0;
}

void hostDeliver(void *, const uint8_t *eth, size_t length)
{
    gHost.last = {};
    mbuf_t m = switch_wlan_awdl_input_packet(eth, length);
    if (!m)
        return;
    gHost.last = switch_wlan_awdl_input_inspect(m);
    gHost.packets.push_back(m);
}

void hostQueued(void *, char *line, size_t size)
{
    switch_wlan_awdl_input_describe(gHost.last, line, size);
}

void hostProbe(void *, const char *line)
{
    gHost.probes.emplace_back(line);
}

class SwitchWLANAwdlInput : public testing::TestWithParam<size_t> {
protected:
    void SetUp() override
    {
        gHost = {};
        memory_ = std::make_unique<std::max_align_t[]>(awdl_core_size() / sizeof(std::max_align_t) + 1);
        core_ = reinterpret_cast<awdl_core *>(memory_.get());
        awdl_config cfg;
        awdl_config_default(&cfg, kUs);
        awdl_host host = {};
        host.tx = hostTx;
        host.deliver = hostDeliver;
        host.probe = hostProbe;
        host.queued = hostQueued;
        ASSERT_EQ(awdl_core_init(core_, &host, &cfg), 0);
        awdl_core_start(core_, kTsf - 1000000);
        gHost.probes.clear();
    }

    void TearDown() override
    {
        for (mbuf_t m : gHost.packets)
            mbuf_freem(m);
        EXPECT_EQ(gMbufLive, 0u);
    }

    std::unique_ptr<std::max_align_t[]> memory_;
    awdl_core *core_ = nullptr;
};

// deliverPackets points the stack's link header at mbuf_data() of the packet's first mbuf and trims it off the
// chain; ether_demux reads the destination and the ethertype there, ip6_input and tcp_input the chain.
TEST_P(SwitchWLANAwdlInput, StackReadsTheFrameThatArrived)
{
    const size_t payload = GetParam();
    const auto eth = tcpFrame(payload);
    const auto record = a3Record(eth, 7);
    switch_a3_raw_rx header = {};
    size_t frameLength = 0;
    ASSERT_TRUE(switch_a3_rx_decode(record.data(), record.size(), 1, 1, 4, &header, &frameLength));
    const awdl_rx_meta meta = {header.tsf, 5745, header.rssi};
    awdl_core_rx(core_, record.data() + sizeof header, frameLength, &meta);
    ASSERT_EQ(gHost.packets.size(), 1u);

    mbuf_t m = gHost.packets[0];
    uint8_t link[ETHER_HDR_LEN];
    memcpy(link, mbuf_data(m), sizeof link);
    mbuf_adj(m, ETHER_HDR_LEN);
    std::vector<uint8_t> ip(eth.size() - ETHER_HDR_LEN);
    EXPECT_EQ(0, memcmp(link, eth.data(), sizeof link)) << "frame of " << eth.size() << " bytes";
    EXPECT_EQ(mbuf_pkthdr_len(m), ip.size());
    ASSERT_EQ(mbuf_copydata(m, 0, ip.size(), ip.data()), 0);
    EXPECT_EQ(0, memcmp(ip.data(), eth.data() + ETHER_HDR_LEN, ip.size()));

    char suffix[64];
    snprintf(suffix, sizeof suffix, " frame %zu ip6 %zu csum 0x0 head %zu", ip.size(), ip.size(), eth.size());
    char line[208];
    snprintf(line, sizeof line,
             "probe tcp in 63390>49767 seq 1938144569 ack 4285386932 flags 0x18 len %zu ch 149 tsf %llu retry 0 "
             "awdl-seq 7%s",
             payload, static_cast<unsigned long long>(kTsf), suffix);
    ASSERT_EQ(gHost.probes.size(), 1u);
    EXPECT_EQ(gHost.probes[0], line);
}

// Through MHLEN (288 bytes) one mbuf, through MINCLSIZE (576) two, then a cluster: the Mac's 88-byte segments, its
// 273 to 366-byte ones, its 1188 and 1428-byte ones, and the edges.
INSTANTIATE_TEST_SUITE_P(FrameShapes, SwitchWLANAwdlInput,
                         testing::ValuesIn(std::vector<size_t>{0, 88, 202, 203, 273, 308, 366, 490, 491, 1188, 1428}));

TEST(SwitchWLANAwdlInputFacts, NameADamagedOrShortPacket)
{
    auto eth = tcpFrame(273);
    eth.back() ^= 0x40;
    mbuf_t m = switch_wlan_awdl_input_packet(eth.data(), eth.size());
    ASSERT_NE(m, nullptr);
    auto facts = switch_wlan_awdl_input_inspect(m);
    EXPECT_EQ(facts.frame, eth.size() - ETHER_HDR_LEN);
    EXPECT_EQ(facts.ip6, facts.frame);
    EXPECT_NE(facts.csum, 0);
    mbuf_freem(m);

    eth = tcpFrame(273);
    put16(eth.data() + ETHER_HDR_LEN + 4, static_cast<uint32_t>(kTcpHeader + 273 + 8));
    m = switch_wlan_awdl_input_packet(eth.data(), eth.size());
    ASSERT_NE(m, nullptr);
    facts = switch_wlan_awdl_input_inspect(m);
    EXPECT_EQ(facts.ip6, facts.frame + 8);
    mbuf_freem(m);

    char line[64];
    switch_wlan_awdl_input_describe({}, line, sizeof line);
    EXPECT_STREQ(line, "frame -");
    EXPECT_EQ(gMbufLive, 0u);
}

TEST(SwitchWLANAwdlInputDriver, HostDeliverAndDeliverPacketsAreWhatTheTestModels)
{
    const auto source = switch_ios::test::SourceText("../SwitchWLANAwdl.cpp");
    const auto deliver = switch_ios::test::Block(source, "void hostDeliver(void *, const uint8_t *eth, size_t length)");
    EXPECT_TRUE(deliver.before("mbuf_t m = switch_wlan_awdl_input_packet(eth, length);",
                               "gQueued = switch_wlan_awdl_input_inspect(m);"));
    EXPECT_FALSE(deliver.has("mbuf_allocpacket("));
    EXPECT_FALSE(deliver.has("mbuf_copyback("));
    const auto input = switch_ios::test::Block(source, "void deliverPackets(ifnet_t ifp, mbuf_t input)");
    EXPECT_TRUE(input.before("mbuf_pkthdr_setheader(m, mbuf_data(m));", "mbuf_adj(m, ETHER_HDR_LEN);"));
    EXPECT_TRUE(input.before("mbuf_adj(m, ETHER_HDR_LEN);", "ifnet_input(ifp, m, &inc);"));
    EXPECT_TRUE(switch_ios::test::Block(source, "void hostQueued(void *, char *line, size_t size)")
                    .has("switch_wlan_awdl_input_describe(gQueued, line, size);"));
}
}  // namespace
