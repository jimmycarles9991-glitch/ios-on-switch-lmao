// SPDX-License-Identifier: MIT
// A shared MTLHeap's arena purged and made again on gm20b::Device over the kernel model (device_harness.h,
// arena.cpp): the purge frees its BO and gives its write-combined pages back, its GPU and CPU addresses and texture
// headers stay, and the restore imports the same host range again and binds each of its mappings where it was. What the
// heap held comes back zero, as the CPU and the GPU read the fresh pages alike (Metal leaves it undefined). No purge
// while a job submitted may still reach it, an m2v view holds it, or a command buffer is committed.
#include "device_harness.h"
#include "../heap_members.h"
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
        return gm20b::DeviceHarness::arena(dev, arena).handle;
    }

    static uint64_t boAt(uint64_t va)
    {
        return kernel_model::model.resolve({va, TegraGM20BSurfaceGpuAddress, 0});
    }

    // A job of one copy whose command buffer used `heap`: what it names
    std::vector<TegraGM20BAbiSurfaceUse> submitUsing(const gm20b::HeapMembers &heap)
    {
        gm20b::Buffer from, to;
        EXPECT_TRUE(dev.createBuffer(64, from) && dev.createBuffer(64, to));
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        cb->internalCompletion();
        cb->useHeap(heap);
        cb->use(from, false);
        cb->use(to, true, false);
        gm20b::BlitEncoder *e = cb->blit();
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
    ASSERT_TRUE(gm20b::DeviceHarness::arenaWriteCombined(dev, arena));
    gm20b::Buffer b;
    gm20b::Texture pitch, chain;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    ASSERT_TRUE(dev.arenaTexture(arena, 16, 16, kRGBA8, pitch));
    gm20b::TextureAsk mips;
    mips.kind = gm20b::TextureAsk::Mips;
    mips.width = mips.height = 64;
    mips.levels = 4;
    mips.format = kRGBA8;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, mips, chain));  // the shared arena's block-linear view
    memset(b.cpu, 0x5A, 4096);
    const uint64_t before = bo();
    const auto pitchHeader = gm20b::DeviceHarness::header(dev, pitch.tic),
               chainHeader = gm20b::DeviceHarness::header(dev, chain.tic);
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
    const gm20b::HeapMembers heap(arena, true);
    EXPECT_EQ(named(submitUsing(heap), before), 0u);

    ASSERT_TRUE(dev.restoreArena(arena));
    EXPECT_FALSE(dev.arenaPurged(arena));
    const uint64_t after = bo();
    ASSERT_NE(after, before);
    const kernel_model::BoState &made = kernel_model::model.bos.at(after);
    EXPECT_TRUE(made.import);
    EXPECT_EQ(made.address, uint64_t(uintptr_t(gm20b::DeviceHarness::arenaPages(dev, arena))));
    EXPECT_EQ(made.bytes, kArenaBytes);
    EXPECT_EQ(boAt(va), after);
    EXPECT_EQ(boAt(pitch.va), after);
    EXPECT_EQ(boAt(chainVa), after);
    EXPECT_EQ(b.cpu, cpu);
    EXPECT_TRUE(zero(cpu, 4096));
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, pitch.tic), pitchHeader);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, chain.tic), chainHeader);
    EXPECT_EQ(dev.arenaBytes(), kArenaBytes);

    gm20b::Buffer later;
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
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    const gm20b::HeapMembers heap(arena, true);
    kernel_model::model.inFlight = true;
    submitUsing(heap);
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a job in flight";
    completeEverything();

    gm20b::DeviceHarness::holdArena(dev, arena, true);
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a lease (a CPU access, or a placement made aliasable)";
    gm20b::DeviceHarness::holdArena(dev, arena, false);

    committed = 1;
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a command buffer committed";
    EXPECT_EQ(dev.purgedArenas(), 0u);
    committed = 0;

    dev.purgeArena(arena, committed);
    EXPECT_TRUE(dev.arenaPurged(arena));
    EXPECT_EQ(dev.purgedArenas(), 1u);
}

// A private heap's arena is a BO from the kernel's spares, no import of pages the creating thread zero-fills, bound at the
// arena's VAs with no CPU mapping; a purge frees it and a restore binds another at the same VAs (22 Nov: backboardd's 8 MiB
// heaps, restored after each idle give-back, zero-filled their pages on the render thread, 4.4 ms each)
TEST_F(DeviceHeapPurge, APrivateArenaIsASpareBoAtItsVasAndComesBackAsAnother)
{
    const size_t imports = kernel_model::model.imports.size();
    uint32_t priv = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, priv));
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture(priv, 64, 64, kRGBA8, t));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(priv, 4096, b));
    const uint64_t first = gm20b::DeviceHarness::arena(dev, priv).handle;
    ASSERT_NE(first, 0u);
    EXPECT_FALSE(kernel_model::model.bos.at(first).import);
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, priv).cpu, nullptr);
    EXPECT_EQ(boAt(t.va), first);
    EXPECT_EQ(boAt(b.va), first);
    EXPECT_EQ(kernel_model::model.imports.size(), imports);

    dev.setArenaVolatile(priv, true);
    dev.purgeArena(priv, committed);
    ASSERT_TRUE(dev.arenaPurged(priv));
    EXPECT_EQ(kernel_model::model.bos.count(first), 0u);
    EXPECT_EQ(boAt(t.va), 0u);

    ASSERT_TRUE(dev.restoreArena(priv));
    const uint64_t again = gm20b::DeviceHarness::arena(dev, priv).handle;
    ASSERT_NE(again, first);
    EXPECT_FALSE(kernel_model::model.bos.at(again).import);
    EXPECT_EQ(boAt(t.va), again);
    EXPECT_EQ(boAt(b.va), again);
    EXPECT_EQ(kernel_model::model.imports.size(), imports);
    dev.arenaRelease(b);
    dev.arenaRelease(t);
    dev.destroyArena(priv);
}

// A private heap's arena is one BO: the idle trim gives none of it back (a shared heap's own pages are what it trims)
TEST_F(DeviceHeapPurge, TheTrimLeavesAPrivateArenaWhole)
{
    uint32_t priv = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, priv));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(priv, 4096, b));
    const uint64_t whole = gm20b::DeviceHarness::arena(dev, priv).handle;
    dev.trimArenas(committed);
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, priv).handle, whole);
    EXPECT_EQ(kernel_model::model.bos.count(whole), 1u);
    dev.arenaRelease(b);
    dev.destroyArena(priv);
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

TEST_F(DeviceHeapPurge, VolatileAndEmptyArenasPurgeWithLiveAliasablePlacements)
{
    for (bool blockLinear : {false, true}) {
        SCOPED_TRACE(blockLinear ? "private" : "shared");
        uint32_t id = 0;
        ASSERT_TRUE(dev.createArena(kArenaBytes, blockLinear, id));
        gm20b::Buffer b;
        ASSERT_TRUE(dev.arenaBuffer(id, 4096, b));
        dev.makeArenaAliasable(b.span, b.lease);
        EXPECT_EQ(dev.arenaUsed(id), 0u);

        dev.setArenaVolatile(id, true);
        dev.purgeArenas(committed);
        EXPECT_TRUE(dev.arenaPurged(id)) << "aliasable lease must not block volatile purge";
        ASSERT_TRUE(dev.restoreArena(id));
        EXPECT_FALSE(dev.arenaPurged(id));

        dev.setArenaVolatile(id, false);
        dev.purgeArena(id, committed);
        EXPECT_TRUE(dev.arenaPurged(id)) << "aliasable lease must not block explicit Empty purge";
        ASSERT_TRUE(dev.restoreArena(id));

        const uint64_t handle = gm20b::DeviceHarness::arena(dev, id).handle;
        ASSERT_NE(handle, 0u);
        dev.trimArenas(committed);
        EXPECT_EQ(gm20b::DeviceHarness::arena(dev, id).handle, handle)
            << "non-volatile trim still keeps mappings while an aliasable placement lives";

        dev.destroyArena(id);
        EXPECT_EQ(kernel_model::model.bos.count(handle), 1u)
            << "destroying the arena waits for the aliasable lease to drop";
        dev.destroyBuffer(b);
        EXPECT_EQ(kernel_model::model.bos.count(handle), 0u);
    }
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
TEST_F(DeviceHeapPurge, PurgeStatsReportsPurgedAndEverySkipReason)
{
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    b.hostVisible = true;
    dev.setArenaVolatile(arena, true);

    gm20b::Device::PurgeStats stats;
    committed.store(2);
    dev.purgeArenas(committed, &stats);
    EXPECT_EQ(stats.purged, 0u);
    EXPECT_EQ(stats.skipped, 1u);
    EXPECT_EQ(stats.skippedBytes, kArenaBytes);
    EXPECT_EQ(stats.committed, 1u);
    EXPECT_EQ(stats.committedCount, 2u);
    EXPECT_EQ(stats.leases, 0u);
    EXPECT_EQ(stats.inFlight, 0u);
    EXPECT_EQ(stats.busy, 0u);
    EXPECT_EQ(stats.nonEmpty, 1u);
    committed.store(0);

    gm20b::HostAccess access;
    ASSERT_TRUE(dev.hostAccess(b, false, access));
    dev.purgeArenas(committed, &stats);
    EXPECT_EQ(stats.purged, 0u);
    EXPECT_EQ(stats.skipped, 1u);
    EXPECT_EQ(stats.leases, 1u);
    EXPECT_EQ(stats.committed, 0u);
    EXPECT_EQ(stats.inFlight, 0u);
    EXPECT_EQ(stats.nonEmpty, 1u);
    ASSERT_TRUE(access.finish());

    const gm20b::HeapMembers heap(arena, true);
    kernel_model::model.inFlight = true;
    submitUsing(heap);
    dev.purgeArenas(committed, &stats);
    EXPECT_EQ(stats.purged, 0u);
    EXPECT_EQ(stats.skipped, 1u);
    EXPECT_EQ(stats.inFlight, 1u);
    EXPECT_EQ(stats.leases, 0u);
    EXPECT_EQ(stats.committed, 0u);
    completeEverything();
    kernel_model::model.inFlight = false;

    dev.makeArenaAliasable(b.span, b.lease);
    dev.purgeArenas(committed, &stats);
    EXPECT_EQ(stats.purged, 1u);
    EXPECT_EQ(stats.purgedBytes, kArenaBytes);
    EXPECT_EQ(stats.skipped, 0u);
    EXPECT_EQ(stats.skippedBytes, 0u);
    EXPECT_TRUE(dev.arenaPurged(arena));

    dev.destroyBuffer(b);
}
#endif

namespace {
gm20b::Device *gHeapCheckDev = nullptr;
std::atomic<int> gHeapLockViolations{0};
uint64_t gExpectAliveBeforeFixedMap = 0;
std::atomic<bool> gSawFixedMap{false};
std::atomic<bool> gFreedBeforeFixedMap{false};

int checkHeapCallsOffLock(uint32_t method, const void *input)
{
    if (gHeapCheckDev &&
        (method == TegraGM20BMethodBoPark || method == TegraGM20BMethodBoUnpark || method == TegraGM20BMethodBoFree ||
         (method == TegraGM20BMethodBoAlloc &&
          (static_cast<const TegraGM20BAbiBoAlloc *>(input)->flags & TegraGM20BBoControl) == 0) ||
         (method == TegraGM20BMethodMap &&
          static_cast<const TegraGM20BAbiMap *>(input)->token != kernel_model::kFenceToken))) {
        if (!gm20b::DeviceHarness::lockFree(*gHeapCheckDev))
            gHeapLockViolations.fetch_add(1, std::memory_order_relaxed);
    }
    if (method == TegraGM20BMethodMap && gExpectAliveBeforeFixedMap &&
        (static_cast<const TegraGM20BAbiMap *>(input)->flags & TegraGM20BMapFixed)) {
        gSawFixedMap.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> hold(kernel_model::callLock);
        if (kernel_model::model.bos.count(gExpectAliveBeforeFixedMap) == 0)
            gFreedBeforeFixedMap.store(true, std::memory_order_release);
    }
    return KERN_SUCCESS;
}

uint64_t resolveVa(uint64_t va)
{
    return kernel_model::model.resolve({va, TegraGM20BSurfaceGpuAddress, 0});
}

bool allZero(const uint8_t *bytes, size_t n)
{
    return std::all_of(bytes, bytes + n, [](uint8_t b) {
        return b == 0;
    });
}
}  // namespace

TEST(DeviceHeapPurgeBoPark, SharedHeapParksOnPurgeAndUnparksResidentWithZeroHostZeroing)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gHeapCheckDev = &dev;
    gHeapLockViolations.store(0, std::memory_order_relaxed);
    gExpectAliveBeforeFixedMap = 0;
    kernel_model::arriving.store(checkHeapCallsOffLock, std::memory_order_release);
    kernel_model::HooksCleared clearHooks;

    const size_t importsBefore = kernel_model::model.imports.size();
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, arena));
    ASSERT_TRUE(gm20b::DeviceHarness::arenaWriteCombined(dev, arena));
    EXPECT_EQ(kernel_model::model.imports.size(), importsBefore);
#if SWITCH_METAL_DIAGNOSTICS
    EXPECT_EQ(dev.stats.hostZeroedBytes, 0u);
#endif

    gm20b::Buffer b;
    gm20b::Texture pitch, chain;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    ASSERT_TRUE(dev.arenaTexture(arena, 16, 16, kRGBA8, pitch));
    gm20b::TextureAsk mips;
    mips.kind = gm20b::TextureAsk::Mips;
    mips.width = mips.height = 64;
    mips.levels = 4;
    mips.format = kRGBA8;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, mips, chain));

    const uint64_t before = gm20b::DeviceHarness::arena(dev, arena).handle;
    ASSERT_NE(before, 0u);
    EXPECT_TRUE(kernel_model::model.coherentFromPool);
    EXPECT_FALSE(kernel_model::model.bos.at(before).import);
    EXPECT_TRUE(kernel_model::model.bos.at(before).coherent);
    EXPECT_TRUE(kernel_model::model.bos.at(before).system);
    EXPECT_FALSE(kernel_model::model.bos.at(before).poolVa);
    uint8_t *const cpu = b.cpu;
    const uint64_t va = b.va, chainVa = chain.va;
    const auto pitchHeader = gm20b::DeviceHarness::header(dev, pitch.tic),
               chainHeader = gm20b::DeviceHarness::header(dev, chain.tic);
    memset(cpu, 0x5A, 4096);

    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, arena).handle, before);

    dev.setArenaVolatile(arena, true);
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    EXPECT_EQ(kernel_model::model.parkCalls, 1u);
    EXPECT_EQ(kernel_model::model.parkedBytes, kArenaBytes);
    EXPECT_EQ(kernel_model::model.bos.at(before).parkState, kernel_model::ParkState::Parked);
    EXPECT_EQ(dev.arenaBytes(), 0u);
    gm20b::Device::Held h = {};
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.heaps, 0u);
    EXPECT_TRUE(allZero(cpu, 4096));
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);

    ASSERT_TRUE(dev.restoreArena(arena));
    EXPECT_FALSE(dev.arenaPurged(arena));
    EXPECT_EQ(kernel_model::model.unparkResident, 1u);
    EXPECT_EQ(kernel_model::model.unparkReclaimed, 0u);
    EXPECT_EQ(kernel_model::model.fixedMaps, 0u);
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, arena).handle, before);
    EXPECT_EQ(b.cpu, cpu);
    EXPECT_EQ(gm20b::DeviceHarness::arenaPages(dev, arena), cpu);
    EXPECT_EQ(resolveVa(va), before);
    EXPECT_EQ(resolveVa(chainVa), before);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, pitch.tic), pitchHeader);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, chain.tic), chainHeader);
    EXPECT_EQ(kernel_model::model.imports.size(), importsBefore);
#if SWITCH_METAL_DIAGNOSTICS
    EXPECT_EQ(dev.stats.hostZeroedBytes, 0u);
#endif
    EXPECT_TRUE(std::all_of(cpu, cpu + 4096, [](uint8_t byte) {
        return byte == 0x5A;
    }));
    EXPECT_EQ(gHeapLockViolations.load(), 0);
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);

    dev.arenaRelease(b);
    dev.arenaRelease(pitch);
    dev.arenaRelease(chain);
    dev.destroyArena(arena);
}

TEST(DeviceHeapPurgeBoPark, SharedHeapReclaimedRestoresViaMapFixedAtIdenticalCpuPointer)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gHeapCheckDev = &dev;
    gHeapLockViolations.store(0, std::memory_order_relaxed);
    gSawFixedMap.store(false, std::memory_order_relaxed);
    gFreedBeforeFixedMap.store(false, std::memory_order_relaxed);
    kernel_model::arriving.store(checkHeapCallsOffLock, std::memory_order_release);
    kernel_model::HooksCleared clearHooks;

    const size_t importsBefore = kernel_model::model.imports.size();
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, arena));
    gm20b::Buffer b;
    gm20b::Texture chain;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    gm20b::TextureAsk mips;
    mips.kind = gm20b::TextureAsk::Mips;
    mips.width = mips.height = 64;
    mips.levels = 4;
    mips.format = kRGBA8;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, mips, chain));

    const uint64_t before = gm20b::DeviceHarness::arena(dev, arena).handle;
    uint8_t *const cpu = b.cpu;
    const uint64_t va = b.va, chainVa = chain.va;
    memset(cpu, 0x5A, 4096);

    std::atomic<uint32_t> committed{0};
    dev.setArenaVolatile(arena, true);
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    ASSERT_TRUE(kernel_model::model.reclaimParked(before));
    EXPECT_EQ(kernel_model::model.bos.at(before).parkState, kernel_model::ParkState::Reclaimed);

    gExpectAliveBeforeFixedMap = before;
    ASSERT_TRUE(dev.restoreArena(arena));
    gExpectAliveBeforeFixedMap = 0;

    EXPECT_TRUE(gSawFixedMap.load());
    EXPECT_FALSE(gFreedBeforeFixedMap.load());
    EXPECT_EQ(kernel_model::model.unparkReclaimed, 1u);
    EXPECT_EQ(kernel_model::model.fixedMaps, 1u);
    EXPECT_EQ(kernel_model::model.bos.count(before), 0u);
    const uint64_t after = gm20b::DeviceHarness::arena(dev, arena).handle;
    ASSERT_NE(after, 0u);
    EXPECT_NE(after, before);
    EXPECT_EQ(b.cpu, cpu);
    EXPECT_EQ(gm20b::DeviceHarness::arenaPages(dev, arena), cpu);
    EXPECT_EQ(resolveVa(va), after);
    EXPECT_EQ(resolveVa(chainVa), after);
    EXPECT_EQ(kernel_model::model.imports.size(), importsBefore);
#if SWITCH_METAL_DIAGNOSTICS
    EXPECT_EQ(dev.stats.hostZeroedBytes, 0u);
#endif
    memset(cpu, 0x3C, 4096);
    ASSERT_NE(kernel_model::model.host(va), nullptr);
    EXPECT_EQ(kernel_model::model.host(va)[0], 0x3C);
    EXPECT_EQ(kernel_model::model.host(va)[4095], 0x3C);
    EXPECT_EQ(gHeapLockViolations.load(), 0);
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);

    dev.arenaRelease(b);
    dev.arenaRelease(chain);
    dev.destroyArena(arena);
}

TEST(DeviceHeapPurgeBoPark, SharedHeapTransientParkBusyAbortsCleanlyAndDestroyWhileParkedOrReclaimed)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    ASSERT_TRUE(kernel_model::model.coherentFromPool);
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    uint32_t busyArena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, busyArena));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(busyArena, 4096, b));
    uint8_t *const cpu = b.cpu;
    const uint64_t va = b.va;
    const uint64_t before = gm20b::DeviceHarness::arena(dev, busyArena).handle;
    ASSERT_NE(before, 0u);
    EXPECT_TRUE(kernel_model::model.bos.at(before).system);
    EXPECT_FALSE(kernel_model::model.bos.at(before).poolVa);
    memset(cpu, 0x5A, 4096);

    std::atomic<uint32_t> committed{0};
    dev.setArenaVolatile(busyArena, true);
    kernel_model::model.forceParkBusy = true;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gm20b::Device::PurgeStats stats = {};
    dev.purgeArenas(committed, &stats);
    EXPECT_EQ(stats.purged, 0u);
    EXPECT_EQ(stats.skipped, 1u);
    EXPECT_EQ(stats.busy, 1u);
    EXPECT_EQ(stats.committed, 0u);
#else
    dev.purgeArenas(committed);
#endif
    EXPECT_FALSE(dev.arenaPurged(busyArena));
    EXPECT_EQ(dev.purgedArenas(), 0u);
    EXPECT_EQ(kernel_model::model.parkCalls, 0u);
    EXPECT_EQ(kernel_model::model.parkRefusals, 1u);
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, busyArena).handle, before);
    EXPECT_EQ(resolveVa(va), before);
    EXPECT_TRUE(std::all_of(cpu, cpu + 4096, [](uint8_t byte) {
        return byte == 0x5A;
    }));
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);

    kernel_model::model.forceParkBusy = false;
    dev.purgeArenas(committed);
    ASSERT_TRUE(dev.arenaPurged(busyArena));
    EXPECT_EQ(kernel_model::model.parkCalls, 1u);
    dev.arenaRelease(b);
    dev.destroyArena(busyArena);

    uint32_t parkedArena = 0, reclaimedArena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, parkedArena));
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, reclaimedArena));
    const uint64_t h1 = gm20b::DeviceHarness::arena(dev, parkedArena).handle;
    const uint64_t h2 = gm20b::DeviceHarness::arena(dev, reclaimedArena).handle;
    dev.setArenaVolatile(parkedArena, true);
    dev.setArenaVolatile(reclaimedArena, true);
    dev.purgeArenas(committed);
    ASSERT_TRUE(dev.arenaPurged(parkedArena));
    ASSERT_TRUE(dev.arenaPurged(reclaimedArena));
    ASSERT_TRUE(kernel_model::model.reclaimParked(h2));

    dev.destroyArena(parkedArena);
    dev.destroyArena(reclaimedArena);
    EXPECT_EQ(kernel_model::model.bos.count(h1), 0u);
    EXPECT_EQ(kernel_model::model.bos.count(h2), 0u);
    EXPECT_TRUE(kernel_model::model.parkedOrder.empty());
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
}

TEST(DeviceHeapPurgeBoPark, SharedHeapUnparkMismatchDropsLiveMappingWithoutLeak)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, arena));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    const uint64_t before = gm20b::DeviceHarness::arena(dev, arena).handle;
    ASSERT_NE(before, 0u);

    std::atomic<uint32_t> committed{0};
    dev.setArenaVolatile(arena, true);
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    ASSERT_EQ(kernel_model::model.parkCalls, 1u);

    kernel_model::model.overrideUnparkBytes = 4096;
    EXPECT_FALSE(dev.restoreArena(arena));
    kernel_model::model.overrideUnparkBytes = 0;
    EXPECT_TRUE(dev.arenaPurged(arena));
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, arena).handle, 0u);
    EXPECT_EQ(kernel_model::model.bos.count(before), 0u);
    EXPECT_TRUE(kernel_model::model.parkedOrder.empty());
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
    EXPECT_FALSE(dev.restoreArena(arena));
    EXPECT_EQ(kernel_model::model.fixedMaps, 0u);
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
    EXPECT_TRUE(dev.setArenaVolatileIfPurged(arena));

    dev.arenaRelease(b);
    dev.destroyArena(arena);
    EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
}

TEST(DeviceHeapPurgeBoPark, SetArenaVolatileIfPurgedSkipsResidentArena)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, arena));
    EXPECT_FALSE(dev.setArenaVolatileIfPurged(arena));
    EXPECT_FALSE(dev.hasBoundVolatileArena());
    dev.destroyArena(arena);
}

TEST(DeviceHeapPurgeBoPark, SharedHeapAboveMaxBoBytesFallsBackToHostImport)
{
    kernel_model::model = {};
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    kernel_model::model.maxBoBytes = kArenaBytes / 2;
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;

    const size_t importsBefore = kernel_model::model.imports.size();
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, arena));
    EXPECT_EQ(kernel_model::model.imports.size(), importsBefore + 1);
    const uint64_t handle = gm20b::DeviceHarness::arena(dev, arena).handle;
    ASSERT_NE(handle, 0u);
    EXPECT_TRUE(kernel_model::model.bos.at(handle).import);
    dev.destroyArena(arena);
}

}  // namespace
