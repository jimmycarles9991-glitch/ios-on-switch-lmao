// SPDX-License-Identifier: MIT
// The device's last submitted seqno, read by a thread that does not submit while command buffers commit, thread-sanitized,
// on a client device over the kernel model. The memory release's purge of idle heaps (TegraGM20BMetal.mm's GpuMemoryRelease,
// Device::purgeArenas) reads it under the device lock, which the submitting thread does not take: NB ios27's
// facade_handlers_tsan_test reported that read against CommandBuffer::commit's store.
#include "kernel_model.h"
#include "empty_pack.h"
#include "../gpu.h"

#include <CoreFoundation/CoreFoundation.h>
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kCommits = 400;

class DeviceSubmitted : public testing::Test {
protected:
    gm20b::Device dev;
    gm20b::Buffer from, to;

    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        ASSERT_TRUE(dev.createBuffer(4096, from));
        ASSERT_TRUE(dev.createBuffer(4096, to));
    }

    // One command buffer of one copy: its seqno, 0 when it was not submitted
    uint32_t copy()
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
};

// The give-back line's control bytes, read on the release's thread before the first commit opens the channel: the submitting
// thread makes the rings under the channel's lock, and nothing orders the read before it (a relaxed flag)
TEST_F(DeviceSubmitted, TheGiveBackLinesControlBytesNeedNotTheRingsOwner)
{
    std::atomic<bool> read{false};
    uint32_t seqno = 0;
    std::thread submitter([&] {
        while (!read.load(std::memory_order_relaxed)) {
        }
        seqno = copy();
    });
    const uint64_t before = dev.controlMappedBytes();
    read.store(true, std::memory_order_relaxed);
    submitter.join();
    EXPECT_NE(seqno, 0u);
    EXPECT_GT(dev.controlMappedBytes(), before) << "the rings, made with the channel, are counted";
}

// Memory release trimming arenas and the stall tick reading completed() while the first commit opens the channel:
// openChannel writes fence_ and channel_ under channelLock_ before channelReady_, and nothing orders the reads before it
TEST_F(DeviceSubmitted, TheMemoryReleasesTrimReadsTheFenceBesideTheFirstChannelOpen)
{
    std::atomic<bool> read{false};
    std::atomic<uint32_t> committed{0};
    uint32_t seqno = 0;
    std::thread submitter([&] {
        while (!read.load(std::memory_order_relaxed)) {
        }
        seqno = copy();
    });
    dev.trimArenas(committed);
    (void)dev.completed();
    read.store(true, std::memory_order_relaxed);
    submitter.join();
    EXPECT_NE(seqno, 0u);
    EXPECT_TRUE(dev.done(seqno));
    EXPECT_GE(int32_t(dev.completed() - seqno), 0);
}

// Same reads while a killed channel reopens in another slot: reopen updates channel_ and fence_ on the submit thread
TEST_F(DeviceSubmitted, TheMemoryReleasesTrimReadsTheFenceBesideAReopen)
{
    ASSERT_NE(copy(), 0u);
    kernel_model::model.kill(1, 1);
    kernel_model::model.foreignTakesFreedSlot = true;
    std::atomic<bool> read{false};
    std::atomic<uint32_t> committed{0};
    uint32_t seqno = 0;
    std::thread submitter([&] {
        while (!read.load(std::memory_order_relaxed)) {
        }
        seqno = copy();
    });
    dev.trimArenas(committed);
    (void)dev.completed();
    read.store(true, std::memory_order_relaxed);
    submitter.join();
    EXPECT_NE(seqno, 0u);
    EXPECT_TRUE(dev.done(seqno));
    EXPECT_GE(int32_t(dev.completed() - seqno), 0);
}

TEST_F(DeviceSubmitted, TheMemoryReleasesPurgeReadsWhatTheSubmitterLastSubmitted)
{
    ASSERT_NE(copy(), 0u);
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> committed{0};
    std::atomic<int> purges{0};
    std::thread release([&] {
        while (!stop.load(std::memory_order_acquire)) {
            dev.purgeArenas(committed);
            purges.fetch_add(1, std::memory_order_relaxed);
        }
    });
    uint32_t last = 0;
    for (int i = 0; i < kCommits; i++) {
        const uint32_t seqno = copy();
        EXPECT_NE(seqno, 0u);
        EXPECT_GT(int32_t(seqno - last), 0);
        last = seqno;
    }
    stop.store(true, std::memory_order_release);
    release.join();
    EXPECT_GT(purges.load(), 0);
    EXPECT_EQ(dev.submitted(), last);
}

// Concurrent teardown, no-copy release/grow, kept-import eviction, arena destroy/purge/trim, memory watch reads and
// submissions run race-free under TSan with every BoFree/vm_deallocate/CFRelease off Device::lock_
TEST_F(DeviceSubmitted, ConcurrentTeardownTrimPurgeAndHeldRunRaceFreeOffTheLock)
{
    constexpr uint32_t kRGBA8 = 70;
    constexpr uint64_t kSurfaceBytes = 256 << 10, kArenaBytes = 4 * (64 << 10);

    struct Surface {
        void *pages = nullptr;
        CFTypeRef owner = nullptr;
        uint32_t id = 0;
    };

    std::vector<Surface> surfaces(10);
    for (uint32_t i = 0; i < 10; ++i) {
        ASSERT_EQ(posix_memalign(&surfaces[i].pages, size_t(getpagesize()), kSurfaceBytes), 0);
        surfaces[i].owner = CFDataCreate(nullptr, nullptr, 0);
        surfaces[i].id = 200 + i;
    }
    void *noCopyPages = nullptr;
    ASSERT_EQ(posix_memalign(&noCopyPages, size_t(getpagesize()), 128 << 10), 0);

    std::atomic<bool> stop{false};
    std::atomic<uint32_t> committed{0};
    std::atomic<int> heldReads{0}, trims{0}, submits{0};

    std::thread releaser([&] {
        while (!stop.load(std::memory_order_acquire)) {
            dev.trim();
            dev.purgeArenas(committed);
            dev.trimArenas(committed);
            trims.fetch_add(1, std::memory_order_relaxed);
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
            const uint32_t seqno = copy();
            committed.fetch_sub(1, std::memory_order_acq_rel);
            if (seqno)
                submits.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int round = 0; round < 20; ++round) {
        // No-copy release followed immediately by a larger wrap over the same pages
        gm20b::Buffer small, big;
        bool tiled = false;
        ASSERT_TRUE(dev.createBufferNoCopy(noCopyPages, 4 << 10, small, tiled));
        dev.destroyBuffer(small);
        ASSERT_TRUE(dev.createBufferNoCopy(noCopyPages, 128 << 10, big, tiled));
        dev.destroyBuffer(big);

        // Kept-import eviction across 10 surfaces
        for (const Surface &s : surfaces) {
            gm20b::Texture t;
            ASSERT_TRUE(
                dev.createSurfaceTexture(s.id, s.pages, kSurfaceBytes, s.owner, 0, 128, 64, 512, kRGBA8, t, true));
            dev.destroyTexture(t);
        }

        // Dedicated WC buffer, shared arena (with aliasable placement + volatile) and private arena
        gm20b::Buffer wc;
        ASSERT_TRUE(dev.createBuffer(2 << 20, wc, true));
        dev.destroyBuffer(wc);

        uint32_t shared = 0, priv = 0;
        ASSERT_TRUE(dev.createArena(kArenaBytes, false, shared));
        gm20b::Buffer placed;
        ASSERT_TRUE(dev.arenaBuffer(shared, 64 << 10, placed));
        dev.makeArenaAliasable(placed.span, placed.lease);
        dev.destroyBuffer(placed);
        dev.setArenaVolatile(shared, true);
        dev.destroyArena(shared);

        ASSERT_TRUE(dev.createArena(kArenaBytes, true, priv));
        dev.setArenaVolatile(priv, true);
        dev.destroyArena(priv);
    }

    stop.store(true, std::memory_order_release);
    submitter.join();
    watcher.join();
    releaser.join();
    dev.trim();
    EXPECT_GT(heldReads.load(), 0);
    EXPECT_GT(trims.load(), 0);
    EXPECT_GT(submits.load(), 0);

    free(noCopyPages);
    for (Surface &s : surfaces) {
        CFRelease(s.owner);
        free(s.pages);
    }
}

// Two threads tearing down their own no-copy buffers while a third trims: each call frees its own BOs synchronously
// with no cross-thread stealing or races under TSan
TEST_F(DeviceSubmitted, ConcurrentDestroyAndTrimEachFreeTheirOwnBosBeforeReturning)
{
    constexpr size_t kBytes = 64 << 10;
    void *pagesA = nullptr, *pagesB = nullptr;
    ASSERT_EQ(posix_memalign(&pagesA, size_t(getpagesize()), kBytes), 0);
    ASSERT_EQ(posix_memalign(&pagesB, size_t(getpagesize()), kBytes), 0);
    std::atomic<bool> stop{false};

    auto worker = [&](void *pages) {
        for (int i = 0; i < 40; ++i) {
            gm20b::Buffer b;
            bool tiled = false;
            ASSERT_TRUE(dev.createBufferNoCopy(pages, kBytes, b, tiled));
            const uint64_t handle = b.import->bo.handle;
            dev.destroyBuffer(b);
            EXPECT_NE(handle, 0u);
        }
    };
    std::thread trimmer([&] {
        while (!stop.load(std::memory_order_acquire))
            dev.trim();
    });
    std::thread a(worker, pagesA);
    std::thread b(worker, pagesB);
    a.join();
    b.join();
    stop.store(true, std::memory_order_release);
    trimmer.join();
    free(pagesA);
    free(pagesB);
}

TEST_F(DeviceSubmitted, ConcurrentAliasableVolatilePurgeAndHostAccessRunRaceFree)
{
    constexpr uint64_t kArenaBytes = 256 << 10;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> committed{0};
    std::atomic<int> purges{0};
    std::thread releaser([&] {
        while (!stop.load(std::memory_order_acquire)) {
            (void)dev.hasBoundVolatileArena();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            gm20b::Device::PurgeStats stats;
            dev.purgeArenas(committed, &stats);
#else
            dev.purgeArenas(committed);
#endif
            dev.trimArenas(committed);
            purges.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (int round = 0; round < 30; ++round) {
        for (bool blockLinear : {false, true}) {
            uint32_t arena = 0;
            ASSERT_TRUE(dev.createArena(kArenaBytes, blockLinear, arena));
            gm20b::Buffer b;
            ASSERT_TRUE(dev.arenaBuffer(arena, 64 << 10, b));
            b.hostVisible = !blockLinear;
            dev.makeArenaAliasable(b.span, b.lease);
            dev.setArenaVolatile(arena, true);
            if (!blockLinear) {
                gm20b::HostAccess access;
                if (dev.hostAccess(b, false, access))
                    EXPECT_TRUE(access.finish());
            }
            ASSERT_TRUE(dev.restoreArena(arena));
            dev.destroyBuffer(b);
            dev.destroyArena(arena);
        }
    }
    stop.store(true, std::memory_order_release);
    releaser.join();
    EXPECT_GT(purges.load(), 0);
}

}  // namespace
