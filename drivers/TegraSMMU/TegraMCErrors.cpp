// SPDX-License-Identifier: BSD-2-Clause-Patent
// The memory controller's error interrupt, the one owner of MC_INTSTATUS, INTMASK and the capture registers (see
// TegraSMMU.h tegra_mc_errors_start). MC General Interrupt: global 77 (Tegra X1 TRM ch.3, p.35), INTID 109, level.
//
// The filter takes the error (TegraMCErrors.h) into a slot only it writes; IOKit keeps a level line masked from a
// filter that returns true until its action has run, so the action reads that slot with no other writer. The action
// prints the line (the first Runs::kFullLines of a client's run, then one count line when the run ends) and publishes
// the error to readers in other drivers (GM20B's stats and hang lines, the engines' stall lines) under a spin lock: they
// read it from any context, and a published error is three words that must stay together. The runs are the work
// loop's: a detach or SC7 ends them through its gate.
//
// The start returns at once: its own thread waits for the GIC, attaches, and takes what latched meanwhile, so nothing
// later in the board thread's order waits on the GIC for it.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread.h>
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

// The ERR_ID clients each translated swgroup's detach ends the runs of (TRM 18.11.1 client table, pp.846-850).
struct Swgroup {
    uint32_t asidReg;
    uint8_t clients[6];
    unsigned count;
};

constexpr Swgroup kSwgroups[] = {
    {TEGRA_SMMU_DC_ASID, {0x01, 0x03, 0x05, 0x10, 0x5a, 0x73}, 6},
    {TEGRA_SMMU_VIC_ASID, {0x6c, 0x6d}, 2},
    {TEGRA_SMMU_XUSB_HOST_ASID, {0x4a, 0x4b}, 2},
    {TEGRA_SMMU_NVJPG_ASID, {0x7e, 0x7f}, 2},
    {TEGRA_SMMU_NVDEC_ASID, {0x78, 0x79}, 2},
    {TEGRA_SMMU_NVENC_ASID, {0x1c, 0x2b}, 2},
    {TEGRA_SMMU_GPU_ASID, {0x58, 0x59, 0x88, 0x89}, 4},
};

Io gIo;
IOWorkLoop *gLoop;
IOFilterInterruptEventSource *gSource;  // published by the attach thread once it is enabled
switch_mc::Error gTaken;                // the filter's
switch_mc::Runs gRuns;                  // the work loop's

IOSimpleLock *gLatestLock;
switch_mc::Error gLatest;
uint32_t gCount;

// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool filter(OSObject *, IOFilterInterruptEventSource *)
{
    return switch_mc::take(gIo, &gTaken);
}

void printEnded(const switch_mc::Runs::Ended *ended, unsigned count)
{
    char line[512];
    for (unsigned i = 0; i < count; ++i)
        IOLog("TegraSMMU: MC error run over: %s\n", switch_mc::describeRun(ended[i], line, sizeof(line)));
}

void occurred(OSObject *, IOInterruptEventSource *, int)
{
    const switch_mc::Error error = gTaken;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLatestLock);
    gLatest = error;
    ++gCount;
    IOSimpleLockUnlockEnableInterrupt(gLatestLock, state);
    switch_mc::Runs::Ended ended[switch_mc::Runs::kOpen];
    unsigned count = 0;
    const bool full = gRuns.error(error, ended, count);
    printEnded(ended, count);
    char line[256];
    if (full)
        IOLog("TegraSMMU: MC error %s\n", switch_mc::describe(error, line, sizeof(line)));
}

IOReturn takeLatched(OSObject *, void *, void *, void *, void *)
{
    if (switch_mc::take(gIo, &gTaken))
        occurred(nullptr, nullptr, 0);
    return kIOReturnSuccess;
}

// NOLINTNEXTLINE(misc-const-correctness): IOWorkLoop::Action's signature
IOReturn endRuns(OSObject *, void *swgroup, void *, void *, void *)
{
    const Swgroup *group = static_cast<const Swgroup *>(swgroup);
    switch_mc::Runs::Ended ended[switch_mc::Runs::kOpen];
    const unsigned count = gRuns.endWhere(
        [group](uint32_t client) {
            if (!group)
                return true;
            for (unsigned i = 0; i < group->count; ++i) {
                if (group->clients[i] == client)
                    return true;
            }
            return false;
        },
        group ? switch_mc::Runs::Cause::kDetach : switch_mc::Runs::Cause::kSC7, ended);
    printEnded(ended, count);
    return kIOReturnSuccess;
}

void unmask()
{
    gIo.write(switch_mc::kIntMask, switch_mc::kErrors);
    (void)gIo.read(switch_mc::kIntMask);
}

void notAttached()
{
    IOLog("TegraSMMU: MC error interrupt (INTID %u) could not be attached: memory controller errors go unreported\n",
          kIntid);
}

IOFilterInterruptEventSource *source()
{
    return __atomic_load_n(&gSource, __ATOMIC_ACQUIRE);
}

// Its own thread: switch_interrupt_nub sleeps until ArmCpu has the GIC up. What the MC latched before is taken first.
void attach(void *, wait_result_t)
{
    IOService *nub = switch_interrupt_nub(kIntid, false);
    IOFilterInterruptEventSource *made =
        nub ? IOFilterInterruptEventSource::filterInterruptEventSource(gLoop, &occurred, &filter, nub, 0) : nullptr;
    if (!made || gLoop->addEventSource(made) != kIOReturnSuccess) {
        notAttached();
    } else {
        gLoop->runAction(&takeLatched, nullptr);
        made->enable();
        unmask();
        __atomic_store_n(&gSource, made, __ATOMIC_RELEASE);
    }
}  // returning ends the thread (Call_continuation -> thread_terminate)
}  // namespace

extern "C" int tegra_mc_errors_start(void)
{
    gIo.mc = switch_mmio_map(kMcPA, 0x1000);
    gLatestLock = IOSimpleLockAlloc();
    gLoop = IOWorkLoop::workLoop();
    thread_t thread = nullptr;
    if (!gIo.mc || !gLatestLock || !gLoop || kernel_thread_start(&attach, nullptr, &thread) != KERN_SUCCESS) {
        notAttached();
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}

void switch_mc_errors_resume(void)
{
    if (!source())
        return;
    gLoop->runAction(&endRuns, nullptr);
    unmask();
}

extern "C" void switch_mc_errors_detached(uint32_t asidReg)
{
    if (!source())
        return;
    for (const Swgroup &group : kSwgroups) {
        if (group.asidReg == asidReg) {
            Swgroup detached = group;  // runAction takes a mutable argument and has run the action when it returns
            gLoop->runAction(&endRuns, nullptr, &detached);
        }
    }
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
