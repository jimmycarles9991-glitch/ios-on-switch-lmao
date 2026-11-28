// SPDX-License-Identifier: BSD-2-Clause-Patent
// The module lock probe's run (SwitchHost1xMlockProbe.h), debug kernels only, on VIC's second and third channels and
// the probes' shared syncpoints. It sleeps on syncpoint thresholds and never polls. A stream left waiting is torn down,
// and its syncpoints released, before it lets the set go.
#include "SwitchHost1xMlockProbe.h"

#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>

namespace {
using namespace switch_host1x_mlock;

constexpr uint32_t kFirst = TEGRA_HOST1X_CHANNEL_VIC_SECOND, kSecond = TEGRA_HOST1X_CHANNEL_VIC_THIRD;

enum Syncpt : uint32_t {
    kGate,
    kHeld,
    kDone,
    kAfter,
    kSyncpts
};

static_assert(static_cast<uint32_t>(kSyncpts) <= SWITCH_HOST1X_PROBE_SYNCPTS);
uint32_t gIds[SWITCH_HOST1X_PROBE_SYNCPTS];

struct Case {
    int status;
    bool held, early, after, settled;
    uint32_t watchedUs;
};

bool reachedWithin(uint32_t id, uint32_t at, uint32_t us)
{
    return tegra_host1x_syncpt_wait(gIds[id], at, us) == TEGRA_HOST1X_OK;
}

uint32_t next(uint32_t id)
{
    return tegra_host1x_syncpt_read(gIds[id]) + 1;
}

template <class Build> int submit(uint32_t channel, uint32_t capacity, Build &&build, uint32_t fence, uint32_t at)
{
    uint32_t *words = nullptr;
    if (const int error = switch_host1x_job_reserve(channel, capacity, &words))
        return error;
    return switch_host1x_job_submit(channel, words, build(words), gIds[fence], at);
}

uint32_t elapsedUs(uint64_t since)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
    return static_cast<uint32_t>(ns / 1000);
}

Case runCase(bool locked)
{
    Case c = {};
    const uint32_t gateAt = next(kGate), heldAt = next(kHeld), doneAt = next(kDone), afterAt = next(kAfter);
    c.status = submit(
        kFirst, kHolderWords,
        [&](uint32_t *w) {
            return holder(w, locked, gIds[kHeld], gIds[kGate], gateAt, gIds[kDone]);
        },
        kDone, doneAt);
    if (c.status)
        return c;
    c.held = reachedWithin(kHeld, heldAt, kRunUs);
    c.status = submit(
        kSecond, kAskerWords,
        [&](uint32_t *w) {
            return asker(w, locked, gIds[kAfter]);
        },
        kAfter, afterAt);
    const uint64_t start = mach_absolute_time();
    c.early = !c.status && reachedWithin(kAfter, afterAt, locked ? kHeldUs : kRunUs);
    c.watchedUs = elapsedUs(start);
    tegra_host1x_syncpt_cpu_increment(gIds[kGate]);
    const bool done = reachedWithin(kDone, doneAt, kRunUs);
    c.after = !c.status && (c.early || reachedWithin(kAfter, afterAt, kRunUs));
    c.settled = done && c.after;
    if (!c.settled) {
        tegra_host1x_channel_teardown(kFirst);
        tegra_host1x_channel_teardown(kSecond);
        tegra_host1x_channel_restart(kFirst);
        tegra_host1x_channel_restart(kSecond);
        tegra_host1x_syncpt_release_failed(gIds[kHeld], heldAt, 1);
        tegra_host1x_syncpt_release_failed(gIds[kDone], doneAt, 1);
        tegra_host1x_syncpt_release_failed(gIds[kAfter], afterAt, 1);
    }
    return c;
}
}  // namespace

extern "C" int switch_host1x_mlock_probe_run(void *out)
{
    auto *reply = static_cast<SwitchHost1xMlockProbeReply *>(out);
    *reply = {SWITCH_HOST1X_MLOCK_PROBE_MAGIC, 0, 0, 0, 0, 0, 0, 0};
    if (const int started = tegra_host1x_start()) {
        reply->status = started;
        return 0;
    }
    if (const int error = switch_host1x_probe_channels_take()) {
        reply->status = error;
        return 0;
    }
    if (const int error = switch_host1x_probe_syncpts_claim(gIds)) {
        reply->status = error;
        return 0;
    }
    const Case control = runCase(false);
    const Case locked = control.status || !control.settled ? Case{} : runCase(true);
    reply->status = control.status ? control.status : locked.status;
    reply->controlSideBySide = control.early;
    reply->heldTaken = locked.held;
    reply->heldOff = locked.held && !locked.early;
    reply->afterRelease = locked.after && !locked.early;
    reply->heldUs = locked.watchedUs;
    reply->settled = control.settled && locked.settled;
    switch_host1x_probe_syncpts_release();
    IOLog("TegraHost1x: module lock probe: status %d, control side by side %u, lock taken %u, second channel held "
          "%u for %u us, ran after the release %u, settled %u\n",
          reply->status, reply->controlSideBySide, reply->heldTaken, reply->heldOff, reply->heldUs, reply->afterRelease,
          reply->settled);
    return 0;
}
#endif
