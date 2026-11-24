// SPDX-License-Identifier: MIT
// gm20b::Device's kept surface imports over the kernel model (device_harness.h): a surface's texture made again after
// its last one went takes the same import (no kernel import, free or layout ask), as Core Animation's texture over its
// pooled surface does every frame; at most eight are kept, the oldest given back first, and trim gives back every one.
#include "device_harness.h"

#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kWidth = 256, kHeight = 64, kPitch = kWidth * 4;
constexpr uint64_t kBytes = 1 << 20;

class KeptImport : public harness::DeviceTest {
protected:
    struct Surface {
        void *pages = nullptr;
        CFTypeRef owner = nullptr;
        uint32_t id = 0;
    };

    std::vector<Surface> surfaces;

    void SetUp() override
    {
        DeviceTest::SetUp();
        for (uint32_t i = 0; i < 10; i++) {
            Surface s;
            ASSERT_EQ(posix_memalign(&s.pages, size_t(getpagesize()), kBytes), 0);
            s.owner = CFDataCreate(nullptr, nullptr, 0);
            s.id = 100 + i;
            surfaces.push_back(s);
        }
    }

    void TearDown() override
    {
        dev.trim();
        for (Surface &s : surfaces) {
            CFRelease(s.owner);
            free(s.pages);
        }
        DeviceTest::TearDown();
    }

    // A texture over surface `s`, made and destroyed: the import it took
    uint64_t frame(const Surface &s)
    {
        gm20b::Texture t;
        EXPECT_TRUE(
            dev.createSurfaceTexture(s.id, s.pages, kBytes, s.owner, 0, kWidth, kHeight, kPitch, kRGBA8, t, true));
        const uint64_t handle = t.import ? t.import->bo.handle : 0;
        dev.destroyTexture(t);
        return handle;
    }

    size_t imports() const
    {
        return kernel_model::model.imports.size();
    }
};

TEST_F(KeptImport, ASurfacesTextureEveryFrameTakesOneImport)
{
    const uint64_t first = frame(surfaces[0]);
    ASSERT_NE(first, 0u);
    const size_t made = imports(), freed = kernel_model::model.freed.size();
    for (int f = 0; f < 30; f++)
        EXPECT_EQ(frame(surfaces[0]), first) << f;
    EXPECT_EQ(imports(), made);
    EXPECT_EQ(kernel_model::model.freed.size(), freed);
}

TEST_F(KeptImport, EightAreKeptAndTrimGivesThemBack)
{
    std::vector<uint64_t> handles;
    for (const Surface &s : surfaces)
        handles.push_back(frame(s));
    // ten surfaces released in order: the two oldest given back, eight kept
    const std::vector<uint64_t> &freed = kernel_model::model.freed;
    ASSERT_EQ(freed.size(), 2u);
    EXPECT_EQ(freed[0], handles[0]);
    EXPECT_EQ(freed[1], handles[1]);
    EXPECT_EQ(frame(surfaces[9]), handles[9]);
    const gm20b::Device::Trimmed trimmed = dev.trim();
    EXPECT_EQ(freed.size(), 10u);
    EXPECT_EQ(trimmed.keptImports, 8u);
    EXPECT_EQ(trimmed.keptImportBytes, 8 * kBytes);
    EXPECT_EQ(dev.trim().keptImports, 0u) << "trim gave every one back";
}

struct FreeWatch {
    static inline gm20b::Device *watched = nullptr;
    static inline std::atomic<int> frees{0}, underLock{0}, heldOk{0};
    static inline std::mutex heldLock;
    static inline std::vector<gm20b::Device::Held> heldSamples;

    static int hook(uint32_t method, const void *)
    {
        if (method != TegraGM20BMethodBoFree)
            return KERN_SUCCESS;
        frees.fetch_add(1, std::memory_order_relaxed);
        bool free = false;
        gm20b::Device::Held h = {};
        bool gotHeld = false;
        std::thread([&] {
            free = watched->lockFree();
            gotHeld = watched->held(h, false);
        }).join();
        if (!free)
            underLock.fetch_add(1, std::memory_order_relaxed);
        if (gotHeld) {
            heldOk.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> l(heldLock);
            heldSamples.push_back(h);
        }
        return KERN_SUCCESS;
    }

    explicit FreeWatch(gm20b::Device &dev)
    {
        watched = &dev;
        frees.store(0, std::memory_order_relaxed);
        underLock.store(0, std::memory_order_relaxed);
        heldOk.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> l(heldLock);
            heldSamples.clear();
        }
        kernel_model::arriving.store(hook);
    }

    ~FreeWatch()
    {
        kernel_model::arriving.store(nullptr);
        watched = nullptr;
    }

    void reset()
    {
        frees.store(0, std::memory_order_relaxed);
        underLock.store(0, std::memory_order_relaxed);
        heldOk.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> l(heldLock);
        heldSamples.clear();
    }
};

TEST_F(KeptImport, TeardownFreesWithTheDeviceLockLetGoAndAccurateHeld)
{
    FreeWatch watch(dev);
    for (const Surface &s : surfaces)
        frame(s);
    EXPECT_EQ(FreeWatch::frees.load(), 2);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "kept-import eviction ran BoFree under Device::lock_";
    EXPECT_EQ(FreeWatch::heldOk.load(), 2);
    for (const gm20b::Device::Held &h : FreeWatch::heldSamples)
        EXPECT_GE(h.kept, 9 * kBytes) << "Device::held() dropped evicted kept import bytes before BoFree finished";

    watch.reset();
    const gm20b::Device::Trimmed trimmed = dev.trim();
    EXPECT_EQ(trimmed.keptImports, 8u);
    EXPECT_EQ(FreeWatch::frees.load(), 8);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "trim() ran BoFree under Device::lock_";
    EXPECT_EQ(FreeWatch::heldOk.load(), 8);
    ASSERT_EQ(FreeWatch::heldSamples.size(), 8u);
    EXPECT_EQ(FreeWatch::heldSamples.front().kept, 8 * kBytes);
    EXPECT_EQ(FreeWatch::heldSamples.back().kept, kBytes);
    EXPECT_EQ(kernel_model::model.freed.size(), 10u) << "trim() returned before freeing every BO";
    gm20b::Device::Held after = {};
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.kept, 0u);
}

TEST_F(KeptImport, NoCopyReleaseFreesSynchronouslyOffTheLockAndAllowsImmediateGrowth)
{
    constexpr uint64_t kSmall = 4 << 10, kBig = 128 << 10;
    void *pages = nullptr;
    ASSERT_EQ(posix_memalign(&pages, size_t(getpagesize()), kBig), 0);
    FreeWatch watch(dev);
    gm20b::Buffer small;
    bool tiled = false;
    ASSERT_TRUE(dev.createBufferNoCopy(pages, kSmall, small, tiled));
    const uint64_t smallHandle = small.import->bo.handle;
    dev.destroyBuffer(small);
    EXPECT_EQ(kernel_model::model.bos.count(smallHandle), 0u) << "no-copy BO stayed live after destroyBuffer returned";
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "no-copy BoFree ran under Device::lock_";

    gm20b::Buffer big;
    ASSERT_TRUE(dev.createBufferNoCopy(pages, kBig, big, tiled));
    const uint64_t bigHandle = big.import->bo.handle;
    EXPECT_NE(bigHandle, smallHandle);
    dev.destroyBuffer(big);
    EXPECT_EQ(kernel_model::model.bos.count(bigHandle), 0u);
    EXPECT_EQ(FreeWatch::frees.load(), 2);
    EXPECT_EQ(FreeWatch::underLock.load(), 0);
    free(pages);
}

TEST_F(KeptImport, ChunksWriteCombinedAndArenasFreeOffTheLockWithAccurateHeld)
{
    FreeWatch watch(dev);

    // Dedicated write-combined buffer: BoFree off lock_, held().writeCombined stays accurate and tables does not spike
    gm20b::Device::Held before = {};
    ASSERT_TRUE(dev.held(before, false));
    gm20b::Buffer bigWc;
    ASSERT_TRUE(dev.createBuffer(2 << 20, bigWc, true));
    ASSERT_TRUE(bigWc.wc);
    watch.reset();
    dev.destroyBuffer(bigWc);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "dedicated WC chunk freed under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 1u);
    EXPECT_GE(FreeWatch::heldSamples[0].writeCombined, uint64_t(2 << 20));
    EXPECT_LE(FreeWatch::heldSamples[0].tables, before.tables);
    gm20b::Device::Held after = {};
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.writeCombined, before.writeCombined);

    // Dedicated pool chunks: 2 kept as spares, 3rd freed immediately off lock_, remaining 2 freed by trim()
    constexpr uint64_t kDedicatedPool = 3 << 20;
    std::vector<gm20b::Buffer> big(3);
    for (gm20b::Buffer &b : big)
        ASSERT_TRUE(dev.createBuffer(kDedicatedPool, b));
    watch.reset();
    for (gm20b::Buffer &b : big)
        dev.destroyBuffer(b);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "dedicated pool chunk free ran under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 1u);
    EXPECT_EQ(FreeWatch::heldSamples[0].chunkCount, 3u);
    EXPECT_EQ(FreeWatch::heldSamples[0].chunks, 3 * kDedicatedPool);
    watch.reset();
    dev.trim();
    EXPECT_EQ(FreeWatch::frees.load(), 2);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "trim chunk free ran under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 2u);
    EXPECT_EQ(FreeWatch::heldSamples.front().chunkCount, 2u);
    EXPECT_EQ(FreeWatch::heldSamples.back().chunkCount, 1u);
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.chunks, 0u);
    EXPECT_EQ(after.chunkCount, 0u);

    // Dedicated block-linear chunk freed immediately off lock_
    gm20b::Texture bigBl;
    ASSERT_TRUE(dev.createTexture(1024, 1024, kRGBA8, bigBl, false, true));
    watch.reset();
    dev.destroyTexture(bigBl);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "dedicated block-linear chunk free ran under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 1u);
    EXPECT_GE(FreeWatch::heldSamples[0].chunkCount, 1u);
    EXPECT_GT(FreeWatch::heldSamples[0].chunks, 0u);
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.chunks, 0u);
    EXPECT_EQ(after.chunkCount, 0u);

    // Shared arena: trimArenas, purgeArena, and destroyArena all run BoFree off lock_ and keep held().heaps accurate
    constexpr uint64_t kArenaBytes = 4 * (64 << 10);
    uint32_t shared = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, shared));
    gm20b::Buffer placed;
    ASSERT_TRUE(dev.arenaBuffer(shared, 64 << 10, placed));
    dev.arenaRelease(placed);
    std::atomic<uint32_t> committed{0};
    watch.reset();
    dev.trimArenas(committed);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "trimArenas ran BoFree under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 1u);
    EXPECT_EQ(FreeWatch::heldSamples[0].heaps, kArenaBytes);

    ASSERT_TRUE(dev.arenaBuffer(shared, 64 << 10, placed));
    dev.arenaRelease(placed);
    dev.setArenaVolatile(shared, true);
    watch.reset();
    dev.purgeArena(shared, committed);
    EXPECT_GE(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "purgeArena (shared) ran BoFree under Device::lock_";
    ASSERT_FALSE(FreeWatch::heldSamples.empty());
    EXPECT_EQ(FreeWatch::heldSamples.front().heaps, kArenaBytes);
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.heaps, 0u);

    ASSERT_TRUE(dev.restoreArena(shared));
    watch.reset();
    dev.destroyArena(shared);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "destroyArena (shared) ran BoFree under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 1u);
    EXPECT_EQ(FreeWatch::heldSamples[0].heaps, kArenaBytes);
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.heaps, 0u);

    // Private arena: purgeArena and destroyArena run BoFree off lock_ and keep held().gpuHeaps accurate
    uint32_t priv = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, priv));
    dev.setArenaVolatile(priv, true);
    watch.reset();
    dev.purgeArena(priv, committed);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "purgeArena (private) ran BoFree under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 1u);
    EXPECT_EQ(FreeWatch::heldSamples[0].gpuHeaps, kArenaBytes);
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.gpuHeaps, 0u);

    ASSERT_TRUE(dev.restoreArena(priv));
    watch.reset();
    dev.destroyArena(priv);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "destroyArena (private) ran BoFree under Device::lock_";
    ASSERT_EQ(FreeWatch::heldSamples.size(), 1u);
    EXPECT_EQ(FreeWatch::heldSamples[0].gpuHeaps, kArenaBytes);
    ASSERT_TRUE(dev.held(after, false));
    EXPECT_EQ(after.gpuHeaps, 0u);
}

TEST(TeardownOffLock, CreationRollbackFreesSynchronouslyOffTheLock)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags = TegraGM20BInfoSurfaceLayout;
    kernel_model::model.surfaceLayout = TegraGM20BLayoutTiled;
    kernel_model::HooksCleared hooksCleared;
    void *pages = nullptr;
    ASSERT_EQ(posix_memalign(&pages, size_t(getpagesize()), kBytes), 0);
    {
        gm20b::Device rollbackDev;
        std::string err;
        ASSERT_TRUE(rollbackDev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        FreeWatch watch(rollbackDev);
        const size_t bosBefore = kernel_model::model.bos.size();
        gm20b::Texture t;
        EXPECT_FALSE(rollbackDev.createSurfaceTexture(100, static_cast<uint8_t *>(pages) + 64, kBytes - 64, nullptr, 0,
                                                      kWidth, kHeight, kPitch, kRGBA8, t, true));
        EXPECT_EQ(FreeWatch::frees.load(), 1);
        EXPECT_EQ(FreeWatch::underLock.load(), 0) << "creation rollback ran BoFree under Device::lock_";
        EXPECT_EQ(kernel_model::model.bos.size(), bosBefore) << "creation rollback stranded its BO";
    }
    free(pages);
}

TEST(TeardownOffLock, DeviceDestructionJoinsWorkersAndFreesKeptImportsOffTheLock)
{
    kernel_model::model = {};
    kernel_model::HooksCleared hooksCleared;
    void *pages[2] = {nullptr, nullptr};
    CFTypeRef owners[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(posix_memalign(&pages[i], size_t(getpagesize()), kBytes), 0);
        owners[i] = CFDataCreate(nullptr, nullptr, 0);
    }
    const CFIndex before0 = CFGetRetainCount(owners[0]);
    const CFIndex before1 = CFGetRetainCount(owners[1]);
    int frees = 0, underLock = 0;
    {
        std::optional<gm20b::Device> local;
        local.emplace();
        std::string err;
        ASSERT_TRUE(local->open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        local->warm();
        for (int i = 0; i < 2; ++i) {
            gm20b::Texture t;
            ASSERT_TRUE(local->createSurfaceTexture(uint64_t(100 + i), pages[i], kBytes, owners[i], 0, kWidth, kHeight,
                                                    kPitch, kRGBA8, t, true));
            local->destroyTexture(t);
        }
        EXPECT_EQ(CFGetRetainCount(owners[0]), before0 + 1);
        EXPECT_EQ(CFGetRetainCount(owners[1]), before1 + 1);
        FreeWatch watch(*local);
        local.reset();
        frees = FreeWatch::frees.load();
        underLock = FreeWatch::underLock.load();
    }
    EXPECT_GE(frees, 2);
    EXPECT_EQ(underLock, 0) << "~Device() ran BoFree under Device::lock_";
    EXPECT_EQ(CFGetRetainCount(owners[0]), before0) << "~Device() leaked kept surface owner 0";
    EXPECT_EQ(CFGetRetainCount(owners[1]), before1) << "~Device() leaked kept surface owner 1";
    for (int i = 0; i < 2; ++i) {
        CFRelease(owners[i]);
        free(pages[i]);
    }
}

#if SWITCH_METAL_DIAGNOSTICS
TEST(TeardownOffLock, CompressedReleaseFreesOffTheLockAndReasksRefusedWants)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags = TegraGM20BInfoCompression;
    kernel_model::HooksCleared hooksCleared;
    gm20b::Device compDev;
    std::string err;
    ASSERT_TRUE(compDev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    auto settle = [&] {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        compDev.compWhenSettled([&] {
            std::lock_guard<std::mutex> l(m);
            done = true;
            cv.notify_one();
        });
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] {
            return done;
        });
    };
    gm20b::Texture small, large;
    EXPECT_FALSE(compDev.createCompressedTexture(256, 256, kRGBA8, gm20b::kCompKindC32, small));
    settle();
    ASSERT_TRUE(compDev.createCompressedTexture(256, 256, kRGBA8, gm20b::kCompKindC32, small));

    // Refuse a larger compressed allocation so small's release cannot keep its BO in comp_.ready and must BoFree it
    kernel_model::model.refuseAllocCount = 1;
    EXPECT_FALSE(compDev.createCompressedTexture(512, 512, kRGBA8, gm20b::kCompKindC32, large));
    settle();

    FreeWatch watch(compDev);
    compDev.destroyTexture(small);
    EXPECT_EQ(FreeWatch::frees.load(), 1);
    EXPECT_EQ(FreeWatch::underLock.load(), 0) << "compRelease ran BoFree under Device::lock_";

    // compGaveBack cleared comp_.refused after BoFree, so asking for the larger texture again now succeeds
    EXPECT_FALSE(compDev.createCompressedTexture(512, 512, kRGBA8, gm20b::kCompKindC32, large));
    settle();
    EXPECT_TRUE(compDev.createCompressedTexture(512, 512, kRGBA8, gm20b::kCompKindC32, large));
    compDev.destroyTexture(large);
}
#endif

}  // namespace
