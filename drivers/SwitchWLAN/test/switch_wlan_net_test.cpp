// en0's data path (drivers/SwitchWLAN/SwitchWLANNet.cpp) between a model of the radio's transmit buffers and a model of
// IONetworkingFamily's queues (switch_wlan_net_stubs.h): every frame crosses once, in order and intact, copied once
// each way; frames wait in the stack's queue while the radio has no buffer; held frames survive a lost buffer and die
// with a purge; the link goes to the controller as IOLinkStatus with the rate; bringing en0 up starts the family's
// output thread, without which no frame ever reaches outputStart.
#include <cstdint>
#include <cstring>
#include <vector>
#include <gtest/gtest.h>
#include "../SwitchWLANNet.h"
#include "../SwitchWLANStation.h"
#include "switch_wlan_net_stubs.h"

namespace {

IOEthernetController gController;
IOEthernetInterface gInterface;
const swlan_events *gEvents;

// The radio: kBuffers transmit buffers, each written by the driver's fill and freed by a completion.
constexpr unsigned kBuffers = 32;
constexpr size_t kBufferBytes = 1600;
unsigned gFree = kBuffers;
int gStealOne;  // buffers another sender (a handshake frame, AWDL) takes between tx_room and tx_frames
std::vector<std::vector<uint8_t>> gSent;
unsigned gDoorbells;
std::vector<std::vector<uint8_t>> gGroups;
bool gAll;

std::vector<uint8_t> frame(uint32_t serial, size_t length)
{
    std::vector<uint8_t> f(length);
    for (size_t i = 0; i < length; ++i)
        f[i] = static_cast<uint8_t>(serial * 7 + i);
    memcpy(f.data() + 14, &serial, sizeof(serial));
    return f;
}

mbuf_t packet(const std::vector<uint8_t> &bytes)
{
    mbuf_t m = nullptr;
    EXPECT_EQ(mbuf_allocpacket(MBUF_DONTWAIT, bytes.size(), nullptr, &m), 0);
    size_t done = 0;
    for (mbuf_t n = m; n; n = n->next) {
        const size_t take = bytes.size() - done < gMbufRoom ? bytes.size() - done : gMbufRoom;
        memcpy(n->room, bytes.data() + done, take);
        n->len = take;
        done += take;
    }
    m->pktlen = bytes.size();
    return m;
}

void complete(unsigned n)
{
    gFree += n;
    gEvents->tx_ready();
}

// IONetworkInterface::if_start: outputStart only once the driver has started the thread.
IOReturn familyStart(IOEthernetInterface &interface)
{
    return interface.outputHalted ? kIOReturnSuccess : swlan::net::output(&interface);
}

std::vector<uint8_t> received(mbuf_t m)
{
    std::vector<uint8_t> out;
    for (; m; m = m->next)
        out.insert(out.end(), m->room + m->lead, m->room + m->lead + m->len);
    return out;
}

void reset()
{
    gFree = kBuffers;
    gStealOne = 0;
    gSent.clear();
    gDoorbells = 0;
    gMbufCopied = 0;
    gMbufRoom = 2048;
    gMbufExhausted = false;
    gInterface.attached = true;
    gInterface.outputHalted = true;
    gInterface.ifp = {};
    for (mbuf_t m : gInterface.delivered)
        mbuf_freem(m);
    gInterface.delivered.clear();
    while (!gInterface.output.empty()) {
        mbuf_freem(gInterface.output.front());
        gInterface.output.pop_front();
    }
}

}  // namespace

extern "C" {
void switch_wlan_set_events(const struct swlan_events *events)
{
    gEvents = events;
}

unsigned switch_wlan_tx_room(void)
{
    return gFree;
}

unsigned switch_wlan_tx_frames(swlan_tx_fill_fn fill, void *context, unsigned max)
{
    if (gStealOne) {
        gFree -= gStealOne;
        gStealOne = 0;
    }
    unsigned taken = 0;
    while (taken < max && gFree) {
        std::vector<uint8_t> buffer(kBufferBytes);
        const size_t length = fill(context, buffer.data(), buffer.size());
        if (!length)
            break;
        buffer.resize(length);
        gSent.push_back(std::move(buffer));
        --gFree;
        ++taken;
    }
    if (taken)
        ++gDoorbells;
    return taken;
}

void switch_wlan_multicast(const uint8_t (*groups)[6], unsigned count, int all)
{
    gGroups.assign(count, std::vector<uint8_t>(6));
    for (unsigned i = 0; i < count; ++i)
        memcpy(gGroups[i].data(), groups[i], 6);
    gAll = all != 0;
}
}

bool swlan::net::publish(IOService *, IOWorkLoop *, const uint8_t[6])
{
    attached(&gController, &gInterface);
    return true;
}

void swlan::net::scan_done()
{}

void swlan::net::associated()
{}

void swlan::net::connect_done(int)
{}

void swlan::net::link_lost(int)
{}

class NetTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
        switch_wlan_net_start(nullptr, nullptr, mac);
        ASSERT_NE(gEvents, nullptr);
    }

    void SetUp() override
    {
        reset();
    }
};

TEST_F(NetTest, UploadCrossesOnceInOrderWithTheRadioOutOfBuffers)
{
    constexpr uint32_t kFrames = 4000;
    size_t payload = 0;
    for (uint32_t i = 0; i < kFrames; ++i) {
        const auto f = frame(i, 60 + (i * 37) % 1455);
        payload += f.size();
        gInterface.output.push_back(packet(f));
    }
    // The start thread runs; whenever it says no resources, completions free a batch of buffers and tx_ready starts it
    // again (here: the family's output thread calls outputStart once per ifnet_start).
    unsigned startsSeen = 0;
    while (!gInterface.output.empty() || gSent.size() < kFrames) {
        const IOReturn r = swlan::net::output(&gInterface);
        ASSERT_TRUE(r == kIOReturnNoResources || r == kIOReturnNoFrames) << r;
        if (r == kIOReturnNoResources) {
            ASSERT_EQ(gFree, 0u);
            const unsigned before = gInterface.ifp.starts;
            complete(8);
            ASSERT_EQ(gInterface.ifp.starts, before + 1);  // tx_ready restarts the start thread
        }
        ASSERT_LT(++startsSeen, 10000u);
    }
    ASSERT_EQ(gSent.size(), kFrames);
    for (uint32_t i = 0; i < kFrames; ++i)
        ASSERT_EQ(gSent[i], frame(i, 60 + (i * 37) % 1455)) << "frame " << i;
    EXPECT_EQ(gMbufCopied, payload);  // each byte copied once, straight into its transmit buffer
    EXPECT_EQ(gMbufLive, 0u);         // every mbuf freed
    EXPECT_EQ(gInterface.ifp.errorsOut, 0u);
    EXPECT_LE(gDoorbells, kFrames / 8 + 4);  // one doorbell per batch, not per frame
}

TEST_F(NetTest, BringingEn0UpStartsTheFamilysHaltedOutputThread)
{
    gInterface.output.push_back(packet(frame(0, 342)));
    EXPECT_EQ(familyStart(gInterface), kIOReturnSuccess);
    EXPECT_TRUE(gSent.empty());
    gLog.clear();
    EXPECT_EQ(swlan::net::enable(&gInterface), kIOReturnSuccess);
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_NE(gLog[0].find("output thread started"), std::string::npos) << gLog[0];
    EXPECT_FALSE(gInterface.outputHalted);
    EXPECT_EQ(gInterface.ifp.starts, 1u);
    EXPECT_EQ(familyStart(gInterface), kIOReturnNoFrames);
    ASSERT_EQ(gSent.size(), 1u);
    EXPECT_EQ(gSent[0], frame(0, 342));
    EXPECT_EQ(gMbufLive, 0u);
}

TEST_F(NetTest, AnEn0NoLongerAttachedIsNotEnabledAndSaysSo)
{
    gInterface.attached = false;
    gLog.clear();
    EXPECT_EQ(swlan::net::enable(&gInterface), kIOReturnNotAttached);
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_NE(gLog[0].find("output thread did not start"), std::string::npos) << gLog[0];
}

TEST_F(NetTest, FramesDequeuedButNotTakenAreHeldAndGoFirst)
{
    for (uint32_t i = 0; i < 40; ++i)
        gInterface.output.push_back(packet(frame(i, 100)));
    gFree = 4;
    gStealOne = 4;  // the four free buffers go to another sender after outputStart saw them
    EXPECT_EQ(swlan::net::output(&gInterface), kIOReturnNoResources);
    EXPECT_TRUE(gSent.empty());
    gFree = 0;
    complete(kBuffers);
    for (unsigned runs = 0; gSent.size() < 40; ++runs) {
        ASSERT_LT(runs, 100u);
        if (swlan::net::output(&gInterface) == kIOReturnNoResources)
            complete(8);
    }
    for (uint32_t i = 0; i < 40; ++i)
        ASSERT_EQ(gSent[i], frame(i, 100)) << "frame " << i;
    EXPECT_EQ(gMbufLive, 0u);
}

TEST_F(NetTest, APurgeDropsTheHeldFramesAndTheQueue)
{
    for (uint32_t i = 0; i < 10; ++i)
        gInterface.output.push_back(packet(frame(i, 100)));
    gFree = 2;
    gStealOne = 2;
    EXPECT_EQ(swlan::net::output(&gInterface), kIOReturnNoResources);
    swlan::net::purge();
    EXPECT_EQ(gInterface.ifp.purges, 1u);
    while (!gInterface.output.empty()) {
        mbuf_freem(gInterface.output.front());
        gInterface.output.pop_front();
    }
    gFree = kBuffers;
    EXPECT_EQ(swlan::net::output(&gInterface), kIOReturnNoFrames);
    EXPECT_TRUE(gSent.empty());
    EXPECT_EQ(gMbufLive, 0u);
}

TEST_F(NetTest, AFrameLargerThanATransmitBufferIsAnErrorNotAStall)
{
    gInterface.output.push_back(packet(frame(0, 100)));
    gInterface.output.push_back(packet(frame(1, kBufferBytes + 1)));
    gInterface.output.push_back(packet(frame(2, 100)));
    EXPECT_EQ(swlan::net::output(&gInterface), kIOReturnNoFrames);
    ASSERT_EQ(gSent.size(), 2u);
    EXPECT_EQ(gSent[1], frame(2, 100));
    EXPECT_EQ(gInterface.ifp.errorsOut, 1u);
    EXPECT_EQ(gMbufLive, 0u);
}

TEST_F(NetTest, ReceivedFramesAreCopiedOnceAlignedAndFlushedPerInterrupt)
{
    gMbufRoom = 256;  // a frame needs a chain: each buffer gets what fits in it
    std::vector<std::vector<uint8_t>> sent;
    for (uint32_t interrupt = 0; interrupt < 50; ++interrupt) {
        for (uint32_t i = 0; i < 8; ++i) {
            sent.push_back(frame(interrupt * 8 + i, 60 + (interrupt * 8 + i) * 13 % 1455));
            gEvents->rx(sent.back().data(), sent.back().size());
        }
        gEvents->rx_flush();
    }
    EXPECT_EQ(gInterface.flushes, 50u);
    ASSERT_EQ(gInterface.delivered.size(), sent.size());
    for (size_t i = 0; i < sent.size(); ++i) {
        mbuf_t m = gInterface.delivered[i];
        EXPECT_EQ(mbuf_pkthdr_len(m), sent[i].size());
        EXPECT_EQ(m->lead % 4, 2u) << "the IP header 4-byte aligned";
        ASSERT_EQ(received(m), sent[i]) << "frame " << i;
    }
    swlan_net_counters c = {};
    switch_wlan_net_counters(&c);
    EXPECT_GE(c.rx_input, sent.size());
}

TEST_F(NetTest, NoMbufOrNoInterfaceDropsTheFrameAndCountsIt)
{
    swlan_net_counters before = {}, after = {};
    switch_wlan_net_counters(&before);
    const auto f = frame(0, 200);
    gMbufExhausted = true;
    gEvents->rx(f.data(), f.size());
    gMbufExhausted = false;
    gInterface.attached = false;
    gEvents->rx(f.data(), f.size());
    gEvents->rx_flush();
    switch_wlan_net_counters(&after);
    EXPECT_EQ(after.rx_no_mbuf - before.rx_no_mbuf, 1u);
    EXPECT_EQ(after.rx_input_errors - before.rx_input_errors, 1u);
    EXPECT_EQ(gMbufLive, 0u);
}

TEST_F(NetTest, TheLinkAndItsRateGoToTheController)
{
    gController.reports.clear();
    switch_wlan_net_rate(866700000);
    swlan::net::set_link(true);
    swlan::net::set_link(false);
    ASSERT_GE(gController.reports.size(), 2u);
    const auto up = gController.reports[gController.reports.size() - 2];
    const auto down = gController.reports.back();
    EXPECT_EQ(up.status, 0x3u);  // kIONetworkLinkValid | kIONetworkLinkActive
    EXPECT_EQ(up.speed, 866700000u);
    EXPECT_EQ(down.status, 0x1u);
    EXPECT_EQ(down.speed, 0u);
}

TEST_F(NetTest, MulticastAllAndTheGroupList)
{
    const uint8_t groups[2][6] = {{0x01, 0x00, 0x5e, 0, 0, 0xfb}, {0x33, 0x33, 0, 0, 0, 1}};
    swlan::net::multicast(groups, 2, false);
    ASSERT_EQ(gGroups.size(), 2u);
    EXPECT_FALSE(gAll);
    swlan::net::multicast_all(true);
    EXPECT_TRUE(gAll);
    swlan::net::multicast_all(false);
    EXPECT_FALSE(gAll);
    EXPECT_EQ(gGroups.size(), 2u);
}
