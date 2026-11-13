// SPDX-License-Identifier: MIT
// A shared MTLHeap's arena purged and made again on raw::Device over the kernel model (raw_device_harness.h,
// raw_arena.cpp): the purge frees its BO and gives its write-combined pages back, its GPU and CPU addresses and texture
// headers stay, and the restore imports the same host range again and binds each of its mappings where it was. What the
// heap held comes back zero, as the CPU and the GPU read the fresh pages alike (Metal leaves it undefined). No purge
// while a job submitted may still reach it, an m2v view holds it, or a command buffer is committed.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_heap_members.h"
#include <algorithm>
#include <atomic>

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint64_t kArenaBytes = 1u << 20;

class DeviceHeapPurge : public harness::DeviceTest {
protected:
    uint32_t arena = 0;
    std::atomic<uint32_t> committed{0};

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createArena(kArenaBytes, false, arena));
    }

    uint64_t bo() const
    {
        return raw::DeviceHarness::arena(dev, arena).handle;
    }

    static uint64_t boAt(uint64_t va)
    {
        return kernel_model::model.resolve({va, TegraGM20BSurfaceGpuAddress, 0});
    }

    // A job of one copy whose command buffer used `heap`: what it names
    std::vector<TegraGM20BAbiSurfaceUse> submitUsing(const raw::HeapMembers &heap)
    {
        raw::Buffer from, to;
        EXPECT_TRUE(dev.createBuffer(64, from) && dev.createBuffer(64, to));
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        cb->internalCompletion();
        cb->useHeap(heap);
        raw::BlitEncoder *e = cb->blit();
        e->copyBytes(from.va, to.va, 64);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return kernel_model::model.jobs.back().uses;
    }

    static size_t named(const std::vector<TegraGM20BAbiSurfaceUse> &uses, uint64_t handle)
    {
        return size_t(std::count_if(uses.begin(), uses.end(), [&](const TegraGM20BAbiSurfaceUse &u) {
            return u.handle == handle;
        }));
    }

    static bool zero(const uint8_t *bytes, size_t n)
    {
        return std::all_of(bytes, bytes + n, [](uint8_t b) {
            return b == 0;
        });
    }

    void completeEverything()
    {
        kernel_model::Slot *s = liveSlot();
        ASSERT_NE(s, nullptr);
        kernel_model::model.fence(liveChannel() - 1).completed = s->submitted;
    }
};

TEST_F(DeviceHeapPurge, APurgedArenaComesBackZeroAtTheSameAddressesAndHeaders)
{
    ASSERT_TRUE(raw::DeviceHarness::arenaWriteCombined(dev, arena));
    raw::Buffer b;
    raw::Texture pitch, chain;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    ASSERT_TRUE(dev.arenaTexture(arena, 16, 16, kRGBA8, pitch));
    raw::TextureAsk mips;
    mips.kind = raw::TextureAsk::Mips;
    mips.width = mips.height = 64;
    mips.levels = 4;
    mips.format = kRGBA8;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, mips, chain));  // the shared arena's block-linear view
    memset(b.cpu, 0x5A, 4096);
    const uint64_t before = bo();
    const auto pitchHeader = raw::DeviceHarness::header(dev, pitch.tic),
               chainHeader = raw::DeviceHarness::header(dev, chain.tic);
    uint8_t *const cpu = b.cpu;
    const uint64_t va = b.va, chainVa = chain.va;
    EXPECT_EQ(boAt(va), before);
    EXPECT_EQ(boAt(chainVa), before);

    dev.setArenaVolatile(arena, true);
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    EXPECT_EQ(kernel_model::model.bos.count(before), 0u);
    EXPECT_EQ(boAt(va), 0u);
    EXPECT_EQ(boAt(chainVa), 0u);
    EXPECT_EQ(dev.arenaBytes(), 0u);
    EXPECT_TRUE(zero(cpu, 4096));  // the host range is fresh pages already, at the same address
    const raw::HeapMembers heap(arena, true);
    EXPECT_EQ(named(submitUsing(heap), before), 0u);

    ASSERT_TRUE(dev.restoreArena(arena));
    EXPECT_FALSE(dev.arenaPurged(arena));
    const uint64_t after = bo();
    ASSERT_NE(after, before);
    const kernel_model::BoState &made = kernel_model::model.bos.at(after);
    EXPECT_TRUE(made.import);
    EXPECT_EQ(made.address, uint64_t(uintptr_t(raw::DeviceHarness::arenaPages(dev, arena))));
    EXPECT_EQ(made.bytes, kArenaBytes);
    EXPECT_EQ(boAt(va), after);
    EXPECT_EQ(boAt(pitch.va), after);
    EXPECT_EQ(boAt(chainVa), after);
    EXPECT_EQ(b.cpu, cpu);
    EXPECT_TRUE(zero(cpu, 4096));
    EXPECT_EQ(raw::DeviceHarness::header(dev, pitch.tic), pitchHeader);
    EXPECT_EQ(raw::DeviceHarness::header(dev, chain.tic), chainHeader);
    EXPECT_EQ(dev.arenaBytes(), kArenaBytes);

    raw::Buffer later;
    ASSERT_TRUE(dev.arenaBuffer(arena, 256, later));
    EXPECT_EQ(boAt(later.va), after);
    memset(later.cpu, 0x3C, 256);
    EXPECT_EQ(cpu[later.va - va], 0x3C);
    const auto uses = submitUsing(heap);
    EXPECT_EQ(named(uses, after), 1u);
    EXPECT_EQ(named(uses, before), 0u);
}

TEST_F(DeviceHeapPurge, NoPurgeWhileAJobAViewOrACommittedCommandBufferMayReachIt)
{
    raw::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    const raw::HeapMembers heap(arena, true);
    kernel_model::model.inFlight = true;
    submitUsing(heap);
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a job in flight";
    completeEverything();

    raw::DeviceHarness::holdArena(dev, arena, true);
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "an m2v view's lease";
    raw::DeviceHarness::holdArena(dev, arena, false);

    committed = 1;
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a command buffer committed";
    EXPECT_EQ(dev.purgedArenas(), 0u);
    committed = 0;

    dev.purgeArena(arena, committed);
    EXPECT_TRUE(dev.arenaPurged(arena));
    EXPECT_EQ(dev.purgedArenas(), 1u);
}

TEST_F(DeviceHeapPurge, OnlyVolatileArenasGoAtTheDevicesPurge)
{
    uint32_t kept = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, kept));
    dev.setArenaVolatile(arena, true);
    dev.purgeArenas(committed);
    EXPECT_TRUE(dev.arenaPurged(arena));
    EXPECT_FALSE(dev.arenaPurged(kept));
    dev.setArenaVolatile(arena, false);
    ASSERT_TRUE(dev.restoreArena(arena));
    EXPECT_EQ(dev.purgedArenas(), 0u);
}

}  // namespace
