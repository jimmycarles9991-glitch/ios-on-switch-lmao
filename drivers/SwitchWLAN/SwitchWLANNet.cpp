// SPDX-License-Identifier: BSD-2-Clause-Patent
// en0's data path: frames between the radio's DMA buffers and IONetworkingFamily's queues, each copied once. Received
// frames go from the receive buffers into mbufs and onto the interface's input queue, one flush (one ifnet input) per
// interrupt; frames to send come from the stack's output queue through outputStart, written from the mbuf chains
// straight into the transmit buffers. The family's ifnet carries the stack's compat nexus and flowswitch, which take
// the mbufs as they are (attached, not copied). Taps, counters and link events are the family's.
#include "OriginalIOEthernetController.h"
#include "OriginalIOEthernetInterface.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <net/if.h>
#include <net/kpi_interface.h>
#include <sys/mbuf.h>
#include "SwitchWLANNet.h"
#include "SwitchWLANStation.h"
#include "SwitchWLANTrace.h"

namespace {

// IONetworkMedium.h (IONetworkingFamily 3.4): IFM_AVALID, IFM_ACTIVE.
constexpr UInt32 kIONetworkLinkValid = 0x1, kIONetworkLinkActive = 0x2;

IOEthernetController *gController;
IOEthernetInterface *gInterface;
uint32_t gRxFrames, gRxErrors, gRxNoBuffer, gTxOversize, gTxStarts, gTxBatches, gRxBatches;

// The link as the controller last reported it, under gLinkLock: setLinkStatus is called from the radio's worker and
// from wifid's requests.
IOLock *gLinkLock;
bool gLinkUp;
uint64_t gLinkBps;

// The multicast filter: the stack's groups and whether a filter wants every group.
uint8_t gGroups[SWLAN_MCAST_MAX][6];
unsigned gGroupCount;
bool gAllGroups;

// Fed by on_rx and tx_fill, both under SwitchWLAN's gate (the work loop's); read under it too. Debug kernels only
// (the stats' probe line).
#if DEVELOPMENT || DEBUG
constexpr bool kTrace = true;
#else
constexpr bool kTrace = false;
#endif
swlan_trace gTrace;

uint64_t trace_now()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

void report_link()
{
    if (!gController || !gLinkLock)
        return;
    IOLockLock(gLinkLock);
    gController->setLinkStatus(kIONetworkLinkValid | (gLinkUp ? kIONetworkLinkActive : 0), nullptr,
                               gLinkUp ? gLinkBps : 0);
    IOLockUnlock(gLinkLock);
}

void push_multicast()
{
    const bool all = gAllGroups || gGroupCount > SWLAN_MCAST_MAX;
    switch_wlan_multicast(gGroups, all ? 0 : gGroupCount, all ? 1 : 0);
}

// Work loop only: a received frame onto the interface's input queue.
void on_rx(const uint8_t *eth, size_t length)
{
    if (!gInterface || length < 14)
        return;
    if (kTrace) {
        const swlan_frame_facts facts = swlan_frame_classify(eth, length);
        if (facts.kind != SWLAN_FRAME_OTHER)
            swlan_trace_note_received(&gTrace, facts, (eth[0] & 1) == 0, trace_now());
    }

    // Two bytes ahead of the Ethernet header put the IP header on a 4-byte boundary. The packet may come back as a
    // chain: each buffer gets only what fits in it (sheng i469: setting the whole length on the first one let the stack
    // read past that buffer's end).
    enum : size_t {
        Align = 2
    };

    mbuf_t m = nullptr;
    if (mbuf_allocpacket(MBUF_DONTWAIT, length + Align, nullptr, &m) || !m) {
        gRxNoBuffer++;
        return;
    }
    size_t done = 0;
    bool first = true;
    for (mbuf_t n = m; n; n = mbuf_next(n)) {
        size_t room = mbuf_maxlen(n) - mbuf_leadingspace(n);
        if (first) {
            if (room < Align + 14)
                break;
            mbuf_setdata(n, static_cast<uint8_t *>(mbuf_datastart(n)) + mbuf_leadingspace(n) + Align, 0);
            room -= Align;
        }
        const size_t take = length - done < room ? length - done : room;
        memcpy(mbuf_data(n), eth + done, take);
        mbuf_setlen(n, take);
        done += take;
        first = false;
    }
    if (done != length) {
        gRxNoBuffer++;
        mbuf_freem(m);
        return;
    }
    mbuf_pkthdr_setlen(m, length);
    if (gInterface->enqueueInputPacket(m) == kIOReturnSuccess) {
        ++gRxFrames;
    } else {
        ++gRxErrors;
    }
}

// Work loop, after each interrupt's frames: one input-thread wakeup for them all.
void on_rx_flush()
{
    if (gInterface && gInterface->flushInputQueue())
        ++gRxBatches;
}

// Work loop: the radio has room again (or the station is up): en0's start thread runs again.
void on_tx_ready()
{
    if (ifnet_t ifp = swlan::net::ifnet())
        ifnet_start(ifp);
}

void on_link_lost(int reason)
{
    swlan::net::link_lost(reason);
}

const struct swlan_events gEvents = {swlan::net::scan_done,
                                     swlan::net::associated,
                                     swlan::net::connect_done,
                                     on_link_lost,
                                     on_rx,
                                     on_rx_flush,
                                     on_tx_ready};

// Frames on their way out: written one at a time into the radio's DMA buffers (switch_wlan_tx_frames, under its
// gate), each copied once, straight from the mbuf chain. Sent and dropped frames are freed by the caller after the
// batch, outside the gate (tx_done).
struct TxCursor {
    mbuf_t next;                // not yet written
    mbuf_t sentHead, sentTail;  // written into the radio's buffers
    mbuf_t droppedHead;         // too short, too long or unreadable
    uint32_t packets, dropped;
};

size_t tx_fill(void *context, uint8_t *buffer, size_t room)
{
    auto *c = static_cast<TxCursor *>(context);
    while (mbuf_t m = c->next) {
        c->next = mbuf_nextpkt(m);
        mbuf_setnextpkt(m, nullptr);
        const size_t length = mbuf_pkthdr_len(m);
        if (length < 14 || length > room || mbuf_copydata(m, 0, length, buffer)) {
            mbuf_setnextpkt(m, c->droppedHead);
            c->droppedHead = m;
            ++c->dropped;
            continue;
        }
        if (kTrace) {
            const swlan_frame_facts facts = swlan_frame_classify(buffer, length);
            if (facts.kind != SWLAN_FRAME_OTHER)
                swlan_trace_note_sent(&gTrace, facts, trace_now());
        }
        if (c->sentTail) {
            mbuf_setnextpkt(c->sentTail, m);
        } else {
            c->sentHead = m;
        }
        c->sentTail = m;
        ++c->packets;
        return length;
    }
    return 0;
}

// The family counted the frames as sent when they were dequeued; the ones the radio could not take are errors.
void tx_done(const TxCursor &c)
{
    if (c.sentHead)
        mbuf_freem_list(c.sentHead);
    if (c.droppedHead)
        mbuf_freem_list(c.droppedHead);
    if (c.dropped) {
        if (ifnet_t ifp = swlan::net::ifnet())
            ifnet_stat_increment_out(ifp, 0, 0, c.dropped);
        __atomic_fetch_add(&gTxOversize, c.dropped, __ATOMIC_RELAXED);
    }
}

constexpr unsigned kTxBatch = 64;  // frames per dequeue: one gate hold and one doorbell
// Frames dequeued but not taken (the room went between the two calls: a handshake frame or AWDL took buffers): held
// here and sent first next time. Start thread only, except gTxPurge.
mbuf_t gTxHeld;
// en0's queued frames are for a network that went: the stack's queue is purged at once, what is held here at the
// start thread's next run, so none go out on the next network.
bool gTxPurge;

}  // namespace

void swlan::net::attached(IOEthernetController *controller, IOEthernetInterface *interface)
{
    gController = controller;
    gInterface = interface;
    report_link();
}

// The start thread (one run at a time): frames from the stack's output queue into the radio, as many as it has
// buffers for. With none, kIOReturnNoResources: the family stops until tx_ready's ifnet_start, and the frames wait in
// the stack's queue meanwhile (its AQM and flow control decide what waits and what is dropped, not a full ring).
IOReturn swlan::net::output(IOEthernetInterface *interface)
{
    __atomic_fetch_add(&gTxStarts, 1u, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&gTxPurge, false, __ATOMIC_ACQ_REL) && gTxHeld) {
        mbuf_freem_list(gTxHeld);
        gTxHeld = nullptr;
    }
    for (;;) {
        const unsigned room = switch_wlan_tx_room();
        if (!room)
            return kIOReturnNoResources;
        if (!gTxHeld) {
            mbuf_t head = nullptr;
            const IOReturn r = interface->dequeueOutputPackets(room < kTxBatch ? room : kTxBatch, &head);
            if (r != kIOReturnSuccess || !head)
                return r == kIOReturnSuccess ? kIOReturnNoFrames : r;
            gTxHeld = head;
        }
        TxCursor c = {gTxHeld, nullptr, nullptr, nullptr, 0, 0};
        switch_wlan_tx_frames(tx_fill, &c, UINT32_MAX);
        gTxHeld = c.next;
        tx_done(c);
        __atomic_fetch_add(&gTxBatches, 1u, __ATOMIC_RELAXED);
        if (gTxHeld && !c.packets && !c.dropped)
            return kIOReturnNoResources;
    }
}

void swlan::net::multicast(const uint8_t (*groups)[6], unsigned count, bool all)
{
    gGroupCount = count;
    memcpy(gGroups, groups, (count < SWLAN_MCAST_MAX ? count : SWLAN_MCAST_MAX) * sizeof(gGroups[0]));
    gAllGroups = all;
    push_multicast();
}

void swlan::net::multicast_all(bool all)
{
    gAllGroups = all;
    push_multicast();
}

void swlan::net::set_link(bool up)
{
    if (!gLinkLock)
        return;
    IOLockLock(gLinkLock);
    gLinkUp = up;
    IOLockUnlock(gLinkLock);
    report_link();
}

void swlan::net::purge()
{
    if (ifnet_t ifp = ifnet())
        ifnet_purge(ifp);
    __atomic_store_n(&gTxPurge, true, __ATOMIC_RELEASE);
}

ifnet_t swlan::net::ifnet()
{
    return gInterface ? gInterface->getIfnet() : nullptr;
}

extern "C" void switch_wlan_net_counters(struct swlan_net_counters *out)
{
    out->rx_input = gRxFrames;
    out->rx_no_mbuf = gRxNoBuffer;
    out->rx_input_errors = gRxErrors;
    out->tx_oversize = __atomic_load_n(&gTxOversize, __ATOMIC_RELAXED);
    out->tx_starts = __atomic_load_n(&gTxStarts, __ATOMIC_RELAXED);
    out->tx_batches = __atomic_load_n(&gTxBatches, __ATOMIC_RELAXED);
    out->rx_batches = gRxBatches;
}

extern "C" void switch_wlan_net_trace(struct swlan_trace_counts *out)
{
    *out = gTrace.c;
}

// The link's transmit PHY rate: the controller's IOLinkSpeed and the ifnet's bandwidths.
extern "C" void switch_wlan_net_rate(uint64_t bps)
{
    if (!bps || !gLinkLock)
        return;
    IOLockLock(gLinkLock);
    const bool changed = gLinkBps != bps;
    gLinkBps = bps;
    IOLockUnlock(gLinkLock);
    if (changed)
        report_link();
}

void switch_wlan_net_start(IOService *radio, IOWorkLoop *loop, const uint8_t mac[6])
{
    if (gLinkLock)
        return;
    gLinkLock = IOLockAlloc();
    if (!gLinkLock) {
        IOLog("SwitchWLAN: no lock for en0\n");
        return;
    }
    switch_wlan_set_events(&gEvents);
    swlan::net::publish(radio, loop, mac);
}
