// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC (SwitchXUDC.h): the stats lines, built into DEVELOPMENT and DEBUG kernels only.
#include "SwitchXUDC.h"

#if DEVELOPMENT || DEBUG
#include <os/log.h>

namespace {
constexpr uint32_t kStatsMs = 2000;  // the stats line: at most this often, only after data moved
}  // namespace

void SwitchXUDC::statsBegin()
{
    nanoseconds_to_absolutetime(static_cast<uint64_t>(kStatsMs) * 1000000ULL, &stats.window);
}

// The end of an event drain that began at `entered`.
void SwitchXUDC::drained(uint64_t entered)
{
    const uint64_t now = XudcStats::clock();
    stats.now.ticks += now - entered;
    if (now - stats.at >= stats.window)
        logStats(now);
}

// Work loop: one os_log line per kStatsMs at most, only after data moved (never the console): MB/s, requests and what
// was bounced per direction, interrupts and events per second, and the driver's own CPU time (the interrupt filter, the
// event drain and the queueing in ioGated, which includes prepare() and the bounce copies) as a share of one core. A
// second line (the kernel's os_log encodes at most about 236 bytes of arguments: one line would be dropped whole): how
// transfer events matched requests (XudcTransfer.h Verdict), failed requests and the most TRBs one request took. A
// third, when requests went into the window: what a mapping and an SMMU flush took, the measurements
// kRemapWorthBytes (XudcIova.h) is to be set from.
void SwitchXUDC::logStats(uint64_t now)
{
    XudcStats::Counts &st = stats.now;
    const XudcStats::Counts &o = stats.logged;
    uint32_t *const mostTrbs = stats.mostTrbs;
    const uint32_t irqs = __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED);
    st.ticks += __atomic_exchange_n(&stats.filterTicks, 0, __ATOMIC_RELAXED);
    if (st.bytes[0] == o.bytes[0] && st.bytes[1] == o.bytes[1]) {
        stats.at = now;
        stats.logged = st;
        stats.interruptsLogged = irqs;
        mostTrbs[0] = mostTrbs[1] = 0;
        windowLogged = smmu.cost();
        return;
    }
    uint64_t ns = 0;
    uint64_t busy = 0;
    absolutetime_to_nanoseconds(now - stats.at, &ns);
    absolutetime_to_nanoseconds(st.ticks - o.ticks, &busy);
    if (!ns)
        ns = 1;
    const auto rate = [ns](uint64_t bytes) {
        return bytes * 100000ULL / ns;
    };  // hundredths of a MB/s (10^6 bytes)
    const uint64_t in = rate(st.bytes[1] - o.bytes[1]);
    const uint64_t out = rate(st.bytes[0] - o.bytes[0]);
    const uint64_t cpu = busy * 10000ULL / ns;  // hundredths of a percent of one core
    os_log(OS_LOG_DEFAULT,
           "SwitchXUDC: stats %llu ms: IN %llu.%02llu MB/s (%llu requests, %llu KiB bounced, %llu serial), OUT "
           "%llu.%02llu MB/s (%llu requests, %llu KiB bounced, %llu serial); %llu interrupts/s, %llu events/s; CPU "
           "%llu.%02llu%% of one core (filter, events, queueing); bounced packets: %u breaks, %u 64K, %u above 4G, %u "
           "cache-line edges; bounce pages free %u/%u",
           ns / 1000000, in / 100, in % 100, st.requests[1] - o.requests[1], (st.bounced[1] - o.bounced[1]) / 1024,
           st.serial[1] - o.serial[1], out / 100, out % 100, st.requests[0] - o.requests[0],
           (st.bounced[0] - o.bounced[0]) / 1024, st.serial[0] - o.serial[0],
           (uint64_t)(irqs - stats.interruptsLogged) * 1000000000ULL / ns,
           (uint64_t)(st.events - o.events) * 1000000000ULL / ns, cpu / 100, cpu % 100,
           st.why.discontinuity - o.why.discontinuity, st.why.boundary - o.why.boundary, st.why.high - o.why.high,
           st.why.edge - o.why.edge, poolFree, pool.va ? kPoolPages : 0);
    os_log(OS_LOG_DEFAULT,
           "SwitchXUDC: transfer events %llu ms: %u TDs ended short before their last TRB, %u second events absorbed, "
           "%u stale, %u for no request; failed requests IN %u OUT %u; most TRBs per request IN %u OUT %u",
           ns / 1000000, st.shortEarly - o.shortEarly, st.second - o.second, st.stale - o.stale,
           st.unexplained - o.unexplained, (uint32_t)(st.failed[1] - o.failed[1]),
           (uint32_t)(st.failed[0] - o.failed[0]), mostTrbs[1], mostTrbs[0]);
    logWindow(ns);
    stats.logged = st;
    stats.interruptsLogged = irqs;
    mostTrbs[0] = mostTrbs[1] = 0;
    stats.at = now;
}

void SwitchXUDC::logWindow(uint64_t ns)
{
    const UsbSmmuSpace::Cost window = smmu.cost();
    const uint64_t maps = window.maps - windowLogged.maps;
    const uint64_t flushes = window.flushes - windowLogged.flushes;
    if (maps && flushes) {
        uint64_t mapNs = 0;
        uint64_t flushNs = 0;
        absolutetime_to_nanoseconds(window.mapTicks - windowLogged.mapTicks, &mapNs);
        absolutetime_to_nanoseconds(window.flushTicks - windowLogged.flushTicks, &flushNs);
        os_log(OS_LOG_DEFAULT,
               "SwitchXUDC: window %llu ms: %llu requests mapped, %llu ns a mapping (PTEs, their clean, one flush); "
               "%llu SMMU flushes, %llu ns each; mapped from %u bounced bytes",
               ns / 1000000, maps, mapNs / maps, flushes, flushNs / flushes, kRemapWorthBytes);
    }
    windowLogged = window;
}

#endif
