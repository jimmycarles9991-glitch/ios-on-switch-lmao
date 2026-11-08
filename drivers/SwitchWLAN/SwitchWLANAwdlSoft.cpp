// SPDX-License-Identifier: BSD-2-Clause-Patent
// The soft AWDL backend's radio services (SwitchWLANAwdlRadio.h §2-§5) on firmware with neither AWDL, injection nor
// channel hops (7.35.252.108). Action frames go out through "actframe", the path brcmfmac's P2P and GAS use
// (brcmf_p2p_tx_action_frame): the firmware writes the 802.11 header with its own address as the source, goes to the
// frame's channel, sends, and stays for the dwell. That stay is the AWDL slot (awdl.h host.tx). AWDL action frames come
// back as WLC_E_ACTION_FRAME_RX; 802.11 frames from monitor mode where the firmware delivers any. No data frame can go
// out: this firmware has no path for a caller-built one.
//
// The stack calls in with its lock held and the radio calls the stack from its work loop: transmissions and monitor
// changes are queued here and done by one worker thread, which waits for each firmware answer.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include "SwitchWLANAwdlRadio.h"
#include "SwitchWLANAwdlSoft.h"
#include "awdl/awdl.h"
#include "awdl/awdl_bcm.h"

namespace {

constexpr uint32_t kWlcSetVar = 263;
constexpr uint32_t kEventActionFrameComplete = 60, kEventOffChannelComplete = 70;
constexpr unsigned kQueueLength = 8;
constexpr size_t kFrameMax = 640;  // PSFs and MIFs are under 400 bytes

enum JobKind : uint8_t {
    kSend,
    kMonitorOn,
    kMonitorOff,
};

struct Job {
    JobKind kind;
    uint16_t freq;
    uint16_t length;
    uint32_t stayUs;
    uint64_t queuedNs;
    uint8_t frame[kFrameMax];
};

struct Counters {
    uint32_t sent, refused, stale, full, acked, unacked, dwellsEnded, rxEvents, rxMonitor;
};

IOLock *gLock;
thread_call_t gWorker;
Job gJobs[kQueueLength];
unsigned gHead, gCount;
bool gBusy, gMonitorOn;
uint32_t gPacketId, gTrial;
Counters gCounters;
swlan_rx_fn gRxFn;
void *gRxCtx;

uint64_t hostNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

// Once per (process, reason), release builds too: data refusals come from whichever process sent on awdl0.
void refusedOnce(unsigned reason, const char *what)
{
    static struct {
        char process[32];
        unsigned reason;
    } said[8];

    static unsigned count;
    char process[32] = {};
    proc_selfname(process, sizeof(process));
    IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);  // never held by a caller of this
    if (lock) {
        IOLockLock(lock);
        bool saidBefore = false;
        for (unsigned i = 0; i < count && !saidBefore; ++i)
            saidBefore = said[i].reason == reason && strncmp(said[i].process, process, sizeof(process)) == 0;
        if (!saidBefore && count < sizeof(said) / sizeof(said[0])) {
            strlcpy(said[count].process, process, sizeof(said[count].process));
            said[count++].reason = reason;
        }
        IOLockUnlock(lock);
        if (saidBefore)
            return;
    }
    IOLog("SwitchWLAN: AWDL soft: refused for %s: %s\n", process, what);
}

void send(const Job &job)
{
    static uint8_t request[BCM_ACTFRAME_REQUEST_LEN];
    const uint64_t waitedUs = (hostNs() - job.queuedNs) / 1000;
    if (job.stayUs && waitedUs >= job.stayUs) {
        ++gCounters.stale;  // its slot is over: the frame's timing fields are wrong now
        return;
    }
    const uint32_t dwellMs = job.stayUs ? static_cast<uint32_t>((job.stayUs - waitedUs) / 1000) : 0;
    const uint16_t chanspec = bcm_chanspec20(awdl_chan_of_freq(job.freq).number, switch_wlan_d11n());
    const size_t n =
        chanspec ? bcm_actframe_request(request, sizeof(request), job.frame, job.length, chanspec, dwellMs, ++gPacketId)
                 : 0;
    if (!n) {
        refusedOnce(0, "an action frame not sent: no 20 MHz chanspec for its channel or not an action frame");
        ++gCounters.refused;
        return;
    }
    const int e = switch_wlan_ioctl_set(kWlcSetVar, request, n);
    if (e) {
        ++gCounters.refused;
        static unsigned said;
        if (said++ < 4) {
            IOLog("SwitchWLAN: AWDL soft: actframe on chanspec %#x (dwell %u ms) answered %d%s%s\n", chanspec, dwellMs,
                  swlan_is_fwerr(e) ? swlan_fwerr_code(e) : e, swlan_is_fwerr(e) ? " " : "",
                  swlan_is_fwerr(e) ? bcme_name(swlan_fwerr_code(e)) : "");
        }
        return;
    }
    if (gCounters.sent++ == 0) {
        IOLog("SwitchWLAN: AWDL soft: first actframe accepted: chanspec %#x, dwell %u ms, %u bytes\n", chanspec,
              dwellMs, job.length);
    }
}

void monitor(bool on)
{
    const uint8_t value[4] = {static_cast<uint8_t>(on), 0, 0, 0};
    const int e = switch_wlan_ioctl_set(BCM_WLC_SET_MONITOR, value, sizeof(value));
    gMonitorOn = on && e == 0;
    IOLog("SwitchWLAN: AWDL soft: WLC_SET_MONITOR %d answered %d%s%s\n", on ? 1 : 0,
          swlan_is_fwerr(e) ? swlan_fwerr_code(e) : e, swlan_is_fwerr(e) ? " " : "",
          swlan_is_fwerr(e) ? bcme_name(swlan_fwerr_code(e)) : "");
}

void work(thread_call_param_t, thread_call_param_t)
{
    static Job job;  // the busy worker's only
    IOLockLock(gLock);
    if (gBusy) {
        IOLockUnlock(gLock);
        return;
    }
    gBusy = true;
    for (;;) {
        if (!gCount) {
            gBusy = false;
            IOLockUnlock(gLock);
            return;
        }
        job = gJobs[gHead];
        gHead = (gHead + 1) % kQueueLength;
        --gCount;
        IOLockUnlock(gLock);
        if (job.kind == kSend) {
            send(job);
        } else {
            monitor(job.kind == kMonitorOn);
        }
        IOLockLock(gLock);
    }
}

bool enqueue(JobKind kind, const swlan_tx_req *r)
{
    if (!gLock)
        return false;
    IOLockLock(gLock);
    if (gCount == kQueueLength) {
        ++gCounters.full;
        IOLockUnlock(gLock);
        refusedOnce(1, "an action frame dropped: eight already wait for the firmware");
        return false;
    }
    Job &j = gJobs[(gHead + gCount++) % kQueueLength];
    j.kind = kind;
    if (r) {
        j.freq = r->freq_mhz;
        j.stayUs = r->stay_us;
        j.length = static_cast<uint16_t>(r->len);
        memcpy(j.frame, r->frame, r->len);
    }
    j.queuedNs = hostNs();
    IOLockUnlock(gLock);
    thread_call_enter(gWorker);
    return true;
}

void deliver(const uint8_t *frame, size_t length, uint16_t freq, int8_t rssi, uint8_t flags)
{
    const swlan_rx_fn fn = __atomic_load_n(&gRxFn, __ATOMIC_ACQUIRE);
    if (!fn)
        return;
    swlan_rx_meta meta = {};
    meta.freq_mhz = freq;
    meta.rssi_dbm = rssi;
    meta.flags = flags;
    fn(__atomic_load_n(&gRxCtx, __ATOMIC_ACQUIRE), frame, length, &meta);
}

// WLC_E_ACTION_FRAME_RX: the frame from its category on, behind brcmf_rx_mgmt_data; the sender is the event's address.
void actionFrame(const uint8_t addr[6], const uint8_t *data, size_t length)
{
    static uint8_t frame[24 + BCM_AF_BODY_MAX];  // the work loop's only
    bcm_rx_frame meta = {};
    const uint8_t *body = nullptr;
    size_t n = 0;
    if (bcm_awdl_rx_af(data, length, &meta, &body, &n) || n > BCM_AF_BODY_MAX)
        return;
    memset(frame, 0, 24);
    frame[0] = 0xd0;
    memset(frame + 4, 0xff, 6);
    memcpy(frame + 10, addr, 6);
    memcpy(frame + 16, awdl_bssid, 6);
    memcpy(frame + 24, body, n);
    const uint8_t ch = bcm_chanspec_primary(meta.chanspec);
    const uint16_t freq = awdl_chan_freq(awdl_chan{ch, awdl_opclass_20mhz(ch)});
    int32_t rssi = meta.version ? meta.rssi : -60;
    rssi = rssi > 0 ? 0 : rssi;
    rssi = rssi < -128 ? -128 : rssi;
    if (gCounters.rxEvents++ == 0) {
        IOLog("SwitchWLAN: AWDL soft: first AWDL action frame (WLC_E_ACTION_FRAME_RX, header v%u, chanspec %#x, "
              "%u bytes) from %02x:%02x:%02x:%02x:%02x:%02x\n",
              meta.version, meta.chanspec, static_cast<unsigned>(n), addr[0], addr[1], addr[2], addr[3], addr[4],
              addr[5]);
    }
    deliver(frame, 24 + n, freq, static_cast<int8_t>(rssi), 0);
}

}  // namespace

extern "C" void switch_wlan_soft_init(void)
{
    if (__atomic_load_n(&gLock, __ATOMIC_ACQUIRE))
        return;
    IOLock *lock = IOLockAlloc();
    thread_call_t worker = thread_call_allocate(work, nullptr);
    if (!lock || !worker) {
        if (lock)
            IOLockFree(lock);
        if (worker)
            thread_call_free(worker);
        refusedOnce(2, "no memory for the action frame queue: nothing will be sent");
        return;
    }
    gWorker = worker;
    __atomic_store_n(&gLock, lock, __ATOMIC_RELEASE);
}

extern "C" void switch_wlan_soft_caps(struct swlan_awdl_caps *caps)
{
#if DEVELOPMENT || DEBUG
    // 1: the soft backend on this firmware for a device run (action frames out, WLC_E_ACTION_FRAME_RX in); 2: and
    // monitor mode, to see what it delivers. AWDL data cannot go out either way.
    static bool read;
    if (!read) {
        read = true;
        uint32_t level = 0;
        if (PE_parse_boot_argn("switch-awdl-soft", &level, sizeof(level)))
            gTrial = level;
    }
    caps->soft_trial = static_cast<uint8_t>(gTrial);
#else
    caps->soft_trial = 0;
#endif
    // Unproven until a device run answers the first actframe (one log line either way).
    caps->can_tx_action = 0;
}

extern "C" void switch_wlan_soft_frame(const uint8_t *frame, size_t length)
{
    if (gCounters.rxMonitor++ == 0) {
        char hex[3 * 24 + 1] = {};
        for (size_t i = 0; i < length && i < 24; ++i)
            snprintf(hex + 3 * i, 4, " %02x", frame[i]);
        IOLog("SwitchWLAN: AWDL soft: first monitor frame, %u bytes:%s\n", static_cast<unsigned>(length), hex);
    }
    // brcmf_netif_mon_rx's plain form: the 802.11 frame and four bytes of receive status
    deliver(frame, length, 0, -60, SWLAN_RX_FCS_PRESENT);
}

extern "C" int switch_wlan_soft_event(uint32_t type, uint32_t status, const uint8_t addr[6], const uint8_t *data,
                                      size_t length)
{
    if (!__atomic_load_n(&gRxFn, __ATOMIC_ACQUIRE))
        return 0;  // the soft backend is not receiving: the native stack's, if any
    switch (type) {
    case WLC_E_ACTION_FRAME_RX:
        actionFrame(addr, data, length);
        return 1;
    case kEventActionFrameComplete:
        if (status) {
            ++gCounters.unacked;
        } else if (gCounters.acked++ == 0) {
            IOLog("SwitchWLAN: AWDL soft: first action frame completion: sent\n");
        }
        return 1;
    case kEventOffChannelComplete:
        if (gCounters.dwellsEnded++ == 0)
            IOLog("SwitchWLAN: AWDL soft: first dwell over (WLC_E_ACTION_FRAME_OFF_CHAN_COMPLETE)\n");
        return 1;
    default:
        return 0;
    }
}

extern "C" int switch_wlan_soft_stat(char *buf, size_t size)
{
    const Counters &c = gCounters;
    if (!c.sent && !c.refused && !c.rxEvents && !c.rxMonitor)
        return 0;
    const int n = snprintf(buf, size,
                           "; soft: actframes %u (refused %u, stale %u, queue full %u), completions %u (failed %u), "
                           "dwells %u, rx events %u, monitor frames %u%s",
                           c.sent, c.refused, c.stale, c.full, c.acked, c.unacked, c.dwellsEnded, c.rxEvents,
                           c.rxMonitor, gMonitorOn ? " (monitor on)" : "");
    return n < 0 ? 0 : n;
}

// NOLINTNEXTLINE(misc-const-correctness): the context goes back to the stack's callback, which takes a void *
extern "C" int swlan_awdl_rx_start(swlan_rx_fn fn, void *ctx)
{
    if (!fn || !gLock)
        return -ENXIO;
    __atomic_store_n(&gRxCtx, ctx, __ATOMIC_RELEASE);
    __atomic_store_n(&gRxFn, fn, __ATOMIC_RELEASE);
    if (gTrial >= 2)
        enqueue(kMonitorOn, nullptr);  // again after a chip recovery, which took the firmware's monitor mode with it
    return 0;
}

extern "C" int swlan_awdl_rx_stop(void)
{
    __atomic_store_n(&gRxFn, static_cast<swlan_rx_fn>(nullptr), __ATOMIC_RELEASE);
    if (gMonitorOn)
        enqueue(kMonitorOff, nullptr);
    return 0;
}

// The firmware answers frames to its own address only, which is the soft backend's AWDL address too.
extern "C" int swlan_awdl_rx_filter(const uint8_t *, const uint8_t *, uint16_t)
{
    return 0;
}

extern "C" int swlan_awdl_tx(const struct swlan_tx_req *req)
{
    if (!req || !req->frame || req->len < 24 || req->len > kFrameMax)
        return -EMSGSIZE;
    if ((req->frame[0] & 0xfc) != 0xd0) {
        refusedOnce(3, "AWDL data not sent: this firmware cannot send a caller-built data frame");
        return -EOPNOTSUPP;
    }
    return enqueue(kSend, req) ? 0 : -ENOBUFS;
}

// This radio cannot run a sequence: each frame's send and stay (swlan_tx_req) is how it follows the slots.
extern "C" int swlan_awdl_chanseq_set(const struct swlan_chan_seq *)
{
    return 0;
}

extern "C" int swlan_awdl_chanseq_clear(void)
{
    return 0;
}
