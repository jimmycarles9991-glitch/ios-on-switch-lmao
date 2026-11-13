// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's command DMA channels (TegraHost1x.h, SwitchHost1xArena.h).
//
// Sources (Linux's host1x driver is GPL-2.0-only, so this is written from it, not copied): hw/channel_hw.c
// host1x_channel_init (channel n at n * 0x4000) and submit_gathers (one GATHER and its address per command buffer),
// hw/cdma_hw.c cdma_start/cdma_flush/cdma_freeze/cdma_resume (DMASTART = push buffer, DMAPUT/DMAGET/DMAEND relative
// to it), cdma.c push_buffer_init (a RESTART after the last slot) and update_cdma_locked (a job's buffers are freed once
// its syncpt_end passes), hw/opcodes.h (GATHER: opcode 6, count in bits 13:0; the gathered words run as opcodes).
// The streams gathered carry their own host1x-class WAIT_SYNCPTs, which host1x's job code was written for (Linux 4.14
// job.c do_waitchks, from NVIDIA's nvhost, patches WAITs at their offsets inside a job's gathers). Mainline turns on
// host1x04/05's gather filter (channel_hw.c host1x_enable_gather_filter: CHANNELCTRL KERNEL_FILTER_GBUFFER) and keeps
// its waits in the push buffer instead, so here the filter is cleared on every channel: it exists to keep a user's
// gathers from host1x's own methods, and every stream here is the kernel's.
// host1x fetches the push buffer and the gathers by physical address with the SMMU bypassed (TegraHost1x.cpp), so both
// are contiguous, below 4 GiB (DMASTART and the GATHER address are 32-bit on host1x05) and cleaned to the point of
// coherency before DMAPUT.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <string.h>
#include "SwitchHost1xArena.h"
#include "SwitchHost1xChannels.h"
#include "SwitchHost1xFence.h"
#include "TegraHost1x.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchIdle/switch_idle.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_HOST1X_STATS 1
#include <os/log.h>
#include "SwitchHost1xGatherStats.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#else
#define SWITCH_HOST1X_STATS 0
#endif

static_assert(SWITCH_HOST1X_ARENA_WORDS <= SwitchHost1xArena::kMaxGather);

namespace {
constexpr uint32_t kChannelStride = 0x4000;

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
    CH_CHANNELCTRL = 0x98,
    CHANNELCTRL_KERNEL_FILTER_GBUFFER = 1u << 2,
};

enum : uint32_t {  // sync unit
    SYNC_CMDPROC_STOP = 0x0ac,
    SYNC_CH_TEARDOWN = 0x0b0,
    SYNC_CBREAD0 = 0xc80,
    SYNC_CBSTAT0 = 0xcc0,
};

struct Channel {
    switch_dma_buffer pushMemory = {}, arenaMemory = {};
    SwitchHost1xPush push;
    SwitchHost1xArena arena{SWITCH_HOST1X_ARENA_WORDS};
    switch_host1x_fence room = {};  // the oldest fenced job's, while a submitter waits for room
    bool roomArmed = false;
    uint32_t waiters = 0;
};

volatile uint8_t *gChannelHost, *gChannelSync;
IOLock *gChannelLock;
Channel gChannels[TEGRA_HOST1X_CHANNELS];
bool gRunning;
uint32_t gFilterStuck;  // channels whose gather filter stayed on when cleared
#if SWITCH_HOST1X_STATS
SwitchHost1xGatherStats gGatherStats;
#endif

uint32_t channelRead(uint32_t channel, uint32_t reg)
{
    return switch_mmio_read32(gChannelHost, channel * kChannelStride + reg);
}

void channelWrite(uint32_t channel, uint32_t reg, uint32_t value)
{
    switch_mmio_write32(gChannelHost, channel * kChannelStride + reg, value);
}

uint32_t syncRead(uint32_t offset)
{
    return switch_mmio_read32(gChannelSync, offset);
}

void syncSet(uint32_t offset, uint32_t value)
{
    switch_mmio_write32(gChannelSync, offset, value);
}

uint32_t *pushRing(Channel &c)
{
    return reinterpret_cast<uint32_t *>(c.pushMemory.va);
}

uint32_t *arenaWords(Channel &c)
{
    return reinterpret_cast<uint32_t *>(c.arenaMemory.va);
}

void wake(Channel &c)
{
    if (c.waiters)
        IOLockWakeup(gChannelLock, &c, false);
}

// The jobs in flight on a channel share its engine's syncpoint, so the walk reads it once.
void reclaim(Channel &c)
{
    uint32_t id = ~0u, value = 0;
    c.arena.reclaim([&](uint32_t syncpt, uint32_t threshold) {
        if (syncpt != id) {
            id = syncpt;
            value = tegra_host1x_syncpt_read(syncpt);
        }
        return static_cast<int32_t>(value - threshold) >= 0;
    });
}

void roomSignaled(switch_host1x_fence *fence, int)
{
    IOLockLock(gChannelLock);
    for (Channel &c : gChannels) {
        if (&c.room != fence)
            continue;
        c.roomArmed = false;
        wake(c);
    }
    IOLockUnlock(gChannelLock);
}

// Lock held. A fence its callback is already on the way for is left to it.
void dropJobs(Channel &c)
{
    c.arena.drop();
    if (c.roomArmed && switch_host1x_fence_remove(&c.room))
        c.roomArmed = false;
    wake(c);
}

void endReservation(Channel &c)
{
    c.arena.cancel();
    wake(c);
}

// Lock held, the words in the channel's reservation: Linux push_buffer_push + cdma_flush for one GATHER.
int gatherLocked(uint32_t channel, const uint32_t *first, uint32_t count, bool fenced, uint32_t syncpt,
                 uint32_t threshold, [[maybe_unused]] bool copied)
{
    Channel &c = gChannels[channel];
    if (!c.arena.reserved())
        return TEGRA_HOST1X_EINVAL;
    const uintptr_t from = reinterpret_cast<uintptr_t>(first), base = reinterpret_cast<uintptr_t>(arenaWords(c));
    const uint32_t at = from >= base && from - base < SWITCH_HOST1X_ARENA_WORDS * 4
                            ? static_cast<uint32_t>((from - base) / 4)
                            : SwitchHost1xArena::kNone;
    if (!gRunning || from % 4 || !c.arena.commit(at, count, fenced, syncpt, threshold)) {
        endReservation(c);
        return gRunning ? TEGRA_HOST1X_EINVAL : TEGRA_HOST1X_ENODEV;
    }
    switch_dcache_clean_lines(first, count * 4);
    const uint32_t slot = c.push.push(pushRing(c), count, static_cast<uint32_t>(c.arenaMemory.pa + at * 4));
    switch_dcache_clean(pushRing(c) + slot / 4, 8);  // its DSB also completes the stream's clean
    switch_emc_gpu_submit();
    channelWrite(channel, CH_DMAPUT, c.push.put);
#if SWITCH_HOST1X_STATS
    gGatherStats.submitted(channel, copied, c.arena.held());
#endif
    wake(c);
    return TEGRA_HOST1X_OK;
}

#if SWITCH_HOST1X_STATS
void statsLine()
{
    char line[200];
    const uint64_t now = switch_now_us();
    IOLockLock(gChannelLock);
    const bool due = gGatherStats.line(now, line, sizeof(line));
    IOLockUnlock(gChannelLock);
    if (due)
        os_log(OS_LOG_DEFAULT, "%s", line);
}
#endif

bool validChannel(uint32_t channel)
{
    return channel < TEGRA_HOST1X_CHANNELS && gChannelLock;
}
}  // namespace

bool switch_host1x_channels_alloc(volatile uint8_t *host, volatile uint8_t *sync, IOLock *lock)
{
    for (Channel &c : gChannels)
        if (!switch_dma_alloc(&c.pushMemory, SwitchHost1xPush::kBytes + 4) ||
            !switch_dma_alloc(&c.arenaMemory, SWITCH_HOST1X_ARENA_WORDS * 4))
            return false;
    gChannelHost = host;
    gChannelSync = sync;
    gChannelLock = lock;
    return true;
}

void switch_host1x_channels_program()
{
    uint32_t stuck = 0;
    for (uint32_t n = 0; n < TEGRA_HOST1X_CHANNELS; n++) {
        Channel &c = gChannels[n];
        uint32_t *ring = pushRing(c);
        memset(ring, 0, SwitchHost1xPush::kBytes + 4);
        ring[SwitchHost1xPush::kBytes / 4] = SwitchHost1xPush::restart(0);
        switch_dcache_clean(ring, SwitchHost1xPush::kBytes + 4);
        syncSet(SYNC_CMDPROC_STOP, syncRead(SYNC_CMDPROC_STOP) & ~(1u << n));
        channelWrite(n, CH_DMACTRL, DMACTRL_STOP);
        channelWrite(n, CH_CHANNELCTRL, channelRead(n, CH_CHANNELCTRL) & ~CHANNELCTRL_KERNEL_FILTER_GBUFFER);
        if (channelRead(n, CH_CHANNELCTRL) & CHANNELCTRL_KERNEL_FILTER_GBUFFER)
            stuck |= 1u << n;
        channelWrite(n, CH_DMASTART, static_cast<uint32_t>(c.pushMemory.pa));
        channelWrite(n, CH_DMAPUT, 0);
        channelWrite(n, CH_DMAEND, SwitchHost1xPush::kBytes + 4);
        channelWrite(n, CH_DMACTRL, DMACTRL_STOP | DMACTRL_GETRST | DMACTRL_INITGET);
        channelWrite(n, CH_DMACTRL, 0);
        c.push.put = 0;
        dropJobs(c);
    }
    gFilterStuck = stuck;
    gRunning = true;
}

uint32_t switch_host1x_channels_filter_stuck()
{
    return gFilterStuck;
}

void switch_host1x_channels_stop()
{
    for (uint32_t n = 0; n < TEGRA_HOST1X_CHANNELS; n++)
        channelWrite(n, CH_DMACTRL, DMACTRL_STOP);
    gRunning = false;
}

extern "C" int switch_host1x_job_reserve(uint32_t channel, uint32_t capacity, uint32_t **words)
{
    if (!words || !capacity || capacity > SWITCH_HOST1X_ARENA_WORDS || channel >= TEGRA_HOST1X_CHANNELS)
        return TEGRA_HOST1X_EINVAL;
    if (!validChannel(channel))
        return TEGRA_HOST1X_ENODEV;
    Channel &c = gChannels[channel];
    IOLockLock(gChannelLock);
    int ret = gRunning ? TEGRA_HOST1X_OK : TEGRA_HOST1X_ENODEV;
    if (!ret) {
        reclaim(c);
        const uint32_t at = c.arena.reserve(capacity);
        if (at == SwitchHost1xArena::kNone)
            ret = TEGRA_HOST1X_EAGAIN;
        else
            *words = arenaWords(c) + at;
    }
    IOLockUnlock(gChannelLock);
    return ret;
}

// Sleeps on the oldest fenced job's fence (or the open reservation's end); only streams without a fence in flight and
// still no room is TEGRA_HOST1X_ENOMEM, as nothing would wake it.
extern "C" int switch_host1x_job_wait_room(uint32_t channel, uint32_t capacity)
{
    if (!capacity || capacity > SWITCH_HOST1X_ARENA_WORDS || channel >= TEGRA_HOST1X_CHANNELS)
        return TEGRA_HOST1X_EINVAL;
    if (!validChannel(channel))
        return TEGRA_HOST1X_ENODEV;
    Channel &c = gChannels[channel];
    int ret = TEGRA_HOST1X_OK;
    IOLockLock(gChannelLock);
    while (!ret) {
        if (!gRunning) {
            ret = TEGRA_HOST1X_ENODEV;
            break;
        }
        reclaim(c);
        if (c.arena.fits(capacity))
            break;
        if (!c.arena.reserved() && !c.roomArmed) {
            const SwitchHost1xArenaEntry *oldest = c.arena.oldestFenced();
            if (!oldest) {
                ret = TEGRA_HOST1X_ENOMEM;
                break;
            }
            switch_host1x_fence_init(&c.room, oldest->syncpt, oldest->threshold, roomSignaled);
            ret = switch_host1x_fence_add(&c.room, 0);
            if (ret)
                break;
            c.roomArmed = true;
        }
#if SWITCH_HOST1X_STATS
        gGatherStats.waited(channel);
#endif
        ++c.waiters;
        IOLockSleep(gChannelLock, &c, THREAD_UNINT);
        --c.waiters;
    }
    IOLockUnlock(gChannelLock);
    return ret;
}

extern "C" int switch_host1x_job_begin(uint32_t channel, uint32_t capacity, uint32_t **words)
{
    for (;;) {
        const int reserved = switch_host1x_job_reserve(channel, capacity, words);
        if (reserved != TEGRA_HOST1X_EAGAIN)
            return reserved;
        if (const int waited = switch_host1x_job_wait_room(channel, capacity))
            return waited;
    }
}

extern "C" int switch_host1x_job_submit(uint32_t channel, const uint32_t *first, uint32_t count, uint32_t syncpt,
                                        uint32_t threshold)
{
    if (!validChannel(channel))
        return TEGRA_HOST1X_EINVAL;
    IOLockLock(gChannelLock);
    const int ret = gatherLocked(channel, first, count, true, syncpt, threshold, false);
    IOLockUnlock(gChannelLock);
#if SWITCH_HOST1X_STATS
    statsLine();
#endif
    return ret;
}

extern "C" void switch_host1x_job_cancel(uint32_t channel)
{
    if (!validChannel(channel))
        return;
    IOLockLock(gChannelLock);
    endReservation(gChannels[channel]);
    IOLockUnlock(gChannelLock);
}

namespace {
int copy(uint32_t channel, const uint32_t *words, uint32_t count, bool fenced, uint32_t syncpt, uint32_t threshold)
{
    if (!words || !count || count > SWITCH_HOST1X_ARENA_WORDS || channel >= TEGRA_HOST1X_CHANNELS)
        return TEGRA_HOST1X_EINVAL;
    if (!validChannel(channel))
        return TEGRA_HOST1X_ENODEV;
    Channel &c = gChannels[channel];
    IOLockLock(gChannelLock);
    int ret = gRunning ? TEGRA_HOST1X_OK : TEGRA_HOST1X_ENODEV;
    if (!ret) {
        reclaim(c);
        const uint32_t at = c.arena.reserve(count);
        if (at == SwitchHost1xArena::kNone) {
            ret = TEGRA_HOST1X_ENOMEM;
        } else {
            memcpy(arenaWords(c) + at, words, count * 4);
            ret = gatherLocked(channel, arenaWords(c) + at, count, fenced, syncpt, threshold, true);
        }
    }
    IOLockUnlock(gChannelLock);
    return ret;
}
}  // namespace

extern "C" int switch_host1x_job_copy(uint32_t channel, const uint32_t *words, uint32_t count, uint32_t syncpt,
                                      uint32_t threshold)
{
    return copy(channel, words, count, true, syncpt, threshold);
}

extern "C" int tegra_host1x_submit_on(uint32_t channel, const uint32_t *words, uint32_t count)
{
    return copy(channel, words, count, false, 0, 0);
}

extern "C" int tegra_host1x_submit(const uint32_t *words, uint32_t count)
{
    return tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_DEFAULT, words, count);
}

extern "C" void tegra_host1x_describe_channel(uint32_t channel, char *buffer, size_t size)
{
    if (!gChannelHost || channel >= TEGRA_HOST1X_CHANNELS) {
        snprintf(buffer, size, "host1x not mapped");
        return;
    }
    const uint32_t cbstat = syncRead(SYNC_CBSTAT0 + 4 * channel);
    snprintf(buffer, size,
             "ch%u DMAGET %#x DMAPUT %#x DMACTRL %#x FIFOSTAT %#x class %#x offset %#x CBREAD %#x STOP %#x", channel,
             channelRead(channel, CH_DMAGET), channelRead(channel, CH_DMAPUT), channelRead(channel, CH_DMACTRL),
             channelRead(channel, CH_FIFOSTAT), (cbstat >> 16) & 0x3ff, cbstat & 0xffff,
             syncRead(SYNC_CBREAD0 + 4 * channel), syncRead(SYNC_CMDPROC_STOP));
}

extern "C" void tegra_host1x_describe(char *buffer, size_t size)
{
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_DEFAULT, buffer, size);
}

// Linux cdma_freeze: the channel's command processor stops and the channel is torn down, so the methods it was
// feeding an engine that stopped taking them are dropped. The engine is reset before tegra_host1x_channel_restart.
extern "C" void tegra_host1x_channel_teardown(uint32_t channel)
{
    if (!validChannel(channel))
        return;
    IOLockLock(gChannelLock);
    if (gRunning) {
        syncSet(SYNC_CMDPROC_STOP, syncRead(SYNC_CMDPROC_STOP) | 1u << channel);
        channelWrite(channel, CH_DMACTRL, DMACTRL_STOP);
        syncSet(SYNC_CH_TEARDOWN, 1u << channel);
    }
    IOLockUnlock(gChannelLock);
}

// Linux cdma_resume with nothing left to run: GET is set to PUT (INITGET loads it from PUT), so the torn-down stream
// is skipped, the next submit runs first, and nothing in the arena is fetched again.
extern "C" void tegra_host1x_channel_restart(uint32_t channel)
{
    if (!validChannel(channel))
        return;
    IOLockLock(gChannelLock);
    if (gRunning) {
        channelWrite(channel, CH_DMACTRL, DMACTRL_STOP);
        channelWrite(channel, CH_DMAPUT, gChannels[channel].push.put);
        channelWrite(channel, CH_DMACTRL, DMACTRL_STOP | DMACTRL_GETRST | DMACTRL_INITGET);
        syncSet(SYNC_CMDPROC_STOP, syncRead(SYNC_CMDPROC_STOP) & ~(1u << channel));
        channelWrite(channel, CH_DMACTRL, 0);
        dropJobs(gChannels[channel]);
    }
    IOLockUnlock(gChannelLock);
}
