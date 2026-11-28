// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decode on NVJPG (TegraNVJPG.h tegra_nvjpg_start, tegra_nvjpg_finish): its submission, its end by its fence or the
// falcon's error line, and the cancellation of one past its bound. The engine it runs on is TegraNVJPG.cpp's.
#include <IOKit/IOLib.h>
#include <string.h>
#include "TegraNVJPGEngine.h"
#include "TegraNVJPGStats.h"
#include "nvidia/cle7d0.h"
#include "../TegraHost1x/SwitchHost1xFence.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" IOLock *tegra_nvdec_power_lock(void);

namespace tegra_nvjpg_engine {
// The job engine. NVJPG has host1x channel 3 to itself and runs one job at a time (gJobLock), so nothing queues ahead
// of a job: its wait is bounded by kMarginUs (the falcon switching to its decoder app, the completion interrupt) plus
// one microsecond per 16 pixels of the MCU grid (16 Mpixel/s; healthy decodes run at 175 to 500), at most kMaxBoundUs.
// Only a hung engine gets there; the firmware's GP timer, on for every job, ends a picture it cannot finish with an
// error status.
constexpr uint64_t kMarginUs = 50000, kMaxBoundUs = 30000000;

inline uint32_t jobBoundUs(uint64_t pixels)
{
    const uint64_t us = kMarginUs + pixels / 16;
    return static_cast<uint32_t>(us < kMaxBoundUs ? us : kMaxBoundUs);
}

// The job on the engine, owned by gJobLock's holder from tegra_nvjpg_start to tegra_nvjpg_finish. Its end is signalled
// exactly once: by the fence's callback (host1x's work loop), or by falconFailed (the falcon line's thread call) when
// it takes the fence back first; switch_host1x_fence_remove decides between them. Whichever signals writes `result`
// first. gFlightLock orders falconFailed's look at `active` against start and finish, so it never takes back a fence
// of a job that has ended; it is held only around those few lines and never across a callback.
struct Flight {
    switch_host1x_fence fence;
    TegraNVJPGSignal signal;
    uint32_t threshold;
    uint64_t startUs;
    int result;
    TegraNVJPGFollow follow;  // the decode's, if any: cancelled() tells it
    bool active;
};

Flight gFlight;
IOLock *gFlightLock;

// A job past its bound: the engine stopped making progress on it (a lost interrupt panics in TegraHost1x instead).
// As L4T's nvhost does on a channel timeout, the job is cancelled, not the engine: channel 3 is torn down so nothing of
// the stream reaches the engine again, the memory client is flushed (tegra_mc_flush), the engine reset with its
// partition kept on and brought up again as at J1 (powerUp releases the flush once the reset is), the job's fence
// released by the CPU, the falcon rebooted and proven as at J2, and the job fails; the next one runs. The release lets
// a re-pack queued behind the fence (TegraNVJPGFollow: VIC or the copy engine) run and end, as NVDEC's and VIC's
// cancellations do. An engine the reset does not bring back, or whose fence host1x does not release, is turned off and
// handed back to the idle gate, which powers it on at a later decode. One line per cancellation, with what the falcon
// and the memory controller held. gJobLock held (the caller's) and the job's use of the gate, so no NVJPG power
// transition runs meanwhile (gateOn runs only in a job, gateOff only with no user, and suspend first takes gJobLock).
int cancelled(const TegraNVJPGJob *job, uint32_t fence, uint64_t us)
{
    char host[200];
    char falcon[400];
    char what[240];
    char why[288];
#if TEGRA_NVJPG_PROBES
    __atomic_fetch_add(&gCancellations, 1, __ATOMIC_RELAXED);
#endif
    if (!gErrors.failed(why, sizeof(why)))
        snprintf(why, sizeof(why), "not reached in %llu ms", us / 1000);
    gErrors.lostCheckStart();
    const nvjpg_dec_drv_pic_setup_s &s = job->setup;
    snprintf(
        what, sizeof(what),
        "selector %u, %dx%d (MCU %dx%d) at 1/%u, stream chroma %d to chroma %d pixel format %d memory mode %d tile "
        "mode %d block height %d, %d bitstream bytes, %u planes, target format %u",
        job->selector, s.frame_width, s.frame_height, s.mcu_width, s.mcu_height, 1u << (s.power2_downscale & 3),
        s.stream_chroma_mode, s.output_chroma_mode, s.output_pixel_format, s.memory_mode, s.tile_mode,
        s.block_linear_height, s.bitstream_size, job->planes, job->target);
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_NVJPG, host, sizeof(host));
    const uint32_t reached = tegra_host1x_syncpt_read(gSyncpt);
    switch_falcon_stall(gJpg, falcon, sizeof(falcon));
    uint32_t mcStatus = 0, mcError = 0, mcAddress = 0;
    const uint32_t mcErrors = tegra_mc_error_latest(&mcStatus, &mcError, &mcAddress);
    tegra_host1x_channel_teardown(TEGRA_HOST1X_CHANNEL_NVJPG);
    IOLock *power = tegra_nvdec_power_lock();
    IOLockLock(power);
    gLine[0] = 0;
    bool wasOn = false;
    const bool drained = switch_engine_power::flush(kPower);
    gErrors.lostCheckEnd();
    gErrors.off();
    switch_falcon_error_clear(gJpg);
    switch_car_reset_assert(kClock);
    IODelay(10);
    const bool reset = powerUp(&wasOn);
    if (gFlight.follow.cancelled)
        gFlight.follow.cancelled(gFlight.follow.context);
    const int released = tegra_host1x_syncpt_release_through(gSyncpt, fence);
    tegra_host1x_channel_restart(TEGRA_HOST1X_CHANNEL_NVJPG);
    const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
    const char *stage = nullptr;
    const bool back = reset && !released && bootFalcon() && !answer(gSyncpt, from, &stage);
    if (back)
        gErrors.on();
    IOLockUnlock(power);
    gNext = tegra_host1x_syncpt_read(gSyncpt);
    __atomic_store_n(&gWedged, !back, __ATOMIC_RELEASE);
    if (!back) {
        engineOff();
        gGate.lost();
    }
    IOLog("TegraNVJPG: job fence %u %s (syncpoint %u at %u; powered on by the idle gate %u times, "
          "the last %llu ms ago); job %s; %s; MC errors %u, latest INTSTATUS %#x ERR_STATUS %#x ERR_ADR %#x; %s; job "
          "cancelled, memory "
          "client %s, engine reset: %s%s\n",
          fence, why, gSyncpt, reached, gPowerOns,
          gPowerOns ? static_cast<unsigned long long>((switch_now_us() - gPoweredAtUs) / 1000) : 0ULL, what, falcon,
          mcErrors, mcStatus, mcError, mcAddress, host, drained ? "drained" : "did not drain in 10 ms",
          back ? "running again" : "it did not come back, the next decode powers it on again: ",
          back ? ""
               : (gLine[0]   ? gLine
                  : released ? "its fence was not released"
                  : stage    ? stage
                             : "falcon"));
    return -5;
}

void flightEnded(switch_host1x_fence *, int status)
{
    __atomic_store_n(&gFlight.result, status, __ATOMIC_RELEASE);
    gFlight.signal.ended(gFlight.signal.context);
}

// The falcon's error interrupt: the decode in flight (one at a time) ends at once, and its finish cancels it with the
// firmware's reason (cancelled()), the engine cleared and reset.
void falconFailed(const switch_falcon_error &)
{
    IOLockLock(gFlightLock);
    const bool taken = gFlight.active && switch_host1x_fence_remove(&gFlight.fence);
    IOLockUnlock(gFlightLock);
    if (!taken)
        return;
    __atomic_store_n(&gFlight.result, TEGRA_HOST1X_EFALCON, __ATOMIC_RELEASE);
    gFlight.signal.ended(gFlight.signal.context);
}
}  // namespace tegra_nvjpg_engine

using namespace tegra_nvjpg_engine;

extern "C" void tegra_nvjpg_lock(void)
{
    IOLockLock(gJobLock);
}

extern "C" void tegra_nvjpg_unlock(void)
{
    IOLockUnlock(gJobLock);
}

#if TEGRA_NVJPG_STATS
void tegra_nvjpg_lock_counts(uint64_t *waitsUnlocked)
{
    *waitsUnlocked = __atomic_exchange_n(&gWaitsUnlocked, 0, __ATOMIC_RELAXED);
}
#endif
#if TEGRA_NVJPG_PROBES
uint64_t tegra_nvjpg_cancellations(void)
{
    return __atomic_load_n(&gCancellations, __ATOMIC_RELAXED);
}
#endif

// Job lock held. The setup and status slots are the kernel's (no client maps them); the stream is SETCLASS, the
// decoder's methods through THI METHOD0/METHOD1 in averne's order (application, control with the GP timer on, picture
// index, setup, bitstream, status, output planes, EXECUTE with AWAKEN), then an OP_DONE increment of NVJPG's
// syncpoint, whose fence ends the job by interrupt (gFlight).
extern "C" int tegra_nvjpg_start(const TegraNVJPGJob *job, const TegraNVJPGSignal *signal,
                                 const TegraNVJPGFollow *follow)
{
    if (!gUp || !job->planes || job->planes > 3)
        return -19;
    if ((job->bitstream & 0xff) || (job->plane[0] & 0xff) || (job->planes > 1 && (job->plane[1] & 0xff)) ||
        (job->planes > 2 && (job->plane[2] & 0xff)))
        return -22;
    if (const int error = gGate.use())
        return error;
    // gGate.use() returning 0 guarantees the engine is powered on and not suspended (gateOn/engineResume publish
    // gWedged with release semantics before releasing the gate).
    if (__atomic_load_n(&gWedged, __ATOMIC_ACQUIRE)) {
        gGate.done();
        return -19;
    }
    memcpy(gSetup.va, &job->setup, sizeof(job->setup));
    switch_dcache_clean_lines(gSetup.va, sizeof(job->setup));
    bzero(gStatus.va, sizeof(nvjpg_dec_status));
    switch_dcache_clean_invalidate(gStatus.va, sizeof(nvjpg_dec_status));
    uint32_t *words = nullptr;
    if (const int error = switch_host1x_job_begin(TEGRA_HOST1X_CHANNEL_NVJPG, 1 + 3 * 11 + 1, &words)) {
        gGate.done();
        return error;
    }
    uint32_t n = 0;
    words[n++] = tegra_host1x_op_setclass(kClass, 0, 0);
    const auto method = [&](uint32_t offset, uint32_t value) {
        words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
        words[n++] = offset / 4;
        words[n++] = value;
    };
    method(NVE7D0_SET_APPLICATION_ID, NVE7D0_SET_APPLICATION_ID_ID_NVJPG_DECODER);
    method(NVE7D0_SET_CONTROL_PARAMS, 1u << 0 | 1u << 1);  // GPTIMER_ON, DUMP_CYCLE_COUNT
    method(NVE7D0_SET_PICTURE_INDEX, gPicture++);
    method(NVE7D0_SET_IN_DRV_PIC_SETUP, (uint32_t)(gSetup.pa >> 8));
    method(NVE7D0_SET_BITSTREAM, (uint32_t)(job->bitstream >> 8));
    method(NVE7D0_SET_OUT_STATUS, (uint32_t)(gStatus.pa >> 8));
    method(NVE7D0_SET_CUR_PIC, (uint32_t)(job->plane[0] >> 8));
    if (job->planes > 1)
        method(NVE7D0_SET_CUR_PIC_CHROMA_U, (uint32_t)(job->plane[1] >> 8));
    if (job->planes > 2)
        method(NVE7D0_SET_CUR_PIC_CHROMA_V, (uint32_t)(job->plane[2] >> 8));
    method(NVE7D0_EXECUTE, 1u << 8);  // AWAKEN
    words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    const uint32_t fence = gNext + 1;
    gFlight.startUs = switch_now_us();
    if (const int error = switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_NVJPG, words, n, gSyncpt, fence)) {
        gGate.done();
        return error;
    }
    gNext = fence;
    gFlight.follow = follow ? *follow : TegraNVJPGFollow{};
    if (follow)
        follow->queue(follow->context, gSyncpt, fence);
    gFlight.signal = *signal;
    gFlight.threshold = fence;
    gFlight.result = 0;
    switch_host1x_fence_init(&gFlight.fence, gSyncpt, fence, flightEnded);
    IOLockLock(gFlightLock);
    const int armed = switch_host1x_fence_add(&gFlight.fence, jobBoundUs(job->pixels));
    gFlight.active = armed == TEGRA_HOST1X_OK;
    // An error the falcon raised before the fence was armed found no job to fail: it fails this one now.
    const bool failed = gFlight.active && __atomic_load_n(gErrors.takenWord(), __ATOMIC_ACQUIRE) != gErrors.onAt() &&
                        switch_host1x_fence_remove(&gFlight.fence);
    IOLockUnlock(gFlightLock);
    if (armed == TEGRA_HOST1X_OK) {
        if (failed) {
            __atomic_store_n(&gFlight.result, TEGRA_HOST1X_EFALCON, __ATOMIC_RELEASE);
            gFlight.signal.ended(gFlight.signal.context);
        }
        return 0;
    }
    // Nothing would end a job on the engine with no fence: it is cancelled now, as one past its bound is.
    const int cancel = cancelled(job, fence, switch_now_us() - gFlight.startUs);
    gGate.done();
    return cancel ? cancel : armed;
}

extern "C" int tegra_nvjpg_finish(const TegraNVJPGJob *job, nvjpg_dec_status *status, uint64_t *busyUs)
{
    IOLockLock(gFlightLock);
    gFlight.active = false;
    IOLockUnlock(gFlightLock);
    int ret = __atomic_load_n(&gFlight.result, __ATOMIC_ACQUIRE);
#if TEGRA_NVJPG_STATS
    __atomic_fetch_add(&gWaitsUnlocked, 1, __ATOMIC_RELAXED);
#endif
    if (ret == TEGRA_HOST1X_ETIMEDOUT || ret == TEGRA_HOST1X_EFALCON)
        ret = cancelled(job, gFlight.threshold, switch_now_us() - gFlight.startUs);
    if (busyUs)
        *busyUs = switch_now_us() - gFlight.startUs;
    switch_dcache_clean_invalidate(gStatus.va, sizeof(nvjpg_dec_status));
    memcpy(status, gStatus.va, sizeof(*status));
    gGate.done();
    return ret;
}
