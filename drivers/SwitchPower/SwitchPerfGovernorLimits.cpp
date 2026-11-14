// SPDX-License-Identifier: BSD-2-Clause-Patent
// The CPU governor's thread side: SwitchThermal's DFLL floor and caps (its work loop), thermalmonitord's ceiling (its
// own thread) and the boot window (System Boot Complete on IOPMrootDomain's thread, or the owner's fallback). They
// share gThreadLock, which no callout and no interrupt filter takes, and publish what the callouts need as one limits
// word (gLimits) and one deadline (gBootUntil).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "SwitchPerfGovernorState.h"
#include "SwitchPower.h"

namespace {
constexpr uint64_t kBootFallbackUs = switchperf::kBootFallbackSeconds * 1000000ULL;  // uptime (SwitchPerfGovernor.h)

uint32_t gFloorMv, gCapMv;  // the DFLL's thermal floor and cap last given to limits() (0: none)
uint32_t gCltm = 0x10000u;  // thermalmonitord's CPU ceiling, a 16.16 share of the top clock (atomic: set before start)
bool gClipNoted;            // a thermal limit clipped the boot window: one line

struct Clip {
    uint32_t khz, capMv, cltm;
    uint64_t at;
};

uint64_t absUs(uint64_t us)
{
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(us * 1000, &abs);
    return abs;
}

// Under gThreadLock: the limits from the DFLL's thermal floor/cap and thermalmonitord's ceiling, published in one
// store. Returns the boot window's clip to log (khz 0: none).
Clip publishLimits(uint64_t now)
{
    const switchperf::Config &in = switchperf::config();
    const unsigned lo = switch_gov_index_at_mv(in.mv, in.count, gFloorMv);
    const unsigned top = in.count - 1;
    unsigned hi = gCapMv ? switch_gov_index_at_mv(in.mv, in.count, gCapMv) : top;
    const bool capped = hi < top;  // the DFLL's cap binds (with none in force it reports the LUT's top voltage)
    const uint32_t share = __atomic_load_n(&gCltm, __ATOMIC_RELAXED);
    const unsigned cltm = switch_gov_index_at_fraction(in.khz, in.count, share);
    if (cltm < hi)
        hi = cltm;
    Clip clip = {};
    if (hi < top && now < __atomic_load_n(&switchperf::gBootUntil, __ATOMIC_RELAXED) && !gClipNoted) {
        gClipNoted = true;
        clip = {in.khz[hi], capped ? gCapMv : 0, share, now};
    }
    // A cap below the floor keeps the cap, as the DFLL's own voltage cap does.
    __atomic_store_n(&switchperf::gLimits, switchperf::packLimits(lo > hi ? hi : lo, hi), __ATOMIC_RELEASE);
    return clip;
}

// Outside the lock, thread context: the boot window's one thermal line, if it was clipped.
void noteClip(const Clip &clip)
{
    if (!clip.khz)
        return;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(clip.at, &ns);
    char cap[16] = "none";
    if (clip.capMv)
        snprintf(cap, sizeof(cap), "%u mV", clip.capMv);
    IOLog("SwitchPerf: thermal limits clip the boot window at %llu.%llu s of uptime: CPU ceiling %u MHz (DFLL voltage "
          "cap %s, thermalmonitord %u%%)\n",
          ns / 1000000000, ns / 100000000 % 10, clip.khz / 1000, cap,
          clip.cltm >= 0x10000u ? 100u : static_cast<unsigned>(clip.cltm * 100ULL >> 16));
}

// Outside the lock: the owner woken if what it would apply now is not what it runs.
void wakeIfMoved(uint64_t now)
{
    if (switchperf::indexNow(now) != __atomic_load_n(&switchperf::gApplied, __ATOMIC_ACQUIRE))
        switchperf::signalHere();
}
}  // namespace

namespace switchperf {
IOLock *gThreadLock;
uint32_t gLimits;
uint64_t gBootUntil;
bool gBootDone;
}  // namespace switchperf

// start(), before it sets gRunning. gThreadLock and gBootDone are stored and loaded sequentially consistent here and in
// bootDone() and switch_perf_cltm_ceiling(), which may run before the lock exists: one side always sees the other's store.
void switchperf::startLimits(IOLock *lock, uint64_t now)
{
    __atomic_store_n(&gThreadLock, lock, __ATOMIC_SEQ_CST);
    IOLockLock(lock);
    gFloorMv = gCapMv = 0;
    // The boot window, once per boot: up to System Boot Complete, at most to kBootFallbackUs of uptime.
    __atomic_store_n(&gBootUntil, __atomic_load_n(&gBootDone, __ATOMIC_SEQ_CST) ? 0 : absUs(kBootFallbackUs),
                     __ATOMIC_RELAXED);
    const Clip clip = publishLimits(now);
    IOLockUnlock(lock);
    noteClip(clip);
}

// The owner: no System Boot Complete by the fallback, so the window closes here. Once per boot, against bootDone():
// the exchange picks the one that logs.
bool switchperf::closeBootWindow(uint64_t now)
{
    const uint64_t until = __atomic_load_n(&gBootUntil, __ATOMIC_RELAXED);
    if (!until || now < until)
        return false;
    IOLockLock(gThreadLock);
    __atomic_store_n(&gBootUntil, 0, __ATOMIC_RELAXED);
    IOLockUnlock(gThreadLock);
    return !__atomic_exchange_n(&gBootDone, true, __ATOMIC_SEQ_CST);
}

void switchperf::limits(unsigned floorMv, unsigned capMv)
{
    if (!running())
        return;
    const uint64_t now = mach_absolute_time();
    IOLockLock(gThreadLock);
    gFloorMv = floorMv;
    gCapMv = capMv;
    const Clip clip = publishLimits(now);
    IOLockUnlock(gThreadLock);
    wakeIfMoved(now);
    noteClip(clip);
}

unsigned switchperf::ceiling()
{
    return limitsHi(__atomic_load_n(&gLimits, __ATOMIC_ACQUIRE));
}

extern "C" unsigned int switch_perf_cltm_ceiling(uint32_t fraction16)
{
    if (fraction16 > 0x10000u)
        fraction16 = 0x10000u;
    __atomic_store_n(&gCltm, fraction16, __ATOMIC_SEQ_CST);
    IOLock *lock = __atomic_load_n(&switchperf::gThreadLock, __ATOMIC_SEQ_CST);
    if (!lock || !switchperf::running())
        return 0;
    const uint64_t now = mach_absolute_time();
    IOLockLock(lock);
    const Clip clip = publishLimits(now);
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): the ceiling is an index of the table in force
    const unsigned khz = switchperf::config().khz[switchperf::ceiling()];
    IOLockUnlock(lock);
    wakeIfMoved(now);
    noteClip(clip);
    return khz;
}

// System Boot Complete (XNU IOPMrootDomain, its thread): the boot window ends, once per boot, with one line.
void switchperf::bootDone()
{
    if (__atomic_exchange_n(&gBootDone, true, __ATOMIC_SEQ_CST))
        return;  // the fallback came first
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    unsigned khz = 0;
    if (IOLock *lock = __atomic_load_n(&gThreadLock, __ATOMIC_SEQ_CST)) {
        IOLockLock(lock);
        __atomic_store_n(&gBootUntil, 0, __ATOMIC_RELAXED);
#if DEVELOPMENT || DEBUG
        __atomic_store_n(&gBootHints, gBootHints + 1, __ATOMIC_RELAXED);
#endif
        if (running())
            khz = khzAt(ceiling());
        IOLockUnlock(lock);
    }
    char cpu[48] = "not governed";
    if (khz)
        snprintf(cpu, sizeof(cpu), "ceiling %u MHz", khz / 1000);
    IOLog("SwitchPerf: boot done at %llu.%llu s of uptime (System Boot Complete): the CPU governor (%s) and the GPU's "
          "idle clock decide from here\n",
          ns / 1000000000, ns / 100000000 % 10, cpu);
}
