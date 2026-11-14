// SPDX-License-Identifier: BSD-2-Clause-Patent
// The memory controller's error interrupt, the one owner of MC_INTSTATUS, INTMASK and the capture registers (see
// TegraSMMU.h tegra_mc_errors_start). MC General Interrupt: global 77 (Tegra X1 TRM ch.3, p.35), INTID 109, level.
//
// The filter takes the error (TegraMCErrors.h) into a slot only it writes; IOKit keeps a level line masked from a
// filter that returns true until its action has run, so the action reads that slot with no other writer. The action
// prints the one line and publishes the error to readers in other drivers (GM20B's stats and hang lines, the engines'
// stall lines) under a spin lock: they read it from any context, and a published error is three words that must stay
// together.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "TegraSMMU.h"
#include "TegraMCErrors.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kIntid = 32 + 77;

struct Io {
    volatile uint8_t *mc;

    uint32_t read(uint32_t offset) const
    {
        return switch_mmio_read32(mc, offset);
    }

    void write(uint32_t offset, uint32_t value) const
    {
        switch_mmio_write32(mc, offset, value);
    }
};

Io gIo;
IOWorkLoop *gLoop;
IOFilterInterruptEventSource *gSource;
switch_mc::Error gTaken;  // the filter's

IOSimpleLock *gLatestLock;
switch_mc::Error gLatest;
uint32_t gCount;

// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool filter(OSObject *, IOFilterInterruptEventSource *)
{
    return switch_mc::take(gIo, &gTaken);
}

void occurred(OSObject *, IOInterruptEventSource *, int)
{
    const switch_mc::Error error = gTaken;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLatestLock);
    gLatest = error;
    ++gCount;
    IOSimpleLockUnlockEnableInterrupt(gLatestLock, state);
    char line[256];
    IOLog("TegraSMMU: MC error %s\n", switch_mc::describe(error, line, sizeof(line)));
}

void unmask()
{
    gIo.write(switch_mc::kIntMask, switch_mc::kErrors);
    (void)gIo.read(switch_mc::kIntMask);
}
}  // namespace

extern "C" int tegra_mc_errors_start(void)
{
    gIo.mc = switch_mmio_map(kMcPA, 0x1000);
    gLatestLock = IOSimpleLockAlloc();
    gLoop = IOWorkLoop::workLoop();
    IOService *nub = switch_interrupt_nub(kIntid, false);
    gSource = gIo.mc && gLatestLock && gLoop && nub
                  ? IOFilterInterruptEventSource::filterInterruptEventSource(gLoop, &occurred, &filter, nub, 0)
                  : nullptr;
    if (!gSource || gLoop->addEventSource(gSource) != kIOReturnSuccess) {
        IOLog(
            "TegraSMMU: MC error interrupt (INTID %u) could not be attached: memory controller errors go unreported\n",
            kIntid);
        return -1;
    }
    if (switch_mc::take(gIo, &gTaken))
        occurred(nullptr, nullptr, 0);
    gSource->enable();
    unmask();
    return 0;
}

void switch_mc_errors_resume(void)
{
    if (gSource)
        unmask();
}

extern "C" uint32_t tegra_mc_error_latest(uint32_t *intstatus, uint32_t *status, uint32_t *address)
{
    if (!gLatestLock) {
        *intstatus = *status = *address = 0;
        return 0;
    }
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLatestLock);
    const switch_mc::Error latest = gLatest;
    const uint32_t count = gCount;
    IOSimpleLockUnlockEnableInterrupt(gLatestLock, state);
    *intstatus = latest.intstatus;
    *status = latest.status;
    *address = latest.address;
    return count;
}
