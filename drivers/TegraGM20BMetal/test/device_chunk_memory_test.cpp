// SPDX-License-Identifier: MIT
#include "device_harness.h"
#include "../grow_ahead.h"
#include <mach/mach.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

using kernel_model::model;

constexpr uint32_t kBgra8 = 80, kRgba16Float = 115;

class ChunkMemory : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::RenderServer)) << err;
        importsAtOpen = model.imports.size();
    }

    size_t importsSinceOpen() const
    {
        return model.imports.size() - importsAtOpen;
    }

    size_t importsAtOpen = 0;
};

// A render server's chunks (pool, block-linear) are cached BoAllocs, which the kernel serves from its zeroed spares: no
// import of anonymous pages, which the CPU zero-fills on the creating thread (boot 21: 9 ms per 16 MiB chunk)
TEST_F(ChunkMemory, TargetsOfAScaleFrameImportNoAnonymousPages)
{
    gm20b::Texture screen, scaled, blur;
    ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, screen, false, true));
    ASSERT_TRUE(dev.createTexture(2048, 1288, kBgra8, scaled, false, true));
    ASSERT_TRUE(dev.createTexture(1344, 768, kRgba16Float, blur, false, true));
    gm20b::Buffer shared;
    ASSERT_TRUE(dev.createBuffer(64 << 10, shared));
    EXPECT_EQ(importsSinceOpen(), 0u);
    ASSERT_NE(scaled.cpu, nullptr);
    scaled.cpu[0] = 1;
    dev.destroyTexture(scaled);
    gm20b::Texture again;
    ASSERT_TRUE(dev.createTexture(2048, 1288, kBgra8, again, false, true));
    EXPECT_EQ(importsSinceOpen(), 0u);
    dev.destroyTexture(again);
    dev.destroyTexture(blur);
    dev.destroyTexture(screen);
}

TEST_F(ChunkMemory, AChunkGivenBackIsFreedNotLeftMapped)
{
    gm20b::Texture big;
    ASSERT_TRUE(dev.createTexture(2048, 2160, kBgra8, big, false, true));
    const size_t live = model.bos.size();
    dev.destroyTexture(big);
    dev.trim();
    EXPECT_LT(model.bos.size(), live);
}

// A launching app's chunks are the kernel's spares too (22 Nov: Photos, Maps and MercuryPosterExtension zero-filled 4-16
// MiB chunks of their own pages on the creating thread, 4.3-9.1 ms each): no import of the app's anonymous pages
TEST(ClientChunkMemory, AClientsChunksAreCachedBoAllocsNotImportsOfItsPages)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    const size_t before = model.imports.size();
    gm20b::Texture tiled, pitch, launch;
    ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, tiled, false, true));
    ASSERT_TRUE(dev.createTexture(256, 256, kBgra8, pitch, false, false));
    ASSERT_TRUE(dev.createTexture(1792, 1472, kRgba16Float, launch, false, true));
    gm20b::Buffer pooled;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled));
    EXPECT_EQ(model.imports.size(), before);
    dev.destroyBuffer(pooled);
    dev.destroyTexture(launch);
    dev.destroyTexture(pitch);
    dev.destroyTexture(tiled);
}

// A no-copy buffer over a buffer's own contents would import a cached BO's pages from past its first page, which the
// kernel hands to another process's BoAlloc once the chunk goes: refused with its line, nothing imported
TEST(ClientChunkMemory, ANoCopyBufferOverTheDevicesOwnMemoryIsRefused)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gm20b::Buffer pooled;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled, false));
    ASSERT_NE(pooled.cpu, nullptr);
    const size_t before = model.imports.size();
    gm20b::Buffer over;
    bool tiled = false;
    EXPECT_FALSE(dev.createBufferNoCopy(pooled.cpu + 16384, 16384, over, tiled));
    EXPECT_FALSE(tiled);
    EXPECT_EQ(model.imports.size(), before);
    dev.destroyBuffer(pooled);
}

// The memory line's split: a chunk a few small buffers keep is pinned until a quarter of it is used; a shared heap (the
// task's pages) and a private one (a kernel BO no task maps, outside the footprint) count apart
TEST(ClientChunkMemory, TheHeldSplitNamesAPinnedChunkAndEachHeapApart)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    constexpr uint64_t kSmall = 64 << 10;
    std::vector<gm20b::Buffer> buffers(1);
    ASSERT_TRUE(dev.createBuffer(kSmall, buffers[0]));
    gm20b::Device::Held h;
    ASSERT_TRUE(dev.held(h, false));
    ASSERT_EQ(h.chunkCount, 1u);
    EXPECT_EQ(h.pinnedCount, 1u) << "one 64 KiB buffer in a " << (h.chunks >> 10) << " KiB chunk";
    EXPECT_EQ(h.pinned, h.chunks);
    const uint64_t chunk = h.chunks;
    while (buffers.size() * kSmall * 4 <= chunk) {
        buffers.emplace_back();
        ASSERT_TRUE(dev.createBuffer(kSmall, buffers.back()));
    }
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.chunks, chunk) << "the buffers share the one chunk";
    EXPECT_EQ(h.pinnedCount, 0u) << "more than a quarter of it used";
    EXPECT_EQ(h.heaps, 0u);
    uint32_t shared = 0, gpuOnly = 0;
    ASSERT_TRUE(dev.createArena(4 << 20, false, shared));
    ASSERT_TRUE(dev.createArena(2 << 20, true, gpuOnly));
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.heaps, uint64_t(4 << 20));
    EXPECT_EQ(h.gpuHeaps, uint64_t(2 << 20));
    EXPECT_EQ(h.chunks, chunk) << "a heap is not a chunk";
    dev.destroyArena(gpuOnly);
    dev.destroyArena(shared);
    for (gm20b::Buffer &b : buffers)
        dev.destroyBuffer(b);
}

TEST(GrowAhead, AChunkAheadKeepsAProcessBelowEightyPercentOfItsLimit)
{
    constexpr uint64_t kMiB = 1ull << 20;
    EXPECT_TRUE(gm20b::clientGrowAhead(10 * kMiB, 90 * kMiB, 4 * kMiB));
    EXPECT_TRUE(gm20b::clientGrowAhead(75 * kMiB, 25 * kMiB, 4 * kMiB)) << "79 of 100 MiB";
    EXPECT_FALSE(gm20b::clientGrowAhead(76 * kMiB, 24 * kMiB, 4 * kMiB)) << "80 of 100 MiB";
    EXPECT_FALSE(gm20b::clientGrowAhead(9 * kMiB, 6 * kMiB, 4 * kMiB)) << "a daemon's 15 MB: 13 MB";
    EXPECT_FALSE(gm20b::clientGrowAhead(10 * kMiB, 0, 4 * kMiB)) << "at its limit, or none reported";
}

// M28: a client's grower starts at its second stall, not its first, and makes a chunk ahead of need only while that
// chunk keeps the process below 80% of its footprint limit
TEST(ClientChunkMemory, AClientGrowsAheadFromItsSecondStallOnlyBelowEightyPercentOfItsLimit)
{
    for (const bool room : {true, false}) {
        kernel_model::model = {};
        gm20b::Device dev;
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        dev.footprintProbe = [room](uint64_t &footprint, uint64_t &available) {
            footprint = room ? 10ull << 20 : 80ull << 20;
            available = room ? 990ull << 20 : 20ull << 20;
            return true;
        };
        constexpr uint64_t kSmall = 64 << 10;
        std::vector<gm20b::Buffer> buffers;
        gm20b::Device::Held h;
        do {
            buffers.emplace_back();
            ASSERT_TRUE(dev.createBuffer(kSmall, buffers.back()));
            ASSERT_TRUE(dev.held(h, true));
            if (h.chunkCount < 2)
                EXPECT_FALSE(dev.growerRunning()) << "the first stall leaves the client as it was";
        } while (h.chunkCount < 2 && buffers.size() < 4096);
        EXPECT_TRUE(dev.growerRunning()) << "the second stall starts it";
        // the allocation that stalled the second time, served from the chunk it made, finds less free than the next
        // chunk: with room the grower makes that one ahead, once
        buffers.emplace_back();
        ASSERT_TRUE(dev.createBuffer(kSmall, buffers.back()));
        for (int waited = 0; waited < 200 && (!room || h.chunkCount < 3); ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(room ? 10 : 1));
            ASSERT_TRUE(dev.held(h, true));
            if (!room && waited == 50)
                break;
        }
        EXPECT_EQ(h.chunkCount, room ? 3u : 2u)
            << (room ? "one chunk ahead of need" : "no chunk ahead at 80% of its limit");
        for (gm20b::Buffer &b : buffers)
            dev.destroyBuffer(b);
    }
}

namespace {
gm20b::Device *gChunkCheckDev = nullptr;
std::atomic<int> gChunkLockViolations{0};

int checkChunkCallsOffLock(uint32_t method, const void *input)
{
    if (!gChunkCheckDev)
        return KERN_SUCCESS;
    if (method == TegraGM20BMethodBoPark || method == TegraGM20BMethodBoUnpark || method == TegraGM20BMethodBoFree ||
        (method == TegraGM20BMethodBoAlloc &&
         (static_cast<const TegraGM20BAbiBoAlloc *>(input)->flags & TegraGM20BBoControl) == 0)) {
        if (!gm20b::DeviceHarness::lockFree(*gChunkCheckDev))
            gChunkLockViolations.fetch_add(1, std::memory_order_relaxed);
    }
    return KERN_SUCCESS;
}

vm_prot_t pageProtection(const void *addr)
{
    vm_address_t region = vm_address_t(uintptr_t(addr));
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info = {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &count, &obj) != KERN_SUCCESS ||
        region > uintptr_t(addr) || region + size <= uintptr_t(addr))
        return VM_PROT_ALL;
    return info.protection;
}
}  // namespace

TEST(ClientChunkMemory, IdleChunksParkWithGuardOnTrimAndUnparkResidentSameVaOffLock)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    ASSERT_TRUE(kernel_model::model.coherentFromPool);
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gChunkCheckDev = &dev;
    gChunkLockViolations.store(0, std::memory_order_relaxed);
    kernel_model::arriving.store(checkChunkCallsOffLock, std::memory_order_release);
    kernel_model::HooksCleared clearHooks;

    gm20b::Buffer pooled, dedicated, wc;
    gm20b::Texture tiled;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled, false, false));
    ASSERT_TRUE(dev.createBuffer(4 << 20, dedicated, false, false));
    ASSERT_TRUE(dev.createBuffer(64 << 10, wc, true, true));
    ASSERT_TRUE(dev.createTexture(512, 512, kBgra8, tiled, false, true));

    const uint64_t pooledVa = pooled.va, dedicatedVa = dedicated.va, tiledVa = tiled.va;
    uint8_t *const pooledCpu = pooled.cpu;
    uint8_t *const dedicatedCpu = dedicated.cpu;
    ASSERT_NE(pooledCpu, nullptr);
    ASSERT_NE(dedicatedCpu, nullptr);
    ASSERT_NE(wc.cpu, nullptr);
    EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, pooledVa, pooled.length));
    EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, dedicatedVa, dedicated.length));
    EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, tiledVa, 4096));
    wc.cpu[0] = 0x33;

    dev.destroyBuffer(pooled);
    dev.destroyBuffer(dedicated);
    dev.destroyBuffer(wc);
    dev.destroyTexture(tiled);

    const size_t importsBefore = model.imports.size();
    dev.trim();

    EXPECT_GE(model.parkCalls, 3u);
    EXPECT_GT(model.parkedBytes, 0u);
    EXPECT_EQ(model.badUnmaps, 0u);
    EXPECT_EQ(gChunkLockViolations.load(), 0);
    EXPECT_EQ(pageProtection(pooledCpu), VM_PROT_NONE);
    EXPECT_EQ(pageProtection(dedicatedCpu), VM_PROT_NONE);
    EXPECT_EQ(dev.wcChunkBytesHeld(), 0u);
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, pooledVa, 4096));
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, dedicatedVa, 4096));
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, tiledVa, 4096));

    gm20b::Device::Held h = {};
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.chunks, 0u);
    EXPECT_EQ(h.chunkCount, 0u);
    EXPECT_EQ(h.writeCombined, 0u);

    gm20b::Buffer pooled2, dedicated2;
    gm20b::Texture tiled2;
    const uint32_t generationBefore = model.sets[0].generation;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled2, false, false));
    ASSERT_TRUE(dev.createBuffer(4 << 20, dedicated2, false, false));
    ASSERT_TRUE(dev.createTexture(512, 512, kBgra8, tiled2, false, true));

    EXPECT_GE(model.unparkResident, 3u);
    EXPECT_EQ(model.unparkReclaimed, 0u);
    EXPECT_EQ(model.imports.size(), importsBefore);
    EXPECT_EQ(model.badUnmaps, 0u);
    EXPECT_EQ(gChunkLockViolations.load(), 0);
    EXPECT_EQ(pooled2.va, pooledVa);
    EXPECT_EQ(pooled2.cpu, pooledCpu);
    EXPECT_EQ(dedicated2.va, dedicatedVa);
    EXPECT_EQ(dedicated2.cpu, dedicatedCpu);
    EXPECT_EQ(tiled2.va, tiledVa);
    EXPECT_EQ(model.sets[0].generation, generationBefore) << "an unpark made a ResourceSetCreate on its caller";
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, pooled2.va, pooled2.length));
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, dedicated2.va, dedicated2.length));
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, tiled2.va, 4096));
    gm20b::CommandBuffer *next = dev.begin();
    ASSERT_NE(next, nullptr);
    next->abandon();
    EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, pooled2.va, pooled2.length));
    EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, dedicated2.va, dedicated2.length));
    EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, tiled2.va, 4096));

    pooled2.cpu[0] = 0x44;
    dedicated2.cpu[0] = 0x55;
    dev.destroyBuffer(pooled2);
    dev.destroyBuffer(dedicated2);
    dev.destroyTexture(tiled2);

    // Overflow system-WC chunks (outside the GPU pool) also park and unpark resident at the same VA.
    model.coherentFromPool = false;
    const unsigned parksBeforeSysWc = model.parkCalls;
    const unsigned unparksBeforeSysWc = model.unparkResident;
    gm20b::Buffer wcSys;
    ASSERT_TRUE(dev.createBuffer(64 << 10, wcSys, true, true));
    const uint64_t wcSysVa = wcSys.va;
    uint8_t *const wcSysCpu = wcSys.cpu;
    ASSERT_NE(wcSysCpu, nullptr);
    wcSysCpu[0] = 0x33;
    dev.destroyBuffer(wcSys);
    dev.trim();
    EXPECT_GT(model.parkCalls, parksBeforeSysWc);
    EXPECT_EQ(pageProtection(wcSysCpu), VM_PROT_NONE);

    gm20b::Buffer wcSys2;
    ASSERT_TRUE(dev.createBuffer(64 << 10, wcSys2, true, true));
    EXPECT_GT(model.unparkResident, unparksBeforeSysWc);
    EXPECT_EQ(wcSys2.va, wcSysVa);
    EXPECT_EQ(wcSys2.cpu, wcSysCpu);
    wcSys2.cpu[0] = 0x66;
    dev.destroyBuffer(wcSys2);
    EXPECT_EQ(model.badUnmaps, 0u);
    EXPECT_EQ(gChunkLockViolations.load(), 0);
}

TEST(ClientChunkMemory, ReclaimedParkedChunksFreeCleanlyAndReallocateWithoutLeak)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Buffer pooled, dedicated;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled, false));
    ASSERT_TRUE(dev.createBuffer(4 << 20, dedicated, false));
    dev.destroyBuffer(pooled);
    dev.destroyBuffer(dedicated);
    dev.trim();

    ASSERT_GE(model.parkedOrder.size(), 2u);
    const std::vector<uint64_t> parkedHandles = model.parkedOrder;
    ASSERT_EQ(model.reclaimAllParked(), parkedHandles.size());

    gm20b::Buffer pooled2, dedicated2;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled2, false));
    ASSERT_TRUE(dev.createBuffer(4 << 20, dedicated2, false));
    EXPECT_GE(model.unparkReclaimed, 2u);
    EXPECT_EQ(model.badUnmaps, 0u);
    for (uint64_t h : parkedHandles)
        EXPECT_EQ(model.bos.count(h), 0u) << "reclaimed chunk BO " << h << " leaked";
    memset(pooled2.cpu, 0x7A, 4096);
    memset(dedicated2.cpu, 0x7B, 4096);
    EXPECT_EQ(pooled2.cpu[4095], 0x7A);
    EXPECT_EQ(dedicated2.cpu[4095], 0x7B);

    dev.destroyBuffer(pooled2);
    dev.destroyBuffer(dedicated2);
}

TEST(ClientChunkMemory, ParkRefusalFallsBackToFreeAndCloseReleasesParkedChunks)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    uint64_t parkedHandle = 0;
    {
        gm20b::Device dev;
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

        model.refuseParkCount = 1;
        model.refuseParkError = kIOReturnBusy;
        gm20b::Buffer pooled;
        ASSERT_TRUE(dev.createBuffer(64 << 10, pooled, false));
        dev.destroyBuffer(pooled);
        dev.trim();
        EXPECT_EQ(model.parkRefusals, 1u);
        EXPECT_TRUE(model.parkedOrder.empty());
        gm20b::Device::Held h = {};
        ASSERT_TRUE(dev.held(h, false));
        EXPECT_EQ(h.chunks, 0u);

        gm20b::Buffer wc;
        ASSERT_TRUE(dev.createBuffer(64 << 10, wc, true));
        dev.destroyBuffer(wc);
        dev.trim();
        EXPECT_EQ(dev.wcChunkBytesHeld(), 0u);
        EXPECT_TRUE(model.parkedOrder.empty());

        gm20b::Buffer keepParked;
        ASSERT_TRUE(dev.createBuffer(64 << 10, keepParked, false));
        dev.destroyBuffer(keepParked);
        dev.trim();
        ASSERT_EQ(model.parkedOrder.size(), 1u);
        parkedHandle = model.parkedOrder.front();
    }
    EXPECT_EQ(model.bos.count(parkedHandle), 0u);
    EXPECT_TRUE(model.parkedOrder.empty());
    EXPECT_EQ(model.badUnmaps, 0u);
}

TEST(ClientChunkMemory, NoCopyBufferRefusesLiveAndParkedWcChunksAndSharedHeaps)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    kernel_model::model.coherentFromPool = false;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    const size_t importsBefore = model.imports.size();

    gm20b::Buffer wc;
    ASSERT_TRUE(dev.createBuffer(64 << 10, wc, true, true));
    uint8_t *const wcCpu = wc.cpu;
    ASSERT_NE(wcCpu, nullptr);
    gm20b::Buffer over;
    bool tiled = false;
    EXPECT_FALSE(dev.createBufferNoCopy(wcCpu + 4096, 4096, over, tiled));
    dev.destroyBuffer(wc);
    dev.trim();
    ASSERT_GE(model.parkCalls, 1u);
    EXPECT_FALSE(dev.createBufferNoCopy(wcCpu + 4096, 4096, over, tiled));

    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(1u << 20, false, arena));
    gm20b::Buffer placed;
    ASSERT_TRUE(dev.arenaBuffer(arena, 8192, placed));
    uint8_t *const arenaCpu = placed.cpu;
    ASSERT_NE(arenaCpu, nullptr);
    EXPECT_FALSE(dev.createBufferNoCopy(arenaCpu + 4096, 4096, over, tiled));

    std::atomic<uint32_t> committed{0};
    dev.setArenaVolatile(arena, true);
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    EXPECT_FALSE(dev.createBufferNoCopy(arenaCpu + 4096, 4096, over, tiled));
    EXPECT_EQ(model.imports.size(), importsBefore);

    dev.arenaRelease(placed);
    dev.destroyArena(arena);
}

TEST(ClientChunkMemory, GrowLoopUnparksParkedPoolAndBlockLinearChunksBeforeAllocating)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::RenderServer)) << err;
    dev.warm();
    dev.trim();
    ASSERT_GE(model.parkCalls, 4u);

    struct GrowWatch {
        std::mutex mu;
        std::condition_variable cv;
        unsigned unparks = 0;
        unsigned nonControlAllocs = 0;
    };

    static GrowWatch *watch = nullptr;
    GrowWatch state;
    watch = &state;
    kernel_model::arriving.store(
        [](uint32_t method, const void *input) -> int {
            if (!watch)
                return KERN_SUCCESS;
            if (method == TegraGM20BMethodBoUnpark) {
                std::lock_guard<std::mutex> lk(watch->mu);
                ++watch->unparks;
                watch->cv.notify_all();
            } else if (method == TegraGM20BMethodBoAlloc &&
                       (static_cast<const TegraGM20BAbiBoAlloc *>(input)->flags & TegraGM20BBoControl) == 0) {
                std::lock_guard<std::mutex> lk(watch->mu);
                ++watch->nonControlAllocs;
                watch->cv.notify_all();
            }
            return KERN_SUCCESS;
        },
        std::memory_order_release);
    kernel_model::HooksCleared clearHooks;

    gm20b::Buffer b;
    gm20b::Texture t;
    ASSERT_TRUE(dev.createBuffer(64 << 10, b, false));
    ASSERT_TRUE(dev.createTexture(64, 64, kBgra8, t, false, true));
    {
        std::unique_lock<std::mutex> lk(state.mu);
        EXPECT_TRUE(state.cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return state.unparks + state.nonControlAllocs >= 4;
        }));
        EXPECT_EQ(state.nonControlAllocs, 0u);
        EXPECT_GE(state.unparks, 4u);
    }
    kernel_model::clearHooks();
    watch = nullptr;
    dev.destroyBuffer(b);
    dev.destroyTexture(t);
    EXPECT_EQ(model.badUnmaps, 0u);
}

TEST(ClientChunkMemory, UnparkMismatchFreesMismatchedChunkWithoutLeak)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Buffer big;
    ASSERT_TRUE(dev.createBuffer(4 << 20, big, false));
    dev.destroyBuffer(big);
    dev.trim();
    ASSERT_EQ(model.parkedOrder.size(), 1u);
    const uint64_t parkedHandle = model.parkedOrder.front();

    model.overrideUnparkBytes = 4096;
    gm20b::Buffer fresh;
    ASSERT_TRUE(dev.createBuffer(4 << 20, fresh, false));
    model.overrideUnparkBytes = 0;

    EXPECT_EQ(model.bos.count(parkedHandle), 0u);
    EXPECT_EQ(model.badUnmaps, 0u);
    ASSERT_NE(fresh.cpu, nullptr);
    fresh.cpu[0] = 0x5E;
    dev.destroyBuffer(fresh);
}

TEST(ClientChunkMemory, ModelEnforcesCpuLeaseParkRefusalAndParkedVmBindOverlap)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    gm20b::Buffer big;
    ASSERT_TRUE(dev.createBuffer(4 << 20, big, false));
    const uint64_t handle = model.resolve({big.va, TegraGM20BSurfaceGpuAddress, 0});
    ASSERT_NE(handle, 0u);

    ASSERT_EQ(model.cpuLock(handle), KERN_SUCCESS);
    const TegraGM20BAbiBoPark parkIn = {handle, TegraGM20BParkGuard, 0};
    EXPECT_EQ(IOConnectCallStructMethod(kernel_model::kFirstConnection, TegraGM20BMethodBoPark, &parkIn, sizeof parkIn,
                                        nullptr, nullptr),
              kIOReturnBusy);
    model.cpuLocked.clear();

    const uint64_t va = big.va;
    dev.destroyBuffer(big);
    dev.trim();
    ASSERT_EQ(model.bos.at(handle).parkState, kernel_model::ParkState::Parked);

    TegraGM20BAbiVmBind bindIn = {};
    bindIn.count = 1;
    bindIn.ops[0] = {TegraGM20BBindUnmap, 0, 0, 0, va, 4096};
    EXPECT_EQ(IOConnectCallStructMethod(kernel_model::kFirstConnection, TegraGM20BMethodVmBind, &bindIn, sizeof bindIn,
                                        nullptr, nullptr),
              kIOReturnNotReady);
}

}  // namespace
