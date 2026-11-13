// SPDX-License-Identifier: BSD-2-Clause-Patent
// A VIC job past its bound: VIC stopped making progress on it (a lost syncpoint interrupt panics in TegraHost1x
// instead). As L4T's nvhost does on a channel timeout, and as TegraNVDEC, TegraNVENC and TegraNVJPG do, the jobs are
// cancelled, not the engine. The fence fails on host1x's work loop, which must not sleep, and the falcon's proof waits
// on a fence that loop delivers, so the cancellation runs on its own thread call; the queue holds new jobs from the
// failure until it is done (SwitchVICJobs.cpp):
//   - channel 1 is torn down, so nothing more of the queued streams reaches the engine;
//   - VIC's memory client is flushed and the engine reset with its partition kept on;
//   - every queued batch the syncpoint has not reached fails, recorded as failed before the syncpoint is released
//     through it (another engine's stream may be waiting on it). Its completion gets the failure, and a display frame
//     it converted is not shown, as for a refused job;
//   - the channel is restarted, the falcon booted and proven as at power-on;
//   - once the cancelled jobs have ended, the queue renumbers from the syncpoint and the next frame's job runs.
// All under the idle gate's lock. An engine sleep has already powered off is not touched; its next power-on boots it.
// An engine the reset does not bring back is parked and VIC refuses work. One line per cancellation.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include "TegraVICEngine.h"
#include "SwitchVICJobs.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace {
thread_call_t gCancel;

struct Stall {
    uint32_t syncpt;
    uint64_t fence;
    int error, producers;
    uint64_t elapsedUs;
} gStall;

void cancel(thread_call_param_t, thread_call_param_t)
{
    char host[200], engine[480];
    IOLock *power = switch_vic_power_lock();
    IOLockLock(power);
    const bool powered = switch_vic_engine_powered();
    const uint32_t syncpt = gStall.syncpt;
    const uint32_t reached = tegra_host1x_syncpt_read(syncpt);
    if (powered)
        switch_vic_engine_describe(engine, sizeof(engine));
    else
        snprintf(engine, sizeof(engine), "falcon powered off by sleep");
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_VIC, host, sizeof(host));
    tegra_host1x_channel_teardown(TEGRA_HOST1X_CHANNEL_VIC);
    bool drained = true;
    const bool reset = !powered || switch_vic_engine_reset(&drained);
    const uint32_t through = switch_vic_jobs_cancel(tegra_host1x_syncpt_read(syncpt));
    const int released = tegra_host1x_syncpt_release_through(syncpt, through);
    tegra_host1x_channel_restart(TEGRA_HOST1X_CHANNEL_VIC);
    const bool back = reset && !released && (!powered || switch_vic_engine_boot());
    const char *why = back ? "" : released ? "its fences were not released" : switch_vic_engine_wedge();
    IOLockUnlock(power);
    IOLog("TegraVIC: job fence %llu not reached in %llu ms (%d, GPU producers %d; syncpoint %u at %u); %s; %s; "
          "cancelled through fence %u, memory client %s, engine reset: %s%s\n",
          gStall.fence, gStall.elapsedUs / 1000, gStall.error, gStall.producers, syncpt, reached, engine, host, through,
          drained ? "drained" : "did not drain in 10 ms",
          back ? "running again" : "it did not come back, VIC refuses work: ", why);
    switch_vic_jobs_recovered(back);
}
}  // namespace

bool switch_vic_cancel_start(uint32_t syncpt)
{
    gStall.syncpt = syncpt;
    gCancel = thread_call_allocate_with_options(cancel, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    return gCancel != nullptr;
}

void switch_vic_job_stalled(uint64_t fence, int error, uint64_t elapsedUs, int producers)
{
    gStall.fence = fence;
    gStall.error = error;
    gStall.producers = producers;
    gStall.elapsedUs = elapsedUs;
    thread_call_enter(gCancel);
}
