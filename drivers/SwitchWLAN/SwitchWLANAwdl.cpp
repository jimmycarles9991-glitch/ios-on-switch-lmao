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
// wifip2pd enables AWDL: that is when the radio is opened (event mask) and the interface made. Every step
// that has never run on hardware fails with one log line and AWDL off.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOBSD.h>
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
#include "SwitchWLANAwdlInput.h"
#include "SwitchWLANAwdlNeighbor.h"
#include "SwitchWLANAwdlPresence.h"
#include "SwitchWLANAwdlSoft.h"
#include "SwitchWLANProbeHooks.h"
#include "SwitchWLANStation.h"
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
#include <pexpert/pexpert.h>
#include "SwitchWLANAwdlHomeTrial.h"
extern "C" int switch_awdl_echo_mark(mbuf_t);
#endif
#if DEVELOPMENT || DEBUG
#include "SwitchWLANA3Diagnostic.h"
#include "SwitchWLANAwdlRxTrace.h"
#include "SwitchWLANTrace.h"
#endif
#include "awdl/awdl.h"
#include "awdl/awdl_apple80211.h"
#include "awdl/awdl_bcm.h"
#include "awdl/awdl_radio.h"

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
switch_awdl_radio_clock gRadioClock;
switch_awdl_radio_clock_request gClockRequest;
swlan_awdl_state gRadioState;
int gRadioError;
bool gRestorePending, gRestoring, gRadioLost;
bool gActivationCommitted;
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
using HomeTrial = swlan::SwitchAwdlHomeTrial;
HomeTrial gHomeTrial;
thread_call_t gHomeTrialTimer;
uint16_t gHomeTrialChannel;
const char *gHomeTrialBlocker;
unsigned gHomeTrialBlockLogs;
const swlan_rx_meta *gEchoRxMeta;
unsigned gEchoRxCount;
#endif
#if DEVELOPMENT || DEBUG
unsigned gActivationTransitions;
uint32_t gRawPsf, gRawMif, gRawData, gRawRxGated;
swlan::SwitchAwdlRxActionBudget gA3RxActionBudget;
#endif
bool gSoft;  // the soft backend (actframe excursions) rather than the firmware's own AWDL
thread_call_t gTimer, gDeferred, gCreate, gIovarWork, gRadioWork;
ifnet_t gIf;
uint8_t gMac[6];
unsigned gUnit;
char gName[16];
bool gCreating, gRxStarted;
bool gAddressChanging;
IOService *gService;
IOBufferMemoryDescriptor *gRingMemory, *gStateMemory;
a80211_ring gRing;
OSAsyncReference64 gAsync;
bool gAsyncArmed, gMonitoring;
IOUserClient *gEventClient;  // the client whose ring this is
IOUserClient *gAsyncClient;

// work done outside gLock: kernel neighbour entries and packets for the stack
using PeerChange = SwitchWLANAwdlPresence::Change;
struct PeerSlot {
    uint64_t incarnation;
    SwitchWLANAwdlPresence presence;
    SwitchWLANPeerIdentity bufferedFor;
    mbuf_t head, tail;
    unsigned count;
};
PeerSlot gPeers[AWDL_MAX_PEERS];
bool gPeerWork;
uint64_t gRetryPresenceAfter;
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
const uint8_t *airAddress(const uint8_t *address = gMac)
{
    if (!gSoft || !gCaps || gCaps->can_inject)
        return address;
    if (memcmp(address, gCaps->self_mac, 6))
        refuseOnce("awdl0's address stays on awdl0: actframe sends AWDL frames with the station's address");
    return gCaps->self_mac;
}

uint64_t hostNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

// The rule for every radio call that waits on the firmware (swlan_awdl_var): never under gLock and never from a radio
// callback (the radio's work loop would wait on itself). They run from iovarWork only, with gLock dropped.
uint64_t nowTsf()
{
    const uint64_t ns = hostNs();
    if (gCaps && gCaps->has_tsf_clock)
        return switch_awdl_radio_clock_valid(&gRadioClock, ns) ? awdl_tsf_clock_now(&gRadioClock.affine, ns) : 0;
    if (!gClock.valid)
        awdl_tsf_clock_anchor(&gClock, ns / 1000, ns);
    return awdl_tsf_clock_now(&gClock, ns);
}

void armTimer(uint64_t tsf)
{
    if (!gTimer)
        return;
    const bool radioClock = gCaps && gCaps->has_tsf_clock;
    if (radioClock && !switch_awdl_radio_clock_valid(&gRadioClock, hostNs())) {
        thread_call_cancel(gTimer);
        return;
    }
    uint64_t host =
        tsf == UINT64_MAX ? UINT64_MAX : awdl_tsf_clock_host(radioClock ? &gRadioClock.affine : &gClock, tsf);
    if (radioClock) {
        const uint64_t clockDeadline = switch_awdl_radio_clock_deadline(&gRadioClock);
        if (clockDeadline < host)
            host = clockDeadline;
    }
    if (host == UINT64_MAX) {
        thread_call_cancel(gTimer);
        return;
    }
    uint64_t deadline = 0;
    uint64_t leeway = 0;
    nanoseconds_to_absolutetime(host, &deadline);
    // AWDL accepts 3 ms of synchronisation error and a slot opens 2 ms in (awdl_internal.h AWDL_SLOT_GUARD_US): a
    // millisecond of leeway lets the timer coalesce with others at no cost to either
    nanoseconds_to_absolutetime(radioClock ? 0 : 1000000, &leeway);
    thread_call_enter_delayed_with_leeway(gTimer, nullptr, deadline, leeway, THREAD_CALL_DELAY_LEEWAY);
}

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
void advanceHomeTrial();

void armHomeTrialTimer()
{
    uint64_t deadline;
    nanoseconds_to_absolutetime(gHomeTrial.deadlineNs, &deadline);
    thread_call_enter_delayed(gHomeTrialTimer, deadline);
}

void endHomeTrial(const char *reason)
{
    if (gHomeTrial.phase == HomeTrial::Phase::Off || gHomeTrial.phase == HomeTrial::Phase::Armed)
        return;
    const bool applied = gHomeTrial.phase == HomeTrial::Phase::Active;
    if (applied)
        switch_wlan_a3_trial_boundary(false, gHomeTrialChannel);
    gHomeTrial.cancel();
    thread_call_cancel(gHomeTrialTimer);
    switch_awdl_core_home_trial(gCore, 0, nowTsf());
    IOLog("SwitchWLAN: AWDL: home trial ended (%s), applied %u channel %u gen %u, restoring normal scheduling\n",
          reason, applied, gHomeTrialChannel, gRadioState.applied_generation);
}

void homeTrialTimeout(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    if (gHomeTrial.deadlineNs && hostNs() < gHomeTrial.deadlineNs)
        armHomeTrialTimer();
    else
        advanceHomeTrial();
    IOLockUnlock(gLock);
}
#endif

void suspendRadio()
{
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
    endHomeTrial("radio recovery");
#endif
    gActivationCommitted = false;
    if (gFacade.core)
        switch_a80211_awdl_suspend(&gFacade);
    if (gCore)
        switch_awdl_core_radio_ready(gCore, 0);
    gRestorePending = gFacade.enabled;
}

bool radioPending(int error)
{
    return error == -EAGAIN || error == -ENOBUFS || error == -EBUSY || error == -ESTALE || error == -ENXIO;
}

void requestClock()
{
    if (gCaps && gCaps->requires_radio_ready && gRadioState.social_channels_valid && !gRadioState.social_channels)
        return;
    if (!switch_awdl_radio_clock_request_begin(&gClockRequest, 0))
        return;
    const int error = swlan_awdl_clock_request();
    if (error < 0)
        switch_awdl_radio_clock_request_complete(&gClockRequest, error);
}

bool radioChannelsReady()
{
    return !gCaps || !gCaps->requires_radio_ready ||
           (gRadioState.social_channels_valid && gRadioState.social_channels);
}

uint16_t radioInfraFreq()
{
    const uint16_t station = swlan_awdl_infra_freq();
    if (!station || !gCaps || !gCaps->requires_radio_ready || !gRadioState.social_channels_valid)
        return station;
    const uint8_t channel = bcm_chanspec_primary(gRadioState.home_chanspec);
    const uint16_t observed = awdl_chan_freq({channel, awdl_opclass_20mhz(channel)});
    return observed ? observed : station;
}

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
const char *homeTrialBlocker(uint16_t station, uint16_t radio, uint64_t tsf)
{
    if (!gFacade.enabled)
        return "waiting for Apple AWDL enable";
    if (gRadioLost || gRadioError || gRadioState.error)
        return "radio recovery or error";
    if (gAddressChanging)
        return "AWDL address changing";
    if (!radioChannelsReady())
        return "no legal social channels";
    if (!tsf)
        return "radio clock unavailable";
    if (!gRxStarted)
        return "receive registration pending";
    if (!gRadioState.ready || !gRadioState.applied_generation)
        return "mandatory radio resources pending";
    if (!awdl_core_running(gCore))
        return "AWDL core not running";
    if (!gActivationCommitted)
        return "activation not committed";
    if (!station)
        return "station not associated";
    if (station != radio)
        return "station and firmware channels differ";
    return nullptr;
}

void advanceHomeTrial()
{
    for (;;) {
        if (gHomeTrial.phase == HomeTrial::Phase::Off)
            return;
        const uint64_t now = hostNs();
        if (gHomeTrial.phase == HomeTrial::Phase::Active && now < gHomeTrial.deadlineNs)
            return;
        const uint16_t station = swlan_awdl_infra_freq(), radio = radioInfraFreq();
        const uint64_t tsf = nowTsf();
        const char *reason = homeTrialBlocker(station, radio, tsf);
        const bool applying = gHomeTrial.phase == HomeTrial::Phase::Applying;
        const bool applied = applying && gFacade.enabled && gActivationCommitted &&
                             switch_wlan_a3_trial_applied(gHomeTrialChannel, &reason);
        const auto action = gHomeTrial.advance(now, gFacade.enabled, gActivationCommitted, !reason, applied);
        if (reason && gHomeTrialBlocker != reason && gHomeTrialBlockLogs < 10) {
            ++gHomeTrialBlockLogs;
            IOLog("SwitchWLAN: AWDL: home trial blocked (%s) station/radio MHz %u/%u legal %#x "
                  "enabled %u committed %u ready %u error %d/%d session %u epoch %u config %u\n",
                  reason, station, radio, gRadioState.social_channels, gFacade.enabled, gActivationCommitted,
                  gRadioState.ready, gRadioError, gRadioState.error, gRadioState.generation,
                  gRadioClock.epoch, gRadioState.applied_generation);
        }
        gHomeTrialBlocker = reason;
        switch (action) {
        case HomeTrial::Action::Request:
            IOLog("SwitchWLAN: AWDL: home trial requested by committed AWDL activation\n");
            armHomeTrialTimer();
            break;
        case HomeTrial::Action::Install:
            gHomeTrialChannel = switch_awdl_core_home_trial(gCore, 1, tsf);
            if (!gHomeTrialChannel) {
                IOLog("SwitchWLAN: AWDL: home trial blocked (core rejected home) station/radio MHz %u/%u legal %#x\n",
                      station, radio, gRadioState.social_channels);
                endHomeTrial("core rejected home channel");
                return;
            }
            break;
        case HomeTrial::Action::Start:
            armHomeTrialTimer();
            switch_wlan_a3_trial_boundary(true, gHomeTrialChannel);
            IOLog("SwitchWLAN: AWDL: home trial started channel %u duration 60s session %u epoch %u config %u\n",
                  gHomeTrialChannel, gRadioState.generation, gRadioClock.epoch, gRadioState.applied_generation);
            return;
        case HomeTrial::Action::Timeout:
            IOLog("SwitchWLAN: AWDL: home trial setup timed out (%s) station/radio MHz %u/%u config %u\n",
                  reason ? reason : "deadline elapsed before commit was observed", station, radio,
                  gRadioState.applied_generation);
            endHomeTrial("setup timeout");
            return;
        case HomeTrial::Action::Finish:
            endHomeTrial("60 seconds");
            return;
        case HomeTrial::Action::None:
            return;
        }
    }
}
#endif

void refreshRadio()
{
    if (!gCore || !gCaps || (!gCaps->requires_radio_ready && !gCaps->has_tsf_clock))
        return;
    swlan_awdl_state state = {};
    const bool got = swlan_awdl_get_state(&state) >= 0 && state.version == SWLAN_AWDL_STATE_VERSION;
    if (got && !gRadioLost) {
        if (gRadioState.generation && state.generation != gRadioState.generation) {
            suspendRadio();
            switch_awdl_radio_clock_clear(&gRadioClock);
            gRxStarted = false;
            gRadioError = 0;
            __atomic_store_n(&gRadioOpen, 0, __ATOMIC_RELEASE);
        }
        if (state.error && !radioPending(state.error))
            gRadioError = state.error;
        else if (radioPending(gRadioError))
            gRadioError = 0;
        if (radioPending(state.error) && gFacade.enabled)
            gRestorePending = true;
        if (gCaps->has_tsf_clock) {
            const uint32_t oldEpoch = gRadioClock.epoch;
            if (oldEpoch && state.clock.epoch != oldEpoch) {
                suspendRadio();
                switch_awdl_radio_clock_clear(&gRadioClock);
                gRxStarted = false;
            }
            switch_awdl_radio_clock_update(&gRadioClock, state.clock.tsf_us, state.clock.host_ns,
                                           state.clock.uncertainty_ns, state.clock.epoch, hostNs());
        }
        const uint16_t oldInfra = radioInfraFreq();
        const uint16_t oldSocial = gRadioState.social_channels_valid ? gRadioState.social_channels : 0;
        gRadioState = state;
        if (gCaps->requires_radio_ready) {
            const uint16_t social = state.social_channels_valid ? state.social_channels : 0;
            const uint16_t infra = radioInfraFreq();
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
            if ((gHomeTrial.phase == HomeTrial::Phase::Applying || gHomeTrial.phase == HomeTrial::Phase::Active) &&
                (awdl_chan_of_freq(infra).number != gHomeTrialChannel || social != oldSocial))
                endHomeTrial("home or regulatory change");
#endif
            if (social != oldSocial) {
                if (!social) {
                    suspendRadio();
                    if (state.social_channels_valid && gRxStarted) {
                        swlan_awdl_chanseq_clear();
                        swlan_awdl_rx_stop();
                        gRxStarted = false;
                    }
                }
                switch_awdl_core_set_channels(gCore, social, nowTsf());
            }
            if (infra != oldInfra)
                awdl_core_set_infra(gCore, infra, nowTsf());
        }
    }
    const bool clockValid = !gCaps->has_tsf_clock || switch_awdl_radio_clock_valid(&gRadioClock, hostNs());
    const bool resources = !gAddressChanging && !gRadioLost && !gRadioError && got && !state.error && state.generation &&
                           radioChannelsReady() &&
                           (!gCaps->requires_radio_ready || (gRxStarted && state.ready && state.applied_generation));
    if (!resources || !awdl_core_running(gCore))
        gActivationCommitted = false;
    else if (clockValid) {
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
        if (!gActivationCommitted)
            gEchoRxCount = 0;
#endif
        if (!gActivationCommitted)
            switch_wlan_probe_activated();
        gActivationCommitted = true;
    }
    switch_awdl_core_radio_ready(gCore, resources && clockValid);
    if (gRadioError && awdl_core_running(gCore)) {
        suspendRadio();
    } else if (!clockValid && !gRadioLost && gFacade.enabled) {
        if (awdl_core_running(gCore))
            requestClock();
    }
}

void rearm()
{
    refreshRadio();
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
    advanceHomeTrial();
#endif
    if (gCore && awdl_core_running(gCore)) {
        const bool radioClock = gCaps && gCaps->has_tsf_clock;
        if (radioClock && gFacade.enabled && switch_awdl_radio_clock_try_renew(&gRadioClock, hostNs()))
            requestClock();
        const uint64_t now = nowTsf();
        armTimer(!now && radioClock ? UINT64_MAX : awdl_core_timer(gCore, now));
    } else if (gTimer) {
        thread_call_cancel(gTimer);
    }
    switch_a80211_awdl_refresh(&gFacade);
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
    if (gAsyncArmed && gAsyncClient == gEventClient)
        wakeEventClient();
}

// ---- what the core asks of us (gLock held) --------------------------------------------------------------------------
// The radio's request ids of the core's unicast data frames, until their outcome. A frame whose entry is replaced
// hears nothing more: the core lets it go and counts it as dropped.
struct DataTx {
    uint32_t request, order;
    uint16_t token;
};
DataTx gDataTx[16];
uint32_t gDataTxOrder;

int hostTx(void *, const uint8_t *frame, size_t length, uint16_t freq, uint64_t stayUntil, int noAck)
{
    if (gRadioError)
        return -1;
    if (gCaps && gCaps->requires_radio_ready && (!switch_awdl_core_ready(gCore) || !gRadioState.tx_credits))
        return AWDL_TX_BUSY;
    struct swlan_tx_req r = {};
    r.frame = frame;
    r.len = length;
    r.freq_mhz = freq;
    r.rate_mcs = 0xff;
    r.flags = static_cast<uint8_t>((noAck ? SWLAN_TX_NO_ACK : 0u) | SWLAN_TX_ON_CHANNEL_ONLY);
    const uint64_t now = nowTsf();
    if (gCaps && gCaps->has_tsf_clock && !now)
        return AWDL_TX_BUSY;
    const uint64_t dwell = stayUntil > now ? stayUntil - now : 0;
    r.stay_us = static_cast<uint32_t>(dwell > UINT32_MAX ? UINT32_MAX : dwell);
    r.deadline_tsf = stayUntil;
    r.generation = gRadioState.generation;
    r.clock_epoch = gRadioClock.epoch;
    const int result = swlan_awdl_tx(&r);
    uint16_t token = 0;
    if (result > 0 && switch_awdl_data_token(frame, length, &token)) {
        DataTx *slot = gDataTx;
        for (auto &sent : gDataTx) {
            if (!sent.request) {
                slot = &sent;
                break;
            }
            if (static_cast<int32_t>(sent.order - slot->order) < 0)
                slot = &sent;
        }
        *slot = {static_cast<uint32_t>(result), ++gDataTxOrder, token};
    }
    return result == -EAGAIN || result == -ENOBUFS ? AWDL_TX_BUSY
           : result == -ETIMEDOUT                  ? AWDL_TX_LATE
           : result < 0                            ? -1
                                                   : 0;
}

void dataTxDone(const swlan_awdl_event &e)
{
    for (auto &sent : gDataTx) {
        if (!sent.request || sent.request != e.id)
            continue;
        const auto outcome = e.status == 0           ? SWITCH_AWDL_TX_ACKED
                             : e.status == EIO       ? SWITCH_AWDL_TX_UNACKED
                             : e.status == ETIMEDOUT ? SWITCH_AWDL_TX_EXPIRED
                             : e.status == ECANCELED ? SWITCH_AWDL_TX_CANCELLED
                                                     : SWITCH_AWDL_TX_FAILED;
        switch_awdl_core_tx_done(gCore, sent.token, outcome, e.tsf, nowTsf());
        sent = {};
        return;
    }
}

void hostSchedule(void *, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchorTsf)
{
    static struct swlan_chan_slot slots[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        slots[i] = {awdl_chan_freq(seq[i]), AWDL_EAW_TU * AWDL_TU_US};
    const struct swlan_chan_seq s = {anchorTsf, AWDL_EAW_TU * AWDL_TU_US * AWDL_SEQ_SLOTS, AWDL_SEQ_SLOTS,
                                     radioInfraFreq(), slots};
    const int e = swlan_awdl_chanseq_set(&s);
    const bool rejectedUpdate = e == -EINVAL && gCaps && gCaps->requires_radio_ready && gActivationCommitted;
    if (!rejectedUpdate && ((gCaps && gCaps->requires_radio_ready) || e < 0))
        switch_awdl_core_radio_ready(gCore, 0);
    if (e < 0 && !rejectedUpdate) {
        if (radioPending(e)) {
            gRestorePending = true;
            requestClock();
        } else {
            gRadioError = e;
        }
    }
    static unsigned said;
    if (e < 0 && said++ < 2)
        IOLog("SwitchWLAN: AWDL: the radio refused the channel sequence (%d)\n", e);
}

void retryPeerPresence()
{
    if (gRetryPresenceAfter && hostNs() >= gRetryPresenceAfter) {
        gRetryPresenceAfter = 0;
        thread_call_enter(gDeferred);
    }
}

void hostPeer(void *, const uint8_t address[6], int present, int8_t)
{
    if (present) {
        for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
            const auto *peer = awdl_core_peer(gCore, i);
            if (peer && !memcmp(peer->addr, address, sizeof peer->addr)) {
                ++gPeers[i].incarnation;
                break;
            }
        }
    }
    gPeerWork = true;
    thread_call_enter(gDeferred);
}

bool desiredPeer(unsigned index, SwitchWLANPeerIdentity &identity)
{
    const auto *peer = awdl_core_peer(gCore, index);
    if (!peer || !peer->reachable || !awdl_core_running(gCore) || gAddressChanging)
        return false;
    memcpy(identity.address, peer->addr, sizeof identity.address);
    identity.generation = switch_awdl_core_generation(gCore);
    identity.incarnation = gPeers[index].incarnation;
    identity.rssi = peer->rssi;
    return true;
}

void appendInput(mbuf_t &head, mbuf_t &tail, mbuf_t first, mbuf_t last)
{
    if (tail)
        mbuf_setnextpkt(tail, first);
    else
        head = first;
    tail = last;
}

void clearPeerInput(PeerSlot &slot)
{
    if (slot.head)
        mbuf_freem_list(slot.head);
    slot.head = slot.tail = nullptr;
    slot.count = 0;
}

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
void logRadioEcho(const uint8_t *eth, size_t length)
{
    static uint16_t ids[8];
    if (!gEchoRxMeta || gEchoRxCount == sizeof ids / sizeof ids[0] || (eth[0] & 1))
        return;
    const auto echo = switch_wlan_echo6_read(eth, length, length);
    if (echo.type != 128 || echo.destination[0] == 0xff)
        return;
    for (unsigned i = 0; i < gEchoRxCount; ++i)
        if (ids[i] == echo.id)
            return;
    ids[gEchoRxCount++] = echo.id;
    char source[SWITCH_WLAN_IPV6_TEXT_LEN], destination[SWITCH_WLAN_IPV6_TEXT_LEN];
    IOLog("SwitchWLAN: AWDL radio RX echo id %u seq %u src %s dst %s freq %u MHz TSF %llu us session %u epoch %u applied %u\n",
          echo.id, echo.sequence, switch_wlan_awdl_ipv6_text(echo.source, source),
          switch_wlan_awdl_ipv6_text(echo.destination, destination), gEchoRxMeta->freq_mhz,
          static_cast<unsigned long long>(gEchoRxMeta->tsf), gEchoRxMeta->generation,
          gEchoRxMeta->clock_epoch, gEchoRxMeta->applied_generation);
}
#endif

#if DEVELOPMENT || DEBUG
switch_wlan_awdl_input_facts gQueued;  // hostDeliver's last packet, for the TCP trace (both under gLock)

void hostQueued(void *, char *line, size_t size)
{
    switch_wlan_awdl_input_describe(gQueued, line, size);
}
#endif

void hostDeliver(void *, const uint8_t *eth, size_t length)
{
#if DEVELOPMENT || DEBUG
    gQueued = {};
#endif
    if (!gIf || length < ETHER_HDR_LEN)
        return;
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
    logRadioEcho(eth, length);
#endif
    mbuf_t m = switch_wlan_awdl_input_packet(eth, length);
    if (!m) {
        ifnet_stat_increment_in(gIf, 0, 0, 1);
        return;
    }
#if DEVELOPMENT || DEBUG
    if (switch_wlan_awdl_input_traced(eth, length))
        gQueued = switch_wlan_awdl_input_inspect(m);
#endif
    mbuf_pkthdr_setrcvif(m, gIf);
    const auto *source = gPeerWork ? awdl_core_find(gCore, eth + ETHER_ADDR_LEN) : nullptr;
    if (source && source->reachable) {
        for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
            if (awdl_core_peer(gCore, i) != source)
                continue;
            SwitchWLANPeerIdentity desired = {};
            auto &slot = gPeers[i];
            if (!desiredPeer(i, desired) || slot.presence.ready(desired))
                break;
            if (slot.head && !slot.bufferedFor.same(desired))
                clearPeerInput(slot);
            if (slot.count < 8) {
                slot.bufferedFor = desired;
                appendInput(slot.head, slot.tail, m, m);
                ++slot.count;
            } else {
#if DEVELOPMENT || DEBUG
                gQueued = {};
#endif
                mbuf_freem(m);
                ifnet_stat_increment_in(gIf, 0, 0, 1);
            }
            retryPeerPresence();
            return;
        }
    }
    appendInput(gInputHead, gInputTail, m, m);
}

void hostLog(void *, const char *line)
{
    IOLog("SwitchWLAN: %s\n", line);
}

#if DEVELOPMENT || DEBUG
void hostProbe(void *, const char *line)
{
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: %s", line);
}
#endif

// Native backend: the core's firmware requests, in order, from a thread (they wait for the firmware's answer), on
// the firmware interface the core names; each answer goes back to the core (awdl_core_fw_result), which ends the
// session on a refused interface creation and resends a refused awdl_payload in its other form.
struct Iovar {
    uint32_t generation;
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
    if (gRadioLost)
        return -1;
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
    v.generation = switch_awdl_core_generation(gCore);
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
        const bool teardown =
            !v.get &&
            (!strcmp(v.name, "interface_remove") ||
             (!strcmp(v.name, "awdl") && v.length == 4 && !(v.value[0] | v.value[1] | v.value[2] | v.value[3])) ||
             ((!strcmp(v.name, "awdl_if") || !strcmp(v.name, "awdl_if2")) && v.length >= 8 &&
              !(v.value[4] | v.value[5] | v.value[6] | v.value[7])));
        if (!gCore || (v.generation != switch_awdl_core_generation(gCore) && !teardown)) {
            IOLockUnlock(gLock);
            continue;
        }
        IOLockUnlock(gLock);
        size_t got = 0;
        const size_t replySize = !strcmp(v.name, "interface_create") ? BCM_IFC_V3_LEN : sizeof(reply);
        const int e = swlan_awdl_var(v.get ? 0 : 1, static_cast<uint8_t>(v.ifidx), v.name, v.value, v.length,
                                     v.get ? reply : nullptr, v.get ? replySize : 0, &got);
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
        if (gCore && v.generation == switch_awdl_core_generation(gCore))
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

#if DEVELOPMENT || DEBUG
struct EchoTrial {
    ifnet_t ifp;
    mbuf_t packet;
    uint32_t generation;
    switch_wlan_echo6 echo;
    bool logged[2];
};
EchoTrial gEchoTrial;
thread_call_t gEchoAfter;
uint32_t gEchoGeneration;
unsigned gEchoLogs[2], gEchoTrials;
uint16_t gLastEchoId;
uint64_t gLastEchoStart;

switch_wlan_echo6 echoSample(mbuf_t packet)
{
    const size_t length = mbuf_pkthdr_len(packet);
    uint8_t prefix[SWITCH_WLAN_ECHO6_PREFIX];
    if (length < sizeof prefix || mbuf_copydata(packet, 0, sizeof prefix, prefix))
        return {};
    return switch_wlan_echo6_read(prefix, sizeof prefix, length);
}

void resetEchoBudget(uint32_t generation)
{
    if (gEchoGeneration != generation) {
        gEchoGeneration = generation;
        gEchoLogs[0] = gEchoLogs[1] = gEchoTrials = 0;
        gLastEchoStart = 0;
    }
}

bool stageEcho(ifnet_t ifp, mbuf_t packet, const switch_wlan_echo6 &echo)
{
    if (echo.type != 128)
        return false;
    IOLockLock(gLock);
    const uint32_t generation = switch_awdl_core_generation(gCore);
    resetEchoBudget(generation);
    const uint64_t now = hostNs();
    const bool eligible = !gEchoTrial.ifp && gEchoTrials < 8 &&
                          (!gLastEchoStart || (echo.id != gLastEchoId && now - gLastEchoStart > 3000000000ULL));
    const bool stage = eligible && gIf == ifp && awdl_core_running(gCore) && !ifnet_reference(ifp);
    if (stage) {
        gEchoTrial = {ifp, packet, generation, echo, {false, false}};
        gLastEchoId = echo.id;
        gLastEchoStart = now;
        ++gEchoTrials;
        if (gCaps && gCaps->requires_radio_ready)
            switch_wlan_a3_echo_snapshot(gRadioState.generation, gRadioState.clock.epoch, echo.id, echo.sequence);
        thread_call_enter(gDeferred);
    }
    IOLockUnlock(gLock);
    return stage;
}

void logEcho(ifnet_t ifp, bool input, const switch_wlan_echo6 &echo, int status)
{
    if (!echo.type)
        return;
    IOLockLock(gLock);
    const uint32_t generation = switch_awdl_core_generation(gCore);
    resetEchoBudget(generation);
    auto &count = gEchoLogs[input ? 0 : 1];
    const bool selected = gEchoTrial.ifp == ifp && gEchoTrial.generation == generation &&
                          gEchoTrial.echo.id == echo.id && gEchoTrial.echo.sequence == echo.sequence;
    auto &logged = gEchoTrial.logged[input ? 0 : 1];
    const bool log = (selected && !logged) || count < 16;
    if (selected)
        logged = true;
    if (count < 16)
        ++count;
    IOLockUnlock(gLock);
    if (log) {
        char source[SWITCH_WLAN_IPV6_TEXT_LEN], destination[SWITCH_WLAN_IPV6_TEXT_LEN];
        IOLog("SwitchWLAN: AWDL ICMPv6 %s type %u id %u seq %u status %d ifindex %u generation %u "
              "src %s dst %s checksum %#x\n", input ? "ifnet_input" : "ifOutput", echo.type, echo.id, echo.sequence,
              status, ifnet_index(ifp), generation, switch_wlan_awdl_ipv6_text(echo.source, source),
              switch_wlan_awdl_ipv6_text(echo.destination, destination), echo.checksum);
    }
}

void snapshotEcho(const char *stage, const EchoTrial &trial)
{
    switch_wlan_echo_counters counts = {};
    switch_wlan_awdl_echo_counters(trial.ifp, trial.echo.source, &counts);
    IOLog("SwitchWLAN: AWDL echo %s ifindex %u generation %u id %u seq %u requests %llu replies %llu "
          "stats-error %d neighbor-state %d neighbor-error %d\n", stage, ifnet_index(trial.ifp), trial.generation,
          trial.echo.id, trial.echo.sequence, counts.input_requests, counts.output_replies, counts.stats_error,
          counts.neighbor_state, counts.neighbor_error);
}

void echoAfter(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    const auto trial = gEchoTrial;
    const bool valid = trial.ifp && gIf == trial.ifp && trial.generation == switch_awdl_core_generation(gCore);
    gEchoTrial = {};
    IOLockUnlock(gLock);
    if (valid)
        snapshotEcho("after", trial);
    if (trial.ifp)
        ifnet_release(trial.ifp);
}
#endif

#if DEVELOPMENT || DEBUG
void deliverPackets(ifnet_t ifp, mbuf_t input, bool measured = false)
#else
void deliverPackets(ifnet_t ifp, mbuf_t input)
#endif
{
    for (mbuf_t m = input, next = nullptr; m; m = next) {
        next = mbuf_nextpkt(m);
        mbuf_setnextpkt(m, nullptr);
        const size_t length = mbuf_pkthdr_len(m);
#if DEVELOPMENT || DEBUG
        const auto echo = echoSample(m);
        if (echo.type == 128 && stageEcho(ifp, m, echo))
            continue;
#endif
        bpf_tap_in(ifp, DLT_EN10MB, m, nullptr, 0);
        mbuf_pkthdr_setheader(m, mbuf_data(m));
        mbuf_adj(m, ETHER_HDR_LEN);
#if DEVELOPMENT || DEBUG
        if (measured) {
            uint16_t sum = 0;
            const auto error = mbuf_len(m) < 40 ? EMSGSIZE : mbuf_inet6_cksum(m, IPPROTO_ICMPV6, 40, echo.payload, &sum);
            IOLog("SwitchWLAN: AWDL echo checksum id %u seq %u residue %#x error %d\n",
                  echo.id, echo.sequence, sum, error);
#if defined(SWITCH_IPSW_KIND_24B119)
            const int tagError = switch_awdl_echo_mark(m);
            if (tagError)
                IOLog("SwitchWLAN: XNU echo tag failed id %u seq %u errno %d\n", echo.id, echo.sequence, tagError);
#endif
        }
#endif
        struct ifnet_stat_increment_param inc = {};
        inc.packets_in = 1;
        inc.bytes_in = static_cast<uint32_t>(length);
        const errno_t inputStatus = ifnet_input(ifp, m, &inc);
#if DEVELOPMENT || DEBUG
        logEcho(ifp, true, echo, inputStatus);
#else
        (void)inputStatus;
#endif
    }
}

void drain()
{
    IOLockLock(gLock);
    mbuf_t input = gInputHead;
    gInputHead = gInputTail = nullptr;
    ifnet_t ifp = gIf;
    const bool retained = input && ifp && !ifnet_reference(ifp);
    IOLockUnlock(gLock);
    if (retained) {
        deliverPackets(ifp, input);
        ifnet_release(ifp);
    } else if (input) {
        mbuf_freem_list(input);
    }
}

bool otherRoute(unsigned index, const SwitchWLANPeerIdentity &peer)
{
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        const auto &p = gPeers[i].presence;
        if (i != index && p.installedRoute && !memcmp(p.installed.address, peer.address, sizeof peer.address))
            return true;
    }
    return false;
}

#if DEVELOPMENT || DEBUG
void measureEcho()
{
    IOLockLock(gLock);
    const auto trial = gEchoTrial;
    if (!trial.packet) {
        IOLockUnlock(gLock);
        return;
    }
    gEchoTrial.packet = nullptr;
    const bool valid = gIf == trial.ifp && trial.generation == switch_awdl_core_generation(gCore);
    if (!valid)
        gEchoTrial = {};
    IOLockUnlock(gLock);
    if (!valid) {
        mbuf_freem(trial.packet);
        ifnet_release(trial.ifp);
        return;
    }
    snapshotEcho("before", trial);
    IOLockLock(gLock);
    const bool current = gIf == trial.ifp && trial.generation == switch_awdl_core_generation(gCore);
    IOLockUnlock(gLock);
    if (current)
        deliverPackets(trial.ifp, trial.packet, true);
    else
        mbuf_freem(trial.packet);
    uint64_t deadline = 0;
    clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(gEchoAfter, deadline);
}
#endif

void deferredWork(thread_call_param_t, thread_call_param_t)
{
#if DEVELOPMENT || DEBUG
    measureEcho();
#endif
    IOLockLock(gLock);
    ifnet_t ifp = gIf;
    if (!ifp || ifnet_reference(ifp)) {
        IOLockUnlock(gLock);
        return;
    }
    bool failed = false;
    const PeerChange passes[] = {PeerChange::Remove, PeerChange::Add};
    for (PeerChange pass : passes) {
        for (unsigned i = 0; i < AWDL_MAX_PEERS && gIf == ifp; ++i) {
            auto &slot = gPeers[i];
            SwitchWLANPeerIdentity desired = {};
            const auto *want = desiredPeer(i, desired) ? &desired : nullptr;
            if (slot.head && (!want || !slot.bufferedFor.same(desired)))
                clearPeerInput(slot);
            const auto change = slot.presence.change(want);
            if (change != pass || (change == PeerChange::Add && otherRoute(i, desired)))
                continue;
            const auto request = change == PeerChange::Add ? desired : slot.presence.installed;
            IOLockUnlock(gLock);
            uint32_t flags = 0;
            const int error =
                switch_wlan_awdl_neighbor_update(ifp, request.address, change == PeerChange::Add, request.rssi, &flags);
            IOLockLock(gLock);
            if (gIf != ifp)
                break;
            if (error && change == PeerChange::Add) {
                if (!failed && !gRetryPresenceAfter)
                    IOLog("SwitchWLAN: AWDL neighbor add failed (%d)\n", error);
                failed = true;
                continue;
            }
            if (error) {
                const uint8_t *a = request.address;
                IOLog("SwitchWLAN: AWDL neighbor remove failed (%d): %02x:%02x:%02x:%02x:%02x:%02x route flags %#x\n",
                      error, a[0], a[1], a[2], a[3], a[4], a[5], flags);
            }
            want = desiredPeer(i, desired) ? &desired : nullptr;
            const int publish = slot.presence.complete(change, request, want);
            if (publish)
                a80211_awdl_peer(&gFacade, request.address, publish > 0, request.rssi);
            if (want && slot.presence.ready(desired) && slot.head && slot.bufferedFor.same(desired)) {
                appendInput(gInputHead, gInputTail, slot.head, slot.tail);
                slot.head = slot.tail = nullptr;
                slot.count = 0;
            }
        }
    }
    if (ifp == gIf) {
        gPeerWork = false;
        for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
            SwitchWLANPeerIdentity desired = {};
            if (gPeers[i].presence.change(desiredPeer(i, desired) ? &desired : nullptr) != PeerChange::None) {
                gPeerWork = true;
                break;
            }
        }
        if (failed)
            gRetryPresenceAfter = hostNs() + 1000000000ULL;
        else if (gPeerWork)
            thread_call_enter(gDeferred);
        else
            gRetryPresenceAfter = 0;
    }
    IOLockUnlock(gLock);
    ifnet_release(ifp);
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
        "(%u), peers +%u -%u, syncs %u, iovars %u refused %u skipped %u, data rx %u tx %u drop %u bad %u refused %u "
        "not-AWDL %u, sent PSF %u MIF %u, sync error %d us, resyncs %u%s",
        awdl_core_fw_ifidx(gCore), st->fw_events, st->fw_unknown, st->aws, st->fw_frames, st->af_rx, st->af_bad,
        st->fw_role, st->roles, st->peer_adds, st->peer_dels, st->syncs_pushed, st->iovars, st->iovar_refused,
        st->iovar_skipped, st->data_rx, st->data_tx, st->data_drop, st->data_bad, st->tx_refused, st->data_not_awdl,
        st->psf_tx, st->mif_tx, st->last_sync_error_us, st->resyncs, soft);
#endif
}

void timerFired(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    refreshRadio();
    if (gCore && switch_awdl_core_ready(gCore))
        awdl_core_set_infra(gCore, radioInfraFreq(), nowTsf());
    rearm();
    statsLine();
    IOLockUnlock(gLock);
    drain();
}

// ---- the radio's callbacks -------------------------------------------------------------------------------------------
#if DEVELOPMENT || DEBUG
enum class ActionDisposition { Gated, Stopped, Ignored, Malformed, Accepted };

void logA3RxAction(const uint8_t *frame, size_t length, const swlan_rx_meta &meta,
                   const swlan::SwitchAwdlRxAction &action, ActionDisposition disposition)
{
    const char *const names[] = {"gated", "stopped", "ignored", "malformed", "accepted"};
    int parseResult = 1;  // awdl_af_parse returns zero or a negative error. One marks a skipped parse.
    if (disposition == ActionDisposition::Accepted)
        parseResult = 0;
    else if (disposition == ActionDisposition::Ignored || disposition == ActionDisposition::Malformed) {
        awdl_af decoded;
        parseResult = awdl_af_parse(frame, length, &decoded);
    }

    char oui[7] = "-";
    if (action.oui >= 0)
        snprintf(oui, sizeof oui, "%06x", static_cast<unsigned>(action.oui));
    IOLog("SwitchWLAN: A3 action RX fc %#x order %u retry %u seq %u frag %u "
          "src %02x:%02x:%02x:%02x:%02x:%02x dst %02x:%02x:%02x:%02x:%02x:%02x "
          "bssid %02x:%02x:%02x:%02x:%02x:%02x no-fcs-bytes %zu channel %u freq %u MHz TSF %llu "
          "session %u epoch %u applied %u body-offset %u category %d BA-action %d OUI %s Apple-type %d "
          "version %d subtype %d core %s parse %d\n",
          action.fc, action.fc >> 15, (action.fc >> 11) & 1u, action.sequenceControl >> 4, action.sequenceControl & 15u,
          frame[10], frame[11], frame[12], frame[13], frame[14], frame[15], frame[4], frame[5], frame[6], frame[7],
          frame[8], frame[9], frame[16], frame[17], frame[18], frame[19], frame[20], frame[21], length,
          awdl_chan_of_freq(meta.freq_mhz).number, meta.freq_mhz, static_cast<unsigned long long>(meta.tsf),
          meta.generation, meta.clock_epoch, meta.applied_generation, action.headerBytes, action.category,
          action.blockAckAction, oui, action.appleType, action.version, action.subtype,
          names[static_cast<unsigned>(disposition)], parseResult);
}
#endif

void radioRx(void *, const uint8_t *frame, size_t length, const struct swlan_rx_meta *meta)
{
    if (meta && (meta->flags & SWLAN_RX_FCS_PRESENT)) {
        if (length < 4)
            return;
        length -= 4;
    }
    IOLockLock(gLock);
    refreshRadio();
    const bool stamped = gCaps && gCaps->has_tsf_clock;
    const bool admitted = stamped ? gActivationCommitted && gRxStarted && gRadioState.ready && meta && meta->tsf &&
                                      meta->generation == gRadioState.generation &&
                                      meta->clock_epoch == gRadioState.clock.epoch &&
                                      meta->applied_generation == gRadioState.applied_generation
                                  : gCore && switch_awdl_core_ready(gCore);
#if DEVELOPMENT || DEBUG
    swlan::SwitchAwdlRxAction sample{};
    const bool candidate = stamped && gCaps->requires_radio_ready && meta && meta->tsf && meta->generation &&
                           meta->clock_epoch && meta->applied_generation &&
                           swlan::switch_awdl_rx_action(frame, length, sample);
    auto disposition = ActionDisposition::Gated;
#endif
    if (gCore && admitted) {
        const bool action = length >= 24 && (frame[0] & 0xfc) == 0xd0;
        const uint32_t parsed = action ? awdl_core_stats(gCore)->af_rx : 0;
#if DEVELOPMENT || DEBUG
        const uint32_t bad = candidate ? awdl_core_stats(gCore)->af_bad : 0;
        const bool canParse = candidate && awdl_core_running(gCore);
#endif
        const struct awdl_rx_meta m = {meta && meta->tsf ? meta->tsf : nowTsf(),
                                       meta ? meta->freq_mhz : static_cast<uint16_t>(0),
                                       meta ? meta->rssi_dbm : static_cast<int8_t>(-90)};
#if defined(SWITCH_IPSW_KIND_24B119)
#if DEVELOPMENT || DEBUG
        gEchoRxMeta = stamped ? meta : nullptr;
#endif
        const bool service = awdl_core_rx(gCore, frame, length, &m);
#if DEVELOPMENT || DEBUG
        gEchoRxMeta = nullptr;
#endif
        if (service && gMonitoring) {
            static uint8_t event[A80211_RING_MAX_PAYLOAD];
            const auto size = switch_a80211_awdl_action_rx(event, sizeof event, frame, length, &m);
            if (size)
                postEvent(A80211_M_AWDL_ACTION_FRAME_RX, event, static_cast<uint32_t>(size));
        }
#else
        awdl_core_rx(gCore, frame, length, &m);
#endif
        if (action && awdl_core_stats(gCore)->af_rx != parsed) {
            retryPeerPresence();
#if DEVELOPMENT || DEBUG
            if (length > 30) {
                gRawPsf += frame[30] == AWDL_SUBTYPE_PSF;
                gRawMif += frame[30] == AWDL_SUBTYPE_MIF;
            }
#endif
        }
#if DEVELOPMENT || DEBUG
        if (candidate) {
            const awdl_stats *stats = awdl_core_stats(gCore);
            if (!canParse)
                disposition = ActionDisposition::Stopped;
            else if (stats->af_rx != parsed)
                disposition = ActionDisposition::Accepted;
            else if (stats->af_bad != bad)
                disposition = ActionDisposition::Malformed;
            else
                disposition = ActionDisposition::Ignored;
        }
        gRawData += length >= 24 && (frame[0] & 0x0c) == 8;
#endif
        rearm();
    }
#if DEVELOPMENT || DEBUG
    else if (stamped) {
        ++gRawRxGated;
    }
    const bool discovery = sample.kind == swlan::SwitchAwdlRxActionKind::Psf ||
                           sample.kind == swlan::SwitchAwdlRxActionKind::Mif;
    const bool unexpected = disposition == ActionDisposition::Malformed || disposition == ActionDisposition::Gated ||
                            disposition == ActionDisposition::Stopped ||
                            (discovery && disposition != ActionDisposition::Accepted);
    const bool selected = candidate && gA3RxActionBudget.take(sample, unexpected);
#endif
    IOLockUnlock(gLock);
#if DEVELOPMENT || DEBUG
    if (selected)
        logA3RxAction(frame, length, *meta, sample, disposition);
#endif
    drain();
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
    drain();
}

// The firmware's AWDL interface: 802.3 frames with the AWDL data header, which the core strips (hostDeliver).
void ethRx(void *, const uint8_t *frame, size_t length)
{
    IOLockLock(gLock);
    if (gCore && awdl_core_running(gCore))
        awdl_core_rx_eth(gCore, frame, length, nowTsf());
    IOLockUnlock(gLock);
    drain();
}

void radioEvent(void *, const struct swlan_awdl_event *e)
{
    IOLockLock(gLock);
    if (e->generation && (e->type == SWLAN_AWDL_RADIO_DOWN || e->type == SWLAN_AWDL_RADIO_UP ||
                          e->type == SWLAN_AWDL_RECOVERY_REQUIRED)) {
        swlan_awdl_state state = {};
        if (!swlan_awdl_get_state(&state) && state.generation != e->generation) {
            IOLockUnlock(gLock);
            return;
        }
    }
    if (e->type == SWLAN_AWDL_CHIP_LOST || e->type == SWLAN_AWDL_RADIO_DOWN ||
        e->type == SWLAN_AWDL_RECOVERY_REQUIRED) {
        IOLog("SwitchWLAN: AWDL: the radio was lost; requested AWDL state retained for recovery\n");
        gRadioLost = true;
        gIovarHead = gIovarCount = 0;
        suspendRadio();
        switch_awdl_radio_clock_clear(&gRadioClock);
        gClockRequest = {};
        gRadioError = 0;
        gRxStarted = false;
        __atomic_store_n(&gRadioOpen, 0, __ATOMIC_RELEASE);
    } else {
        const bool powered = e->type == SWLAN_AWDL_RADIO_UP;
        if (powered) {
            gRadioLost = false;
            gRadioError = 0;
        }
        const bool stateEvent = powered || e->type == SWLAN_AWDL_CONFIG_CHANGED || e->type == SWLAN_AWDL_CLOCK_CHANGED;
        if (gRadioLost && stateEvent) {
            if (gCaps && gCaps->requires_radio_ready && (!e->generation || e->generation == gRadioState.generation)) {
                IOLockUnlock(gLock);
                return;
            }
            gRadioLost = false;
        } else if (e->generation && gRadioState.generation && e->generation != gRadioState.generation && !stateEvent) {
            IOLockUnlock(gLock);
            return;
        }
        if (e->type == SWLAN_AWDL_CLOCK_CHANGED)
            switch_awdl_radio_clock_request_complete(&gClockRequest, e->status);
        else if (stateEvent || e->type == SWLAN_AWDL_TXDONE || e->type == SWLAN_AWDL_TX_CREDIT)
            gClockRequest.blocked = 0;
        const uint32_t credits = gRadioState.tx_credits, applied = gRadioState.applied_generation;
        refreshRadio();
        const bool reopened = stateEvent && gRadioState.tx_credits && (!credits || applied != gRadioState.applied_generation);
        if (e->type == SWLAN_AWDL_TXDONE && gCore)
            dataTxDone(*e);
        if (reopened || e->type == SWLAN_AWDL_TXDONE || e->type == SWLAN_AWDL_TX_CREDIT)
            if (gCore && (!gCaps || !gCaps->has_tsf_clock || !e->clock_epoch || e->clock_epoch == gRadioClock.epoch))
                switch_awdl_core_tx_ready(gCore, nowTsf());
        if (stateEvent && gRestorePending && gFacade.enabled && !gRadioError && gRadioWork)
            thread_call_enter(gRadioWork);
    }
    rearm();
    IOLockUnlock(gLock);
    drain();
}

// ---- the facade's operations (gLock held) ---------------------------------------------------------------------------
int opStart(void *)
{
    if (gAddressChanging) {
        gRestorePending = true;
        return EINPROGRESS;
    }
    const int open = __atomic_load_n(&gRadioOpen, __ATOMIC_ACQUIRE);
    if (open < 0)
        return ENXIO;
    const int registered = swlan_awdl_events(radioEvent, nullptr);
    if (registered < 0)
        return -registered;
    if (gRadioLost || !open) {
        gRestorePending = true;
        if (!gRadioLost)
            thread_call_enter(gRadioWork);
        return EINPROGRESS;
    }
    refreshRadio();
    if (gRadioError)
        return gRadioError < 0 ? -gRadioError : gRadioError;
    if (!radioChannelsReady() ||
        (gCaps && gCaps->has_tsf_clock && !switch_awdl_radio_clock_valid(&gRadioClock, hostNs()))) {
        gRestorePending = true;
        requestClock();
        return EINPROGRESS;
    }
    if (gCaps && gCaps->requires_radio_ready)
        switch_awdl_core_radio_ready(gCore, 0);
    else
        switch_awdl_core_radio_ready(gCore, 1);
    awdl_core_set_infra(gCore, radioInfraFreq(), nowTsf());
    if (!gRxStarted && !gSoft) {
        int e = swlan_awdl_fw_events(fwEvent, nullptr);
        if (e >= 0)
            e = swlan_awdl_eth_rx(ethRx, nullptr);
        if (e < 0) {
            IOLog("SwitchWLAN: AWDL: the radio would not deliver the firmware's AWDL events and frames (%d): off\n", e);
            return ENXIO;
        }
        swlan_awdl_events(radioEvent, nullptr);
        gRxStarted = true;
    }
    if (!gRxStarted) {
        int e = swlan_awdl_events(radioEvent, nullptr);
        if (e >= 0)
            e = swlan_awdl_rx_filter(gMac, awdl_bssid, 0);
        if (e >= 0)
            e = swlan_awdl_rx_start(radioRx, nullptr);
        if (e < 0) {
            IOLog("SwitchWLAN: AWDL: the radio refused receive configuration (%d): AWDL off\n", e);
            const bool pending = radioPending(e);
            if (pending)
                gRestorePending = true;
            else
                gRadioError = e;
            switch_awdl_core_radio_ready(gCore, 0);
            return pending ? EINPROGRESS : -e;
        }
        gRxStarted = true;
    }
    return 0;
}

int opActive(void *)
{
    return gCaps && (gCaps->requires_radio_ready || gCaps->has_tsf_clock)
               ? gActivationCommitted
               : gCore && switch_awdl_core_ready(gCore);
}

void opStop(void *)
{
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
    endHomeTrial("disabled");
#endif
    gActivationCommitted = false;
    gRestorePending = false;
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
#if DEVELOPMENT || DEBUG
    if (event == A80211_M_AWDL_SYNC_STATE_CHANGED && gActivationTransitions < 32) {
        ++gActivationTransitions;
        const bool active = switch_a80211_awdl_active(&gFacade);
        const char *reason = "resources-pending";
        if (active)
            reason = "committed";
        else if (!gFacade.enabled)
            reason = "disabled";
        else if (gRadioLost)
            reason = "radio-lost";
        else if (gAddressChanging)
            reason = "address-change";
        else if (gRadioError)
            reason = "radio-error";
        else if (!gCore || !awdl_core_running(gCore))
            reason = "core-stopped";
        IOLog("SwitchWLAN: AWDL activation: active=%d desired=%d committed=%d data_ready=%d "
              "session=%u epoch=%u applied=%u reason=%s status=%d transition=%u\n",
              active, gFacade.enabled, opActive(nullptr), gCore && switch_awdl_core_ready(gCore),
              gRadioState.generation, gRadioClock.epoch, gRadioState.applied_generation, reason, gRadioError,
              gActivationTransitions);
    }
#endif
    postEvent(event, payload, length);
}

void opLog(void *, const char *line)
{
    IOLog("SwitchWLAN: %s\n", line);
}

#if DEVELOPMENT || DEBUG
void logPeerProbes()
{
    for (unsigned index = 0; index < AWDL_MAX_PEERS; ++index) {
        const awdl_peer *peer = awdl_core_peer(gCore, index);
        switch_awdl_data_probe p = {};
        if (!peer || !peer->reachable || !switch_awdl_core_data_probe(gCore, index, &p))
            continue;
        os_log(OS_LOG_DEFAULT,
               "SwitchWLAN: probe peer %02x:%02x:%02x:%02x:%02x:%02x rx ch6 %u ch44 %u ch149 %u other %u retry %u "
               "dup %u to-awdl0 %u; tx queued %u handed %u acked %u unacked %u expired %u cancelled %u failed %u "
               "requeued %u; longest queue wait %llu us, hand-off to done %llu us",
               p.addr[0], p.addr[1], p.addr[2], p.addr[3], p.addr[4], p.addr[5], p.rx_frames[0], p.rx_frames[1],
               p.rx_frames[2], p.rx_frames[3], p.rx_retry, p.rx_dup, p.rx_delivered, p.tx_queued, p.tx_handed,
               p.tx_acked, p.tx_unacked, p.tx_expired, p.tx_cancelled, p.tx_failed, p.tx_requeued,
               static_cast<unsigned long long>(p.max_wait_us), static_cast<unsigned long long>(p.max_done_us));
    }
}
#endif

// A short suffix for SwitchWLAN's 5-s stats line, so en0 rx and AWDL state read off one line (empty when off).
#if DEVELOPMENT || DEBUG
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
        unsigned live = 0;
        for (unsigned index = 0; index < AWDL_MAX_PEERS; ++index) {
            const awdl_peer *peer = awdl_core_peer(gCore, index);
            live += peer && peer->reachable;
        }
        n = snprintf(buf, size, "; AWDL %s %u/16 away, peers %u af_rx %u bad %u PSF %u MIF %u data %u gated %u",
                     roleName, awdl_slots_away(awdl_core_sequence(gCore), radioInfraFreq()), live,
                     st->af_rx, st->af_bad, gRawPsf, gRawMif, gRawData, gRawRxGated);
        logPeerProbes();
    }
    IOLockUnlock(gLock);
    return n < 0 ? 0 : n;
}
#endif

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
    refreshRadio();
    if (ifp == gIf && !gAddressChanging && length >= ETHER_HDR_LEN && length <= sizeof frame - 64 && gCore &&
        !mbuf_copydata(m, 0, length, frame)) {
        gStash = &stash;
        r = awdl_core_output(gCore, frame, length, nowTsf());
        gStash = nullptr;
        rearm();
    }
    IOLockUnlock(gLock);
    if (!r && stash.length && swlan_awdl_eth_tx(stash.frame, stash.length) < 0)
        r = -1;
    IOLockUnlock(gSend);
#if DEVELOPMENT || DEBUG
    logEcho(ifp, false, echoSample(m), r);
#endif
    if (r) {
        ifnet_stat_increment_out(ifp, 0, 0, 1);
    } else {
        ifnet_stat_increment_out(ifp, 1, static_cast<uint32_t>(length), 0);
    }
    mbuf_freem(m);
    return 0;
}

// The first AWDL enable request opens the radio (the AWDL event bits in event_msgs; the station is not
// touched before). Thread context, without gLock (it waits for the firmware). 0 or an errno for wifip2pd.
int ensureRadio()
{
    IOLockLock(gOpenLock);
    int e = 0;
    const int open = __atomic_load_n(&gRadioOpen, __ATOMIC_ACQUIRE);
    if (open < 0) {
        e = ENXIO;
    } else if (!open) {
        const struct swlan_awdl_caps *caps = nullptr;
        const int r = swlan_awdl_open(&caps);
        if (r >= 0 && caps) {
            IOLockLock(gLock);
            gCaps = caps;
            IOLockUnlock(gLock);
            __atomic_store_n(&gRadioOpen, 1, __ATOMIC_RELEASE);
        } else {
            // the radio says why in its own line; a radio that was only down (recovering) is asked again next time
            IOLog("SwitchWLAN: AWDL: wifip2pd enabled AWDL but the radio did not open for it (%d): AWDL off%s\n", r,
                  r == -ENXIO ? " until the next enable" : "");
            const bool pending = radioPending(r);
            if (!pending)
                __atomic_store_n(&gRadioOpen, -1, __ATOMIC_RELEASE);
            if (pending) {
                IOLockLock(gLock);
                gRestorePending = true;
                IOLockUnlock(gLock);
            } else {
                e = r < 0 ? -r : ENXIO;
            }
        }
    }
    IOLockUnlock(gOpenLock);
    return e;
}

void radioWork(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    if (gAddressChanging || gRestoring || !gRestorePending || !gFacade.enabled || gRadioLost || gRadioError) {
        IOLockUnlock(gLock);
        return;
    }
    gRestoring = true;
    IOLockUnlock(gLock);
    const int opened = ensureRadio();
    IOLockLock(gLock);
    refreshRadio();
    if (!opened && !gAddressChanging && !gRadioLost && !gRadioError && gFacade.enabled &&
        __atomic_load_n(&gRadioOpen, __ATOMIC_ACQUIRE) == 1) {
        if (!radioChannelsReady() ||
            (gCaps && gCaps->has_tsf_clock && !switch_awdl_radio_clock_valid(&gRadioClock, hostNs()))) {
            requestClock();
        } else {
            gRestorePending = false;
            if (switch_a80211_awdl_resume(&gFacade, nowTsf()))
                gRestorePending = true;
        }
    }
    gRestoring = false;
    rearm();
    IOLockUnlock(gLock);
    drain();
}

int awdlRequest(ifnet_t ifp, bool set, apple80211req &rq)
{
    if (rq.req_len > kMaxRequest || (set && rq.req_len && !rq.req_data))
        return EINVAL;
    const uint32_t capacity = rq.req_len;
    uint8_t *data = capacity ? static_cast<uint8_t *>(IOMallocZeroData(capacity)) : nullptr;
    if (capacity && !data)
        return ENOMEM;
    int e = 0;
    if (set && capacity && rq.req_data)
        e = copyin(rq.req_data, data, capacity);
    if (!e && set && rq.req_type == A80211_IOC_AWDL_SYNC_ENABLED) {
        a80211_awdl_sync_enabled request = {};
        if (capacity != sizeof request) {
            e = EINVAL;
        } else {
            memcpy(&request, data, sizeof request);
            if (request.version != 1 || request.enabled > 1)
                e = EINVAL;
            else if (request.enabled)
                e = ensureRadio();
        }
    }
    uint32_t length = set ? capacity : 0;
    if (!e) {
        IOLockLock(gLock);
        if (ifp != gIf) {
            e = ENXIO;
        } else {
            if (set && (rq.req_type == A80211_IOC_AWDL_SYNC_ENABLED ||
                        rq.req_type == A80211_IOC_VIRTUAL_IF_STATE ||
                        rq.req_type == A80211_IOC_AWDL_PEER_SERVICE_REQUEST ||
                        rq.req_type == A80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION))
                gClockRequest.blocked = 0;
            if (!set)
                refreshRadio();
            e = a80211_awdl_request(&gFacade, static_cast<int>(set), rq.req_type, &rq.req_val, data, &length, capacity,
                                    nowTsf());
#if DEVELOPMENT || DEBUG
            if (!e && set) {
                if (rq.req_type == A80211_IOC_AWDL_PEER_SERVICE_REQUEST && capacity > sizeof(a80211_peer_service_request) &&
                    !reinterpret_cast<const a80211_peer_service_request *>(data)->delete_request) {
                    switch_wlan_a3_discovery_trial();
                } else if (rq.req_type == A80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION &&
                         capacity == sizeof(a80211_awdl_peer_traffic_registration) &&
                         reinterpret_cast<const a80211_awdl_peer_traffic_registration *>(data)->active)
                    switch_wlan_a3_discovery_trial();
            }
#endif
            if (!e && set && !gFacade.enabled &&
                (rq.req_type == A80211_IOC_AWDL_SYNC_ENABLED || rq.req_type == A80211_IOC_VIRTUAL_IF_STATE)) {
                gRadioError = 0;
                gRestorePending = false;
            }
            rearm();
        }
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

errno_t changeAddress(ifnet_t ifp, const uint8_t *mac)
{
    IOLockLock(gSend);
    IOLockLock(gLock);
    errno_t error = 0;
    const bool changed = memcmp(mac, gMac, ETHER_ADDR_LEN) != 0;
    bool filterQueued = false;
    mbuf_t oldInput = nullptr;
    if (ifp != gIf) {
        error = ENXIO;
    } else if (gAddressChanging) {
        error = EBUSY;
    } else if (changed) {
        if (!gSoft && gCore && awdl_core_running(gCore)) {
            refuseOnce("awdl0's address changes only while AWDL is off");
            error = EBUSY;
        } else {
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
            endHomeTrial("address change");
#endif
            filterQueued = gRxStarted && !gRadioLost;
            const int filter = filterQueued ? swlan_awdl_rx_filter(mac, awdl_bssid, 0) : 0;
            if (filter < 0) {
                error = -filter;
            } else {
                gAddressChanging = true;
                gActivationCommitted = false;
                gClockRequest.blocked = 0;
                if (gCore) {
                    switch_awdl_core_radio_ready(gCore, 0);
                    awdl_core_set_addr(gCore, airAddress(mac));
                }
                oldInput = gInputHead;
                gInputHead = gInputTail = nullptr;
                switch_a80211_awdl_refresh(&gFacade);
            }
        }
    }
    IOLockUnlock(gLock);
    IOLockUnlock(gSend);
    if (error || !changed)
        return error;

    if (oldInput)
        mbuf_freem_list(oldInput);
    // XNU posts link-address events from this call; they must be free to enter the driver again.
    error = ifnet_set_lladdr(ifp, mac, ETHER_ADDR_LEN);

    IOLockLock(gLock);
    gAddressChanging = false;
    if (ifp == gIf) {
        if (!error) {
            memcpy(gMac, mac, ETHER_ADDR_LEN);
        } else {
            if (gCore)
                awdl_core_set_addr(gCore, airAddress());
            if (filterQueued && swlan_awdl_rx_filter(gMac, awdl_bssid, 0) < 0) {
                gRxStarted = false;
                suspendRadio();
            }
        }
        if (gRestorePending && gFacade.enabled && !gRadioLost && !gRadioError)
            thread_call_enter(gRadioWork);
        rearm();
    }
    IOLockUnlock(gLock);
    drain();
    return error;
}

errno_t ifIoctl(ifnet_t ifp, unsigned long cmd, void *arg)
{
    switch (cmd) {
    case SIOCGA80211:
    case SIOCSA80211:
        return awdlRequest(ifp, cmd == SIOCSA80211, *static_cast<apple80211req *>(arg));
    case SIOCGIFMEDIA64: {
        IOLockLock(gLock);
        refreshRadio();
        const bool ready = ifp == gIf && switch_a80211_awdl_active(&gFacade);
        IOLockUnlock(gLock);
        return answerMedia(arg, IFM_AVALID | (ready ? IFM_ACTIVE : 0u));
    }
    case SIOCSIFLLADDR: {
        // wifip2pd's AWDL address: the stack transmits with it and the kernel derives fe80::/64 from it
        const struct ifreq *ifr = static_cast<const struct ifreq *>(arg);
        if (ifr->ifr_addr.sa_len != ETHER_ADDR_LEN)
            return EINVAL;
        const uint8_t *mac = reinterpret_cast<const uint8_t *>(ifr->ifr_addr.sa_data);
        static const uint8_t zero[6] = {};
        if ((mac[0] & 1u) || !memcmp(mac, zero, sizeof zero))
            return EINVAL;
        return changeAddress(ifp, mac);
    }
    case SIOCSIFMTU: {
        const int mtu = static_cast<const struct ifreq *>(arg)->ifr_mtu;
        if (mtu <= 0 || mtu > (gSoft ? 1500 : SWITCH_AWDL_MTU))
            return EINVAL;
        return ifnet_set_mtu(ifp, static_cast<uint32_t>(mtu));
    }
    case SIOCSIFFLAGS:
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
        gEventClient = nullptr;
    }
    if (gAsyncClient == this) {
        if (gAsyncArmed)
            releaseAsyncReference64(gAsync);
        gAsyncArmed = false;
        gAsyncClient = nullptr;
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
    refreshRadio();
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
    case 1:  // useIOUCWhenPossible: no, keep Apple80211RawGet/RawSet on the socket
#if defined(SWITCH_IPSW_KIND_24B119)
    case 7:  // isDKSupported
#endif
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
            if (gAsyncArmed && gAsyncClient != this) {
                releaseAsyncReference64(gAsync);
                gAsyncArmed = false;
                gAsyncClient = nullptr;
            }
        }
        gEventClient = this;
        a80211_ring_init(&gRing, gRingMemory->getBytesNoCopy(), kRingSize,
                         static_cast<volatile uint32_t *>(gStateMemory->getBytesNoCopy()));
        gMonitoring = true;
        postEvent(A80211_M_AWDL_SYNC_STATE_CHANGED, nullptr, 0);
        if (switch_a80211_awdl_active(&gFacade)) {
            for (unsigned index = 0; index < AWDL_MAX_PEERS; ++index) {
                SwitchWLANPeerIdentity current = {};
                if (desiredPeer(index, current) && gPeers[index].presence.ready(current))
                    a80211_awdl_peer(&gFacade, current.address, 1, current.rssi);
            }
        }
        IOLockUnlock(gLock);
        return kIOReturnSuccess;
    }
    case 4: {  // stop
        IOLockLock(gLock);
        if (gEventClient == this) {
            gMonitoring = false;
            gEventClient = nullptr;
        }
        if (gAsyncClient == this) {
            if (gAsyncArmed)
                releaseAsyncReference64(gAsync);
            gAsyncArmed = false;
            gAsyncClient = nullptr;
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
        gAsyncClient = this;
        if (gEventClient == this && gMonitoring && gRing.state &&
            __atomic_load_n(&gRing.state[0], __ATOMIC_ACQUIRE) != __atomic_load_n(&gRing.state[1], __ATOMIC_ACQUIRE))
            wakeEventClient();
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
#if defined(SWITCH_A80211_AWDL_CLIENT_ENTITLEMENT)
        if (!IOTaskHasEntitlement(task, SWITCH_A80211_AWDL_CLIENT_ENTITLEMENT))
            return kIOReturnNotPrivileged;
#endif
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
    s->setProperty("IO80211InterfaceRole", SWITCH_A80211_AWDL_ROLE);
    s->setProperty("IO80211VirtualInterfaceRole", SWITCH_A80211_AWDL_VIRTUAL_ROLE);
#if defined(SWITCH_A80211_AWDL_CLIENT_PROCESS)
    if (!s->setProperty("NativeSandboxGrantProcess", SWITCH_A80211_AWDL_CLIENT_PROCESS)) {
        s->release();
        IOLog("SwitchWLAN: AWDL: could not set client access policy for %s\n", gName);
        return;
    }
#endif
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
    ifnet_set_mtu(ifp, gSoft ? 1500 : SWITCH_AWDL_MTU);
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
    ifnet_set_flags(ifp, IFF_RUNNING, IFF_RUNNING);
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
    gDeferred = thread_call_allocate_with_options(deferredWork, nullptr, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
    if (!gDeferred)
        return false;
#if DEVELOPMENT || DEBUG
    gEchoAfter = thread_call_allocate_with_options(echoAfter, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!gEchoAfter)
        return false;
#endif
    gCreate = thread_call_allocate(createInterface, nullptr);
    if (!gCreate)
        return false;
    gIovarWork = thread_call_allocate(iovarWork, nullptr);
    gRadioWork = thread_call_allocate(radioWork, nullptr);
    return gIovarWork && gRadioWork;
}

}  // namespace

namespace {

void startStack(const awdl_radio_verdict &v, const struct swlan_awdl_caps *caps)
{
    gActivationCommitted = false;
#if DEVELOPMENT || DEBUG
    gActivationTransitions = 0;
#endif
    gClockRequest = {};
    gCaps = caps;
    gSoft = v.mode == AWDL_RADIO_SOFT;
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
    unsigned homeTrial = 0;
    const bool option = PE_parse_boot_argn("switch-awdl-home-trial", &homeTrial, sizeof homeTrial);
    if (!option || homeTrial != 1) {
        IOLog("SwitchWLAN: AWDL: home trial disabled by boot argument (present %u value %u)\n", option, homeTrial);
    } else if (!gSoft || !caps->requires_radio_ready || !caps->has_tsf_clock) {
        IOLog("SwitchWLAN: AWDL: home trial blocked (backend) soft %u needs-ready %u has-TSF %u\n",
              gSoft, caps->requires_radio_ready, caps->has_tsf_clock);
    } else {
        gHomeTrialTimer = thread_call_allocate_with_options(homeTrialTimeout, nullptr, THREAD_CALL_PRIORITY_HIGH,
                                                           THREAD_CALL_OPTIONS_ONCE);
        if (gHomeTrialTimer) {
            gHomeTrial.arm();
            IOLog("SwitchWLAN: AWDL: home trial armed, waiting for committed AWDL activation\n");
        } else {
            IOLog("SwitchWLAN: AWDL: home trial blocked (timer allocation failed)\n");
        }
    }
#endif
    memcpy(gMac, caps->self_mac, 6);
    awdl_config cfg;
    awdl_config_default(&cfg, gMac);
    cfg.backend = gSoft ? AWDL_BACKEND_SOFT : AWDL_BACKEND_NATIVE;
    cfg.fw_api = static_cast<uint8_t>(v.api);
    cfg.fw_absent = bcm_probe_absent(caps->probe);
    awdl_host host = {};
    host.tx = hostTx;
    host.schedule = hostSchedule;
    host.peer = hostPeer;
    host.deliver = hostDeliver;
    host.log = hostLog;
    host.iovar = hostIovar;
    host.tx_eth = hostTxEth;
#if DEVELOPMENT || DEBUG
    host.probe = hostProbe;
    host.queued = hostQueued;
#endif
    awdl_core_init(gCore, &host, &cfg);
    if (caps->requires_radio_ready)
        switch_awdl_core_set_channels(gCore, 0, 0);
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

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_IPSW_KIND_24B119)
extern "C" void switch_wlan_awdl_station_change(void)
{
    if (__atomic_load_n(&gCapable, __ATOMIC_ACQUIRE) != 1 || !gHomeTrialTimer)
        return;
    IOLockLock(gLock);
    endHomeTrial("station ownership");
    IOLockUnlock(gLock);
}
#endif

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
    if (r->mac[0] & 1u)
        return EINVAL;
    IOLockLock(gLock);
    if (!gIf && !gCreating) {
        gUnit = r->unit < 8 ? r->unit : 0;
        snprintf(gName, sizeof gName, "awdl%u", gUnit);
        static const uint8_t zero[6] = {};
        if (memcmp(r->mac, zero, 6))
            memcpy(gMac, r->mac, 6);
        awdl_core_set_addr(gCore, airAddress());
        uint8_t caps[12] = {0x4c, 0x46, 0x0a, 0, 0x01};  // en0's bitmap (SwitchWLANNet kCardCaps) plus bit 32
        const a80211_awdl_ops ops = {nullptr, opStart, opStop, opPost, opLog, opActive};
        gActivationCommitted = false;
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
    rearm();
    // NOLINTNEXTLINE(misc-const-correctness): a const ifnet_t is a const pointer (misc-misplaced-const)
    ifnet_t ifp = gIf;
    gIf = nullptr;
    for (auto &slot : gPeers) {
        clearPeerInput(slot);
        slot.presence = {};
    }
    mbuf_t oldInput = gInputHead;
    gInputHead = gInputTail = nullptr;
    gPeerWork = false;
    gRetryPresenceAfter = 0;
    IOService *s = gService;
    gService = nullptr;
    IOLockUnlock(gLock);
    if (oldInput)
        mbuf_freem_list(oldInput);
    if (s) {
        s->terminate();
        s->release();
    }
    ifnet_detach(ifp);  // released in ifDetached
    IOLog("SwitchWLAN: AWDL: %s deleted\n", gName);
    return 0;
}

#if defined(SWITCH_IPSW_KIND_24B119)
extern "C" int switch_wlan_awdl_vif_state(const void *payload, size_t length)
{
    if (!gLock)
        return ENXIO;
    IOLockLock(gLock);
    // en0 receives wifip2pd's observed-state feedback. The radio applies coexistence with its schedule.
    const int status = gIf ? switch_a80211_awdl_parent_state(&gFacade, payload, length) : ENXIO;
    IOLockUnlock(gLock);
    return status;
}
#endif
