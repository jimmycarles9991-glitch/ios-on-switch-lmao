// SPDX-License-Identifier: BSD-2-Clause-Patent
// The PMU monotonic clock J120 publishes (AppleD2400PMU's AppleDialogPMURTC), with its sysctls as Apple names and types
// them: kern.monotonicclock (seconds, IU), kern.monotonicclock_usecs ({usec, mach_absolute_time} latched together, Q),
// kern.monotonicclock_rate_usecs (the clock's tick, Q) and kern.monotoniclock_offset_usecs (calendar = monotonic +
// offset, Q; Apple's spelling). XNU reads _usecs at calendar init and at every wake to measure the time asleep
// (clock.c, ENABLE_LEGACY_CLOCK_CODE) and panics if it fails after it once worked; locationd, timed and
// BackgroundTaskAgent read the others.
//
// The source is the Tegra RTC at the start of the PMC page (Linux rtc-tegra.c): a millisecond counter in the always-on
// partition that keeps counting through SC7 and that nothing writes. Reading MILLI_SECONDS latches SECONDS into
// SHADOW_SECONDS, so the two reads are one critical section. Its registers need the APB clock (CAR L4), checked before
// every read so a read never fails once the clock is registered.
#include "SwitchMonotonicClock.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchSysctl/SwitchSysctl.h"

namespace {
enum : uint32_t {
    RTC_SHADOW_SECONDS = 0x00c,
    RTC_MILLI_SECONDS = 0x010,
};

constexpr unsigned kRtcClock = 4;  // TEGRA210_CLK_RTC: CAR L bit 4
constexpr uint64_t kRateUsec = 1000;
constexpr uint64_t kUsecPerSec = 1000000;

struct LatchedTime {
    uint64_t monotonicUsec;
    uint64_t machTime;
};

volatile uint8_t *gRtc;
IOSimpleLock *gLatch;
// Added to every reading when the RTC read below mach time at start: XNU subtracts mach time from the monotonic clock
// in unsigned arithmetic, and a constant cancels in its sleep arithmetic.
uint64_t gBaseUsec;

LatchedTime latch()
{
    if (!switch_car_clock_enabled(kRtcClock))
        switch_car_clock_enable(kRtcClock);
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLatch);
    const uint32_t millis = *reinterpret_cast<volatile uint32_t *>(gRtc + RTC_MILLI_SECONDS);
    const uint32_t seconds = *reinterpret_cast<volatile uint32_t *>(gRtc + RTC_SHADOW_SECONDS);
    const uint64_t now = mach_absolute_time();
    IOSimpleLockUnlockEnableInterrupt(gLatch, state);
    return {((uint64_t)seconds * 1000 + millis) * 1000 + gBaseUsec, now};
}

bool calendarOffsetUsec(const LatchedTime &t, int64_t *offset)
{
    uint64_t utc = 0;
    if (switch_rtc_utc_usec(&utc) != kIOReturnSuccess)
        return false;
    *offset = (int64_t)(utc - t.monotonicUsec);
    return true;
}

int monotonicSeconds SYSCTL_HANDLER_ARGS
{
    const uint32_t seconds = (uint32_t)(latch().monotonicUsec / kUsecPerSec);
    return SYSCTL_OUT(req, &seconds, sizeof(seconds));
}

// Apple's handler hands back as much of the pair as the caller has room for.
int monotonicUsecs SYSCTL_HANDLER_ARGS
{
    const LatchedTime t = latch();
    size_t length = sizeof(t);
    if (req->oldptr && req->oldlen < length)
        length = req->oldlen;
    return SYSCTL_OUT(req, &t, length);
}

int monotonicRate SYSCTL_HANDLER_ARGS
{
    const uint64_t rate = kRateUsec;
    return SYSCTL_OUT(req, &rate, sizeof(rate));
}

int monotonicOffset SYSCTL_HANDLER_ARGS
{
    int64_t offset = 0;
    (void)calendarOffsetUsec(latch(), &offset);
    return SYSCTL_OUT(req, &offset, sizeof(offset));
}

struct sysctl_oid gOids[4];
}  // namespace

extern "C" bool switch_monotonic_clock_start(void)
{
    if (gRtc)
        return true;
    if (switch_car_reset_asserted(kRtcClock)) {
        IOLog("SwitchMonotonicClock: the Tegra RTC is held in reset; no monotonic clock, sleep is timed by the UTC "
              "RTC\n");
        return false;
    }
    volatile uint8_t *rtc = switch_mmio_map(SWITCH_PMC_PA & ~0xfffULL, 0x1000);
    IOSimpleLock *lock = IOSimpleLockAlloc();
    if (!rtc || !lock) {
        if (lock)
            IOSimpleLockFree(lock);
        IOLog("SwitchMonotonicClock: cannot map the Tegra RTC; no monotonic clock\n");
        return false;
    }
    gLatch = lock;
    gRtc = rtc;
    const LatchedTime first = latch();
    uint64_t machNsec = 0;
    absolutetime_to_nanoseconds(first.machTime, &machNsec);
    if (first.monotonicUsec < machNsec / 1000)
        gBaseUsec = machNsec / 1000 - first.monotonicUsec + kUsecPerSec;

    constexpr unsigned kRead = CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED;
    switch_sysctl_oid(&gOids[0], &sysctl__kern_children, "monotonicclock", static_cast<int>(CTLTYPE_INT | kRead),
                      monotonicSeconds, "IU", "elapsed time");
    switch_sysctl_oid(&gOids[1], &sysctl__kern_children, "monotonicclock_usecs", static_cast<int>(CTLTYPE_QUAD | kRead),
                      monotonicUsecs, "Q", "elapsed time");
    switch_sysctl_oid(&gOids[2], &sysctl__kern_children, "monotonicclock_rate_usecs",
                      static_cast<int>(CTLTYPE_QUAD | kRead), monotonicRate, "Q", "elapsed time rate");
    switch_sysctl_oid(&gOids[3], &sysctl__kern_children, "monotoniclock_offset_usecs",
                      static_cast<int>(CTLTYPE_QUAD | kRead), monotonicOffset, "Q", "calendar time offset");
    for (struct sysctl_oid &oid : gOids)
        sysctl_register_oid(&oid);
#if DEVELOPMENT || DEBUG
    IOLog("SwitchMonotonicClock: Tegra RTC at %llu ms%s\n", first.monotonicUsec / 1000,
          gBaseUsec ? " (behind the arm timer, offset added)" : "");
#endif
    return true;
}

extern "C" bool switch_monotonic_clock_read(uint64_t *usec, uint64_t *mach_time)
{
    if (!gRtc)
        return false;
    const LatchedTime t = latch();
    *usec = t.monotonicUsec;
    *mach_time = t.machTime;
    return true;
}

extern "C" bool switch_monotonic_clock_offset(int64_t *usec)
{
    return gRtc && calendarOffsetUsec(latch(), usec);
}
