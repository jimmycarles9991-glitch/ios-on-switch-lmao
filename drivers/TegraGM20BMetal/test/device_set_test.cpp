// SPDX-License-Identifier: MIT
#include "device_harness.h"
#include "../heap_members.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace {

using kernel_model::model;

constexpr uint64_t kQuarter = 0x40000, kArenaBytes = 4 * kQuarter;
constexpr uint32_t kDepth32 = 252;

class DeviceSet : public harness::DeviceTest {
protected:
    // One command buffer that used `heap` and copied a page of `from` to `to`: its seqno, 0 when it was not submitted
    uint32_t copyInHeap(const gm20b::HeapMembers &heap, const gm20b::Buffer &from, const gm20b::Buffer &to)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e)
            return 0;
        cb->useHeap(heap);
        cb->use(from, false);
        cb->use(to, true, false);
        e->copyBytes(from.va, to.va, 4096);
        e->end();
        uint32_t seqno = 0;
        return cb->commit(seqno) ? seqno : 0;
    }

    // The facade's idle give-back (TegraGM20BMetal.mm) with no command buffer committed
    void giveBack()
    {
        const std::atomic<uint32_t> idle{0};
        dev.trim();
        dev.purgeArenas(idle);
        dev.trimArenas(idle);
    }

    void completeEverything()
    {
        kernel_model::Slot *s = liveSlot();
        ASSERT_NE(s, nullptr);
        model.fence(liveChannel() - 1).completed = s->submitted;
        model.inFlight = false;
    }

    // The members the model's live sets list under `handle`
    static size_t listed(uint64_t handle)
    {
        size_t n = 0;
        for (const kernel_model::SetState &set : model.sets)
            for (const TegraGM20BAbiSurfaceUse &m : set.members)
                n += set.live && m.handle == handle;
        return n;
    }

    // Their accesses
    static uint32_t listedAccess(uint64_t handle)
    {
        uint32_t access = 0;
        for (const kernel_model::SetState &set : model.sets)
            for (const TegraGM20BAbiSurfaceUse &m : set.members)
                access |= set.live && m.handle == handle ? m.access : 0u;
        return access;
    }

    // A sampled depth texture's ask: in a private heap, its arena's view of the depth kind
    static gm20b::TextureAsk depthAsk()
    {
        gm20b::TextureAsk depth;
        depth.kind = gm20b::TextureAsk::Zeta;
        depth.width = 64;
        depth.height = 48;
        depth.format = kDepth32;
        depth.sampled = true;
        return depth;
    }

    // A member of a live set whose BO the connection no longer holds, 0: none
    static uint64_t goneMember()
    {
        for (const kernel_model::SetState &set : model.sets)
            for (const TegraGM20BAbiSurfaceUse &m : set.members)
                if (set.live && !model.bos.count(m.handle))
                    return m.handle;
        return 0;
    }

    // A BO the jobs from `first` on name (the set's members among their uses) that the connection no longer holds, 0:
    // none. The kernel refuses such a job (TegraGM20BUserClientJobSubmit.cpp); the model runs it.
    static uint64_t goneUse(size_t first)
    {
        for (size_t j = first; j < model.jobs.size(); ++j)
            for (const TegraGM20BAbiSurfaceUse &u : model.jobs[j].uses)
                if (!model.bos.count(u.handle))
                    return u.handle;
        return 0;
    }

    // A write-combined buffer in a chunk made for it, freed again: the chunk is the device's empty spare, a member the
    // CPU lease of the buffer's creation took out of the kernel's set while the device's list still names it
    uint64_t spareWcChunk()
    {
        const uint64_t chunk = model.nextHandle;
        gm20b::Buffer wc;
        if (!dev.createBuffer(4096, wc, true) || !wc.wc)
            return 0;
        dev.destroyBuffer(wc);
        return chunk;
    }

    std::string refusal() const
    {
        return "the kernel refused the set for member BO " + std::to_string(model.refusedHandle) +
               (freed(model.refusedHandle) ? ", which the device had freed" : "");
    }
};

// Boot 18 (T-v4b, SwitchHWTest 20:27:37-20:27:47): the app's device runs, a second raw device opens and closes in the
// process on a connection of its own (HW Test's bench), the device's idle give-back takes its pool to no chunks and
// frees its spare write-combined chunk, and the next allocation makes the set again. Every command buffer after it runs
// raw, on the set, and the kernel refuses no registration.
TEST_F(DeviceSet, TheIdleGiveBackAfterABenchLeavesASetTheKernelTakes)
{
    gm20b::Buffer a, b;
    ASSERT_TRUE(dev.createBuffer(4096, a));
    ASSERT_TRUE(dev.createBuffer(4096, b));
    ASSERT_NE(copy(a, b, 4096), 0u);
    const uint64_t spare = spareWcChunk();
    ASSERT_NE(spare, 0u);
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
    {
        gm20b::Device bench;
        std::string err;
        ASSERT_TRUE(bench.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        gm20b::Buffer x, y;
        ASSERT_TRUE(bench.createBuffer(4096, x));
        ASSERT_TRUE(bench.createBuffer(4096, y));
        gm20b::CommandBuffer *cb = bench.begin();
        gm20b::BlitEncoder *e = cb ? cb->blit() : nullptr;
        ASSERT_NE(e, nullptr);
        cb->use(x, false);
        cb->use(y, true, false);
        e->copyBytes(x.va, y.va, 4096);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        ASSERT_TRUE(bench.wait(seqno));
    }
    ASSERT_TRUE(model.others.empty()) << "the bench's connection stayed open";
    dev.trim();
    EXPECT_EQ(dev.stats.poolChunks.load(), 0u);
    EXPECT_TRUE(freed(spare)) << "the give-back kept the spare write-combined chunk";
    const unsigned jobs = model.setJobs;
    gm20b::Buffer c, d;
    ASSERT_TRUE(dev.createBuffer(4096, c));
    ASSERT_TRUE(dev.createBuffer(4096, d));
    for (int i = 0; i < 4; i++)
        EXPECT_NE(copy(c, d, 4096), 0u) << "command buffer " << i << " was refused: " << refusal();
    EXPECT_EQ(model.setJobs, jobs + 4) << "command buffers left the set";
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
}

// The same without the bench: the give-back's free of the spare chunk alone is enough
TEST_F(DeviceSet, AFreedSpareChunkLeavesTheSet)
{
    const uint64_t spare = spareWcChunk();
    ASSERT_NE(spare, 0u);
    dev.trim();
    ASSERT_TRUE(freed(spare));
    gm20b::Buffer a, b;
    ASSERT_TRUE(dev.createBuffer(4096, a));
    ASSERT_TRUE(dev.createBuffer(4096, b));
    EXPECT_NE(copy(a, b, 4096), 0u) << refusal();
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
}

// A second raw device's life on its own connection leaves the first device's set and channel as they were
TEST_F(DeviceSet, ABenchOnItsOwnConnectionLeavesTheSetAlone)
{
    gm20b::Buffer a, b;
    ASSERT_TRUE(dev.createBuffer(4096, a));
    ASSERT_TRUE(dev.createBuffer(4096, b));
    ASSERT_NE(copy(a, b, 4096), 0u);
    const unsigned stale = model.staleSets;
    {
        gm20b::Device bench;
        std::string err;
        ASSERT_TRUE(bench.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        gm20b::Buffer x;
        ASSERT_TRUE(bench.createBuffer(4096, x));
        bench.destroyBuffer(x);
        bench.trim();
    }
    EXPECT_NE(copy(b, a, 4096), 0u) << refusal();
    EXPECT_EQ(model.staleSets, stale);
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
}

// A private heap's depth texture makes its arena a view of the depth kind, whose VA is one more member under the arena's
// handle. The arena's free takes the BO out at every VA, and the next registration and command buffer name none of them.
TEST_F(DeviceSet, AnArenaFreedWithAViewAliasInTheSetLeavesNeitherItsBoNorTheAliasInTheNextRegistration)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, arena));
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, depthAsk(), t));
    ASSERT_GE(gm20b::DeviceHarness::arenaViews(dev, arena), 2u) << "no view of the depth kind";
    const uint64_t bo = gm20b::DeviceHarness::arena(dev, arena).handle;
    const uint64_t alias = t.span.va - t.span.offset;
    ASSERT_EQ(model.resolve({alias, TegraGM20BSurfaceGpuAddress, 0}), bo);
    ASSERT_TRUE(gm20b::DeviceHarness::setHolds(dev, alias)) << "the depth view is not a member";
    ASSERT_NE(listed(bo), 0u);

    dev.destroyTexture(t);
    dev.destroyArena(arena);
    ASSERT_TRUE(freed(bo));
    EXPECT_EQ(listed(bo), 0u) << "the registration after the free names the freed BO";
    EXPECT_EQ(goneMember(), 0u);
    EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, alias)) << "the device's set still holds the freed view's VA";
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
    EXPECT_EQ(model.refusedHandle, 0u) << refusal();

    uint32_t next = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, next));
    gm20b::Buffer a, b;
    ASSERT_TRUE(dev.arenaBuffer(next, 4096, a));
    ASSERT_TRUE(dev.arenaBuffer(next, 4096, b));
    // A client device opens its channel at its first command buffer, and the channel's context is a set job of its own
    ASSERT_TRUE(dev.ensureChannel()) << dev.channelError();
    const unsigned jobs = model.setJobs;
    const size_t first = model.jobs.size();
    const uint32_t seqno = copy(a, b, 4096);
    ASSERT_NE(seqno, 0u) << refusal();
    EXPECT_EQ(model.setJobs, jobs + 1) << "the command buffer left the set";
    EXPECT_EQ(goneUse(first), 0u);
    EXPECT_TRUE(dev.wait(seqno));
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
    EXPECT_EQ(dev.setCounts().refusals, 0u) << dev.setLastRefusal();
}

// The depth view joins after a copy wrote the arena's pitch view: a read member under the handle of written ones. The set is
// registered with the BO once, written. The texture's free leaves the arena a member at all three VAs, and a command
// buffer after it runs on the set naming the BO once; the arena's free takes all of them.
TEST_F(DeviceSet, AnArenasViewAliasesAreOneWrittenMemberUntilTheArenasFreeTakesThemAll)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, arena));
    gm20b::Buffer a, b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    ASSERT_NE(copy(a, b, 4096), 0u) << refusal();
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, depthAsk(), t));
    const uint64_t bo = gm20b::DeviceHarness::arena(dev, arena).handle;
    const uint64_t blockLinear = gm20b::DeviceHarness::arena(dev, arena).vaBase, pitch = a.va - a.span.offset,
                   alias = t.span.va - t.span.offset;
    ASSERT_GE(gm20b::DeviceHarness::arenaViews(dev, arena), 2u) << "no view of the depth kind";
    for (const uint64_t va : {blockLinear, pitch, alias}) {
        ASSERT_EQ(model.resolve({va, TegraGM20BSurfaceGpuAddress, 0}), bo) << std::hex << va;
        ASSERT_TRUE(gm20b::DeviceHarness::setHolds(dev, va)) << std::hex << va;
    }
    EXPECT_EQ(listed(bo), 1u);
    EXPECT_EQ(model.setRepeats, 0u) << "a registration named the arena's BO in more than one record";
    EXPECT_TRUE(listedAccess(bo) & TegraGM20BSurfaceWrite) << "the depth view's read member dropped the written ones";

    dev.destroyTexture(t);
    EXPECT_FALSE(freed(bo));
    EXPECT_EQ(listed(bo), 1u) << "the texture's free took the arena out of the set";
    for (const uint64_t va : {blockLinear, pitch, alias})
        EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, va)) << std::hex << va;
    const unsigned jobs = model.setJobs;
    const uint32_t seqno = copy(b, a, 4096);
    ASSERT_NE(seqno, 0u) << refusal();
    EXPECT_EQ(model.setJobs, jobs + 1) << "the command buffer left the set";
    const auto &uses = model.jobs.back().uses;
    EXPECT_EQ(std::count_if(uses.begin(), uses.end(),
                            [&](const TegraGM20BAbiSurfaceUse &u) {
                                return u.handle == bo;
                            }),
              1);
    EXPECT_TRUE(dev.wait(seqno));

    dev.arenaRelease(a);
    dev.arenaRelease(b);
    dev.destroyArena(arena);
    ASSERT_TRUE(freed(bo));
    EXPECT_EQ(listed(bo), 0u);
    EXPECT_EQ(goneMember(), 0u);
    for (const uint64_t va : {blockLinear, pitch, alias})
        EXPECT_FALSE(gm20b::DeviceHarness::setHolds(dev, va)) << std::hex << va;
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
    EXPECT_EQ(dev.setCounts().refusals, 0u) << dev.setLastRefusal();
}

// The set could not be made again for a depth view of a private heap's arena (the kernel had no set left to make). The
// arena's BO stays the member the kernel's set holds, at every VA, the view's too, so a copy through its pitch view runs on
// the set and names no member as a use.
TEST_F(DeviceSet, AViewAliasTheSetCouldNotTakeLeavesItsArenaAMemberAtEveryVa)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, arena));
    gm20b::Buffer a, b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    ASSERT_NE(copy(a, b, 4096), 0u) << refusal();
    const uint64_t bo = gm20b::DeviceHarness::arena(dev, arena).handle;
    const uint64_t blockLinear = gm20b::DeviceHarness::arena(dev, arena).vaBase, pitch = a.va - a.span.offset;

    model.refuseSets = 1;
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, depthAsk(), t));
    ASSERT_EQ(model.refuseSets, 0u) << "the depth view's registration was not the one refused";
    const uint64_t alias = t.span.va - t.span.offset;
    for (const uint64_t va : {blockLinear, pitch, alias})
        EXPECT_TRUE(gm20b::DeviceHarness::setHolds(dev, va)) << std::hex << va;
    EXPECT_EQ(listed(bo), 1u);

    const unsigned jobs = model.setJobs;
    const uint32_t seqno = copy(b, a, 4096);
    ASSERT_NE(seqno, 0u) << "the copy named the arena, a member of the kernel's set, as a use";
    EXPECT_EQ(model.setJobs, jobs + 1) << "the command buffer left the set";
    EXPECT_TRUE(dev.wait(seqno));
    EXPECT_EQ(goneMember(), 0u);
}

// The give-back purges no volatile heap's arena while a command buffer that used the heap is in flight; once it completed,
// the arena's BO is freed. The set lists no freed BO, and the command buffers that use the heap once it is bound again,
// one of them placing where a released buffer was, run raw on the set naming none.
TEST_F(DeviceSet, AHeapsArenaPurgedOnceTheCommandBufferNamingItCompletedLeavesNoFreedBoInTheSetOrTheNextJobs)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, arena));
    ASSERT_FALSE(dev.setArenaVolatile(arena, true));
    gm20b::Buffer a, b, c;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    dev.arenaRelease(b);
    const gm20b::HeapMembers heap(arena, false);
    const uint64_t whole = gm20b::DeviceHarness::arena(dev, arena).handle;

    model.inFlight = true;
    uint32_t seqno = copyInHeap(heap, a, c);
    ASSERT_NE(seqno, 0u) << refusal();
    giveBack();
    EXPECT_FALSE(freed(whole)) << "the arena was purged under a command buffer in flight";
    completeEverything();
    ASSERT_TRUE(dev.wait(seqno));

    giveBack();
    ASSERT_TRUE(freed(whole)) << "the give-back kept the volatile arena";
    EXPECT_EQ(listed(whole), 0u);
    EXPECT_EQ(goneMember(), 0u);
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
    dev.setArenaVolatile(arena, false);
    ASSERT_TRUE(dev.restoreArena(arena));

    const unsigned jobs = model.setJobs;
    size_t first = model.jobs.size();
    seqno = copyInHeap(heap, a, c);
    ASSERT_NE(seqno, 0u) << refusal();
    EXPECT_EQ(model.setJobs, jobs + 1) << "the command buffer left the set";
    EXPECT_EQ(goneUse(first), 0u);
    EXPECT_TRUE(dev.wait(seqno));

    gm20b::Buffer d;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, d));
    ASSERT_EQ(d.span.offset, kQuarter) << "the placement is not where the released buffer was";
    first = model.jobs.size();
    seqno = copyInHeap(heap, d, a);
    ASSERT_NE(seqno, 0u) << refusal();
    EXPECT_EQ(model.setJobs, jobs + 2) << "the command buffer left the set";
    EXPECT_EQ(goneUse(first), 0u);
    EXPECT_EQ(goneMember(), 0u);
    EXPECT_TRUE(dev.wait(seqno));
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
    EXPECT_EQ(model.refusedHandle, 0u) << refusal();
    EXPECT_EQ(dev.setCounts().refusals, 0u) << dev.setLastRefusal();
}

// Shared buffers live in VOL=1 write-combined chunks, which no CPU lease takes out of the set (createBuffer skips the
// lease, and a write-combined buffer's invalidateForCpu takes none): such a chunk is a written member from the start, so
// the first GPU write to one of its buffers names no use of the chunk and does not make the set again
TEST_F(DeviceSet, AWriteToASharedBufferNamesNoUseAndKeepsTheSetsGeneration)
{
    gm20b::Buffer a, b;
    ASSERT_TRUE(dev.createBuffer(768, a, true, true, true));
    ASSERT_TRUE(dev.createBuffer(768, b, true, true, true));
    ASSERT_TRUE(a.gpuVolatile);
    ASSERT_EQ(a.span.chunk, b.span.chunk) << "the test wants both in one chunk";
    uint64_t chunk = 0;
    for (const auto &[va, bound] : model.binds)
        if (a.va >= va && a.va - va < bound.second)
            chunk = bound.first;
    ASSERT_NE(chunk, 0u);
    EXPECT_TRUE(listedAccess(chunk) & TegraGM20BSurfaceWrite) << "the chunk joined the set read";
    {
        gm20b::CommandBuffer *warm = dev.begin();
        ASSERT_NE(warm, nullptr);
        uint32_t seqno = 0;
        ASSERT_TRUE(warm->commit(seqno));
    }
    const auto generations = [] {
        std::vector<uint32_t> g;
        for (const kernel_model::SetState &set : model.sets)
            g.push_back(set.live ? set.generation : 0);
        return g;
    };
    const std::vector<uint32_t> before = generations();
    const size_t first = model.jobs.size();
    gm20b::CommandBuffer *cb = dev.begin();
    gm20b::BlitEncoder *e = cb ? cb->blit() : nullptr;
    ASSERT_NE(e, nullptr);
    cb->use(b, false);
    cb->use(a, true);
    e->copyBytes(b.va, a.va, 768);
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_EQ(generations(), before) << "the write made the set again for its chunk";
    ASSERT_GT(model.jobs.size(), first);
    // a job's uses in the model are its listed ones and then the set's members, and the model refuses a job that lists
    // a member: the chunk once, through the set
    for (size_t j = first; j < model.jobs.size(); ++j) {
        size_t named = 0;
        for (const TegraGM20BAbiSurfaceUse &u : model.jobs[j].uses)
            named += u.handle == chunk;
        EXPECT_LE(named, 1u) << "a job lists the chunk beside the set that names it";
    }
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
}

}  // namespace
