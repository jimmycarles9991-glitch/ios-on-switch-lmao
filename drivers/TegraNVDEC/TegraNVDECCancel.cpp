// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decode past its bound: NVDEC stopped making progress on it (the GP timer ends a picture the firmware cannot finish
// long before; a lost syncpoint interrupt panics in TegraHost1x instead). As L4T's nvhost does on a channel timeout,
// and as TegraNVENC and TegraNVJPG do, the jobs are cancelled, not the engine. The fence fails on host1x's work loop,
// which must not sleep, and the falcon's proof waits on a fence that loop delivers, so the cancellation runs on its own
// thread call; the queue holds new jobs from the failure until it is done (SwitchNVDECJobs.cpp):
//   - channel 0 is torn down, so nothing more of the queued streams reaches the engine;
//   - NVDEC's memory client is flushed and the engine reset with its partition kept on;
//   - every queued job the syncpoint has not reached fails (-5, a failed picture for VideoToolbox), recorded as failed
//     before the syncpoint is released through it: another engine's stream may be waiting on it;
//   - the channel is restarted, the falcon booted and proven as at power-on;
//   - once the cancelled jobs have ended, the queue renumbers from the syncpoint and the next job runs.
// All under the power lock, as at power-on: NVJPG's power path writes MC_HOTRESET_CTRL_1 too. An engine sleep has
// already powered off is not touched; its next power-on boots it. An engine the reset does not bring back is parked and
// NVDEC refuses work. One line per cancellation.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include "TegraNVDECEngine.h"
#include "SwitchNVDECJobs.h"
#include "../TegraHost1x/TegraHost1x.h"

extern "C" IOLock *tegra_nvdec_power_lock(void);

namespace {
thread_call_t gCancel;

struct Stall {
    uint32_t syncpt, fence;
    int error;
    uint64_t elapsedUs;
} gStall;

void cancel(thread_call_param_t, thread_call_param_t)
{
    char host[200], engine[480];
    IOLock *power = tegra_nvdec_power_lock();
    IOLockLock(power);
    const bool powered = switch_nvdec_engine_powered();
    const uint32_t syncpt = gStall.syncpt;
    const uint32_t reached = tegra_host1x_syncpt_read(syncpt);
    if (powered)
        switch_nvdec_engine_describe(engine, sizeof(engine));
    else
        snprintf(engine, sizeof(engine), "falcon powered off by sleep");
    tegra_host1x_describe(host, sizeof(host));
    tegra_host1x_channel_teardown(TEGRA_HOST1X_CHANNEL_DEFAULT);
    bool drained = true;
    const bool reset = !powered || switch_nvdec_engine_reset(&drained);
    const uint32_t through = switch_nvdec_jobs_cancel(tegra_host1x_syncpt_read(syncpt));
    const int released = tegra_host1x_syncpt_release_through(syncpt, through);
    tegra_host1x_channel_restart(TEGRA_HOST1X_CHANNEL_DEFAULT);
    const bool back = reset && !released && (!powered || switch_nvdec_engine_boot());
    const char *why = back ? "" : released ? "its fences were not released" : switch_nvdec_engine_wedge();
    IOLockUnlock(power);
    IOLog("TegraNVDEC: job fence %u not reached in %llu ms (%d; syncpoint %u at %u); %s; %s; cancelled through fence "
          "%u, memory client %s, engine reset: %s%s\n",
          gStall.fence, gStall.elapsedUs / 1000, gStall.error, syncpt, reached, engine, host, through,
          drained ? "drained" : "did not drain in 10 ms",
          back ? "running again" : "it did not come back, NVDEC refuses work: ", why);
    switch_nvdec_jobs_recovered(back);
}
}  // namespace

bool switch_nvdec_cancel_start(uint32_t syncpt)
{
    gStall.syncpt = syncpt;
    gCancel = thread_call_allocate_with_options(cancel, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    return gCancel != nullptr;
}

void switch_nvdec_job_stalled(uint32_t fence, int error, uint64_t elapsedUs)
{
    gStall.fence = fence;
    gStall.error = error;
    gStall.elapsedUs = elapsedUs;
    thread_call_enter(gCancel);
}
