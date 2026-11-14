// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraNVJPGStats.h. Debug kernels only: a release kernel has none of this code or its strings.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <os/log.h>
#include "TegraNVJPGStats.h"
#if TEGRA_NVJPG_STATS
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
IOLock *gLock;

struct {
    uint64_t windowStart, decodes, refused, failed, engineUs, cpuUs, pixels, bytes;
} gWindow;

struct Process {
    int pid;
    TegraNVJPGRouteCounts counts;
};

Process gByProcess[8];

uint64_t activity(const TegraNVJPGRouteCounts &c)
{
    return c.decodes + c.failed + c.refused;
}

// The process's slot; a new process takes the least active one. gLock held.
TegraNVJPGRouteCounts &slotOf(int pid)
{
    Process *slot = &gByProcess[0];
    for (auto &entry : gByProcess) {
        if (entry.pid == pid)
            return entry.counts;
        if (activity(entry.counts) < activity(slot->counts))
            slot = &entry;
    }
    *slot = {pid, {}};
    return slot->counts;
}

void ask(TegraNVJPGRouteCounts &c, uint32_t format)
{
    if (format < TEGRA_NVJPG_OUT_COUNT)
        c.asked[format]++;
}
}  // namespace

bool tegra_nvjpg_stats_start(void)
{
    gLock = IOLockAlloc();
    return gLock != nullptr;
}

void tegra_nvjpg_count_refused(int pid, uint32_t format)
{
    IOLockLock(gLock);
    gWindow.refused++;
    TegraNVJPGRouteCounts &c = slotOf(pid);
    c.refused++;
    ask(c, format);
    IOLockUnlock(gLock);
}

void tegra_nvjpg_count_job(int pid, uint32_t format, bool failed, uint64_t engineUs, uint64_t cpuUs, uint64_t pixels,
                           uint64_t bytes)
{
    IOLockLock(gLock);
    gWindow.decodes++;
    gWindow.failed += failed ? 1 : 0;
    gWindow.engineUs += engineUs;
    gWindow.cpuUs += cpuUs;
    gWindow.pixels += pixels;
    gWindow.bytes += bytes;
    TegraNVJPGRouteCounts &c = slotOf(pid);
    ask(c, format);
    if (failed) {
        c.failed++;
    } else {
        c.decodes++;
        c.formats |= 1ull << format;
    }
    IOLockUnlock(gLock);
}

void tegra_nvjpg_counts_of(int pid, TegraNVJPGRouteCounts *out)
{
    *out = {};
    IOLockLock(gLock);
    for (const auto &entry : gByProcess) {
        if (entry.pid == pid)
            *out = entry.counts;
    }
    IOLockUnlock(gLock);
}

void tegra_nvjpg_stats_line(void)
{
    IOLockLock(gLock);
    const uint64_t now = switch_now_us();
    if (!gWindow.windowStart)
        gWindow.windowStart = now;
    if (now - gWindow.windowStart < 5000000 || !(gWindow.decodes + gWindow.refused)) {
        IOLockUnlock(gLock);
        return;
    }
    const auto w = gWindow;
    bzero(&gWindow, sizeof(gWindow));
    gWindow.windowStart = now;
    IOLockUnlock(gLock);
    uint64_t waitsUnlocked = 0;
    uint64_t stateWaitMaxUs = 0;
    tegra_nvjpg_lock_counts(&waitsUnlocked, &stateWaitMaxUs);
    char line[280];
    const uint64_t n = w.decodes ? w.decodes : 1;
    snprintf(line, sizeof(line),
             "TegraNVJPG: %llus decodes=%llu refused=%llu failed=%llu engine=%lluus/decode cpu=%lluus/decode "
             "pixels=%lluK/decode scan=%lluKB/decode unlocked_waits=%llu state_lock_wait_max=%lluus",
             (now - w.windowStart) / 1000000, w.decodes, w.refused, w.failed, w.engineUs / n, w.cpuUs / n,
             w.pixels / n / 1000, w.bytes / n / 1024, waitsUnlocked, stateWaitMaxUs);
    os_log(OS_LOG_DEFAULT, "%s", line);
}
#endif
