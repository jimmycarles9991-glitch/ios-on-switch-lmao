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
// The SMMU stays bypassed as for every other driver here: host1x fetches its commands (SwitchHost1xChannels.cpp) by
// physical address.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "TegraHost1x.h"
#include "SwitchHost1xChannels.h"
#include "SwitchHost1xFailureRelease.h"
#include "SwitchHost1xIntr.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kHost1xPA = 0x50000000, kHost1xSize = 0x34000;
constexpr uint64_t kMcPA = 0x70019000;
constexpr unsigned kClock = 28;  // TEGRA210_CLK_HOST1X: CAR bank L bit 28, also its reset
constexpr uint32_t kSyncOffset = 0x2100;
constexpr uint32_t kIntid = 32 + 65;  // "syncpt" (tegra210.dtsi host1x@50000000)
constexpr uint32_t kSyncpts = 192, kFirstOwn = 160;

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

uint32_t gNextSyncpt = kFirstOwn;
int gState;                 // 0 not started, 1 up, 2 suspended, < 0 failed
uint32_t gSaved[kSyncpts];  // syncpoint values across SC7, which resets the sync unit
unsigned gClockMHz;

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

// Primary interrupt context: the filter waiters, then true, which masks the line until the action has acked and
// re-armed (a passed threshold stays disabled until re-armed, as in mainline intr.c). The action owns the status and
// enable writes because the armed set they are masked with is its lock's.
// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool syncptFilter(OSObject *, IOFilterInterruptEventSource *)
{
    switch_host1x_intr_filter();
    return true;
}

int fail(const char *why, uint32_t value)
{
    IOLog("TegraHost1x: H1 FAIL %s (%#x)\n", why, value);
    gState = TEGRA_HOST1X_ENODEV;
    return gState;
}

// Adopts the clock and reset the boot chain left for the display; enables them only if they are off. Never pulses
// the reset: every engine and the display controller are reached through host1x.
bool clockOn(unsigned *mhz, bool *enabled)
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
    *mhz = (source >> 29) == 4 ? 408u * 2 / ((source & 0xff) + 2) : 0;  // only PLLP is known for certain
    return switch_car_clock_enabled(kClock) && !switch_car_reset_asserted(kClock);
}

// host1x_intr_init_host_sync: what the sync unit needs after a reset (boot or SC7). Every threshold interrupt starts
// disabled and clear; waiters arm their own.
void syncConfigure()
{
    syncWrite(SYNC_IP_BUSY_TIMEOUT, 0);
    syncWrite(SYNC_CTXSW_TIMEOUT_CFG, 0xff);
    if (gClockMHz)
        syncWrite(SYNC_USEC_CLK, gClockMHz);
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
    if (!clockOn(&gClockMHz, &enabled))
        return fail("clock or reset stuck, CAR CLK_OUT_ENB_L", switch_car_read(CLK_OUT_ENB_L));
    if (switch_mmio_read32(gMc, MC_SMMU_HC_ASID) & SMMU_ENABLE)
        return fail("SMMU translation is on for HC, MC 0x250", switch_mmio_read32(gMc, MC_SMMU_HC_ASID));
    if (switch_mmio_read32(gMc, MC_HOTRESET_CTRL) & MC_HOTRESET_HC)
        switch_mmio_write32(gMc, MC_HOTRESET_CTRL, switch_mmio_read32(gMc, MC_HOTRESET_CTRL) & ~MC_HOTRESET_HC);
    syncConfigure();
    if (!attachInterrupt())
        return fail("cannot attach the syncpoint interrupt, INTID", kIntid);
    if (!switch_host1x_channels_alloc(gHost, gHost + kSyncOffset, gSubmitLock))
        return fail("no push buffer or command arena below 4 GiB", 0);
    switch_host1x_channels_program();
    gState = 1;
    switch_host1x_intr_start();
    const uint32_t filtered = switch_host1x_channels_filter_stuck();
    IOLog("TegraHost1x: H1 ok channel 0, host1x %u MHz%s, gather filter %s\n", gClockMHz,
          enabled ? ", enabled here" : "", filtered ? "on" : "off");
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

extern "C" int tegra_host1x_syncpt_alloc(uint32_t *id)
{
    const uint32_t next = __atomic_fetch_add(&gNextSyncpt, 1, __ATOMIC_RELAXED);
    if (next >= kSyncpts)
        return TEGRA_HOST1X_ENOMEM;
    *id = next;
    return TEGRA_HOST1X_OK;
}

extern "C" uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
    return gHost && id < kSyncpts ? sync(SYNC_SYNCPT + 4 * id) : 0;
}

extern "C" int tegra_host1x_syncpt_release_failed(uint32_t id, uint32_t threshold, uint32_t max_increments)
{
    if (!gHost || !gSubmitLock)
        return TEGRA_HOST1X_ENODEV;
    IOLockLock(gSubmitLock);

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

    const int result = gState == 1 ? switch_host1x_release_failed(registers, id, threshold, max_increments,
                                                                  __atomic_load_n(&gNextSyncpt, __ATOMIC_ACQUIRE))
                                   : TEGRA_HOST1X_ENODEV;
    IOLockUnlock(gSubmitLock);
    return result;
}

// Clients have quiesced (their last fences reached) before this. SC7 powers the sync unit and channels down, so the
// syncpoint values handed out are saved (Linux host1x_syncpt_save) and the channels are stopped.
extern "C" void tegra_host1x_suspend(void)
{
    if (gState != 1)
        return;
    switch_host1x_intr_stop();
    IOLockLock(gSubmitLock);
    for (uint32_t id = kFirstOwn; id < kSyncpts && id < gNextSyncpt; id++)
        gSaved[id] = sync(SYNC_SYNCPT + 4 * id);
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
    if (!clockOn(&gClockMHz, &enabled)) {
        IOLog("TegraHost1x: H1 FAIL resume: clock or reset stuck, CAR CLK_OUT_ENB_L %#x\n",
              switch_car_read(CLK_OUT_ENB_L));
        gState = TEGRA_HOST1X_ENODEV;
        IOLockUnlock(gSubmitLock);
        return;
    }
    syncConfigure();
    for (uint32_t id = kFirstOwn; id < kSyncpts && id < gNextSyncpt; id++)
        syncWrite(SYNC_SYNCPT + 4 * id, gSaved[id]);
    switch_host1x_channels_program();
    gState = 1;
    switch_host1x_intr_start();
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
