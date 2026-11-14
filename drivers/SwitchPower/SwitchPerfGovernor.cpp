// SPDX-License-Identifier: BSD-2-Clause-Patent
// On-demand CPU clock for the A57 cluster on the DFLL (SwitchThermal's governed mode, the default).
//
// What iOS does on Apple silicon (CLPC, the performance controller behind XNU's sched_perfcontrol_* callouts) and what
// Horizon does by hand with its boost mode: clocks up when there is work, down when there is none.
//   * Load: XNU's context-switch callout (sched_perfcontrol_csw_t, osfmk/arm/machine_routines_common.c
//     machine_switch_perfcontrol_context, called by thread_dispatch on every switch with interrupts masked) marks each
//     CPU busy or idle (IDLE = switching to the idle thread) and adds up its busy time, in its own slot
//     (SwitchCpuLoad.h). No sampling, no timer of ours.
//   * Tick: XNU's perfcontrol deadline (sched_perfcontrol_update_callback_deadline, the deadline_passed callout),
//     which the scheduler checks for free in sched_timeshare_consider_maintenance: at every context switch, quantum
//     expiry and interrupt wake-up of an idle CPU. While the system runs it fires within a context switch of each
//     kCpuWindowUs; a fully idle system has no ticks and needs none (its clock was already lowered, or is lowered at
//     the first wake-up after the hold).
//   * Decision (SwitchGovernor.h, host-tested): a window's busy share is the busiest CPU's, or the sum over all CPUs
//     capped at one CPU, whichever is larger (the cluster shares one clock, and a thread that migrated between CPUs in
//     the window shows up in the sum). At or above kCpuUpPermille: the top point at once; else the lowest point whose
//     kCpuTargetPermille share covers the work; down only after kCpuDownHoldUs of lower targets.
//   * Boosts (switch_perf_hint, SwitchPower.h): a touch (SwitchTouch's interrupt), a frame that took two or more
//     refresh periods (NativeLegacyDisplay's present stamps), an exec (XNU exec_activate_image), each holding the top
//     point for a while, applied at once (the owner applies the top while any boost runs; the tick folds the boosts in
//     at its next window); and the boot itself, held at the top from the governor's start (as soon as the DFLL is up)
//     until SpringBoard's System Boot Complete (XNU IOPMrootDomain, SWITCH_PERF_HINT_BOOT_DONE) or kBootFallbackUs of
//     uptime, whichever is first. The GPU's idle clock is held off for that window too (the idle tick below), so the
//     boot runs both at their full clocks. After it the governor's own rule holds the top while the post-boot load
//     saturates the CPU (11-05 r8a: 100 % residency at the ceiling with 0 down-steps from the floor's end at 60 s to
//     158 s, busy mean 86-100 %), and lowers it once that load ends.
//   * Thermal: SwitchThermal's DFLL floor/cap indices (L4T's T210B01 trips) become the governor's lowest/highest
//     points (limits()), applied at once, boot window included (one line if they clip it). The DFLL's own voltage cap
//     stays in force underneath in hardware.
//   * iPadOS's thermal ceiling: thermalmonitord (CLTM) caps the CPU with the DVD factor it writes on ApplePMGR
//     (kits/qemu/ShengPlatformPower.cpp hands it to switch_perf_cltm_ceiling, SwitchPower.h): the highest point is
//     also at or below that share of the top clock (SwitchGovernor.h switch_gov_index_at_fraction), whichever ceiling
//     is lower. Applied at once, from thermalmonitord's thread; kept across a governor restart.
// The callouts and hints only decide; when the clock must move they wake SwitchThermal's work loop (one software
// interrupt source per CPU), which owns the DFLL and calls take()/applied(). No callout and no hint takes a lock: who
// writes what is in SwitchPerfGovernorState.h.
//
// Points: the DFLL's own OPP table (L4T cvb.c from this chip's CPUB01_CVB_TABLE row, via tegra_dfll_cpu_opps), from the
// highest row at the CVB Vmin (918 MHz at 620 mV on speedo 1659; the rows below share its voltage and would run on the
// DFLL's pulse skipper) to Horizon's boost clock, 1785 MHz, never above (TegraDFLL refuses more).
//
// Periodic line (os_log only, every kLogWindowUs while the system runs): residency per point, steps, boosts, the
// window's busy share, and the DFLL's target and measured rate, to check kprof's probe ("core MHz") against.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include "SwitchEMC.h"
#include "SwitchPerfGovernorLine.h"
#include "SwitchPerfGovernorState.h"
#include "SwitchPower.h"
#include "../NativeSwitchBlock/SwitchCryptex.h"  // switch_boot_io_complete
#include "../TegraGM20B/TegraGM20BGpu.h"         // tegra_gm20b_perf_hint, tegra_gm20b_idle_tick: the GPU's idle clock

namespace {
// Tunables, in microseconds (test/switch_governor_test.cpp reads these lines).
constexpr uint32_t kCpuWindowUs = 8000;       // two windows per 60 Hz frame
constexpr uint32_t kCpuUpPermille = 900;      // ondemand's up_threshold is 80-95 %
constexpr uint32_t kCpuTargetPermille = 750;  // schedutil aims at 80 % (util x 1.25)
constexpr uint32_t kCpuDownHoldUs = 80000;    // five frames
constexpr uint32_t kBoostTouchUs = 600000;    // refreshed by every touch event while a finger moves
constexpr uint32_t kBoostFrameUs = 1000000;   // after a frame that took two refresh periods or more
constexpr uint32_t kBoostLaunchUs = 1500000;  // after an exec
constexpr uint64_t kLogWindowUs = 10000000;   // the periodic line, as kprof's window

uint64_t absUs(uint64_t us)
{
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(us * 1000, &abs);
    return abs;
}

// The tick's own state. XNU runs deadline_passed on one CPU at a time: the CPU whose cmpxchg took
// sched_perfcontrol_callback_deadline to 0 (sched_timeshare_consider_maintenance, both trees), until deadlinePassed
// re-arms it, last. That cmpxchg and the re-arm's exchange are relaxed, so the tick's fences hand this state from one
// CPU to the next.
struct Tick {
    uint32_t generation;  // gGeneration this state was made for
    switch_gov gov;
    uint64_t prevTotal[switchperf::kCpus], prevNow;
    uint32_t limits;  // gLimits as last folded in
    uint64_t logDue;
    switchperf::LineStats stats;
};

Tick gTick;

void restartTick(const switchperf::Config &config, uint32_t generation, uint64_t now)
{
    gTick.generation = generation;
    switch_gov_init(&gTick.gov, config.khz, config.count, config.start, config.tunables, now);
    for (unsigned c = 0; c < switchperf::kCpus; ++c)
        gTick.prevTotal[c] = switch_cpu_load_busy(&switchperf::gLoad[c], now);
    gTick.prevNow = now;
    gTick.limits = switchperf::packLimits(0, config.count - 1);
    gTick.logDue = now + config.logWindow;
    gTick.stats = {};
    gTick.stats.since = now;
}

// The scheduler's context-switch callout (interrupts masked, the switching CPU's own slot only).
void csw(perfcontrol_event event, uint32_t cpu, uint64_t timestamp, uint32_t, struct perfcontrol_thread_data *,
         struct perfcontrol_thread_data *, struct perfcontrol_cpu_counters *, switchperf::CswTimeout)
{
    if (cpu < switchperf::kCpus)
        switch_cpu_load_switch(&switchperf::gLoad[cpu], timestamp, event != IDLE);
}

// One window's busy share, in permille.
uint32_t windowBusy(const switchperf::Config &config, uint64_t now)
{
    const uint64_t span = now - gTick.prevNow;
    uint64_t sum = 0;
    uint64_t peak = 0;
    for (unsigned c = 0; c < config.cpus; ++c) {
        const uint64_t total = switch_cpu_load_busy(&switchperf::gLoad[c], now);
        const uint64_t done = total > gTick.prevTotal[c] ? total - gTick.prevTotal[c] : 0;
        gTick.prevTotal[c] = total;
        uint64_t permille = span ? done * 1000 / span : 0;
        if (permille > 1000)
            permille = 1000;
        sum += permille;
        if (permille > peak)
            peak = permille;
    }
    gTick.prevNow = now;
    if (sum > 1000)
        sum = 1000;
    return static_cast<uint32_t>(peak > sum ? peak : sum);
}

// One window: the limits and boosts the other sides posted, folded in; the step; the decision published. Returns
// whether the owner must be woken (the clock moves, or the periodic line is due); `boot` whether the boot window runs.
bool step(const switchperf::Config &config, uint64_t now, bool *boot)
{
    uint32_t busy = windowBusy(config, now);
    const uint32_t limits = __atomic_load_n(&switchperf::gLimits, __ATOMIC_RELAXED);
    if (limits != gTick.limits) {
        gTick.limits = limits;
        switch_gov_limit(&gTick.gov, switchperf::limitsLo(limits), switchperf::limitsHi(limits), now);
    }
    const uint64_t until = switchperf::boostUntil();
    if (until > now)
        switch_gov_boost(&gTick.gov, gTick.gov.count - 1, until, now);
    switchperf::lineWindow(&gTick.stats, busy);
    *boot = now < __atomic_load_n(&switchperf::gBootUntil, __ATOMIC_RELAXED);
    if (*boot)
        busy = 1000;  // the boot runs at the top point (the ceiling: thermal limits still apply)
    switch_gov_step(&gTick.gov, now, busy);
    __atomic_store_n(&switchperf::gDecided, gTick.gov.cur | gTick.generation << 8, __ATOMIC_RELEASE);
    bool line = false;
    if (now >= gTick.logDue) {
        line = true;
        gTick.logDue = now + config.logWindow;
        switchperf::linePublish(&gTick.stats, &gTick.gov, now);
    }
    return line || gTick.gov.cur != __atomic_load_n(&switchperf::gApplied, __ATOMIC_RELAXED);
}

// XNU's perfcontrol deadline: the window's tick (interrupts masked; context-switch, quantum or idle-wake context).
void deadlinePassed(uint64_t)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&switchperf::gRunning, __ATOMIC_ACQUIRE))
        return;
    const uint32_t generation = __atomic_load_n(&switchperf::gGeneration, __ATOMIC_ACQUIRE);
    const switchperf::Config &config = switchperf::gConfig[generation & 1];
    const uint64_t now = mach_absolute_time();
    if (gTick.generation != generation)
        restartTick(config, generation, now);
    bool boot = false;
    if (step(config, now, &boot)) {
        switchperf::gSignal(switchperf::gCtx, static_cast<unsigned>(cpu_number()));
        switchperf::lineWake(&gTick.stats);
    }
    // The GPU's idle clock takes this tick too (loads; a work item once it is idle), from the end of the boot window:
    // until then the GPU never leaves its full clock.
    if (!boot)
        tegra_gm20b_idle_tick();
    switch_emc_tick(now);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    sched_perfcontrol_update_callback_deadline(now + config.window);
}

struct sched_perfcontrol_callbacks gCallbacks = {
    .version = SCHED_PERFCONTROL_CALLBACKS_VERSION_5,
    .deadline_passed = deadlinePassed,
    .csw = csw,
};

// A boost source's own deadline, from its one writer with interrupts masked: the owner is woken when the boost starts
// (it applies the top at once) or while the clock it applied is below the ceiling. The deadline is a time and publishes
// nothing else; the owner reads it after interruptOccurred's work-loop lock, the tick at its next window.
void boost(switchperf::Boost *source, uint64_t until, uint64_t now, unsigned cpu)
{
    const uint64_t was = __atomic_load_n(&source->until, __ATOMIC_RELAXED);
    __atomic_store_n(&source->until, until, __ATOMIC_RELAXED);
    const unsigned ceiling = switchperf::limitsHi(__atomic_load_n(&switchperf::gLimits, __ATOMIC_RELAXED));
    const bool wake = was <= now || __atomic_load_n(&switchperf::gApplied, __ATOMIC_RELAXED) < ceiling;
    if (wake)
        switchperf::gSignal(switchperf::gCtx, cpu);
#if DEVELOPMENT || DEBUG
    __atomic_store_n(&source->hints, source->hints + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&source->wakes, source->wakes + (wake ? 1 : 0), __ATOMIC_RELAXED);
#endif
}
}  // namespace

namespace switchperf {
Config gConfig[2];
uint32_t gGeneration;
bool gRunning;
Signal gSignal;
void *gCtx;
switch_cpu_load gLoad[kCpus];
Boost gTouch, gFrame, gLaunch[kCpus];
uint32_t gApplied, gAppliedKhz;
uint32_t gDecided;
#if DEVELOPMENT || DEBUG
uint32_t gBootHints;
#endif

uint64_t boostUntil()
{
    uint64_t until = __atomic_load_n(&gTouch.until, __ATOMIC_RELAXED);
    const uint64_t frame = __atomic_load_n(&gFrame.until, __ATOMIC_RELAXED);
    if (frame > until)
        until = frame;
    for (const Boost &slot : gLaunch) {
        const uint64_t launch = __atomic_load_n(&slot.until, __ATOMIC_RELAXED);
        if (launch > until)
            until = launch;
    }
    return until;
}

unsigned indexNow(uint64_t now)
{
    const uint32_t generation = __atomic_load_n(&gGeneration, __ATOMIC_ACQUIRE);
    const Config &in = gConfig[generation & 1];
    const uint32_t decided = __atomic_load_n(&gDecided, __ATOMIC_ACQUIRE);
    unsigned index = (decided >> 8) == (generation & 0xffffffu) ? decided & 0xffu : in.start;
    if (boostUntil() > now)
        index = in.count - 1;
    const uint32_t limits = __atomic_load_n(&gLimits, __ATOMIC_ACQUIRE);
    const unsigned hi = limitsHi(limits);
    const unsigned lo = limitsLo(limits) > hi ? hi : limitsLo(limits);
    if (index > hi)
        index = hi;
    if (index < lo)
        index = lo;
    return index;
}

void signalHere()
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    gSignal(gCtx, static_cast<unsigned>(cpu_number()));
    ml_set_interrupts_enabled(enabled);
}
}  // namespace switchperf

bool switchperf::start(const uint32_t *khz, const uint32_t *mv, unsigned count, unsigned startIndex, Signal signal,
                       void *ctx, char *why, size_t len)
{
    if (__atomic_load_n(&gRunning, __ATOMIC_ACQUIRE)) {
        snprintf(why, len, "already running");
        return false;
    }
    if (!count || !khz || !mv || !signal) {
        snprintf(why, len, "no operating points");
        return false;
    }
    IOLock *lock = __atomic_load_n(&gThreadLock, __ATOMIC_SEQ_CST);
    if (!lock)
        lock = IOLockAlloc();
    if (!lock) {
        snprintf(why, len, "no lock");
        return false;
    }
    if (count > SWITCH_GOV_MAX_OPPS)
        count = SWITCH_GOV_MAX_OPPS;
    const uint64_t now = mach_absolute_time();
    const uint32_t generation = gGeneration + 1;
    Config &next = gConfig[generation & 1];
    for (unsigned i = 0; i < SWITCH_GOV_MAX_OPPS; ++i) {
        next.khz[i] = i < count ? khz[i] : 0;
        next.mv[i] = i < count ? mv[i] : 0;
    }
    next.count = count;
    next.start = startIndex < count ? startIndex : count - 1;
    next.cpus = ml_get_cpu_count();
    if (next.cpus > kCpus)
        next.cpus = kCpus;
    if (!next.cpus)
        next.cpus = 1;
    next.tunables = {kCpuUpPermille, kCpuTargetPermille, absUs(kCpuDownHoldUs)};
    next.window = absUs(kCpuWindowUs);
    next.logWindow = absUs(kLogWindowUs);
    next.touchHold = absUs(kBoostTouchUs);
    next.frameHold = absUs(kBoostFrameUs);
    next.launchHold = absUs(kBoostLaunchUs);
    gSignal = signal;
    gCtx = ctx;
    __atomic_store_n(&gApplied, next.start, __ATOMIC_RELEASE);
    __atomic_store_n(&gAppliedKhz, next.khz[next.start], __ATOMIC_RELEASE);
    __atomic_store_n(&gGeneration, generation, __ATOMIC_RELEASE);
    startLimits(lock, now);
    __atomic_store_n(&gRunning, true, __ATOMIC_RELEASE);
    if (indexNow(now) != next.start)
        signalHere();  // a ceiling thermalmonitord set before this start: the clock moves down
    sched_perfcontrol_register_callbacks(&gCallbacks, 0);
    sched_perfcontrol_update_callback_deadline(now + next.window);
    switch_emc_retick();
    return true;
}

void switchperf::stop()
{
    if (!__atomic_load_n(&gRunning, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&gRunning, false, __ATOMIC_RELEASE);
    __atomic_store_n(&gAppliedKhz, 0u, __ATOMIC_RELEASE);
    sched_perfcontrol_update_callback_deadline(0);
    sched_perfcontrol_register_callbacks(nullptr, 0);  // XNU's defaults again
    switch_emc_retick();
}

bool switchperf::running()
{
    return __atomic_load_n(&gRunning, __ATOMIC_ACQUIRE);
}

uint32_t switchperf::khzAt(unsigned index)
{
    const Config &in = config();
    return index < in.count ? in.khz[index] : 0;
}

unsigned switchperf::take(char *line, size_t len)
{
    if (line && len)
        line[0] = 0;
    if (!running())
        return 0;
    const uint64_t now = mach_absolute_time();
    const bool fellBack = closeBootWindow(now);
    LineTaken taken = {};
    const bool logDue = lineTake(&taken);
    const unsigned index = indexNow(now);
    if (fellBack) {
        IOLog("SwitchPerf: boot done at %llu s of uptime (no System Boot Complete by then): the CPU governor (ceiling "
              "%u MHz) and the GPU's idle clock decide from here\n",
              static_cast<unsigned long long>(kBootFallbackSeconds), khzAt(ceiling()) / 1000);
    }
    if (logDue && line && len)
        lineFormat(taken, index, line, len);
    return index;
}

void switchperf::applied(unsigned index)
{
    const Config &in = config();
    if (index >= in.count)
        return;
    __atomic_store_n(&gApplied, index, __ATOMIC_RELEASE);
    __atomic_store_n(&gAppliedKhz, in.khz[index], __ATOMIC_RELEASE);
}

extern "C" void switch_perf_hint(unsigned int hint)
{
    tegra_gm20b_perf_hint(hint);  // the GPU back at its full clock if it idles low
    switch_emc_hint(hint);
    if (hint == SWITCH_PERF_HINT_BOOT_DONE) {
        switch_boot_io_complete();
        switchperf::bootDone();
        return;
    }
    if (!switchperf::running() || hint >= 5 || !hint)
        return;
    const switchperf::Config &in = switchperf::config();
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const auto cpu = static_cast<unsigned>(cpu_number());
    const uint64_t now = mach_absolute_time();
    if (hint == SWITCH_PERF_HINT_TOUCH) {
        boost(&switchperf::gTouch, now + in.touchHold, now, cpu);
    } else if (hint == SWITCH_PERF_HINT_FRAME_MISS) {
        boost(&switchperf::gFrame, now + in.frameHold, now, cpu);
    } else if (cpu < switchperf::kCpus) {
        boost(&switchperf::gLaunch[cpu], now + in.launchHold, now, cpu);
    }
    ml_set_interrupts_enabled(enabled);
}

extern "C" unsigned int switch_perf_cpu_khz(void)
{
    return __atomic_load_n(&switchperf::gAppliedKhz, __ATOMIC_ACQUIRE);
}

extern "C" bool switch_perf_boot_done(void)
{
    return __atomic_load_n(&switchperf::gBootDone, __ATOMIC_ACQUIRE);
}
