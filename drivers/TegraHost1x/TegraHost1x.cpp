// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 host1x (version 5); see TegraHost1x.h.
//
// Sources (register map and order; Linux's host1x driver is GPL-2.0-only, so this is written from it, not copied):
//   Linux drivers/gpu/host1x/dev.c host1x05_info (192 syncpoints, 14 channels, sync unit at +0x2100),
//   hw/hw_host1x05_sync.h, hw/intr_hw.c (threshold interrupt: THRESH, then ENABLE_CPU0; the handler DISABLEs and
//   clears INT_STATUS), host1x_intr_init_host_sync (IP_BUSY_TIMEOUT 0, CTXSW_TIMEOUT_CFG 0xff, USEC_CLK).
//   tegra210.dtsi host1x@50000000: interrupts SPI 65 (syncpt) and 67 (general), CAR clock and reset 28.
//   drivers/memory/tegra/tegra210.c: MC SMMU ASID of HC 0x250, client hot reset HC 0x200 bit 6.
//   hekate bdk/soc/clock.c _clock_host1x: PLLP / 2.5 = 163.2 MHz, enabled for the display (di.c waits on syncpt 9).
// host1x fetches its commands (SwitchHost1xChannels.cpp) by physical address, through the SMMU at those addresses
// (SwitchHost1xSmmu.cpp): the push buffers and arenas are held, every other address faults.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <os/log.h>
#include "TegraHost1x.h"
#include "SwitchHost1xChannels.h"
#include "SwitchHost1xCpuIncrement.h"
#include "SwitchHost1xErrors.h"
#include "SwitchHost1xIntr.h"
#include "SwitchHost1xSyncptPool.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kHost1xPA = 0x50000000, kHost1xSize = 0x34000;
constexpr uint64_t kMcPA = 0x70019000;
constexpr unsigned kClock = 28;  // TEGRA210_CLK_HOST1X: CAR bank L bit 28, also its reset
constexpr uint32_t kSyncOffset = 0x2100;
constexpr uint32_t kIntid = 32 + 65;  // "syncpt" (tegra210.dtsi host1x@50000000)
constexpr uint32_t kSyncpts = SwitchHost1xSyncptPool::kSyncpts;

enum : uint32_t {  // clock and reset controller (Linux clk.h, clk-tegra-periph.c)
    CLK_OUT_ENB_L = 0x010,
    CLK_SOURCE_HOST1X = 0x180,
    SOURCE_PLLP = 4u << 29,
    HOST1X_DIV_2_5 = 3,  // 7.1 divider: 408 MHz * 2 / (3 + 2) = 163.2 MHz
};

enum : uint32_t {
    MC_HOTRESET_CTRL = 0x200,
    MC_HOTRESET_HC = 1u << 6,
    MC_SMMU_HC_ASID = 0x250,
    SMMU_ENABLE = 1u << 31
};

enum : uint32_t {  // sync unit, relative to kSyncOffset
    SYNC_MOD_TEARDOWN = 0xb4,
    SYNC_USEC_CLK = 0x1a4,
    SYNC_CTXSW_TIMEOUT_CFG = 0x1a8,
    SYNC_IP_BUSY_TIMEOUT = 0x1bc,
    SYNC_INT_STATUS = 0xe80,
    SYNC_INT_DISABLE = 0xf00,
    SYNC_SYNCPT = 0xf80,
};

volatile uint8_t *gHost, *gMc;
IOWorkLoop *gLoop;
IOFilterInterruptEventSource *gIrq;
IOLock *gSubmitLock;  // every channel's push buffer and arena, and suspend/resume

// Never handed out: 0, mainline host1x's no-op syncpoint, and 9, which DC A's CONT_SYNCPT_VSYNC counts at every vblank
// (TegraDisplayPower.cpp, as hekate's di.c and L4T's dc.c set it). T210's HOST1X_SYNC_SYNCPT bank is unreset SRAM
// whose power-on values are arbitrary non-zero bits, so bringUp cannot probe it for boot-chain syncpoints.
SwitchHost1xSyncptPool gPool = {{1ULL << 0 | 1ULL << 9, 0, 0}, {}, {}, {}, 0, 0, 2};
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
// Debug kernels: bit 32 = set, bits 31:0 = highest threshold a channel's fenced job was given per syncpoint.
uint64_t gOwed[kSyncpts];
#endif
int gState;                 // 0 not started, 1 up, 2 suspended, < 0 failed
uint32_t gSaved[kSyncpts];  // syncpoint values across SC7, which resets the sync unit
unsigned gClockKHz;

inline uint32_t sync(uint32_t o)
{
    return switch_mmio_read32(gHost, kSyncOffset + o);
}

inline void syncWrite(uint32_t o, uint32_t v)
{
    switch_mmio_write32(gHost, kSyncOffset + o, v);
}

// The syncpoint line (level): its fences expire and their syncpoints re-arm (SwitchHost1xFenceList.h).
void interruptOccurred(OSObject *, IOInterruptEventSource *, int)
{
    switch_host1x_intr_handle();
}

// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool syncptFilter(OSObject *, IOFilterInterruptEventSource *)
{
    return switch_host1x_intr_filter();
}

int fail(const char *why, uint32_t value)
{
    IOLog("TegraHost1x: H1 FAIL %s (%#x)\n", why, value);
    gState = TEGRA_HOST1X_ENODEV;
    return gState;
}

// Adopts the clock and reset the boot chain left for the display; enables them only if they are off. Never pulses
// the reset: every engine and the display controller are reached through host1x.
bool clockOn(unsigned *khz, bool *enabled)
{
    *enabled = false;
    if (!switch_car_clock_enabled(kClock)) {
        switch_car_write(CLK_SOURCE_HOST1X, SOURCE_PLLP | HOST1X_DIV_2_5);
        IODelay(2);
        switch_car_clock_enable(kClock);
        *enabled = true;
    }
    if (switch_car_reset_asserted(kClock)) {
        IODelay(2);
        switch_car_reset_deassert(kClock);
        *enabled = true;
    }
    const uint32_t source = switch_car_read(CLK_SOURCE_HOST1X);
    *khz = (source >> 29) == 4 ? 408000u * 2 / ((source & 0xff) + 2) : 0;  // only PLLP is known for certain
    return switch_car_clock_enabled(kClock) && !switch_car_reset_asserted(kClock);
}

// host1x_intr_init_host_sync: what the sync unit needs after a reset (boot or SC7). Every threshold interrupt starts
// disabled and clear; waiters arm their own.
void syncConfigure()
{
    syncWrite(SYNC_IP_BUSY_TIMEOUT, 0);
    syncWrite(SYNC_CTXSW_TIMEOUT_CFG, 0xff);
    if (gClockKHz)
        syncWrite(SYNC_USEC_CLK, gClockKHz / 1000);
    for (uint32_t i = 0; i < kSyncpts / 32; i++) {
        syncWrite(SYNC_INT_DISABLE + 4 * i, 0xffffffffu);
        syncWrite(SYNC_INT_STATUS + 4 * i, 0xffffffffu);
    }
}

// Before any engine behind host1x is proven, a missing interrupt is a bring-up failure (one line, nothing published),
// not a panic: host1x comes up by default for NVDEC, and an unproven engine must not stop the boot.
bool attachInterrupt()
{
    gLoop = IOWorkLoop::workLoop();
    IOService *nub = switch_interrupt_nub(kIntid, false);
    gIrq = gLoop && nub ? IOFilterInterruptEventSource::filterInterruptEventSource(gLoop, &interruptOccurred,
                                                                                   &syncptFilter, nub, 0)
                        : nullptr;
    if (!gIrq || !switch_host1x_intr_init(gLoop, gHost + kSyncOffset, kIntid) ||
        gLoop->addEventSource(gIrq) != kIOReturnSuccess)
        return false;
    gIrq->enable();
    return true;
}

int bringUp()
{
    gHost = switch_mmio_map(kHost1xPA, kHost1xSize);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gSubmitLock = IOLockAlloc();
    if (!gHost || !gMc || !gSubmitLock)
        return fail("no mapping", 0);
    bool enabled = false;
    if (!clockOn(&gClockKHz, &enabled))
        return fail("clock or reset stuck, CAR CLK_OUT_ENB_L", switch_car_read(CLK_OUT_ENB_L));
    if (switch_mmio_read32(gMc, MC_SMMU_HC_ASID) & SMMU_ENABLE)
        return fail("SMMU translation is on for HC, MC 0x250", switch_mmio_read32(gMc, MC_SMMU_HC_ASID));
    tegra_mc_hot_reset(MC_HOTRESET_CTRL, MC_HOTRESET_HC, false);
    syncConfigure();
    if (!attachInterrupt())
        return fail("cannot attach the syncpoint interrupt, INTID", kIntid);
    if (!switch_host1x_channels_alloc(gHost, gHost + kSyncOffset, gSubmitLock))
        return fail("no push buffer or command arena below 4 GiB", 0);
    char why[160] = {};
    if (switch_host1x_channels_confine(why, sizeof(why)))
        IOLog("TegraHost1x: command DMA through the SMMU (ASID %u): host1x reaches its push buffers and arenas\n",
              TEGRA_SMMU_ASID_HOST1X);
    else
        IOLog("TegraHost1x: command DMA not behind the SMMU (%s): host1x can reach all of RAM below 4 GiB\n", why);
    switch_host1x_channels_program(gClockKHz);
    gState = 1;
    switch_host1x_intr_start();
    switch_host1x_errors_start(gLoop, gHost + kSyncOffset);
    const uint32_t filtered = switch_host1x_channels_filter_stuck();
    IOLog("TegraHost1x: H1 ok channel 0, host1x %u MHz%s, gather filter %s, %u syncpoints to hand out\n",
          gClockKHz / 1000, enabled ? ", enabled here" : "", filtered ? "on" : "off", gPool.capacity());
    if (filtered)
        IOLog("TegraHost1x: H1 FAIL the gather filter stays on for channels %#x: their streams' waits do not run\n",
              filtered);
    return TEGRA_HOST1X_OK;
}
}  // namespace

extern "C" int tegra_host1x_start(void)
{
    static volatile int started;
    static IOLock *startLock;
    if (!startLock) {
        IOLock *fresh = IOLockAlloc();
        // NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
        IOLock *expected = nullptr;
        if (!__atomic_compare_exchange_n(&startLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    IOLockLock(startLock);
    if (!started) {
        started = 1;
        bringUp();
    }
    const int state = gState;
    IOLockUnlock(startLock);
    return state == 1 ? TEGRA_HOST1X_OK : TEGRA_HOST1X_ENODEV;
}

extern "C" int tegra_host1x_syncpt_alloc_named(uint32_t *id, const char *caller)
{
    if (gPool.alloc(id))
        return TEGRA_HOST1X_OK;
    os_log_error(OS_LOG_DEFAULT, "TegraHost1x: no syncpoint for %s: all %u the pool holds are in use", caller,
                 gPool.capacity());
    return TEGRA_HOST1X_ENOMEM;
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
void switch_host1x_syncpt_owed(uint32_t id, uint32_t threshold)
{
    if (id >= kSyncpts)
        return;
    const uint64_t next = (1ULL << 32) | threshold;
    uint64_t cur = __atomic_load_n(&gOwed[id], __ATOMIC_RELAXED);
    while (!(cur >> 32) || static_cast<int32_t>(threshold - static_cast<uint32_t>(cur)) > 0) {
        if (__atomic_compare_exchange_n(&gOwed[id], &cur, next, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            break;
    }
}

// Another channel's job on the same id is forgotten with it: a check missed, never a false panic.
void switch_host1x_syncpt_dropped(uint32_t id)
{
    if (id < kSyncpts)
        __atomic_store_n(&gOwed[id], 0ULL, __ATOMIC_RELEASE);
}
#endif

extern "C" void tegra_host1x_syncpt_free(uint32_t id)
{
    if (switch_host1x_fence_names(id))
        panic("TegraHost1x: syncpoint %u freed while a fence or waiter still waits on it", id);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    const uint32_t reached = tegra_host1x_syncpt_read(id);
    if (id < kSyncpts && gPool.owns(id)) {
        const uint64_t owed = __atomic_exchange_n(&gOwed[id], 0ULL, __ATOMIC_ACQ_REL);
        if ((owed >> 32) && static_cast<int32_t>(static_cast<uint32_t>(owed) - reached) > 0)
            panic("TegraHost1x: syncpoint %u freed at %u while a job a channel ran still takes it to %u", id, reached,
                  static_cast<uint32_t>(owed));
    }
#endif
    if (!gPool.free(id))
        panic("TegraHost1x: syncpoint %u freed, but it is not handed out (never, reserved, or freed already)", id);
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
namespace {
uint32_t gProbeSyncpts[SWITCH_HOST1X_PROBE_SYNCPTS];  // written only by the holder
bool gProbeSyncptsHeld;
}  // namespace

extern "C" int switch_host1x_probe_syncpts_claim(uint32_t ids[SWITCH_HOST1X_PROBE_SYNCPTS])
{
    if (__atomic_exchange_n(&gProbeSyncptsHeld, true, __ATOMIC_ACQUIRE))
        return TEGRA_HOST1X_EBUSY;
    for (uint32_t &id : gProbeSyncpts)
        if (!id && tegra_host1x_syncpt_alloc(&id) != TEGRA_HOST1X_OK) {
            id = 0;
            __atomic_store_n(&gProbeSyncptsHeld, false, __ATOMIC_RELEASE);
            return TEGRA_HOST1X_ENOMEM;
        }
    for (uint32_t i = 0; i < SWITCH_HOST1X_PROBE_SYNCPTS; i++)
        ids[i] = gProbeSyncpts[i];
    return TEGRA_HOST1X_OK;
}

extern "C" void switch_host1x_probe_syncpts_release(void)
{
    __atomic_store_n(&gProbeSyncptsHeld, false, __ATOMIC_RELEASE);
}
#endif

extern "C" void tegra_host1x_module_teardown(uint32_t module)
{
    if (gHost && module < 32)
        syncWrite(SYNC_MOD_TEARDOWN, 1u << module);
}

extern "C" uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
    return gHost && id < kSyncpts ? sync(SYNC_SYNCPT + 4 * id) : 0;
}

extern "C" bool switch_nvdec_host1x_cancelled(uint32_t syncpt, uint32_t reached) __attribute__((weak));
extern "C" bool switch_vic_host1x_cancelled(uint32_t syncpt, uint32_t reached) __attribute__((weak));
extern "C" void switch_gpu_bridge_gate_close(void) __attribute__((weak));

namespace {
int ownSyncpt(uint32_t id)
{
    if (!gHost || !gSubmitLock || gState != 1)
        return TEGRA_HOST1X_ENODEV;
    if (!gPool.owns(id))
        return TEGRA_HOST1X_EINVAL;
    return TEGRA_HOST1X_OK;
}

// Cancel NVDEC/VIC jobs waiting on `id` short of `reached`, then (for a failed release, before any CPU increment)
// close the GPU bridge gate so bridges waiting on `id` or those newly cancelled jobs see them failed, then cancel the
// display stream and increment to `threshold`.
int cancelThenRelease(uint32_t id, uint32_t reached, uint32_t threshold, bool closeGate)
{
    bool changed = false;
    do {
        changed = switch_nvdec_host1x_cancelled != nullptr && switch_nvdec_host1x_cancelled(id, reached);
        changed |= switch_vic_host1x_cancelled != nullptr && switch_vic_host1x_cancelled(id, reached);
    } while (changed);
    if (closeGate && switch_gpu_bridge_gate_close != nullptr)
        switch_gpu_bridge_gate_close();
    IOLockLock(gSubmitLock);
    const int result = gState == 1 ? TEGRA_HOST1X_OK : TEGRA_HOST1X_ENODEV;
    if (!result)
        switch_host1x_channels_release_failed(id, threshold);
    IOLockUnlock(gSubmitLock);
    return result;
}
}  // namespace

extern "C" int tegra_host1x_syncpt_release_failed(uint32_t id, uint32_t threshold, uint32_t max_increments)
{
    if (const int own = ownSyncpt(id))
        return own;
    const uint32_t reached = tegra_host1x_syncpt_read(id);
    const int32_t distance = static_cast<int32_t>(threshold - reached);
    if (distance <= 0)
        return TEGRA_HOST1X_OK;
    if (static_cast<uint32_t>(distance) > max_increments)
        return TEGRA_HOST1X_EINVAL;
    return cancelThenRelease(id, reached, threshold, true);
}

extern "C" int tegra_host1x_syncpt_cancel_waiters(uint32_t id, uint32_t threshold)
{
    if (const int own = ownSyncpt(id))
        return own;
    const uint32_t reached = tegra_host1x_syncpt_read(id);
    if (static_cast<int32_t>(threshold - reached) <= 0)
        return TEGRA_HOST1X_OK;
    return cancelThenRelease(id, reached, reached, false);
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
extern "C" int tegra_host1x_syncpt_cpu_increment(uint32_t id)
{
    if (!gHost || !gSubmitLock)
        return TEGRA_HOST1X_ENODEV;

    struct Registers {
        uint32_t read(uint32_t offset)
        {
            return sync(offset);
        }

        void write(uint32_t offset, uint32_t value)
        {
            syncWrite(offset, value);
        }

        void barrier()
        {
            __asm__ volatile("dsb sy" ::: "memory");
        }
    } registers;

    IOLockLock(gSubmitLock);
    const int result = gState == 1 ? switch_host1x_cpu_increment(registers, id, gPool.owns(id)) : TEGRA_HOST1X_ENODEV;
    IOLockUnlock(gSubmitLock);
    return result;
}
#endif

// Clients have quiesced (their last fences reached) before this. SC7 powers the sync unit and channels down, so the
// syncpoint values handed out are saved (Linux host1x_syncpt_save) and the channels are stopped.
extern "C" void tegra_host1x_suspend(void)
{
    if (gState != 1)
        return;
    switch_host1x_errors_stop();
    switch_host1x_intr_stop();
    IOLockLock(gSubmitLock);
    for (uint32_t id = 0; id < kSyncpts; id++) {
        if (gPool.everOwned(id))
            gSaved[id] = sync(SYNC_SYNCPT + 4 * id);
    }
    switch_host1x_channels_stop();
    gState = 2;
    IOLockUnlock(gSubmitLock);
}

// Clock adopted (or enabled) again, the sync unit configured as at boot, the saved syncpoint values written back
// (host1x_syncpt_restore) so outstanding fence numbers stay meaningful, and the channels restarted on their push buffers.
extern "C" void tegra_host1x_resume(void)
{
    if (gState != 2)
        return;
    IOLockLock(gSubmitLock);
    // SC7 resets the CAR: put host1x back on the boot chain's PLLP / 2.5 if it came back on another source (it is
    // idle, and nothing behind it has resumed yet: the display's wake hook runs after this one).
    if ((switch_car_read(CLK_SOURCE_HOST1X) >> 29) != 4) {
        switch_car_write(CLK_SOURCE_HOST1X, SOURCE_PLLP | HOST1X_DIV_2_5);
        IODelay(2);
    }
    bool enabled = false;
    if (!clockOn(&gClockKHz, &enabled)) {
        IOLog("TegraHost1x: H1 FAIL resume: clock or reset stuck, CAR CLK_OUT_ENB_L %#x\n",
              switch_car_read(CLK_OUT_ENB_L));
        gState = TEGRA_HOST1X_ENODEV;
        IOLockUnlock(gSubmitLock);
        return;
    }
    syncConfigure();
    for (uint32_t id = 0; id < kSyncpts; id++) {
        if (gPool.everOwned(id))
            syncWrite(SYNC_SYNCPT + 4 * id, gSaved[id]);
    }
    switch_host1x_channels_program(gClockKHz);
    gState = 1;
    switch_host1x_intr_start();
    switch_host1x_errors_resume();
    IOLockUnlock(gSubmitLock);
}

// The sleep agent's hooks (SwitchSuspend calls them in thread context, interrupts on). They live here, in the file
// that is always built, so they exist whether or not NVDEC is; NVDEC, NVENC, VIC and NVJPG register their quiesce and
// reload once they are up. Clients quiesce in the reverse order of registration and reload in order.
// Registration may race (each engine registers from its own bring-up thread), so it is serialised by a lock: the
// entry is written, then published by a release store of the count the sleep agent reads, so the agent never calls a
// half-written entry and a fifth client can never write past the table.
namespace {
constexpr uint32_t kSleepClients = 4;

struct SleepClient {
    void (*suspend)(), (*resume)();
} gSleepClients[kSleepClients];

uint32_t gSleepClientCount;

// NOLINTNEXTLINE(misc-const-correctness): the compare-exchange stores through slot
IOLock *lazyLock(IOLock **slot)
{
    if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE)) {
        IOLock *fresh = IOLockAlloc();
        // NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
        IOLock *expected = nullptr;
        if (!__atomic_compare_exchange_n(slot, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    return *slot;
}

IOLock *gRegisterLock;
}  // namespace

extern "C" void tegra_host1x_set_sleep_client(void (*suspend)(), void (*resume)())
{
    IOLock *lock = lazyLock(&gRegisterLock);
    IOLockLock(lock);
    const uint32_t i = gSleepClientCount;
    if (i < kSleepClients) {
        gSleepClients[i] = {suspend, resume};
        __atomic_store_n(&gSleepClientCount, i + 1, __ATOMIC_RELEASE);
    }
    IOLockUnlock(lock);
    if (i >= kSleepClients)
        IOLog("TegraHost1x: a fifth sleep client was refused (four at most); its engine is not reloaded after sleep\n");
}

extern "C" void tegra_nvdec_suspend(void)
{
    for (uint32_t i = __atomic_load_n(&gSleepClientCount, __ATOMIC_ACQUIRE); i-- > 0;)
        gSleepClients[i].suspend();
    tegra_host1x_suspend();
}

extern "C" void tegra_nvdec_resume(void)
{
    tegra_host1x_resume();
    const uint32_t count = __atomic_load_n(&gSleepClientCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; i++)
        gSleepClients[i].resume();
}
