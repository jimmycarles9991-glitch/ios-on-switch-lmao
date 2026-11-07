// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 host1x (version 5); see TegraHost1x.h.
//
// Sources (register map and order; Linux's host1x driver is GPL-2.0-only, so this is written from it, not copied):
//   Linux drivers/gpu/host1x/dev.c host1x05_info (192 syncpoints, 14 channels, sync unit at +0x2100),
//   hw/channel_hw.c host1x_channel_init (channel n at n * 0x4000), hw/hw_host1x05_{sync,channel}.h, hw/opcodes.h,
//   hw/cdma_hw.c cdma_start/cdma_flush (DMASTART = push buffer, DMAPUT/DMAGET/DMAEND relative to it, a RESTART
//   opcode after the last slot), hw/intr_hw.c (threshold interrupt: THRESH, then ENABLE_CPU0; the handler DISABLEs
//   and clears INT_STATUS), host1x_intr_init_host_sync (IP_BUSY_TIMEOUT 0, CTXSW_TIMEOUT_CFG 0xff, USEC_CLK).
//   tegra210.dtsi host1x@50000000: interrupts SPI 65 (syncpt) and 67 (general), CAR clock and reset 28.
//   drivers/memory/tegra/tegra210.c: MC SMMU ASID of HC 0x250, client hot reset HC 0x200 bit 6.
//   hekate bdk/soc/clock.c _clock_host1x: PLLP / 2.5 = 163.2 MHz, enabled for the display (di.c waits on syncpt 9).
// The SMMU stays bypassed as for every other driver here: host1x fetches the push buffer by physical address, so it
// is contiguous, below 4 GiB (DMASTART is 32-bit on host1x05) and cleaned to the point of coherency before DMAPUT.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "TegraHost1x.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
constexpr uint64_t kHost1xPA = 0x50000000, kHost1xSize = 0x34000;
constexpr uint64_t kMcPA = 0x70019000;
constexpr unsigned kClock = 28;  // TEGRA210_CLK_HOST1X: CAR bank L bit 28, also its reset
constexpr uint32_t kSyncOffset = 0x2100;
constexpr uint32_t kIntid = 32 + 65;  // "syncpt" (tegra210.dtsi host1x@50000000)
constexpr uint32_t kSyncpts = 192, kFirstOwn = 160;
constexpr uint32_t kPushBytes = 16384;  // 2048 two-word slots (4x Linux's), + RESTART: room for 8 NVDEC jobs
constexpr uint32_t kGraceMs = 50;

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

enum : uint32_t {  // channel registers, channel 0 at the start of the aperture
    CH_FIFOSTAT = 0x00,
    CH_DMASTART = 0x14,
    CH_DMAPUT = 0x18,
    CH_DMAGET = 0x1c,
    CH_DMAEND = 0x20,
    CH_DMACTRL = 0x24,
    DMACTRL_STOP = 1u << 0,
    DMACTRL_GETRST = 1u << 1,
    DMACTRL_INITGET = 1u << 2,
};

enum : uint32_t {  // sync unit, relative to kSyncOffset
    SYNC_CMDPROC_STOP = 0x0ac,
    SYNC_USEC_CLK = 0x1a4,
    SYNC_CTXSW_TIMEOUT_CFG = 0x1a8,
    SYNC_IP_BUSY_TIMEOUT = 0x1bc,
    SYNC_CBREAD0 = 0xc80,
    SYNC_CBSTAT0 = 0xcc0,
    SYNC_INT_STATUS = 0xe80,
    SYNC_INT_DISABLE = 0xf00,
    SYNC_INT_ENABLE_CPU0 = 0xf20,
    SYNC_SYNCPT = 0xf80,
    SYNC_INT_THRESH = 0x1380,
};

constexpr uint32_t opRestart(uint32_t byteOffset)
{
    return (5u << 28) | (byteOffset >> 4);
}

constexpr uint32_t kOpNop = 2u << 28;  // NONINCR, offset 0, count 0

volatile uint8_t *gHost, *gMc;
IOWorkLoop *gLoop;
IOInterruptEventSource *gIrq;
IOLock *gLock;        // syncpoint waits and the interrupt action
IOLock *gSubmitLock;  // every channel's push buffer, and suspend/resume

// A command DMA channel and its push buffer. Channel n's registers are at n * 0x4000 (Linux host1x_channel_init).
struct Channel {
    switch_dma_buffer push;
    uint32_t *va;
    uint32_t put;  // byte offset of the next word
};

constexpr uint32_t kChannelStride = 0x4000;
Channel gChannels[TEGRA_HOST1X_CHANNELS];
uint32_t gWake;  // wait channel; also counts interrupt actions
uint32_t gNextSyncpt = kFirstOwn;
uint32_t gArmed[kSyncpts / 32];  // threshold interrupts a waiter enabled (gLock); INT_STATUS bits are sticky for all
int gState;                      // 0 not started, 1 up, 2 suspended, < 0 failed
uint32_t gSaved[kSyncpts];       // syncpoint values across SC7, which resets the sync unit
unsigned gClockMHz;
bool gStrict;  // a client proved its engine: a lost interrupt panics from here on (tegra_host1x_set_strict)

inline uint32_t sync(uint32_t o)
{
    return switch_mmio_read32(gHost, kSyncOffset + o);
}

inline void syncWrite(uint32_t o, uint32_t v)
{
    switch_mmio_write32(gHost, kSyncOffset + o, v);
}

inline bool reached(uint32_t value, uint32_t threshold)
{
    return (int32_t)(value - threshold) >= 0;
}

// The syncpoint line (level). Every armed status bit seen is disabled and cleared, as Linux syncpt_thresh_isr does,
// so the line is quiet on return; waiters re-read their syncpoint. Status bits of syncpoints nobody armed (another
// engine counting) are sticky but do not drive the line, and are left alone.
void interruptOccurred(OSObject *, IOInterruptEventSource *, int)
{
    IOLockLock(gLock);
    for (uint32_t i = 0; i < kSyncpts / 32; i++) {
        const uint32_t status = sync(SYNC_INT_STATUS + 4 * i) & gArmed[i];
        if (!status)
            continue;
        syncWrite(SYNC_INT_DISABLE + 4 * i, status);
        syncWrite(SYNC_INT_STATUS + 4 * i, status);
        gArmed[i] &= ~status;
    }
    gWake++;
    IOLockWakeup(gLock, &gWake, false);
    IOLockUnlock(gLock);
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

uint32_t channelRead(uint32_t channel, uint32_t reg)
{
    return switch_mmio_read32(gHost, channel * kChannelStride + reg);
}

void channelWrite(uint32_t channel, uint32_t reg, uint32_t value)
{
    switch_mmio_write32(gHost, channel * kChannelStride + reg, value);
}

// Linux cdma_start for every channel: the push buffers are allocated once and reprogrammed after SC7.
bool channelAlloc()
{
    for (Channel &c : gChannels) {
        if (!switch_dma_alloc(&c.push, kPushBytes + PAGE_SIZE))
            return false;
        c.va = (uint32_t *)c.push.va;
    }
    return true;
}

void channelProgram()
{
    for (uint32_t n = 0; n < TEGRA_HOST1X_CHANNELS; n++) {
        Channel &c = gChannels[n];
        memset(c.va, 0, kPushBytes + 4);
        c.va[kPushBytes / 4] = opRestart(0);
        switch_dcache_clean(c.va, kPushBytes + 4);
        syncWrite(SYNC_CMDPROC_STOP, sync(SYNC_CMDPROC_STOP) & ~(1u << n));
        channelWrite(n, CH_DMACTRL, DMACTRL_STOP);
        channelWrite(n, CH_DMASTART, (uint32_t)c.push.pa);
        channelWrite(n, CH_DMAPUT, 0);
        channelWrite(n, CH_DMAEND, kPushBytes + 4);
        channelWrite(n, CH_DMACTRL, DMACTRL_STOP | DMACTRL_GETRST | DMACTRL_INITGET);
        channelWrite(n, CH_DMACTRL, 0);
        c.put = 0;
    }
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
    gIrq = gLoop && nub ? IOInterruptEventSource::interruptEventSource(gLoop, &interruptOccurred, nub, 0) : nullptr;
    if (!gIrq || gLoop->addEventSource(gIrq) != kIOReturnSuccess)
        return false;
    gIrq->enable();
    return true;
}

int bringUp()
{
    gHost = switch_mmio_map(kHost1xPA, kHost1xSize);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gLock = IOLockAlloc();
    gSubmitLock = IOLockAlloc();
    if (!gHost || !gMc || !gLock || !gSubmitLock)
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
    if (!channelAlloc())
        return fail("no push buffer below 4 GiB", 0);
    channelProgram();
    gState = 1;

    uint32_t id = 0;
    tegra_host1x_syncpt_alloc(&id);
    const uint32_t before = tegra_host1x_syncpt_read(id);
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id),
    };
    const uint64_t began = switch_now_us();
    int ret = tegra_host1x_submit(stream, 2);
    if (!ret)
        ret = tegra_host1x_syncpt_wait(id, before + 1, 100000);
    if (ret) {
        char state[160];
        tegra_host1x_describe(state, sizeof(state));
        IOLog("TegraHost1x: H1 FAIL self-test syncpoint %u at %u (%s), %s\n", id, tegra_host1x_syncpt_read(id),
              ret == TEGRA_HOST1X_ELOST ? "reached, but its interrupt never came" : "not reached", state);
        gState = TEGRA_HOST1X_ENODEV;
        return gState;
    }
    IOLog("TegraHost1x: H1 ok syncpoint %u %u->%u by interrupt in %llu us (channel 0, host1x %u MHz%s)\n", id, before,
          tegra_host1x_syncpt_read(id), switch_now_us() - began, gClockMHz, enabled ? ", enabled here" : "");
    return TEGRA_HOST1X_OK;
}
}  // namespace

extern "C" int tegra_host1x_start(void)
{
    static volatile int started;
    static IOLock *startLock;
    if (!startLock) {
        IOLock *fresh = IOLockAlloc(), *expected = nullptr;
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

// Linux host1x_intr_add_fence order: threshold, then enable; the value is read again after arming so an increment
// that landed in between is not slept on. A wait that ends with the syncpoint's status bit still set at the
// controller, after a grace period for an action under way, means the interrupt never reached the CPU: a panic once
// strict (an engine was proven and published), TEGRA_HOST1X_ELOST while its probe is still running.
extern "C" int tegra_host1x_syncpt_wait(uint32_t id, uint32_t threshold, uint32_t timeoutUs)
{
    if (gState != 1)
        return TEGRA_HOST1X_ENODEV;
    if (id >= kSyncpts)
        return TEGRA_HOST1X_EINVAL;
    const uint32_t word = id / 32, bit = 1u << (id % 32);
    IOLockLock(gLock);
    syncWrite(SYNC_INT_THRESH + 4 * id, threshold);
    syncWrite(SYNC_INT_STATUS + 4 * word, bit);
    syncWrite(SYNC_INT_ENABLE_CPU0 + 4 * word, bit);
    gArmed[word] |= bit;
    uint64_t deadline;
    clock_interval_to_deadline(timeoutUs, kMicrosecondScale, &deadline);
    int ret = TEGRA_HOST1X_OK;
    for (bool grace = false; !reached(tegra_host1x_syncpt_read(id), threshold);) {
        if (IOLockSleepDeadline(gLock, &gWake, deadline, THREAD_UNINT) != THREAD_TIMED_OUT)
            continue;
        if (!reached(tegra_host1x_syncpt_read(id), threshold)) {
            ret = TEGRA_HOST1X_ETIMEDOUT;  // the engine did not get there: its failure, not the interrupt's
            break;
        }
        if (!(sync(SYNC_INT_STATUS + 4 * word) & bit))
            break;
        if (grace) {
            if (!__atomic_load_n(&gStrict, __ATOMIC_ACQUIRE)) {
                ret = TEGRA_HOST1X_ELOST;
                break;
            }
            char gic[192];
            switch_interrupt_describe(kIntid, gic, sizeof(gic));
            panic("TegraHost1x: syncpoint %u threshold interrupt lost (INTID %u): value %u threshold %u INT_STATUS[%u] "
                  "%#x, %s",
                  id, kIntid, tegra_host1x_syncpt_read(id), threshold, word, sync(SYNC_INT_STATUS + 4 * word), gic);
        }
        grace = true;
        clock_interval_to_deadline(kGraceMs, kMillisecondScale, &deadline);
    }
    syncWrite(SYNC_INT_DISABLE + 4 * word, bit);
    syncWrite(SYNC_INT_STATUS + 4 * word, bit);
    gArmed[word] &= ~bit;
    IOLockUnlock(gLock);
    return ret;
}

extern "C" void tegra_host1x_set_strict(void)
{
    __atomic_store_n(&gStrict, true, __ATOMIC_RELEASE);
}

// Linux push_buffer_push + cdma_flush: words go in at PUT (wrapping at the RESTART slot), are cleaned to DRAM, and
// DMAPUT moves past them. The space check uses DMAGET, which host1x keeps relative to DMASTART like PUT.
extern "C" int tegra_host1x_submit_on(uint32_t channel, const uint32_t *words, uint32_t count)
{
    if (gState != 1)
        return TEGRA_HOST1X_ENODEV;
    if (!words || !count || count >= kPushBytes / 4 || channel >= TEGRA_HOST1X_CHANNELS)
        return TEGRA_HOST1X_EINVAL;
    Channel &c = gChannels[channel];
    IOLockLock(gSubmitLock);
    const uint32_t get = channelRead(channel, CH_DMAGET) % kPushBytes;
    const uint32_t room = (get + kPushBytes - c.put - 4) % kPushBytes;
    // A job must not straddle the RESTART slot: Linux's push buffer only holds two-word GATHERs, but these are the
    // job's own opcodes, and a RESTART landing inside an opcode's data is taken as data, so the command processor runs
    // on to DMAEND and stops (DMAGET 0x4004, DMAPUT 0x84, the fence never reached). Such a job starts at 0, the rest
    // of the buffer NOPs (host1x_opcode_nop: NONINCR, count 0) up to the RESTART.
    const uint32_t tail = kPushBytes - c.put, bytes = count * 4;
    const uint32_t pad = bytes > tail ? tail : 0;
    if (pad + bytes > room) {
        IOLockUnlock(gSubmitLock);
        return TEGRA_HOST1X_ENOMEM;
    }
    if (pad) {
        for (uint32_t at = c.put; at < kPushBytes; at += 4)
            c.va[at / 4] = kOpNop;
        switch_dcache_clean_lines((uint8_t *)c.va + c.put, pad);
        c.put = 0;
    }
    const uint32_t from = c.put;
    for (uint32_t i = 0; i < count; i++)
        c.va[from / 4 + i] = words[i];
    switch_dcache_clean((uint8_t *)c.va + from, bytes);  // its DSB also completes the padding's
    c.put = (from + bytes) % kPushBytes;                 // a job that ends exactly at the RESTART slot leaves PUT at 0
    channelWrite(channel, CH_DMAPUT, c.put);
    IOLockUnlock(gSubmitLock);
    return TEGRA_HOST1X_OK;
}

extern "C" int tegra_host1x_submit(const uint32_t *words, uint32_t count)
{
    return tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_DEFAULT, words, count);
}

extern "C" void tegra_host1x_describe_channel(uint32_t channel, char *buffer, size_t size)
{
    if (!gHost || channel >= TEGRA_HOST1X_CHANNELS) {
        snprintf(buffer, size, "host1x not mapped");
        return;
    }
    const uint32_t cbstat = sync(SYNC_CBSTAT0 + 4 * channel);
    snprintf(buffer, size,
             "ch%u DMAGET %#x DMAPUT %#x DMACTRL %#x FIFOSTAT %#x class %#x offset %#x CBREAD %#x STOP %#x", channel,
             channelRead(channel, CH_DMAGET), channelRead(channel, CH_DMAPUT), channelRead(channel, CH_DMACTRL),
             channelRead(channel, CH_FIFOSTAT), (cbstat >> 16) & 0x3ff, cbstat & 0xffff,
             sync(SYNC_CBREAD0 + 4 * channel), sync(SYNC_CMDPROC_STOP));
}

extern "C" void tegra_host1x_describe(char *buffer, size_t size)
{
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_DEFAULT, buffer, size);
}

// Clients have quiesced (their last fences reached) before this. SC7 powers the sync unit and channels down, so the
// syncpoint values handed out are saved (Linux host1x_syncpt_save) and the channels are stopped.
extern "C" void tegra_host1x_suspend(void)
{
    if (gState != 1)
        return;
    IOLockLock(gSubmitLock);
    for (uint32_t id = kFirstOwn; id < kSyncpts && id < gNextSyncpt; id++)
        gSaved[id] = sync(SYNC_SYNCPT + 4 * id);
    for (uint32_t n = 0; n < TEGRA_HOST1X_CHANNELS; n++)
        channelWrite(n, CH_DMACTRL, DMACTRL_STOP);
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
    IOLockLock(gLock);
    syncConfigure();
    for (uint32_t i = 0; i < kSyncpts / 32; i++)
        gArmed[i] = 0;
    IOLockUnlock(gLock);
    for (uint32_t id = kFirstOwn; id < kSyncpts && id < gNextSyncpt; id++)
        syncWrite(SYNC_SYNCPT + 4 * id, gSaved[id]);
    channelProgram();
    gState = 1;
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
    void (*suspend)(void), (*resume)(void);
} gSleepClients[kSleepClients];

uint32_t gSleepClientCount;

IOLock *lazyLock(IOLock **slot)
{
    if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE)) {
        IOLock *fresh = IOLockAlloc(), *expected = nullptr;
        if (!__atomic_compare_exchange_n(slot, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    return *slot;
}

IOLock *gRegisterLock;
}  // namespace

extern "C" void tegra_host1x_set_sleep_client(void (*suspend)(void), void (*resume)(void))
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
