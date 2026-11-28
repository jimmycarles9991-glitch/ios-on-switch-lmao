// SPDX-License-Identifier: MIT
#include "kernel_model.h"
#include "empty_pack.h"
#include "../gpu.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kBgra8 = 80;

uint32_t submitCopy(gm20b::Device &dev, const gm20b::Buffer &from, const gm20b::Buffer &to)
{
    gm20b::CommandBuffer *cb = dev.begin();
    gm20b::BlitEncoder *e = cb ? cb->blit() : nullptr;
    if (!e)
        return 0;
    cb->use(from, false);
    cb->use(to, true, false);
    e->copyBytes(from.va, to.va, 256);
    e->end();
    uint32_t seqno = 0;
    return cb->commit(seqno) ? seqno : 0;
}

TEST(DeviceParkedTsan, ConcurrentParkUnparkReclaimAllocPurgeRestoreHeldAndSubmitRunRaceFree)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    kernel_model::model.parkCapBytes = 16ull << 20;

    unsigned parkCalls = 0;
    unsigned badUnmaps = 0;
    {
        gm20b::Device dev;
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

        gm20b::Buffer from, to;
        ASSERT_TRUE(dev.createBuffer(4096, from));
        ASSERT_TRUE(dev.createBuffer(4096, to));

        std::atomic<bool> stop{false};
        std::atomic<uint32_t> committed{0};
        std::atomic<int> trims{0}, heldReads{0}, submits{0};

        std::thread releaser([&] {
            while (!stop.load(std::memory_order_acquire)) {
                dev.trim();
                dev.purgeArenas(committed);
                dev.trimArenas(committed);
                trims.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::thread reclaimer([&] {
            while (!stop.load(std::memory_order_acquire)) {
                {
                    std::lock_guard<std::mutex> hold(kernel_model::callLock);
                    kernel_model::model.reclaimAllParked();
                }
                std::this_thread::yield();
            }
        });
        std::thread watcher([&] {
            while (!stop.load(std::memory_order_acquire)) {
                gm20b::Device::Held h = {};
                if (dev.held(h, false))
                    heldReads.fetch_add(1, std::memory_order_relaxed);
                (void)dev.controlMappedBytes();
                (void)dev.wcChunkBytesHeld();
            }
        });
        std::thread submitter([&] {
            while (!stop.load(std::memory_order_acquire)) {
                committed.fetch_add(1, std::memory_order_acq_rel);
                const uint32_t seqno = submitCopy(dev, from, to);
                committed.fetch_sub(1, std::memory_order_acq_rel);
                if (seqno)
                    submits.fetch_add(1, std::memory_order_relaxed);
            }
        });

        auto allocator = [&](int rounds) {
            for (int r = 0; r < rounds; ++r) {
                const bool hostVisible = (r & 1) != 0;
                gm20b::Buffer pooled, dedicated, wc;
                if (dev.createBuffer(64 << 10, pooled, false, hostVisible)) {
                    if (hostVisible)
                        pooled.cpu[0] = uint8_t(r);
                    dev.destroyBuffer(pooled);
                }
                if (dev.createBuffer(4 << 20, dedicated, false, hostVisible)) {
                    if (hostVisible)
                        dedicated.cpu[0] = uint8_t(r);
                    dev.destroyBuffer(dedicated);
                }
                if (dev.createBuffer(64 << 10, wc, true, true)) {
                    wc.cpu[0] = uint8_t(r);
                    dev.destroyBuffer(wc);
                }
                gm20b::Buffer volWc;
                if (dev.createBuffer(64 << 10, volWc, true, true, true)) {
                    volWc.cpu[0] = uint8_t(r);
                    dev.destroyBuffer(volWc);
                }
                gm20b::Texture tiled;
                if (dev.createTexture(512, 512, kBgra8, tiled, false, true))
                    dev.destroyTexture(tiled);

                uint32_t arena = 0;
                if (dev.createArena(512 << 10, false, arena)) {
                    gm20b::Buffer placed;
                    if (dev.arenaBuffer(arena, 4096, placed)) {
                        memset(placed.cpu, 0x5A, 4096);
                        dev.setArenaVolatile(arena, true);
                        dev.purgeArena(arena, committed);
                        EXPECT_TRUE(dev.restoreArena(arena));
                        memset(placed.cpu, 0xA5, 4096);
                        dev.arenaRelease(placed);
                    }
                    dev.destroyArena(arena);
                }
            }
        };

        std::thread allocWorker(allocator, 20);
        allocator(20);
        allocWorker.join();

        stop.store(true, std::memory_order_release);
        submitter.join();
        watcher.join();
        reclaimer.join();
        releaser.join();

        dev.destroyBuffer(from);
        dev.destroyBuffer(to);
        dev.trim();

        EXPECT_GT(trims.load(), 0);
        EXPECT_GT(heldReads.load(), 0);
        EXPECT_GT(submits.load(), 0);
        std::lock_guard<std::mutex> hold(kernel_model::callLock);
        parkCalls = kernel_model::model.parkCalls;
        badUnmaps = kernel_model::model.badUnmaps;
    }

    EXPECT_GT(parkCalls, 0u);
    EXPECT_EQ(badUnmaps, 0u);
    std::lock_guard<std::mutex> hold(kernel_model::callLock);
    EXPECT_TRUE(kernel_model::model.parkedOrder.empty());
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
}

TEST(DeviceParkedTsan, CloseWhileChunksAndHeapsAreParkedAndReclaimedReleasesAllCleanly)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;

    std::vector<uint64_t> parkedHandles;
    {
        gm20b::Device dev;
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

        gm20b::Buffer pooled, dedicated, wc, volWc;
        ASSERT_TRUE(dev.createBuffer(64 << 10, pooled, false));
        ASSERT_TRUE(dev.createBuffer(4 << 20, dedicated, false));
        ASSERT_TRUE(dev.createBuffer(64 << 10, wc, true));
        ASSERT_TRUE(dev.createBuffer(64 << 10, volWc, true, true, true));
        dev.destroyBuffer(pooled);
        dev.destroyBuffer(dedicated);
        dev.destroyBuffer(wc);
        dev.destroyBuffer(volWc);
        dev.trim();

        uint32_t a1 = 0, a2 = 0;
        ASSERT_TRUE(dev.createArena(1u << 20, false, a1));
        ASSERT_TRUE(dev.createArena(1u << 20, false, a2));
        std::atomic<uint32_t> committed{0};
        dev.setArenaVolatile(a1, true);
        dev.setArenaVolatile(a2, true);
        dev.purgeArenas(committed);

        std::lock_guard<std::mutex> hold(kernel_model::callLock);
        parkedHandles = kernel_model::model.parkedOrder;
        ASSERT_GE(parkedHandles.size(), 4u);
        ASSERT_TRUE(kernel_model::model.reclaimParked(parkedHandles.front()));
        ASSERT_TRUE(kernel_model::model.reclaimParked(parkedHandles.back()));
    }

    std::lock_guard<std::mutex> hold(kernel_model::callLock);
    for (uint64_t h : parkedHandles)
        EXPECT_EQ(kernel_model::model.bos.count(h), 0u) << "BO " << h << " leaked on close";
    EXPECT_TRUE(kernel_model::model.parkedOrder.empty());
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
}

TEST(DeviceParkedTsan, TeardownWaitsForInFlightOffLockParkUnparkAndAllocWithoutUaf)
{
    struct Gate {
        std::mutex mu;
        std::condition_variable cv;
        uint32_t targetMethod = 0;
        bool entered = false;
        bool release = false;
        bool workerDone = false;
        std::atomic<bool> yieldOnFree{false};
    };

    static Gate *gGate = nullptr;

    auto runCase = [&](const char *label, uint32_t method, auto setup, auto workerFn, bool expectWorkerFalse) {
        SCOPED_TRACE(label);
        kernel_model::model = {};
        kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;

        auto dev = std::make_unique<gm20b::Device>();
        std::string err;
        ASSERT_TRUE(dev->open(emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        setup(*dev);

        Gate gate;
        gate.targetMethod = method;
        gGate = &gate;
        kernel_model::arriving.store(
            [](uint32_t m, const void *input) -> int {
                Gate *g = gGate;
                if (!g)
                    return KERN_SUCCESS;
                if (m == g->targetMethod) {
                    if (m == TegraGM20BMethodBoAlloc &&
                        (static_cast<const TegraGM20BAbiBoAlloc *>(input)->flags & TegraGM20BBoControl) != 0)
                        return KERN_SUCCESS;
                    std::unique_lock<std::mutex> lk(g->mu);
                    if (!g->entered) {
                        g->entered = true;
                        g->cv.notify_all();
                        g->cv.wait(lk, [&] {
                            return g->release;
                        });
                    }
                } else if (m == TegraGM20BMethodBoFree && g->yieldOnFree.load(std::memory_order_acquire)) {
                    for (int i = 0; i < 64; ++i)
                        std::this_thread::yield();
                }
                return KERN_SUCCESS;
            },
            std::memory_order_release);

        bool workerOk = true;
        std::thread worker([&] {
            workerOk = workerFn(*dev);
            std::lock_guard<std::mutex> lk(gate.mu);
            gate.workerDone = true;
            gate.cv.notify_all();
        });

        bool entered = false;
        {
            std::unique_lock<std::mutex> lk(gate.mu);
            gate.cv.wait(lk, [&] {
                return gate.entered || gate.workerDone;
            });
            entered = gate.entered;
        }
        EXPECT_TRUE(entered) << "worker never reached target off-lock kernel call";

        std::thread closer([&] {
            dev.reset();
        });
        if (entered) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::lock_guard<std::mutex> lk(gate.mu);
            gate.yieldOnFree.store(true, std::memory_order_release);
            gate.release = true;
            gate.cv.notify_all();
        }
        worker.join();
        closer.join();

        kernel_model::clearHooks();
        gGate = nullptr;

        if (expectWorkerFalse && entered)
            EXPECT_FALSE(workerOk);
        std::lock_guard<std::mutex> hold(kernel_model::callLock);
        EXPECT_TRUE(kernel_model::model.parkedOrder.empty());
        EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
    };

    runCase(
        "finishParkChunks", TegraGM20BMethodBoPark,
        [](gm20b::Device &d) {
            gm20b::Buffer b;
            ASSERT_TRUE(d.createBuffer(4 << 20, b, false));
            d.destroyBuffer(b);
        },
        [](gm20b::Device &d) {
            d.trim();
            return true;
        },
        false);

    runCase(
        "tryUnparkChunk", TegraGM20BMethodBoUnpark,
        [](gm20b::Device &d) {
            gm20b::Buffer b;
            ASSERT_TRUE(d.createBuffer(4 << 20, b, false));
            d.destroyBuffer(b);
            d.trim();
        },
        [](gm20b::Device &d) {
            gm20b::Buffer b;
            const bool ok = d.createBuffer(4 << 20, b, false);
            if (ok)
                d.destroyBuffer(b);
            return ok;
        },
        true);

    runCase(
        "newChunk", TegraGM20BMethodBoAlloc, [](gm20b::Device &) {},
        [](gm20b::Device &d) {
            gm20b::Buffer b;
            const bool ok = d.createBuffer(4 << 20, b, false);
            if (ok)
                d.destroyBuffer(b);
            return ok;
        },
        true);

    runCase(
        "volatileWcAlloc", TegraGM20BMethodBoAlloc, [](gm20b::Device &) {},
        [](gm20b::Device &d) {
            gm20b::Buffer b;
            const bool ok = d.createBuffer(64 << 10, b, true, true, true);
            if (ok)
                d.destroyBuffer(b);
            return ok;
        },
        true);
}

}  // namespace
