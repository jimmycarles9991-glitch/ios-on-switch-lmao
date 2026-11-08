// SPDX-License-Identifier: BSD-2-Clause-Patent
// awdl0: the AWDL interface iPadOS's wifip2pd asks en0 to create (VIRTUAL_IF_CREATE, role AWDL), as 21H461 expects it
// (out/agent-scratch/r12-awdl-stack/AWDL-CONTRACT.md):
//   - an Ethernet ifnet "awdl<unit>" with IFEF_AWDL (so XNU uses ND6ALT: no neighbour discovery; peers are installed
//     with ifnet_notice_node_presence when the stack finds them);
//   - its Apple80211 requests on the socket path (awdl/awdl_apple80211.c answers them);
//   - an IOService wifip2pd matches (IOInterfaceName, IO80211InterfaceRole "AWDL") with the IO80211 user client that
//     IO80211.framework's P2P event monitoring requires: selectors 0-5, the event ring (memory type 1) and its state
//     page (type 2), and the async wake-up;
//   - the AWDL protocol itself (awdl/awdl_core.c) on the radio services of SwitchWLANAwdlRadio.h (r12-awdl).
//
// en0 advertises AWDL (capability bit 32) when a backend can carry it: the firmware's own AWDL (the bring-up probe
// found interface_create v3 or awdl_if), or the soft one (monitor rx, data injection and action frames on a chosen
// channel). Otherwise it never does, and awdl0 exists but stays down (a80211_awdl_down_if) with no IO80211 service and
// no AWDL stack; 7.35.252.108 has neither. A debug build's switch-awdl-soft boot-arg runs the soft backend there
// anyway, for a device run to show what its actframe and receive paths do. Nothing touches the firmware before
// wifip2pd enables AWDL on awdl0: that is when the radio is opened (event mask) and the interface made. Every step
// that has never run on hardware fails with one log line and AWDL off.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include <sys/param.h>  // (before sys/mbuf.h: the cluster sizes)
#include <sys/errno.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}
#include "SwitchWLANAwdlRadio.h"
#include "SwitchWLANAwdlSoft.h"
#include "SwitchWLANStation.h"
#include "awdl/awdl.h"
#include "awdl/awdl_apple80211.h"
#include "awdl/awdl_bcm.h"

#define SIOCSA80211 0x802869c8UL
#define SIOCGA80211 0xc02869c9UL

namespace {

struct apple80211req {
    char req_if_name[16];
    int32_t req_type;
    int32_t req_val;
    uint32_t req_len;
    user_addr_t req_data __attribute__((aligned(8)));
};

constexpr uint32_t kRingSize = 64 * 1024;
constexpr IOOptionBits kSharedOptions =
    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
constexpr uint32_t kMaxRequest = 4096;  // largest awdl0 request payload we copy (CHANNELS_INFO is answered EOPNOTSUPP)

IOLock *gLock;      // the core, the facade, the ring and the interface state
IOLock *gSend;      // one awdl0 output at a time: taken before gLock, held across the radio's transmit
int gCapable = -1;  // -1 not decided yet (the radio cannot answer yet), 0 off, 1 on
// The radio is opened for AWDL (event_msgs) the first time wifip2pd enables AWDL, never before (ensureRadio).
IOLock *gOpenLock;
int gRadioOpen;  // 0 not yet, 1 open, -1 refused for good (one log line)
const struct swlan_awdl_caps *gCaps;
struct awdl_core *gCore;
a80211_awdl gFacade;
awdl_tsf_clock gClock;
bool gSoft;  // the soft backend (actframe excursions) rather than the firmware's own AWDL
thread_call_t gTimer, gDeferred, gCreate, gIovarWork;
ifnet_t gIf;
uint8_t gMac[6];
unsigned gUnit;
char gName[16];
bool gCreating, gRxStarted;
IOService *gService;
IOBufferMemoryDescriptor *gRingMemory, *gStateMemory;
a80211_ring gRing;
OSAsyncReference64 gAsync;
bool gAsyncArmed, gMonitoring;
IOUserClient *gEventClient;  // the client whose ring this is

// work done outside gLock: kernel neighbour entries and packets for the stack
struct Presence {
    uint8_t addr[6];
    int8_t rssi;
    uint8_t present;
};

Presence gPresence[16];
unsigned gPresenceCount;
mbuf_t gInputHead, gInputTail;

// One log line per (process, reason) for a request awdl0 turns down.
void refuseOnce(const char *reason)
{
    static struct {
        char process[32];
        const char *reason;
    } said[8];

    static unsigned count;
    char process[32] = {};
    proc_selfname(process, sizeof(process));
    for (unsigned i = 0; i < count; ++i) {
        if (said[i].reason == reason && !strncmp(said[i].process, process, sizeof(process)))
            return;
    }
    if (count < sizeof(said) / sizeof(said[0])) {
        strlcpy(said[count].process, process, sizeof(said[count].process));
        said[count++].reason = reason;
    }
    IOLog("SwitchWLAN: AWDL: refused for %s: %s\n", process, reason);
}

// The address our AWDL frames leave with: awdl0's, unless the radio sends them through actframe, which takes the
// station's whatever awdl0 is (said once: peers then know us by the station's address).
const uint8_t *airAddress()
{
    if (!gSoft || !gCaps || gCaps->can_inject)
        return gMac;
    if (memcmp(gMac, gCaps->self_mac, 6))
        refuseOnce("awdl0's address stays on awdl0: actframe sends AWDL frames with the station's address");
    return gCaps->self_mac;
}

uint64_t hostNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

// The stack's clock: the host's, in microseconds. Neither backend needs the radio's TSF: the firmware keeps AWDL time
// in the native one, and the soft one times everything, receive times included, on this clock.
// The rule for every radio call that waits on the firmware (swlan_awdl_var): never under gLock and never from a radio
// callback (the radio's work loop would wait on itself). They run from iovarWork only, with gLock dropped.
uint64_t nowTsf()
{
    const uint64_t ns = hostNs();
    if (!gClock.valid)
        awdl_tsf_clock_anchor(&gClock, ns / 1000, ns);
    return awdl_tsf_clock_now(&gClock, ns);
}

void armTimer(uint64_t tsf)
{
    if (!gTimer || tsf == UINT64_MAX)
        return;
    uint64_t deadline = 0;
    uint64_t leeway = 0;
    nanoseconds_to_absolutetime(awdl_tsf_clock_host(&gClock, tsf), &deadline);
    // AWDL accepts 3 ms of synchronisation error and a slot opens 2 ms in (awdl_internal.h AWDL_SLOT_GUARD_US): a
    // millisecond of leeway lets the timer coalesce with others at no cost to either
    nanoseconds_to_absolutetime(1000000, &leeway);
    thread_call_enter_delayed_with_leeway(gTimer, nullptr, deadline, leeway, THREAD_CALL_DELAY_LEEWAY);
}

void rearm()
{
    if (gCore && awdl_core_running(gCore))
        armTimer(awdl_core_timer(gCore, nowTsf()));
}

// ---- events to wifip2pd ---------------------------------------------------------------------------------------------
void wakeEventClient();  // below the user client (sendAsyncResult64 is its protected member)

void postEvent(uint32_t event, const void *payload, uint32_t length)
{
    if (!gMonitoring)
        return;
    if (a80211_ring_post(&gRing, event, gName, payload, length)) {
        static unsigned said;
        if (said++ < 2)
            IOLog("SwitchWLAN: AWDL: event ring full, event %u dropped (%u so far)\n", event, gRing.dropped);
        return;
    }
    if (gAsyncArmed)
        wakeEventClient();
}

// ---- what the core asks of us (gLock held) --------------------------------------------------------------------------
int hostTx(void *, const uint8_t *frame, size_t length, uint16_t freq, uint64_t stayUntil, int noAck)
{
    struct swlan_tx_req r = {};
    r.frame = frame;
    r.len = length;
    r.freq_mhz = freq;
    r.rate_mcs = 0xff;
    r.flags = static_cast<uint8_t>((noAck ? SWLAN_TX_NO_ACK : 0u) | SWLAN_TX_ON_CHANNEL_ONLY);
    const uint64_t now = nowTsf();
    r.stay_us = stayUntil > now ? static_cast<uint32_t>(stayUntil - now) : 0;
    return swlan_awdl_tx(&r) < 0 ? -1 : 0;
}

// The radio takes slots of at most 65535 us (RADIO-API §4): each 64-TU AWDL slot goes as two halves.
void hostSchedule(void *, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchorTsf)
{
    static struct swlan_chan_slot slots[2 * AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        slots[static_cast<size_t>(2) * i] = slots[2 * i + 1] = {awdl_chan_freq(seq[i]), 32768};
    swlan_status st;
    switch_wlan_status_get(&st);
    const struct swlan_chan_seq s = {anchorTsf, 32768u * 2 * AWDL_SEQ_SLOTS, 2 * AWDL_SEQ_SLOTS,
                                     static_cast<uint16_t>(st.state == SWLAN_STA_UP ? st.freq_mhz : 0), slots};
    const int e = swlan_awdl_chanseq_set(&s);
    static unsigned said;
    if (e < 0 && said++ < 2)
        IOLog("SwitchWLAN: AWDL: the radio refused the channel sequence (%d)\n", e);
}

void hostPeer(void *, const uint8_t addr[6], int present, int8_t rssi)
{
    a80211_awdl_peer(&gFacade, addr, present, rssi);
    if (gPresenceCount < sizeof gPresence / sizeof gPresence[0]) {
        Presence &p = gPresence[gPresenceCount++];
        memcpy(p.addr, addr, 6);
        p.rssi = rssi;
        p.present = static_cast<uint8_t>(present != 0);
        if (gDeferred)
            thread_call_enter(gDeferred);
    }
}

void hostDeliver(void *, const uint8_t *eth, size_t length)
{
    if (!gIf)
        return;
    mbuf_t m = nullptr;
    if (mbuf_allocpacket(MBUF_DONTWAIT, length, nullptr, &m) || !m)
        return;
    if (mbuf_copyback(m, 0, length, eth, MBUF_DONTWAIT)) {
        mbuf_freem(m);
        return;
    }
    mbuf_pkthdr_setlen(m, length);
    mbuf_pkthdr_setrcvif(m, gIf);
    if (gInputTail) {
        mbuf_setnextpkt(gInputTail, m);
    } else {
        gInputHead = m;
    }
    gInputTail = m;
}

void hostLog(void *, const char *line)
{
    IOLog("SwitchWLAN: %s\n", line);
}

// Native backend: the core's firmware requests, in order, from a thread (they wait for the firmware's answer), on
// the firmware interface the core names; each answer goes back to the core (awdl_core_fw_result), which ends the
// session on a refused interface creation and resends a refused awdl_payload in its other form.
struct Iovar {
    int32_t ifidx;
    uint8_t get;
    char name[32];
    uint16_t length;
    uint8_t value[512];
};

Iovar gIovars[40];  // the series' bring-up is 18 requests after the creation, a resync 6
unsigned gIovarHead, gIovarCount;
bool gIovarBusy;  // one worker drains the queue (a thread call entered while it runs may run again beside it)

int hostIovar(void *, int32_t ifidx, int get, const char *name, const void *buf, size_t length)
{
    if (gIovarCount == sizeof gIovars / sizeof gIovars[0] || length > sizeof gIovars[0].value || ifidx < 0 ||
        ifidx > 255) {
        static unsigned said;
        if (said++ < 4) {
            IOLog("SwitchWLAN: AWDL: %s not queued (queue %u, %zu bytes, ifidx %d)\n", name, gIovarCount, length,
                  ifidx);
        }
        return -1;
    }
    Iovar &v = gIovars[(gIovarHead + gIovarCount++) % (sizeof gIovars / sizeof gIovars[0])];
    v.ifidx = ifidx;
    v.get = static_cast<uint8_t>(get != 0);
    strlcpy(v.name, name, sizeof v.name);
    v.length = static_cast<uint16_t>(length);
    if (length)
        memcpy(v.value, buf, length);
    if (gIovarWork)
        thread_call_enter(gIovarWork);
    return 0;
}

void iovarWork(thread_call_param_t, thread_call_param_t)
{
    static Iovar v;  // the busy worker's only (gIovarBusy)
    static uint8_t reply[64];
    IOLockLock(gLock);
    if (gIovarBusy) {  // the running worker takes what was queued, in order
        IOLockUnlock(gLock);
        return;
    }
    gIovarBusy = true;
    IOLockUnlock(gLock);
    for (;;) {
        IOLockLock(gLock);
        if (!gIovarCount) {
            gIovarBusy = false;
            IOLockUnlock(gLock);
            return;
        }
        v = gIovars[gIovarHead];
        gIovarHead = (gIovarHead + 1) % (sizeof gIovars / sizeof gIovars[0]);
        gIovarCount--;
        IOLockUnlock(gLock);
        size_t got = 0;
        const int e = swlan_awdl_var(v.get ? 0 : 1, static_cast<uint8_t>(v.ifidx), v.name, v.value, v.length,
                                     v.get ? reply : nullptr, v.get ? sizeof reply : 0, &got);
        int status = swlan_is_fwerr(e) ? swlan_fwerr_code(e) : AWDL_FW_NO_ANSWER;
        if (!e)
            status = 0;
        if (status) {
            static unsigned said;
            if (said++ < 24) {
                IOLog("SwitchWLAN: AWDL: %s (%u bytes, ifidx %d) answered %d %s\n", v.name, v.length, v.ifidx, status,
                      bcme_name(status));
            }
        }
        IOLockLock(gLock);
        if (gCore)
            awdl_core_fw_result(gCore, v.name, status, nowTsf());
        rearm();
        IOLockUnlock(gLock);
    }
}

// The firmware's AWDL interface takes frames under the radio's gate, and the radio calls the stack (under gLock)
// from its work loop with that gate held: so a frame is never handed over with gLock held. ifOutput gives the core a
// stash for the one frame it may send, and passes it on after dropping gLock.
struct TxStash {
    size_t length;
    uint8_t frame[AWDL_MAX_FRAME];
};

TxStash *gStash;  // only while ifOutput holds gLock

int hostTxEth(void *, const uint8_t *eth, size_t length)
{
    if (!gStash || gStash->length || length > sizeof gStash->frame)
        return -1;
    memcpy(gStash->frame, eth, length);
    gStash->length = length;
    return 0;
}

// Neighbour entries and received packets, outside gLock (ifnet_notice_node_presence takes the routing lock, which
// the stack's output path may hold while it calls us).
// From the radio's work loop only packets are passed on (presence = false): neighbour entries take the routing lock and
// wait for gDeferred's thread.
void drain(bool presence = true)
{
    Presence todo[16];
    unsigned n = 0;
    mbuf_t input = nullptr;
    IOLockLock(gLock);
    if (presence) {
        n = gPresenceCount;
        memcpy(todo, gPresence, n * sizeof todo[0]);
        gPresenceCount = 0;
    }
    input = gInputHead;
    gInputHead = gInputTail = nullptr;
    // NOLINTNEXTLINE(misc-const-correctness): a const ifnet_t is a const pointer (misc-misplaced-const)
    ifnet_t ifp = gIf;
    IOLockUnlock(gLock);
    for (mbuf_t m = input, next = nullptr; m; m = next) {
        next = mbuf_nextpkt(m);
        mbuf_setnextpkt(m, nullptr);
        if (!ifp) {
            mbuf_freem(m);
            continue;
        }
        bpf_tap_in(ifp, DLT_EN10MB, m, nullptr, 0);
        mbuf_pkthdr_setheader(m, mbuf_data(m));
        const size_t length = mbuf_pkthdr_len(m);
        mbuf_adj(m, ETHER_HDR_LEN);
        struct ifnet_stat_increment_param inc = {};
        inc.packets_in = 1;
        inc.bytes_in = static_cast<uint32_t>(length);
        ifnet_input(ifp, m, &inc);
    }
    for (unsigned i = 0; ifp && i < n; ++i) {
        struct sockaddr_dl sdl = {};
        sdl.sdl_len = sizeof sdl;
        sdl.sdl_family = AF_LINK;
        sdl.sdl_type = IFT_ETHER;
        sdl.sdl_index = static_cast<u_short>(ifnet_index(ifp));
        sdl.sdl_alen = ETHER_ADDR_LEN;
        memcpy(LLADDR(&sdl), todo[i].addr, ETHER_ADDR_LEN);
        uint8_t srvinfo[48] = {};
        const errno_t e = todo[i].present ? ifnet_notice_node_presence(ifp, reinterpret_cast<struct sockaddr *>(&sdl),
                                                                       todo[i].rssi, 0, 0, srvinfo)
                                          : ifnet_notice_node_absence(ifp, reinterpret_cast<struct sockaddr *>(&sdl));
        if (e)
            IOLog("SwitchWLAN: AWDL: node %s failed (%d)\n", todo[i].present ? "presence" : "absence", e);
    }
}

void deferredWork(thread_call_param_t, thread_call_param_t)
{
    drain();
}

// Once a minute, to the unified log (not the console): whether the firmware's AWDL frames and events arrive at all.
void statsLine()
{
#if DEVELOPMENT || DEBUG
    static uint64_t last;
    const uint64_t ns = hostNs();
    if (ns - last < 60000000000ULL || !gCore)
        return;
    last = ns;
    const awdl_stats *st = awdl_core_stats(gCore);
    char soft[160] = "";
    switch_wlan_soft_stat(soft, sizeof(soft));
    os_log(
        OS_LOG_DEFAULT,
        "SwitchWLAN: AWDL stats: ifidx %d, fw events %u (unknown %u), AWs %u, frames %u parsed %u bad %u, fw role %d "
        "(%u), peers +%u -%u, syncs %u, iovars %u refused %u skipped %u, data rx %u tx %u drop %u refused %u "
        "not-AWDL %u, mode changes %u, sent PSF %u MIF %u, sync error %d us, resyncs %u%s",
        awdl_core_fw_ifidx(gCore), st->fw_events, st->fw_unknown, st->aws, st->fw_frames, st->af_rx, st->af_bad,
        st->fw_role, st->roles, st->peer_adds, st->peer_dels, st->syncs_pushed, st->iovars, st->iovar_refused,
        st->iovar_skipped, st->data_rx, st->data_tx, st->data_drop, st->tx_refused, st->data_not_awdl, st->mode_changes,
        st->psf_tx, st->mif_tx, st->last_sync_error_us, st->resyncs, soft);
#endif
}

void timerFired(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    if (gCore && awdl_core_running(gCore))
        awdl_core_set_infra(gCore, swlan_awdl_infra_freq(), nowTsf());
    rearm();
    statsLine();
    IOLockUnlock(gLock);
    drain();
}

// ---- the radio's callbacks -------------------------------------------------------------------------------------------
void radioRx(void *, const uint8_t *frame, size_t length, const struct swlan_rx_meta *meta)
{
    if (meta && (meta->flags & SWLAN_RX_FCS_PRESENT)) {
        if (length < 4)
            return;
        length -= 4;
    }
    IOLockLock(gLock);
    if (gCore && awdl_core_running(gCore)) {
        const struct awdl_rx_meta m = {meta && meta->tsf ? meta->tsf : nowTsf(),
                                       meta ? meta->freq_mhz : static_cast<uint16_t>(0),
                                       meta ? meta->rssi_dbm : static_cast<int8_t>(-90)};
        awdl_core_rx(gCore, frame, length, &m);
        rearm();
    }
    IOLockUnlock(gLock);
    if (gInputHead)
        drain(false);
}

// Native backend: the firmware's AWDL events and the frames of its AWDL interface.
void fwEvent(void *, const struct swlan_fw_event *e, const uint8_t *data, size_t length)
{
    IOLockLock(gLock);
    if (gCore && awdl_core_running(gCore)) {
        const struct awdl_fw_event fe = {
            e->type,  e->status,   e->reason, {e->addr[0], e->addr[1], e->addr[2], e->addr[3], e->addr[4], e->addr[5]},
            e->ifidx, e->bsscfgidx};
        awdl_core_fw_event(gCore, &fe, data, length, nowTsf());
        rearm();
    }
    IOLockUnlock(gLock);
    if (gInputHead)
        drain(false);
}

// The firmware's AWDL interface: 802.3 frames with the AWDL data header, which the core strips (hostDeliver).
void ethRx(void *, const uint8_t *frame, size_t length)
{
    IOLockLock(gLock);
    if (gCore && awdl_core_running(gCore))
        awdl_core_rx_eth(gCore, frame, length, nowTsf());
    IOLockUnlock(gLock);
    drain(false);
}

void radioEvent(void *, const struct swlan_awdl_event *e)
{
    if (e->type != SWLAN_AWDL_CHIP_LOST)
        return;
    IOLog("SwitchWLAN: AWDL: the radio was lost; AWDL stopped (wifip2pd re-enables it after recovery)\n");
    IOLockLock(gLock);
    if (gCore)
        awdl_core_stop(gCore);
    gFacade.enabled = 0;
    gRxStarted = false;
    IOLockUnlock(gLock);
    drain();
}

// ---- the facade's operations (gLock held) ---------------------------------------------------------------------------
int opStart(void *)
{
    // the access point's channel now; timerFired follows it (the core re-sends the schedule when it changes)
    awdl_core_set_infra(gCore, swlan_awdl_infra_freq(), nowTsf());
    if (!gRxStarted && !gSoft) {
        int e = swlan_awdl_fw_events(fwEvent, nullptr);
        if (e >= 0)
            e = swlan_awdl_eth_rx(ethRx, nullptr);
        if (e < 0) {
            IOLog("SwitchWLAN: AWDL: the radio would not deliver the firmware's AWDL events and frames (%d): off\n", e);
            return ENXIO;
        }
        swlan_awdl_events(radioEvent, nullptr);  // chip loss
        gRxStarted = true;
    }
    if (!gRxStarted) {
        const int e = swlan_awdl_rx_start(radioRx, nullptr);
        if (e < 0) {
            IOLog("SwitchWLAN: AWDL: the radio would not start receiving (%d): AWDL off\n", e);
            return ENXIO;
        }
        swlan_awdl_events(radioEvent, nullptr);
        swlan_awdl_rx_filter(gMac, awdl_bssid, 0);  // our address, so the radio ACKs unicast to it (STATUS.md Q1)
        gRxStarted = true;
    }
    return 0;
}

void opStop(void *)
{
    if (gTimer)
        thread_call_cancel(gTimer);
    if (gSoft) {  // native: the core's "awdl_if down" stops the firmware; its events stay on
        swlan_awdl_chanseq_clear();
        swlan_awdl_rx_stop();
        gRxStarted = false;
    }
}

void opPost(void *, uint32_t event, const void *payload, uint32_t length)
{
    postEvent(event, payload, length);
}

void opLog(void *, const char *line)
{
    IOLog("SwitchWLAN: %s\n", line);
}

// A short suffix for SwitchWLAN's 5-s stats line, so en0 rx and AWDL state read off one line (empty when off).
extern "C" int switch_wlan_awdl_stat(char *buf, size_t size)
{
    if (!gLock || !gCore)
        return 0;
    int n = 0;
    IOLockLock(gLock);
    if (gCore && awdl_core_running(gCore)) {
        const awdl_stats *st = awdl_core_stats(gCore);
        const enum awdl_role role = awdl_core_role(gCore);
        const char *roleName = role == AWDL_ROLE_FOLLOWER ? "follower" : "off";
        if (role == AWDL_ROLE_ROOT)
            roleName = "root";
        n = snprintf(buf, size, "; AWDL running %s %s, %u/16 slots away, peers +%u-%u af_rx %u aws %u fw_ev %u",
                     roleName, awdl_core_mode(gCore) == AWDL_MODE_TRANSFER ? "transfer" : "idle",
                     awdl_slots_away(awdl_core_sequence(gCore), swlan_awdl_infra_freq()), st->peer_adds, st->peer_dels,
                     st->af_rx, st->aws, st->fw_events);
    }
    IOLockUnlock(gLock);
    return n < 0 ? 0 : n;
}

// ---- the ifnet ------------------------------------------------------------------------------------------------------
errno_t ifOutput(ifnet_t ifp, mbuf_t m)
{
    bpf_tap_out(ifp, DLT_EN10MB, m, nullptr, 0);
    const size_t length = mbuf_pkthdr_len(m);
    static uint8_t frame[AWDL_MAX_FRAME];  // under gLock
    static TxStash stash;                  // filled under gLock, sent after it (one output at a time: gSend)
    int r = -1;
    IOLockLock(gSend);
    IOLockLock(gLock);
    stash.length = 0;
    if (length >= ETHER_HDR_LEN && length <= sizeof frame - 64 && gCore && !mbuf_copydata(m, 0, length, frame)) {
        gStash = &stash;
        r = awdl_core_output(gCore, frame, length, nowTsf());
        gStash = nullptr;
        rearm();
    }
    IOLockUnlock(gLock);
    if (!r && stash.length && swlan_awdl_eth_tx(stash.frame, stash.length) < 0)
        r = -1;
    IOLockUnlock(gSend);
    if (r) {
        ifnet_stat_increment_out(ifp, 0, 0, 1);
    } else {
        ifnet_stat_increment_out(ifp, 1, static_cast<uint32_t>(length), 0);
    }
    mbuf_freem(m);
    return 0;
}

// The first AWDL_SYNC_ENABLED on: the radio opens for AWDL (the AWDL event bits in event_msgs; the station is not
// touched before). Thread context, without gLock (it waits for the firmware). 0 or an errno for wifip2pd.
int ensureRadio()
{
    IOLockLock(gOpenLock);
    int e = 0;
    if (gRadioOpen < 0) {
        e = ENXIO;
    } else if (!gRadioOpen) {
        const struct swlan_awdl_caps *caps = nullptr;
        const int r = swlan_awdl_open(&caps);
        if (r >= 0 && caps) {
            gRadioOpen = 1;
        } else {
            // the radio says why in its own line; a radio that was only down (recovering) is asked again next time
            IOLog("SwitchWLAN: AWDL: wifip2pd enabled AWDL but the radio did not open for it (%d): AWDL off%s\n", r,
                  r == -ENXIO ? " until the next enable" : "");
            if (r != -ENXIO)
                gRadioOpen = -1;
            e = ENXIO;
        }
    }
    IOLockUnlock(gOpenLock);
    return e;
}

int awdlRequest(bool set, apple80211req &rq)
{
    if (rq.req_len > kMaxRequest)
        return EINVAL;
    const uint32_t capacity = rq.req_len;
    uint8_t *data = capacity ? static_cast<uint8_t *>(IOMallocZeroData(capacity)) : nullptr;
    if (capacity && !data)
        return ENOMEM;
    int e = 0;
    if (set && capacity && rq.req_data)
        e = copyin(rq.req_data, data, capacity);
    // apple80211_awdl_sync_enabled {version, flags, enabled}: AWDL on needs the radio opened first, without gLock
    if (!e && set && rq.req_type == A80211_IOC_AWDL_SYNC_ENABLED && capacity >= 12 &&
        (static_cast<unsigned>(data[8]) | data[9] | data[10] | data[11]))
        e = ensureRadio();
    uint32_t length = set ? capacity : 0;
    if (!e) {
        IOLockLock(gLock);
        e = a80211_awdl_request(&gFacade, static_cast<int>(set), rq.req_type, &rq.req_val, data, &length, capacity,
                                nowTsf());
        rearm();
        IOLockUnlock(gLock);
    }
    if (!e && !set && length && rq.req_data)
        e = copyout(data, rq.req_data, length);
    if (!e && !set)
        rq.req_len = length;
    if (data)
        IOFreeData(data, capacity);
    drain();
    return e;
}

errno_t answerMedia(void *arg, int status)
{
    struct ifmediareq64 *m = static_cast<struct ifmediareq64 *>(arg);
    static const int media = IFM_IEEE80211 | IFM_AUTO;  // wifip2pd: (ifm_active & 0xe0) == IFM_IEEE80211
    m->ifm_current = media;
    m->ifm_mask = 0;
    m->ifm_status = status;
    m->ifm_active = media;
    if (m->ifmu_ulist && m->ifm_count > 0 && copyout(&media, m->ifmu_ulist, sizeof media))
        return EFAULT;
    m->ifm_count = 1;
    return 0;
}

errno_t ifIoctl(ifnet_t ifp, unsigned long cmd, void *arg)
{
    switch (cmd) {
    case SIOCGA80211:
    case SIOCSA80211:
        return awdlRequest(cmd == SIOCSA80211, *static_cast<apple80211req *>(arg));
    case SIOCGIFMEDIA64:
        return answerMedia(arg, IFM_AVALID | (gCore && awdl_core_running(gCore) ? IFM_ACTIVE : 0u));
    case SIOCSIFLLADDR: {
        // wifip2pd's AWDL address: the stack transmits with it and the kernel derives fe80::/64 from it
        const struct ifreq *ifr = static_cast<const struct ifreq *>(arg);
        if (ifr->ifr_addr.sa_len != ETHER_ADDR_LEN)
            return EINVAL;
        const uint8_t *mac = reinterpret_cast<const uint8_t *>(ifr->ifr_addr.sa_data);
        if (!memcmp(mac, gMac, 6))
            return 0;
        // While AWDL runs on the firmware, its awdl_if and payload carry the address: changed only while off
        if (!gSoft && gCore && awdl_core_running(gCore)) {
            refuseOnce("awdl0's address changes only while AWDL is off");
            return EBUSY;
        }
        const errno_t e = ifnet_set_lladdr(ifp, mac, ETHER_ADDR_LEN);
        if (e)
            return e;
        IOLockLock(gLock);
        memcpy(gMac, mac, 6);
        if (gCore)
            awdl_core_set_addr(gCore, airAddress());
        if (gRxStarted)
            swlan_awdl_rx_filter(gMac, awdl_bssid, 0);
        IOLockUnlock(gLock);
        return 0;
    }
    case SIOCSIFFLAGS:
    case SIOCSIFMTU:
    case SIOCADDMULTI:
    case SIOCDELMULTI:
    case SIOCSIFADDR:
    case SIOCSIFMEDIA:
        return 0;
    default: {
        static unsigned said;
        if (said++ < 8)
            IOLog("SwitchWLAN: AWDL: %s request %lx not handled\n", gName, cmd);
        return EOPNOTSUPP;
    }
    }
}

void ifDetached(ifnet_t ifp)
{
    ifnet_release(ifp);
}

void ifEventStub(ifnet_t, const struct kev_msg *)
{}

}  // namespace

// ---- the IOService and its user client -----------------------------------------------------------------------------
// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchAwdlUserClient : public IOUserClient {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchAwdlUserClient);

public:
    static void wake(OSAsyncReference64 reference)
    {
        sendAsyncResult64(reference, kIOReturnSuccess, nullptr, 0);
    }

    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

private:
    static IOReturn passthrough(IOExternalMethodArguments *a);
};

OSDefineMetaClassAndStructors(SwitchAwdlUserClient, IOUserClient);

namespace {
void wakeEventClient()
{
    SwitchAwdlUserClient::wake(gAsync);
}
}  // namespace

IOReturn SwitchAwdlUserClient::clientClose()
{
    IOLockLock(gLock);
    if (gEventClient == this) {
        gMonitoring = false;
        if (gAsyncArmed)
            releaseAsyncReference64(gAsync);
        gAsyncArmed = false;
        gEventClient = nullptr;
    }
    IOLockUnlock(gLock);
    terminate();
    return kIOReturnSuccess;
}

IOReturn SwitchAwdlUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    IOBufferMemoryDescriptor *md = type == 2 ? gStateMemory : nullptr;
    if (type == 1)
        md = gRingMemory;
    if (!md)
        return kIOReturnBadArgument;
    md->retain();
    *memory = md;
    *options = type == 1 ? kIOMapReadOnly : 0;
    return kIOReturnSuccess;
}

// Selector 0: an Apple80211 request over the user client. The input is a buffer whose last 0x28 bytes are an
// apple80211req (name, type, value, length, pointer) and whose payload starts at +8; IO80211's Bind sends
// DRIVER_VERSION this way as its sanity test. Larger than an inband struct, so it arrives as a descriptor.
IOReturn SwitchAwdlUserClient::passthrough(IOExternalMethodArguments *a)
{
    const uint32_t size = a->structureInputDescriptor ? static_cast<uint32_t>(a->structureInputDescriptor->getLength())
                                                      : a->structureInputSize;
    if (size < 0x30 + 8 || size > 0x40000)
        return kIOReturnBadArgument;
    uint8_t head[8];
    uint8_t trailer[0x28];
    if (a->structureInputDescriptor) {
        IOMemoryDescriptor *md = a->structureInputDescriptor;
        if (md->prepare() != kIOReturnSuccess)
            return kIOReturnVMError;
        const bool ok = md->readBytes(0, head, 8) == 8 && md->readBytes(size - 0x28, trailer, 0x28) == 0x28;
        md->complete();
        if (!ok)
            return kIOReturnVMError;
    } else {
        memcpy(head, a->structureInput, 8);
        memcpy(trailer, static_cast<const uint8_t *>(a->structureInput) + size - 0x28, 0x28);
    }
    uint32_t cmd = 0;
    int32_t type = 0;
    int32_t val = 0;
    memcpy(&cmd, head, 4);
    memcpy(&type, trailer + 0x10, 4);
    memcpy(&val, trailer + 0x14, 4);
    if (cmd != SIOCGA80211)
        return kIOReturnUnsupported;  // sets go through the socket (useIOUCWhenPossible = 0)
    uint8_t out[512] = {};
    uint32_t length = 0;
    const uint32_t capacity = a->structureOutputSize < sizeof out ? a->structureOutputSize : sizeof out;
    IOLockLock(gLock);
    const int e = a80211_awdl_request(&gFacade, 0, type, &val, out, &length, capacity, nowTsf());
    IOLockUnlock(gLock);
    if (e)
        return kIOReturnUnsupported;
    memcpy(a->structureOutput, out, length);
    a->structureOutputSize = length;
    return kIOReturnSuccess;
}

IOReturn SwitchAwdlUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                              IOExternalMethodDispatch *, OSObject *, void *)
{
    switch (selector) {
    case 0:
        return passthrough(a);
    case 1:    // useIOUCWhenPossible: no, keep Apple80211RawGet/RawSet on the socket
    case 5: {  // useIOUCEventPipe: yes
        if (a->structureOutputSize < 1 || !a->structureOutput)
            return kIOReturnBadArgument;
        *static_cast<uint8_t *>(a->structureOutput) = static_cast<uint8_t>(selector == 5);
        a->structureOutputSize = 1;
        return kIOReturnSuccess;
    }
    case 3: {  // start event monitoring: this client owns the ring from now on
        IOLockLock(gLock);
        if (gEventClient && gEventClient != this) {
            IOLog("SwitchWLAN: AWDL: a second event monitor on %s takes over the ring\n", gName);
            if (gAsyncArmed)
                releaseAsyncReference64(gAsync);
            gAsyncArmed = false;
        }
        gEventClient = this;
        a80211_ring_init(&gRing, gRingMemory->getBytesNoCopy(), kRingSize,
                         static_cast<volatile uint32_t *>(gStateMemory->getBytesNoCopy()));
        gMonitoring = true;
        IOLockUnlock(gLock);
        return kIOReturnSuccess;
    }
    case 4: {  // stop
        IOLockLock(gLock);
        if (gEventClient == this) {
            gMonitoring = false;
            if (gAsyncArmed)
                releaseAsyncReference64(gAsync);
            gAsyncArmed = false;
            gEventClient = nullptr;
        }
        IOLockUnlock(gLock);
        return kIOReturnSuccess;
    }
    case 2: {  // the wake-up for new events
        if (!a->asyncReference || a->asyncReferenceCount < kOSAsyncRef64Count)
            return kIOReturnBadArgument;
        IOLockLock(gLock);
        if (gAsyncArmed)
            releaseAsyncReference64(gAsync);
        bcopy(a->asyncReference, gAsync, sizeof(OSAsyncReference64));
        gAsyncArmed = true;
        IOLockUnlock(gLock);
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnUnsupported;
    }
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchAwdlInterface : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchAwdlInterface);

public:
    IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE
    {
        if (type != 0)
            return kIOReturnBadArgument;
        SwitchAwdlUserClient *client = OSTypeAlloc(SwitchAwdlUserClient);
        if (!client || !client->initWithTask(task, security, type, properties)) {
            OSSafeReleaseNULL(client);
            return kIOReturnNoMemory;
        }
        if (!client->attach(this)) {
            client->release();
            return kIOReturnError;
        }
        if (!client->start(this)) {
            client->detach(this);
            client->release();
            return kIOReturnError;
        }
        *handler = client;
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(SwitchAwdlInterface, IOService);

namespace {

void publishService()
{
    SwitchAwdlInterface *s = OSTypeAlloc(SwitchAwdlInterface);
    IOService *platform = IOService::getPlatform();
    if (!s || !platform || !s->init()) {
        OSSafeReleaseNULL(s);
        IOLog("SwitchWLAN: AWDL: no registry entry for %s\n", gName);
        return;
    }
    s->setName(gName);
    s->setProperty("IOInterfaceName", gName);
    s->setProperty("BSD Name", gName);
    s->setProperty("IOInterfaceNamePrefix", "awdl");
    // The role is a string under IO80211InterfaceRole, the one wifip2pd prints and matches ("Infrastructure" for en0;
    // the real AppleBCMWLAN driver sets "AWDL" here for awdl0 and sets nothing else). IO80211VirtualInterfaceRole is
    // only for NAN sub-roles ("WiFi-Aware Discovery"/"+Data"); setting it sent wifip2pd down its virtual-interface
    // path (AppleDevice.swift:362), which does not know "AWDL", so it logged "Unable to find role for interface:
    // awdl0" and never built an AWDLInterface or enabled AWDL (r7u, 11-05). Only the normal role key, as the driver does.
    s->setProperty("IO80211InterfaceRole", "AWDL");
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(gUnit), 32)) {
        s->setProperty("IOInterfaceUnit", n);
        n->release();
    }
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(IFT_ETHER), 32)) {
        s->setProperty("IOInterfaceType", n);
        n->release();
    }
    if (OSData *mac = OSData::withBytes(gMac, 6)) {
        s->setProperty("IOMACAddress", mac);
        mac->release();
    }
    if (!s->attach(platform) || !s->start(platform)) {
        s->release();
        IOLog("SwitchWLAN: AWDL: registry entry for %s did not start\n", gName);
        return;
    }
    s->registerService();
    gService = s;
}

// awdl0 is made on a thread call: VIRTUAL_IF_CREATE arrives inside en0's ioctl, and wifip2pd waits for the interface
// (and its IOService) anyway.
void createInterface(thread_call_param_t, thread_call_param_t)
{
    struct ifnet_init_eparams p = {};
    p.ver = IFNET_INIT_CURRENT_VERSION;
    p.len = sizeof p;
    p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
    p.name = "awdl";
    p.unit = gUnit;
    p.family = IFNET_FAMILY_ETHERNET;
    p.subfamily = IFNET_SUBFAMILY_WIFI;
    p.type = IFT_ETHER;
    p.output = ifOutput;
    p.demux = ether_demux;
    p.add_proto = ether_add_proto;
    p.del_proto = ether_del_proto;
    p.check_multi = ether_check_multi;
    p.framer = ether_frameout;
    p.ioctl = ifIoctl;
    p.event = ifEventStub;
    p.detach = ifDetached;
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    p.broadcast_addr = bcast;
    p.broadcast_len = 6;
    ifnet_t ifp = nullptr;
    errno_t e = ifnet_allocate_extended(&p, &ifp);
    if (e || !ifp) {
        IOLog("SwitchWLAN: AWDL: %s allocation failed (%d)\n", gName, e);
        gCreating = false;
        return;
    }
    ifnet_set_mtu(ifp, 1500);
    ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
    ifnet_set_baudrate(ifp, 300000000ULL);
    ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
    ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
    ifnet_set_eflags(ifp, IFEF_AWDL, IFEF_AWDL);  // XNU adds LOCALNET_PRIVATE, IPV6_ND6ALT, RESTRICTED_RECV
    e = ifnet_attach(ifp, nullptr);
    if (e) {
        IOLog("SwitchWLAN: AWDL: %s attach failed (%d)\n", gName, e);
        ifnet_release(ifp);
        gCreating = false;
        return;
    }
    ifnet_set_lladdr(ifp, gMac, 6);
    bpf_attach(ifp, DLT_EN10MB, ETHER_HDR_LEN, nullptr, nullptr);
    IOLockLock(gLock);
    gIf = ifp;
    gCreating = false;
    IOLockUnlock(gLock);
    publishService();
    IOLog("SwitchWLAN: AWDL: %s created (%02x:%02x:%02x:%02x:%02x:%02x)\n", gName, gMac[0], gMac[1], gMac[2], gMac[3],
          gMac[4], gMac[5]);
}

thread_call_t gDownCreate;
static_assert(A80211_IFF_BROADCAST == IFF_BROADCAST && A80211_IFF_SIMPLEX == IFF_SIMPLEX &&
              A80211_IFF_MULTICAST == IFF_MULTICAST && A80211_IFM_AVALID == IFM_AVALID);

errno_t downOutput(ifnet_t ifp, mbuf_t m)
{
    ifnet_stat_increment_out(ifp, 0, 0, 1);
    mbuf_freem(m);
    return ENETDOWN;
}

// Apple80211 requests are refused (EOPNOTSUPP): this awdl0 has no AWDL behind it, and without an IO80211 service
// wifip2pd never adopts it.
errno_t downIoctl(ifnet_t, unsigned long cmd, void *arg)
{
    switch (cmd) {
    case SIOCGIFMEDIA64:
        return answerMedia(arg, a80211_awdl_down_media_status());
    case SIOCSIFFLAGS:
    case SIOCSIFMTU:
    case SIOCADDMULTI:
    case SIOCDELMULTI:
    case SIOCSIFADDR:
        return 0;
    default:
        return EOPNOTSUPP;
    }
}

// en0 is attached before any caller decides the verdict (en0's own CARD_CAPABILITIES, or the worker after en0).
void createDownInterface(thread_call_param_t, thread_call_param_t)
{
    uint8_t station[6] = {};
    ifnet_t en0 = nullptr;
    errno_t e = ifnet_find_by_name("en0", &en0);
    if (!e) {
        e = ifnet_lladdr_copy_bytes(en0, station, sizeof station);
        ifnet_release(en0);
    }
    struct a80211_awdl_down_if d;
    if (e || a80211_awdl_down_if(station, &d)) {
        IOLog("SwitchWLAN: AWDL: awdl0 not made: no station address (%d)\n", e);
        return;
    }
    struct ifnet_init_eparams p = {};
    p.ver = IFNET_INIT_CURRENT_VERSION;
    p.len = sizeof p;
    p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
    p.name = "awdl";
    p.unit = 0;
    p.family = IFNET_FAMILY_ETHERNET;
    p.subfamily = IFNET_SUBFAMILY_WIFI;
    p.type = IFT_ETHER;
    p.output = downOutput;
    p.demux = ether_demux;
    p.add_proto = ether_add_proto;
    p.del_proto = ether_del_proto;
    p.check_multi = ether_check_multi;
    p.framer = ether_frameout;
    p.ioctl = downIoctl;
    p.event = ifEventStub;
    p.detach = ifDetached;
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    p.broadcast_addr = bcast;
    p.broadcast_len = 6;
    ifnet_t ifp = nullptr;
    e = ifnet_allocate_extended(&p, &ifp);
    if (e || !ifp) {
        IOLog("SwitchWLAN: AWDL: awdl0 allocation failed (%d)\n", e);
        return;
    }
    ifnet_set_mtu(ifp, d.mtu);
    ifnet_set_flags(ifp, d.if_flags, 0xffff);
    ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
    ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
    ifnet_set_eflags(ifp, IFEF_AWDL, IFEF_AWDL);
    e = ifnet_attach(ifp, nullptr);
    if (e) {
        IOLog("SwitchWLAN: AWDL: awdl0 attach failed (%d)\n", e);
        ifnet_release(ifp);
        return;
    }
    ifnet_set_lladdr(ifp, d.lladdr, 6);
    bpf_attach(ifp, DLT_EN10MB, ETHER_HDR_LEN, nullptr, nullptr);
    IOLog("SwitchWLAN: AWDL: awdl0 present and down (%02x:%02x:%02x:%02x:%02x:%02x): this radio cannot run AWDL\n",
          d.lladdr[0], d.lladdr[1], d.lladdr[2], d.lladdr[3], d.lladdr[4], d.lladdr[5]);
}

// Under switch_wlan_awdl_capable's `deciding`, once a boot; the caller may be inside en0's ioctl.
void scheduleDownInterface()
{
    gDownCreate = thread_call_allocate(createDownInterface, nullptr);
    if (!gDownCreate) {
        IOLog("SwitchWLAN: AWDL: awdl0 not made: no thread call\n");
        return;
    }
    thread_call_enter(gDownCreate);
}

// The stack's locks, core and thread calls, in this order; false at the first one there is no memory for.
bool allocateStack()
{
    gLock = IOLockAlloc();
    if (!gLock)
        return false;
    gSend = IOLockAlloc();
    if (!gSend)
        return false;
    gOpenLock = IOLockAlloc();
    if (!gOpenLock)
        return false;
    gCore = static_cast<awdl_core *>(IOMallocZero(awdl_core_size()));
    if (!gCore)
        return false;
    gTimer = thread_call_allocate_with_priority(timerFired, nullptr, THREAD_CALL_PRIORITY_HIGH);
    if (!gTimer)
        return false;
    gDeferred = thread_call_allocate(deferredWork, nullptr);
    if (!gDeferred)
        return false;
    gCreate = thread_call_allocate(createInterface, nullptr);
    if (!gCreate)
        return false;
    gIovarWork = thread_call_allocate(iovarWork, nullptr);
    return gIovarWork != nullptr;
}

}  // namespace

namespace {

void startStack(const awdl_radio_verdict &v, const struct swlan_awdl_caps *caps)
{
    gCaps = caps;
    gSoft = v.mode == AWDL_RADIO_SOFT;
    memcpy(gMac, caps->self_mac, 6);
    awdl_config cfg;
    awdl_config_default(&cfg, gMac);
    cfg.backend = gSoft ? AWDL_BACKEND_SOFT : AWDL_BACKEND_NATIVE;
    cfg.fw_api = static_cast<uint8_t>(v.api);
    cfg.fw_absent = bcm_probe_absent(caps->probe);
    const awdl_host host = {nullptr, hostTx, hostSchedule, hostPeer, hostDeliver, hostLog, hostIovar, hostTxEth};
    awdl_core_init(gCore, &host, &cfg);
    if (v.trial) {
        IOLog("SwitchWLAN: AWDL: soft backend trial (switch-awdl-soft=%u) on a radio without %s: action frames out "
              "through actframe, in through WLC_E_ACTION_FRAME_RX%s; AWDL data cannot leave this radio\n",
              caps->soft_trial, v.off == AWDL_OFF_NO_API ? "firmware AWDL" : "a soft backend's needs",
              caps->soft_trial >= 2 ? " and monitor mode" : "");
        return;
    }
    IOLog("SwitchWLAN: AWDL: available, %s backend (native %u, %s; monitor %u, inject %u, action frames %u, hops %u, "
          "mchan %u)\n",
          gSoft ? "soft" : "firmware", caps->has_native_awdl, bcm_awdl_api_name(v.api), caps->can_monitor,
          caps->can_inject, caps->can_tx_action, caps->can_scheduled_hops, caps->can_stay_assoc);
}

}  // namespace

extern "C" int switch_wlan_awdl_capable(void)
{
    if (__atomic_load_n(&gCapable, __ATOMIC_ACQUIRE) >= 0)
        return gCapable;
    // one caller decides; a concurrent one is told "not (yet)" and asks again at its next request
    static unsigned deciding;
    if (__atomic_exchange_n(&deciding, 1u, __ATOMIC_ACQ_REL))
        return 0;
    if (gCapable < 0) {
        int verdict = -1;
        const struct swlan_awdl_caps *caps = nullptr;
        const int e = swlan_awdl_query(&caps);  // what the bring-up found; no firmware request
        const awdl_radio_verdict v = awdl_radio_verdict_of(
            e == 0 && !caps ? -ENODEV : e,
            static_cast<int>((caps != nullptr) && caps->abi_version == SWLAN_AWDL_ABI_VERSION),
            static_cast<int>((caps != nullptr) && caps->has_native_awdl != 0u),
            static_cast<int>((caps != nullptr) && caps->can_monitor != 0u),
            static_cast<int>((caps != nullptr) && caps->can_inject != 0u),
            static_cast<int>((caps != nullptr) && caps->can_tx_action != 0u),
            static_cast<int>((caps != nullptr) && caps->soft_trial != 0u), caps ? caps->probe : nullptr);
        if (v.mode == AWDL_RADIO_UNDECIDED) {
        } else if (v.mode != AWDL_RADIO_OFF && caps && !allocateStack()) {
            IOLog("SwitchWLAN: AWDL: no memory for the stack: off\n");
            verdict = 0;
        } else if (v.mode != AWDL_RADIO_OFF && caps) {
            startStack(v, caps);
            verdict = 1;
        } else if (v.off == AWDL_OFF_NO_RADIO || !caps) {
            IOLog("SwitchWLAN: AWDL: the radio has no AWDL (%d): off\n", e);
            verdict = 0;
        } else if (v.off == AWDL_OFF_ABI) {
            IOLog("SwitchWLAN: AWDL: radio ABI %u, the stack speaks %u: off\n", caps->abi_version,
                  SWLAN_AWDL_ABI_VERSION);
            verdict = 0;
        } else if (v.off == AWDL_OFF_NO_BACKEND) {
            IOLog("SwitchWLAN: AWDL: radio native=%u monitor=%u inject=%u action frames=%u: neither backend can run: "
                  "off\n",
                  caps->has_native_awdl, caps->can_monitor, caps->can_inject, caps->can_tx_action);
            verdict = 0;
        } else {
            IOLog(
                "SwitchWLAN: AWDL: the firmware lists awdl but has neither interface_create v3 nor awdl_if (%s): off\n",
                caps->probe ? "AWDL probe lines" : "probe did not run");
            verdict = 0;
        }
        if (verdict >= 0)
            __atomic_store_n(&gCapable, verdict, __ATOMIC_RELEASE);
        if (verdict == 0)
            scheduleDownInterface();
    }
    __atomic_store_n(&deciding, 0u, __ATOMIC_RELEASE);
    return gCapable > 0 ? 1 : 0;
}

extern "C" int switch_wlan_awdl_vif_create(void *payload, size_t length)
{
    if (length < sizeof(a80211_virt_if_create))
        return EINVAL;
    a80211_virt_if_create *r = static_cast<a80211_virt_if_create *>(payload);
    if (r->role != A80211_VIF_ROLE_AWDL || switch_wlan_awdl_capable() != 1)
        return EOPNOTSUPP;
    IOLockLock(gLock);
    if (!gIf && !gCreating) {
        gUnit = r->unit < 8 ? r->unit : 0;
        snprintf(gName, sizeof gName, "awdl%u", gUnit);
        static const uint8_t zero[6] = {};
        if (memcmp(r->mac, zero, 6))
            memcpy(gMac, r->mac, 6);
        awdl_core_set_addr(gCore, airAddress());
        uint8_t caps[12] = {0x4c, 0x46, 0x0a, 0, 0x01};  // en0's bitmap (SwitchWLANNet kCardCaps) plus bit 32
        const a80211_awdl_ops ops = {nullptr, opStart, opStop, opPost, opLog};
        a80211_awdl_init(&gFacade, gCore, &ops, gName, "en0", caps);
        if (!gRingMemory)
            gRingMemory = IOBufferMemoryDescriptor::withOptions(kSharedOptions, kRingSize, PAGE_SIZE);
        if (!gStateMemory)
            gStateMemory = IOBufferMemoryDescriptor::withOptions(kSharedOptions, PAGE_SIZE, PAGE_SIZE);
        if (!gRingMemory || !gStateMemory) {
            IOLockUnlock(gLock);
            IOLog("SwitchWLAN: AWDL: no memory for the event ring\n");
            return ENOMEM;
        }
        a80211_ring_init(&gRing, gRingMemory->getBytesNoCopy(), kRingSize,
                         static_cast<volatile uint32_t *>(gStateMemory->getBytesNoCopy()));
        gCreating = true;
        thread_call_enter(gCreate);
    }
    memset(r->bsd_name, 0, sizeof r->bsd_name);
    strlcpy(r->bsd_name, gName, sizeof r->bsd_name);
    IOLockUnlock(gLock);
    return 0;
}

extern "C" int switch_wlan_awdl_vif_delete(const void *payload, size_t length)
{
    if (length < sizeof(a80211_virt_if_delete) || gCapable != 1)
        return EOPNOTSUPP;
    const a80211_virt_if_delete *d = static_cast<const a80211_virt_if_delete *>(payload);
    IOLockLock(gLock);
    if (!gIf || strncmp(d->name, gName, sizeof d->name)) {
        IOLockUnlock(gLock);
        return ENXIO;
    }
    awdl_core_stop(gCore);
    opStop(nullptr);
    gFacade.enabled = 0;
    // NOLINTNEXTLINE(misc-const-correctness): a const ifnet_t is a const pointer (misc-misplaced-const)
    ifnet_t ifp = gIf;
    gIf = nullptr;
    IOService *s = gService;
    gService = nullptr;
    IOLockUnlock(gLock);
    drain();
    if (s) {
        s->terminate();
        s->release();
    }
    ifnet_detach(ifp);  // released in ifDetached
    IOLog("SwitchWLAN: AWDL: %s deleted\n", gName);
    return 0;
}
