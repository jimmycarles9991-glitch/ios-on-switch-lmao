// SPDX-License-Identifier: BSD-2-Clause-Patent
// The memory controller's error interrupt, the one owner of MC_INTSTATUS, INTMASK and the capture registers (see
// TegraSMMU.h tegra_mc_errors_start). MC General Interrupt: global 77 (Tegra X1 TRM ch.3, p.35), INTID 109, level.
//
// A fatal kind (TegraMCErrors.h kFatal) panics in the filter with the error's line. The filter takes any other error
// into a slot only it writes; IOKit keeps a level line masked from a filter that returns true until its action has
// run, so the action reads that slot with no other writer. The action
// prints the line (the first Runs::kFullLines of a client's run, then one count line when the run ends) and publishes
// the error to readers in other drivers (GM20B's stats and hang lines, the engines' stall lines) under a spin lock: they
// read it from any context, and a published error is several words that must stay together. The runs are the work
// loop's: a detach or SC7 ends them through its gate.
//
// The start returns at once: its own thread waits for the GIC, attaches, and takes what latched meanwhile, so nothing
// later in the board thread's order waits on the GIC for it.
//
// Debug kernels: an expected fault (TegraSMMU.h switch_mc_errors_expect) is taken by the filter, counted and handed to
// the action, which wakes the probe waiting in switch_mc_errors_expect_end and prints nothing.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
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
    {TEGRA_SMMU_GPUB_ASID, {0x58, 0x59, 0x88, 0x89}, 4},
    {TEGRA_SMMU_SDMMC1A_ASID, {0x60, 0x64}, 2},
    {TEGRA_SMMU_HC_ASID, {0x16, 0x17, 0x36}, 3},
    {TEGRA_SMMU_HC1_ASID, {0x16}, 1},
    {TEGRA_SMMU_APE_ASID, {0x7a, 0x7b}, 2},
};

Io gIo;
IOWorkLoop *gLoop;
IOFilterInterruptEventSource *gSource;  // published by the attach thread once it is enabled
switch_mc::Error gTaken;                // the filter's
switch_mc::Runs gRuns;                  // the work loop's

IOSimpleLock *gLatestLock;
switch_mc::Error gLatest;
uint32_t gCount;

#if DEVELOPMENT || DEBUG
// The probe's thread writes the range and arms it before its DMA starts and disarms it after the end has seen every
// expected error taken; the filter alone writes `first` and then `taken` (release) meanwhile.
struct Expected {
    uint32_t client, low, high;
    bool armed;
    uint32_t taken;
    switch_mc::Error first;
};

Expected gExpected;
bool gTakenExpected;  // the filter's, for the action: gTaken was expected

bool takeExpected(const switch_mc::Error &error)
{
    if (!__atomic_load_n(&gExpected.armed, __ATOMIC_ACQUIRE) ||
        !switch_mc::expected(error, gExpected.client, gExpected.low, gExpected.high))
        return false;
    const uint32_t taken = __atomic_load_n(&gExpected.taken, __ATOMIC_RELAXED);
    if (!taken)
        gExpected.first = error;
    __atomic_store_n(&gExpected.taken, taken + 1, __ATOMIC_RELEASE);
    return true;
}
#endif

// In the filter, interrupts off: a fatal error panics there, before the faulting client's next access, and with no
// work loop needed (a wedged fabric never runs the action).
void panicIfFatal(const switch_mc::Error &error)
{
    if (!(error.intstatus & switch_mc::kFatal))
        return;
    char line[384];
    panic("TegraSMMU: MC error %s", switch_mc::describe(error, line, sizeof(line)));
}

// gTaken from the registers; false when no error is pending. A fatal one panics unless it was expected.
bool takeError()
{
    if (!switch_mc::take(gIo, &gTaken))
        return false;
#if DEVELOPMENT || DEBUG
    gTakenExpected = takeExpected(gTaken);
    if (gTakenExpected)
        return true;
#endif
    panicIfFatal(gTaken);
    return true;
}

// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool filter(OSObject *, IOFilterInterruptEventSource *)
{
    return takeError();
}

void printEnded(const switch_mc::Runs::Ended *ended, unsigned count)
{
    char line[512];
    for (unsigned i = 0; i < count; ++i)
        IOLog("TegraSMMU: MC error run over: %s\n", switch_mc::describeRun(ended[i], line, sizeof(line)));
}

void occurred(OSObject *, IOInterruptEventSource *, int)
{
#if DEVELOPMENT || DEBUG
    if (gTakenExpected) {
        thread_wakeup(&gExpected);
        return;
    }
#endif
    const switch_mc::Error error = gTaken;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLatestLock);
    gLatest = error;
    ++gCount;
    IOSimpleLockUnlockEnableInterrupt(gLatestLock, state);
    switch_mc::Runs::Ended ended[switch_mc::Runs::kOpen];
    unsigned count = 0;
    const bool full = gRuns.error(error, ended, count);
    printEnded(ended, count);
    char line[384];
    if (full)
        IOLog("TegraSMMU: MC error %s\n", switch_mc::describe(error, line, sizeof(line)));
}

IOReturn takeLatched(OSObject *, void *, void *, void *, void *)
{
    if (takeError())
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

// A missing or masked MC error interrupt would leave every confined client's fault squashed with nothing said
// (TRM 18.5.12.2: the MC drops a faulting write's bytes and returns ones for a read), so either panics.
void unmask()
{
    gIo.write(switch_mc::kIntMask, switch_mc::kErrors);
    const uint32_t mask = gIo.read(switch_mc::kIntMask);
    if ((mask & switch_mc::kRequired) != switch_mc::kRequired)
        panic("TegraSMMU: MC error interrupt (INTID %u) masked: INTMASK reads %#x after %#x was written", kIntid, mask,
              switch_mc::kErrors);
    static bool told;
    if ((mask & switch_mc::kErrors) != switch_mc::kErrors && !told) {
        told = true;
        IOLog("TegraSMMU: MC INTMASK keeps %#x of %#x: DECERR_GENERALIZED_CARVEOUT stays masked\n", mask,
              switch_mc::kErrors);
    }
}

[[noreturn]] void notAttached(const char *why)
{
    panic("TegraSMMU: MC error interrupt (INTID %u) could not be attached: %s", kIntid, why);
}

IOFilterInterruptEventSource *source()
{
    return __atomic_load_n(&gSource, __ATOMIC_ACQUIRE);
}

// Its own thread: switch_interrupt_nub sleeps until ArmCpu has the GIC up. What the MC latched before is taken first.
void attach(void *, wait_result_t)
{
    IOService *nub = switch_interrupt_nub(kIntid, false);
    if (!nub)
        notAttached("no interrupt nub");
    IOFilterInterruptEventSource *made =
        IOFilterInterruptEventSource::filterInterruptEventSource(gLoop, &occurred, &filter, nub, 0);
    if (!made)
        notAttached("no event source");
    if (gLoop->addEventSource(made) != kIOReturnSuccess)
        notAttached("the work loop refused the event source");
    gLoop->runAction(&takeLatched, nullptr);
    made->enable();
    unmask();
    __atomic_store_n(&gSource, made, __ATOMIC_RELEASE);
}  // returning ends the thread (Call_continuation -> thread_terminate)
}  // namespace

extern "C" int tegra_mc_errors_start(void)
{
    gIo.mc = switch_mmio_map(kMcPA, 0x1000);
    gLatestLock = IOSimpleLockAlloc();
    gLoop = IOWorkLoop::workLoop();
    thread_t thread = nullptr;
    if (!gIo.mc || !gLatestLock || !gLoop)
        notAttached("no registers, lock or work loop");
    if (kernel_thread_start(&attach, nullptr, &thread) != KERN_SUCCESS)
        notAttached("no thread to attach it");
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

#if DEVELOPMENT || DEBUG
extern "C" void switch_mc_errors_expect(uint32_t client, uint32_t low, uint32_t high)
{
    gExpected.client = client;
    gExpected.low = low;
    gExpected.high = high;
    gExpected.first = {};
    __atomic_store_n(&gExpected.taken, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&gExpected.armed, true, __ATOMIC_RELEASE);
}

namespace {
// Every expected error the DMA raised is taken: one was, and no SMMU fault is latched behind it.
bool settled()
{
    return __atomic_load_n(&gExpected.taken, __ATOMIC_ACQUIRE) &&
           !(gIo.read(switch_mc::kIntStatus) & switch_mc::kInvalidSmmuPage);
}
}  // namespace

extern "C" void switch_mc_errors_expect_end(uint32_t ms, switch_mc_expected *out)
{
    *out = {};
    if (!source()) {
        __atomic_store_n(&gExpected.armed, false, __ATOMIC_RELEASE);
        return;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!settled()) {
        assert_wait_deadline(&gExpected, THREAD_UNINT, deadline);
        if (settled()) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            break;
        }
        if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT)
            break;
    }
    const uint32_t latched = gIo.read(switch_mc::kIntStatus) & switch_mc::kErrors;
    __atomic_store_n(&gExpected.armed, false, __ATOMIC_RELEASE);
    const uint32_t taken = __atomic_load_n(&gExpected.taken, __ATOMIC_ACQUIRE);
    if (latched & switch_mc::kInvalidSmmuPage)
        panic("TegraSMMU: MC error interrupt (INTID %u) lost: INTSTATUS %#x still latched %u ms after a deliberate "
              "SMMU fault, %u expected errors delivered",
              kIntid, latched, ms, taken);
    // `first` is written before `taken` moves (release); with none taken a late filter may still be writing it.
    if (taken)
        *out = {1, taken, gExpected.first.intstatus, gExpected.first.status, gExpected.first.address};
    else
        *out = {1, 0, 0, 0, 0};
}
#endif
