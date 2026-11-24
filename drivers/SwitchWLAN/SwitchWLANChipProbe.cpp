// SPDX-License-Identifier: BSD-2-Clause-Patent
#if DEVELOPMENT || DEBUG
#include <kern/thread_call.h>
#include <os/log.h>
#include "SwitchWLANProbeHooks.h"

namespace {
// Stock firmware 7.35.252.108's allocator: arena total, bytes in use, block-header overhead in use (8 bytes a block),
// high-water mark of in-use plus overhead, failed allocations; then the failures of packet-buffer allocations, of
// packet-ID allocations and of the shared-pool fallback.
constexpr uint32_t kHeapWords[] = {0x18102c, 0x18104c, 0x181074, 0x1df200, 0x181028, 0x1df2e8, 0x1df220, 0x1df418};
constexpr unsigned kHeapCount = sizeof kHeapWords / sizeof kHeapWords[0];

// Stock firmware 7.35.252.108's receive state, from wl at 0x1df208 (wl_attach's callback argument): wl+8 wlc; wlc+0
// pub, +0x130 A-MSDU module, +0x212 A-MSDU receive, +0x213 rx_amsdu_in_ampdu; pub+0x3e pktc (receive loop 0x1a7bdc);
// module+0 wlc, +0x6c counters of subframes reassembled, A-MSDUs completed, subframes out of order, partials flushed.
constexpr uint32_t kWlSlot[] = {0x1df208};
constexpr uint32_t kWlFields[] = {0x8};
constexpr uint32_t kWlcFields[] = {0x0, 0x130, 0x210};
constexpr uint32_t kPubFields[] = {0x3c};
constexpr uint32_t kModuleFields[] = {0x0, 0x6c};
constexpr uint32_t kCounterFields[] = {0x2c, 0x30, 0x38, 0x50};

// Stored at every radio bring-up, read by the worker and the AWDL start thread call.
switch_wlan_probe_read gRead;
thread_call_t gAwdlStart;

void awdlStart(thread_call_param_t, thread_call_param_t)
{
    switch_wlan_probe_heap("awdl-start");
}

bool enter(thread_call_t &call)
{
    const thread_call_t c = __atomic_load_n(&call, __ATOMIC_ACQUIRE);
    if (c)
        thread_call_enter(c);
    return c != nullptr;
}

template <unsigned N>
bool readFields(switch_wlan_probe_read read, uint32_t base, const uint32_t (&offsets)[N], uint32_t (&values)[N])
{
    uint32_t addresses[N];
    for (unsigned i = 0; i < N; ++i) {
        if (offsets[i] > UINT32_MAX - base)
            return false;
        addresses[i] = base + offsets[i];
    }
    return read(addresses, values, N);
}

void logChainBroke(const char *moment, unsigned hop, const char *name, uint32_t pointer)
{
    os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe amsdu %s: chain broke at hop %u (%s %#x)", moment, hop, name, pointer);
}

void logAmsdu(switch_wlan_probe_read read, const char *moment)
{
    uint32_t wl[1], wlc[1], fields[3], pub[1];
    if (!read(kWlSlot, wl, 1)) {
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe amsdu %s: chip not reachable", moment);
        return;
    }
    if (!readFields(read, wl[0], kWlFields, wlc)) {
        logChainBroke(moment, 1, "wl", wl[0]);
        return;
    }
    if (!readFields(read, wlc[0], kWlcFields, fields)) {
        logChainBroke(moment, 2, "wlc", wlc[0]);
        return;
    }
    if (!readFields(read, fields[0], kPubFields, pub)) {
        logChainBroke(moment, 3, "pub", fields[0]);
        return;
    }
    const unsigned pktc = pub[0] >> 16 & 0xff;
    const unsigned amsduRx = fields[2] >> 16 & 0xff;
    const unsigned inAmpdu = fields[2] >> 24;
    const char *const reassembly = pktc && inAmpdu ? "on" : "off";

    uint32_t module[2], counters[4];
    unsigned brokenHop = 0;
    const char *brokenName = nullptr;
    uint32_t brokenPointer = 0;
    if (!readFields(read, fields[1], kModuleFields, module) || module[0] != wlc[0]) {
        brokenHop = 4;
        brokenName = "A-MSDU module";
        brokenPointer = fields[1];
    } else if (!readFields(read, module[1], kCounterFields, counters)) {
        brokenHop = 5;
        brokenName = "counters";
        brokenPointer = module[1];
    }
    if (brokenHop) {
        os_log(OS_LOG_DEFAULT,
               "SwitchWLAN: probe amsdu %s: pktc %u rx_amsdu_in_ampdu %u amsdu-rx %u, reassembly before the hook %s; "
               "counters: chain broke at hop %u (%s %#x)",
               moment, pktc, inAmpdu, amsduRx, reassembly, brokenHop, brokenName, brokenPointer);
        return;
    }
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe amsdu %s: pktc %u rx_amsdu_in_ampdu %u amsdu-rx %u, reassembly before the hook %s; "
           "subframes %u completed %u out-of-order %u flushed %u",
           moment, pktc, inAmpdu, amsduRx, reassembly, counters[0], counters[1], counters[2], counters[3]);
}
}

void switch_wlan_probe_attached(switch_wlan_probe_read read)
{
    __atomic_store_n(&gRead, read, __ATOMIC_RELEASE);
    if (!gAwdlStart)
        __atomic_store_n(&gAwdlStart, thread_call_allocate(awdlStart, nullptr), __ATOMIC_RELEASE);
    switch_wlan_probe_heap("attached");
}

void switch_wlan_probe_heap(const char *moment)
{
    const switch_wlan_probe_read read = __atomic_load_n(&gRead, __ATOMIC_ACQUIRE);
    uint32_t v[kHeapCount] = {};
    if (!read || !read(kHeapWords, v, kHeapCount)) {
        os_log(OS_LOG_DEFAULT, "SwitchWLAN: probe heap %s: chip not reachable", moment);
        return;
    }
    os_log(OS_LOG_DEFAULT,
           "SwitchWLAN: probe heap %s: total %u in-use %u overhead %u free %u high-water %u failed %u "
           "packet-failed %u packet-id-failed %u pool-failed %u",
           moment, v[0], v[1], v[2], v[0] - v[1] - v[2], v[3], v[4], v[5], v[6], v[7]);
    logAmsdu(read, moment);
}

void switch_wlan_probe_activated()
{
    static bool started;
    if (!started)
        started = enter(gAwdlStart);
}
#endif
