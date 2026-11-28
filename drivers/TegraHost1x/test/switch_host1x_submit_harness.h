// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraHost1x.cpp and SwitchHost1xChannels.cpp compiled into the test over a memory register file, with the stand-ins
// they call and the Host1xSubmit fixture: one copy for the release shape (switch_host1x_submit_test) and the debug
// one (switch_host1x_probe_resources_test, DEVELOPMENT=1).
#pragma once
#include "switch_host1x_submit_stubs.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "bazel/testing/source_text.h"
#define SWITCH_MMIO_HOST_MODEL        1
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../../../kits/SwitchMMIO/SwitchMMIO.h"

// The host model's publishing write is a plain store, so the drivers' publishing writes are counted here instead.
void switch_test_publish(volatile uint8_t *base, uint32_t offset, uint32_t value);
#define switch_mmio_write32_publish switch_test_publish
#include "../TegraHost1x.cpp"
#include "../SwitchHost1xChannels.cpp"

namespace {
alignas(16384) uint8_t gHostRegisters[kHost1xSize];
alignas(4096) uint8_t gMcRegisters[0x1000];
alignas(4096) uint8_t gPushBuffers[TEGRA_HOST1X_CHANNELS][SwitchHost1xPush::kBytes + 4];
alignas(4096) uint8_t gArenas[TEGRA_HOST1X_CHANNELS][SWITCH_HOST1X_ARENA_WORDS * 4];
uint32_t gAllocated;
std::vector<std::array<uint32_t, TEGRA_HOST1X_CHANNELS>> gDemands;  // every channel's DMAPUT at each demand
std::vector<switch_host1x_fence *> gFencesAdded;
std::vector<std::pair<uint32_t, uint32_t>> gRegisterWrites;
bool gCpuIncrements, gPublishing;
uint32_t gStuckSyncpt;                   // a syncpoint whose CPU increments do not take
std::vector<uint32_t> gPlainIncrements;  // CPU increments written without ordering the stores before them
uint32_t gDisplayTeardowns;
uint32_t gFailedProducer;
int gProducerPhysical;

struct FailedNode {
    uint32_t engine, syncpt, source;
    int physical;
};

std::vector<FailedNode> gFailedNodes;
uint32_t gFailurePasses;
std::mutex gGateCloseMutex;
std::vector<uint32_t> gGateCloses;  // failed producer's syncpoint at each close of the GPU bridge's gate
std::function<void()> gOnGateClose;

constexpr uint64_t kPushPA = 0x80000000u, kArenaPA = 0x90000000u;

uint32_t doorbell(uint32_t channel)
{
    return switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_DMAPUT);
}

uint32_t &syncpoint(uint32_t id)
{
    return *reinterpret_cast<uint32_t *>(gHostRegisters + kSyncOffset + SYNC_SYNCPT + 4 * id);
}

[[maybe_unused]] const uint32_t *push(uint32_t channel)
{
    return reinterpret_cast<const uint32_t *>(gPushBuffers[channel]);
}

[[maybe_unused]] uint32_t arenaAddress(uint32_t channel, const uint32_t *words)
{
    return static_cast<uint32_t>(kArenaPA + channel * 0x100000u +
                                 (reinterpret_cast<const uint8_t *>(words) - gArenas[channel]));
}
}  // namespace

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return *reinterpret_cast<uint32_t *>(address);
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t bits)
{
    *reinterpret_cast<uint32_t *>(address) = bits;
    const uintptr_t from = reinterpret_cast<uintptr_t>(gHostRegisters);
    if (address < from || address - from >= sizeof(gHostRegisters))
        return;
    const uint32_t offset = uint32_t(address - from);
    gRegisterWrites.emplace_back(offset, bits);
    if (offset == kSyncOffset + SYNC_CH_TEARDOWN && (bits & (1u << TEGRA_HOST1X_CHANNEL_DISPLAY)))
        ++gDisplayTeardowns;
    const bool increment = offset >= kSyncOffset + 0xf60 && offset < kSyncOffset + 0xf78;
    if (increment && !gPublishing)
        gPlainIncrements.push_back(bits);
    if (gCpuIncrements && increment) {
        const uint32_t first = (offset - kSyncOffset - 0xf60) / 4 * 32;
        for (uint32_t bit = 0; bit < 32; ++bit)
            if ((bits & (1u << bit)) && first + bit != gStuckSyncpt)
                ++syncpoint(first + bit);
    }
}

void switch_test_publish(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    gPublishing = true;
    switch_mmio_write32(base, offset, value);
    gPublishing = false;
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *)
{
    return 1;
}

// A GPU producer's jobless bridge as the display stream sees it: its syncpoint, its status, its stored failure and
// reason, and its owner's release. beforeLock runs once, inside the syncpoint read the submit makes before its lock, as a
// failure handled then would.
struct switch_gpu_bridge {
    uint32_t syncpoint, threshold;
    int status;
    uint32_t releases;
    bool lockedWhenAsked;
    int failed = 0, why = 0, sourceFailure = 0;
    bool sourceReadLocked = false;
    bool failedReadUnlocked = false, whyReadAfterRelease = false;
    unsigned statusReads = 0;
    void (*beforeLock)(switch_gpu_bridge *) = nullptr;
};

extern "C" void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *bridge, uint32_t *syncpoint, uint32_t *threshold)
{
    auto *b = const_cast<switch_gpu_bridge *>(bridge);
    *syncpoint = b->syncpoint;
    *threshold = b->threshold;
    if (auto *before = b->beforeLock) {
        b->beforeLock = nullptr;
        before(b);
    }
}

inline bool displayLockHeld()
{
    return gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock && gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock->held;
}

extern "C" int switch_gpu_bridge_status(const switch_gpu_bridge *bridge)
{
    auto *b = const_cast<switch_gpu_bridge *>(bridge);
    b->lockedWhenAsked |= displayLockHeld();
    ++b->statusReads;
    return b->status;
}

extern "C" int switch_gpu_bridge_failed(const switch_gpu_bridge *bridge)
{
    const_cast<switch_gpu_bridge *>(bridge)->failedReadUnlocked |= !displayLockHeld();
    return __atomic_load_n(&bridge->failed, __ATOMIC_ACQUIRE);
}

extern "C" int switch_gpu_bridge_source_failure(const switch_gpu_bridge *bridge)
{
    const_cast<switch_gpu_bridge *>(bridge)->sourceReadLocked |= displayLockHeld();
    return bridge->sourceFailure;
}

extern "C" int switch_gpu_bridge_why(const switch_gpu_bridge *bridge)
{
    EXPECT_FALSE(displayLockHeld());
    const_cast<switch_gpu_bridge *>(bridge)->whyReadAfterRelease |= bridge->releases != 0;
    return bridge->why;
}

extern "C" void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
    EXPECT_FALSE(displayLockHeld());
    ++(*bridge)->releases;
    *bridge = nullptr;
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token, int)
{
    for (const auto &node : gFailedNodes)
        if (node.engine == token->engine && node.syncpt == token->queue)
            return node.physical;
    return token->queue == gFailedProducer ? gProducerPhysical : 0;
}

namespace {
bool failNodes(uint32_t engine, uint32_t syncpt, uint32_t reached)
{
    EXPECT_FALSE(gSubmitLock->held);
    bool changed = false;
    for (auto &node : gFailedNodes) {
        if (node.engine != engine || node.physical < 0)
            continue;
        bool failed = node.source == syncpt && !reached;
        for (const auto &source : gFailedNodes)
            failed |= source.syncpt == node.source && source.physical < 0;
        if (failed) {
            node.physical = -5;
            changed = true;
        }
    }
    return changed;
}
}  // namespace

extern "C" bool switch_nvdec_host1x_cancelled(uint32_t syncpt, uint32_t reached)
{
    ++gFailurePasses;
    return failNodes(SWITCH_GPU_ENGINE_NVDEC, syncpt, reached);
}

extern "C" void switch_gpu_bridge_gate_close(void)
{
    EXPECT_FALSE(gSubmitLock->held);
    {
        std::lock_guard<std::mutex> guard(gGateCloseMutex);
        gGateCloses.push_back(syncpoint(gFailedProducer));
    }
    if (gOnGateClose)
        gOnGateClose();
}

extern "C" bool switch_vic_host1x_cancelled(uint32_t syncpt, uint32_t reached)
{
    bool changed = failNodes(SWITCH_GPU_ENGINE_VIC, syncpt, reached);
    if (syncpt == gFailedProducer && gProducerPhysical >= 0) {
        gProducerPhysical = -5;
        changed = true;
    }
    return changed;
}

extern "C" uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    return reinterpret_cast<uintptr_t>(pa == kHost1xPA ? gHostRegisters : gMcRegisters);
}

extern "C" bool switch_car_clock_enabled(unsigned)
{
    return true;
}

extern "C" bool switch_car_reset_asserted(unsigned)
{
    return false;
}

extern "C" uint32_t switch_car_read(uint32_t)
{
    return SOURCE_PLLP | HOST1X_DIV_2_5;
}

extern "C" void switch_car_write(uint32_t, uint32_t)
{}

extern "C" void switch_car_clock_enable(unsigned)
{}

extern "C" void switch_car_reset_deassert(unsigned)
{}

// SwitchHost1xSmmu.cpp's: the register file has no SMMU behind it.
bool switch_host1x_channels_confine(char *line, size_t size)
{
    snprintf(line, size, "no SMMU in the host test");
    return false;
}

bool switch_host1x_channels_confine_channel(const switch_dma_buffer *, const switch_dma_buffer *)
{
    return true;
}

extern "C" void tegra_mc_hot_reset(uint32_t, uint32_t, bool)
{}

// Each channel's push buffer, then its arena.
bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    const uint32_t channel = gAllocated / 2;
    const bool arena = gAllocated % 2;
    if (channel == TEGRA_HOST1X_CHANNELS || size != (arena ? sizeof(gArenas[0]) : sizeof(gPushBuffers[0])))
        return false;
    ++gAllocated;
    out->va = arena ? gArenas[channel] : gPushBuffers[channel];
    out->pa = (arena ? kArenaPA : kPushPA) + channel * 0x100000u;
    out->size = size;
    return true;
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return &nub;
}

void switch_interrupt_describe(uint32_t, char *buffer, size_t size)
{
    snprintf(buffer, size, "host test");
}

bool switch_host1x_intr_init(IOWorkLoop *, volatile uint8_t *, uint32_t)
{
    return true;
}

void switch_host1x_intr_start()
{}

void switch_host1x_intr_stop()
{}

void switch_host1x_intr_handle()
{}

bool switch_host1x_fence_names(uint32_t)
{
    return false;
}

// SwitchHost1xFence.cpp's and SwitchHost1xErrors.cpp's, which this test does not build: what was failed and rearmed.
std::vector<std::pair<uint32_t, int>> gFencesFailed;
std::vector<uint32_t> gRearmed;

uint32_t switch_host1x_fences_fail(uint32_t id, int status)
{
    gFencesFailed.emplace_back(id, status);
    return 1;
}

std::vector<uint32_t> gFencesResumed;

void switch_host1x_fences_resume(uint32_t id)
{
    gFencesResumed.push_back(id);
}

void switch_host1x_errors_start(IOWorkLoop *, volatile uint8_t *)
{}

void switch_host1x_errors_stop()
{}

void switch_host1x_errors_resume()
{}

extern "C" void switch_host1x_errors_rearm(uint32_t channel)
{
    gRearmed.push_back(channel);
}

bool switch_host1x_intr_filter()
{
    return false;
}

extern "C" void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                                         void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

extern "C" int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t)
{
    gFencesAdded.push_back(fence);
    return TEGRA_HOST1X_OK;
}

extern "C" bool switch_host1x_fence_remove(switch_host1x_fence *)
{
    return true;
}

extern "C" void switch_emc_gpu_submit(void)
{
    std::array<uint32_t, TEGRA_HOST1X_CHANNELS> puts{};
    for (uint32_t channel = 0; channel < TEGRA_HOST1X_CHANNELS; channel++)
        puts[channel] = doorbell(channel);
    gDemands.push_back(puts);
}

namespace {
struct Host1xSubmit : testing::Test {
    void SetUp() override
    {
        ASSERT_EQ(tegra_host1x_start(), TEGRA_HOST1X_OK);
        IOLockLock(gSubmitLock);
        switch_host1x_channels_program(163200);
        IOLockUnlock(gSubmitLock);
        gDemands.clear();
        gFencesAdded.clear();
        gRegisterWrites.clear();
        gDisplayStream = nullptr;
        gCpuIncrements = gPublishing = false;
        gStuckSyncpt = 0;
        gPlainIncrements.clear();
        gDisplayTeardowns = 0;
        gFailedProducer = 0;
        gProducerPhysical = 0;
        gFailedNodes.clear();
        gFailurePasses = 0;
        {
            std::lock_guard<std::mutex> guard(gGateCloseMutex);
            gGateCloses.clear();
        }
        gOnGateClose = nullptr;
        switch_test_sleep = nullptr;
    }

    // An engine's job: SETCLASS, `methods` method pairs, OP_DONE, built where host1x will read it.
    static const uint32_t *job(uint32_t channel, uint32_t methods, uint32_t threshold)
    {
        uint32_t *words = nullptr;
        EXPECT_EQ(switch_host1x_job_reserve(channel, 2 + 3 * methods, &words), TEGRA_HOST1X_OK);
        if (!words)
            return nullptr;
        uint32_t n = 0;
        words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_VIC, 0, 0);
        for (uint32_t i = 0; i < methods; ++i) {
            words[n++] = tegra_host1x_op_incr(0x10, 2);
            words[n++] = i;
            words[n++] = threshold;
        }
        words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, 170);
        EXPECT_EQ(switch_host1x_job_submit(channel, words, n, 170, threshold), TEGRA_HOST1X_OK);
        return words;
    }
};

// A display stream's completion, as the display hears it.
struct DisplayCompletion {
    uint32_t calls = 0;
    int status = 1;

    static void heard(void *context, int status)
    {
        auto &done = *static_cast<DisplayCompletion *>(context);
        EXPECT_FALSE(displayLockHeld());
        ++done.calls;
        done.status = status;
    }
};
}  // namespace
