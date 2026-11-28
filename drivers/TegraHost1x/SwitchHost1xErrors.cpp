// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's general interrupt (SwitchHost1xErrors.h). The filter takes the channels whose illegal opcode is pending
// (masked from then on, status cleared) for the action, on host1x's work loop. The action names each channel in one
// line and fails its jobs (switch_host1x_channels_failed), which only expires fences and settles them, as a bound
// does: no step there sleeps or waits on the loop it runs on. Debug kernels can expect one channel's error (the illegal
// opcode probe): the action then wakes the probe instead.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include "SwitchHost1xErrors.h"
#include "SwitchHost1xChannels.h"
#include "TegraHost1x.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#include "SwitchHost1xErrorProbe.h"
#endif

namespace {
using namespace switch_host1x_errors;

constexpr uint32_t kGeneralIntid = 32 + 67;  // "host" (tegra210.dtsi host1x@50000000)

struct ErrorRegisters {
    volatile uint8_t *sync;

    uint32_t read(uint32_t offset) const
    {
        return switch_mmio_read32(sync, offset);
    }

    void write(uint32_t offset, uint32_t value) const
    {
        switch_mmio_write32(sync, offset, value);
    }
};

ErrorRegisters gErrorRegisters;
Unit<ErrorRegisters> gErrorsUnit;
IOSimpleLock *gErrorsLock;  // gErrorsUnit and HINTMASK_EXT: the filter's take against a restart's rearm
IOFilterInterruptEventSource *gErrorsSource;
uint32_t gErrorsPending;  // the filter's channels, for the action

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
IOLock *gExpectLock;
uint32_t gExpectedChannel = kChannels;  // kChannels: none
uint32_t gExpectedTaken;

bool takeExpected(uint32_t channel)
{
    IOLockLock(gExpectLock);
    const bool expected = channel == gExpectedChannel;
    if (expected) {
        ++gExpectedTaken;
        IOLockWakeup(gExpectLock, &gExpectedTaken, false);
    }
    IOLockUnlock(gExpectLock);
    return expected;
}
#endif

// The line is level and only these masks are ours, so a fire with none of our opcodes pending is a source this driver
// set up wrong: turning it off would lose every later stop as surely as a lost interrupt.
[[noreturn]] void unclaimed()
{
    const uint32_t ext = gErrorRegisters.read(kHintStatusExt), maskExt = gErrorRegisters.read(kHintMaskExt),
                   masked = gErrorsUnit.masked, latchedMasked = ext & masked & kIllegalAll;
    if (latchedMasked && !(ext & ~masked & kIllegalAll))
        panic("TegraHost1x: general interrupt (INTID %u) fired for channels %#x, masked since their stop "
              "(HINTSTATUS_EXT %#x HINTMASK_EXT %#x HINTSTATUS %#x): HINTMASK_EXT does not keep a stopped channel off "
              "the line",
              kGeneralIntid, latchedMasked, ext, maskExt, gErrorRegisters.read(kHintStatus));
    panic("TegraHost1x: general interrupt (INTID %u) fired with no illegal opcode of ours pending (INTSTATUS %#x "
          "HINTSTATUS %#x HINTSTATUS_EXT %#x HINTMASK_EXT %#x INTMASK %#x)",
          kGeneralIntid, gErrorRegisters.read(kIntStatus), gErrorRegisters.read(kHintStatus), ext, maskExt,
          gErrorRegisters.read(kIntMask));
}

// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool errorsFilter(OSObject *, IOFilterInterruptEventSource *)
{
    IOSimpleLockLock(gErrorsLock);
    const uint32_t taken = gErrorsUnit.take(gErrorRegisters);
    if (!taken)
        unclaimed();
    IOSimpleLockUnlock(gErrorsLock);
    __atomic_fetch_or(&gErrorsPending, taken, __ATOMIC_RELEASE);
    return true;
}

void channelStopped(uint32_t channel)
{
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (takeExpected(channel))
        return;
#endif
    char host[200], syncpts[160];
    const uint32_t stat = gErrorRegisters.read(kCmdprocStat);
    tegra_host1x_describe_channel(channel, host, sizeof(host));
    switch_host1x_channels_failed(channel, syncpts, sizeof(syncpts));
    IOLog("TegraHost1x: channel %u stopped on an illegal opcode (CMDPROC_STAT %#x): %s; its jobs fail now (%s%s)\n",
          channel, stat, host, syncpts[0] ? "" : "no fenced job in flight", syncpts);
}

void errorsOccurred(OSObject *, IOInterruptEventSource *, int)
{
    for (uint32_t channels = __atomic_exchange_n(&gErrorsPending, 0, __ATOMIC_ACQUIRE); channels;
         channels &= channels - 1)
        channelStopped(static_cast<uint32_t>(__builtin_ctz(channels)));
}

[[noreturn]] void notAttached(const char *why)
{
    panic("TegraHost1x: general interrupt (INTID %u) could not be attached: %s", kGeneralIntid, why);
}

void errorsConfigure()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gErrorsLock);
    const Configured c = gErrorsUnit.configure(gErrorRegisters);
    IOSimpleLockUnlockEnableInterrupt(gErrorsLock, state);
    if (!(c.intmask & kCpu0All))
        panic("TegraHost1x: general interrupt (INTID %u) not enabled: INTMASK reads %#x after CPU0's enable was "
              "written",
              kGeneralIntid, c.intmask);
    if (!(c.intc0 & kHostInt))
        panic("TegraHost1x: general interrupt (INTID %u) not enabled: INTC0MASK reads %#x after HOST_INT was written",
              kGeneralIntid, c.intc0);
    if (c.hintmask != kExtEnable)
        panic("TegraHost1x: general interrupt (INTID %u) not enabled: HINTMASK reads %#x after %#x was written",
              kGeneralIntid, c.hintmask, kExtEnable);
    if (c.hintmaskExt != kIllegalAll)
        panic("TegraHost1x: general interrupt (INTID %u) not enabled: HINTMASK_EXT reads %#x after %#x was written",
              kGeneralIntid, c.hintmaskExt, kIllegalAll);
    if (c.intc0Was & ~kHostInt)
        IOLog("TegraHost1x: INTC0MASK read %#x: the engines' bits cleared (each has its own line), HOST_INT kept\n",
              c.intc0Was);
    if (c.latched)
        IOLog("TegraHost1x: illegal opcodes latched before the general interrupt was on: channels %#x (cleared)\n",
              c.latched);
}
}  // namespace

// In this translation unit, so the display's definition (SwitchHost1xDisplayChannel.h) replaces it at the static link.
#ifndef SWITCH_HOST1X_DISPLAY_CHANNEL_DEFINED
__attribute__((weak)) void switch_host1x_display_channel_stopped_locked()
{}
#endif

void switch_host1x_errors_start(IOWorkLoop *loop, volatile uint8_t *sync)
{
    gErrorRegisters.sync = sync;
    gErrorsLock = IOSimpleLockAlloc();
    if (!gErrorsLock)
        notAttached("no lock");
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    gExpectLock = IOLockAlloc();
    if (!gExpectLock)
        notAttached("no lock");
#endif
    IOService *nub = switch_interrupt_nub(kGeneralIntid, false);
    if (!nub)
        notAttached("no interrupt nub");
    IOFilterInterruptEventSource *made =
        IOFilterInterruptEventSource::filterInterruptEventSource(loop, &errorsOccurred, &errorsFilter, nub, 0);
    if (!made)
        notAttached("no event source");
    if (loop->addEventSource(made) != kIOReturnSuccess)
        notAttached("the work loop refused the event source");
    errorsConfigure();
    made->enable();
    __atomic_store_n(&gErrorsSource, made, __ATOMIC_RELEASE);
}

void switch_host1x_errors_stop()
{
    if (IOFilterInterruptEventSource *source = __atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE))
        source->disable();
}

void switch_host1x_errors_resume()
{
    if (IOFilterInterruptEventSource *source = __atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE)) {
        errorsConfigure();
        source->enable();
    }
}

extern "C" void switch_host1x_errors_rearm(uint32_t channel)
{
    if (!gErrorsLock || !__atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE))
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gErrorsLock);
    gErrorsUnit.rearm(gErrorRegisters, channel);
    IOSimpleLockUnlockEnableInterrupt(gErrorsLock, state);
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
bool switch_host1x_errors_attached()
{
    return __atomic_load_n(&gErrorsSource, __ATOMIC_ACQUIRE) != nullptr;
}

void switch_host1x_errors_expect(uint32_t channel)
{
    IOLockLock(gExpectLock);
    gExpectedChannel = channel;
    gExpectedTaken = 0;
    IOLockUnlock(gExpectLock);
}

uint32_t switch_host1x_errors_expect_end(uint32_t ms)
{
    const uint32_t channel = gExpectedChannel, bit = 1u << channel;
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    IOLockLock(gExpectLock);
    while (!gExpectedTaken &&
           IOLockSleepDeadline(gExpectLock, &gExpectedTaken, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    const uint32_t taken = gExpectedTaken;
    gExpectedChannel = kChannels;
    IOLockUnlock(gExpectLock);
    const uint32_t pending = gErrorRegisters.read(kHintStatusExt) & bit,
                   stat = gErrorRegisters.read(kCmdprocStat) & bit;
    if (!taken && (pending || stat))
        panic("TegraHost1x: general interrupt (INTID %u) lost: channel %u's illegal opcode latched (HINTSTATUS_EXT %#x "
              "CMDPROC_STAT %#x) and not delivered in %u ms",
              kGeneralIntid, channel, gErrorRegisters.read(kHintStatusExt), gErrorRegisters.read(kCmdprocStat), ms);
    return taken;
}

uint32_t switch_host1x_errors_stat()
{
    return gErrorRegisters.read(kCmdprocStat);
}

SwitchHost1xErrorLine switch_host1x_errors_line()
{
    return {gErrorRegisters.read(kIntMask), gErrorRegisters.read(kHintStatus), gErrorRegisters.read(kHintStatusExt),
            gErrorRegisters.read(kHintMaskExt)};
}
#endif
